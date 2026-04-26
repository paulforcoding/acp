#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <filesystem>
#include <set>
#include <sys/xattr.h>
#include "base/event_reporter.hpp"
#include "lib/combined/combined.hpp"

#ifdef HAS_LIBACL
#include <sys/acl.h>
#endif

CPFilePair::CPFilePair(std::string_view src_path,
                       std::string_view dst_path,
                       bool direct_io,
                       bool cksum,
                       bool cksum_only,
                       bool preserve_meta,
                       std::shared_ptr<ILogger> logger,
                       FileLogReporter* reporter)
    : mSrcPath(src_path),
      mDstPath(dst_path),
#ifdef O_DIRECT
      mDirectIO(direct_io),
#endif
      mCksum(cksum),
      mCksumOnly(cksum_only),
      mPreserveMeta(preserve_meta),
      mLogger(logger),
      mReporter(reporter)
{
    memset(&mSrcStat, 0, sizeof(struct stat));
    memset(&mDstStat, 0, sizeof(struct stat));
#ifndef O_DIRECT
    (void)direct_io;
#endif
}

tl::expected<void, StackError> CPFilePair::CheckAndInit()
{
    namespace fs = std::filesystem;
    // check if source file is symlink, if so, create a coresponding symlink on dst path
    if (fs::is_symlink(mSrcPath))
    {
        mIsSymlink = true;

        if (lstat(mSrcPath.c_str(), &mSrcStat) < 0)
        {
            return tl::unexpected(StackError(
                fmt::format("lstat src symlink failed: {}, errno: {}, errstr: {}", mSrcPath, errno, strerror(errno))));
        }

        if (mCksumOnly)
        {
            mSkipBlockCksum = true;
            mReadBytes = GetSrcFileSize();
            mWrittenBytes = GetSrcFileSize();
            if (mReporter)
                mReporter->FileStart(mSrcPath, mDstPath, 0);
            return {};
        }

        std::error_code ec;
        auto target_path = fs::read_symlink(mSrcPath, ec);
        if (ec)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to read symlink target of source file: {}, errstr: {}", mSrcPath, ec.message())));
        }

        // we must remove existing dst_path first if any
        // Note: std::filesystem::exists() follows symlinks. For a dangling symlink,
        // exists() returns false even though the symlink entry exists. Handle both.
        if (fs::is_symlink(fs::symlink_status(mDstPath)) || fs::exists(mDstPath))
        {
            fs::remove(mDstPath, ec); // removes the symlink itself if it's a symlink
            if (ec)
            {
                return tl::unexpected(StackError(
                    fmt::format("Failed to remove existing destination path: {}, errstr: {}", mDstPath, ec.message())));
            }
        }

        fs::create_symlink(target_path, mDstPath, ec);
        if (ec)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to create symlink at destination: {}, pointing to: {}, errstr: {}",
                            mDstPath, target_path.string(), ec.message())));
        }
        if (mPreserveMeta)
            PreserveMetadata();

        if (mReporter)
        {
            mReporter->FileStart(mSrcPath, mDstPath, 0);
            mReporter->FileComplete(mSrcPath, mDstPath, 0, 0);
            mReporter->IncrementFilesDone();
        }
        return {};
    }

    // check if source file is directory, create destination directory if so
    if (fs::is_directory(mSrcPath))
    {
        mIsDir = true;

        if (lstat(mSrcPath.c_str(), &mSrcStat) < 0)
        {
            return tl::unexpected(StackError(
                fmt::format("lstat src directory failed: {}, errno: {}, errstr: {}", mSrcPath, errno, strerror(errno))));
        }

        if (mCksumOnly)
        {
            mSkipBlockCksum = true;
            mReadBytes = GetSrcFileSize();
            mWrittenBytes = GetSrcFileSize();
            if (mReporter)
                mReporter->FileStart(mSrcPath, mDstPath, 0);
            return {};
        }

        std::error_code ec;
        fs::create_directories(mDstPath, ec);
        if (ec)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to create destination directory: {}, errstr: {}", mDstPath, ec.message())));
        }
        if (mPreserveMeta)
            PreserveMetadata();

        if (mReporter)
        {
            mReporter->FileStart(mSrcPath, mDstPath, 0);
            mReporter->FileComplete(mSrcPath, mDstPath, 0, 0);
            mReporter->IncrementFilesDone();
        }
        return {};
    }

    // check if source file is not regular file, return error
    if (!fs::is_regular_file(mSrcPath))
    {
        // std::filesystem::file_type is not directly formattable by fmt, cast to int for diagnostic
        if (mReporter)
        {
            mReporter->FileError(mSrcPath, fmt::format("unsupported file type: {}", static_cast<int>(fs::status(mSrcPath).type())), ENOTSUP, "skipped");
        }
        return tl::unexpected(StackError(
            fmt::format("src: {} is not supported file type: {}", mSrcPath, static_cast<int>(fs::status(mSrcPath).type())), ENOTSUP));
    }

    // check source file
    mLogger->trace("Opening source file: {}", mSrcPath);
#ifdef O_DIRECT
    if (mDirectIO)
    {
        mSrcFd = open(mSrcPath.c_str(), O_RDONLY | O_DIRECT);
    }
    else
#endif
    {
        mSrcFd = open(mSrcPath.c_str(), O_RDONLY);
    }

    if (mSrcFd < 0)
    {
        if (mReporter)
        {
            mReporter->FileError(mSrcPath, strerror(errno), errno, "failed");
        }
        return tl::unexpected(StackError(
            fmt::format("open src file failed: {}, errno: {}, errstr: {}", mSrcPath, errno, strerror(errno)), errno));
    }
    if (fstat(mSrcFd, &mSrcStat) < 0)
    {
        return tl::unexpected(StackError(
            fmt::format("fstat() src file failed: {}, errno: {}, errstr: {}", mSrcPath, errno, strerror(errno)), errno));
    }
    if (!S_ISREG(mSrcStat.st_mode))
    {
        return tl::unexpected(StackError("Source file is not a regular file: " + mSrcPath));
    }

    // Heuristic: if actual disk blocks are fewer than size/512, file is probably sparse
    mIsProbablySparse = (mSrcStat.st_blocks * 512 < mSrcStat.st_size);

    // check dst file path, create if not exist
    std::string dst_dir = fs::path(mDstPath).parent_path().string();
    if (!fs::exists(dst_dir))
    {
        std::error_code ec;
        fs::create_directories(dst_dir, ec);
        if (ec)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to create destination directory: {}, errstr: {}", dst_dir, ec.message())));
        }
    }

    mLogger->trace("Opening/creating destination file: {}", mDstPath);
    int dstOpenFlags = 0;
    if (mCksumOnly)
    {
        dstOpenFlags = O_RDONLY;
    }
    else
    {
        dstOpenFlags = O_CREAT;
        if (mCksum)
        {
            dstOpenFlags |= O_RDWR;
        }
        else
        {
            dstOpenFlags |= O_WRONLY | O_TRUNC;
        }
    }
#ifdef O_DIRECT
    if (mDirectIO)
    {
        dstOpenFlags |= O_DIRECT;
    }
#endif
    mDstFd = open(mDstPath.c_str(), dstOpenFlags, 0644);

    if (mDstFd < 0)
    {
        if (mCksumOnly && errno == ENOENT)
        {
            close(mSrcFd);
            mSrcFd = -1;
            mSkipBlockCksum = true;
            mReadBytes = static_cast<size_t>(mSrcStat.st_size);
            mWrittenBytes = static_cast<size_t>(mSrcStat.st_size);
            RecordCksumContent("dst_missing");
            return {};
        }
        if (mReporter)
        {
            mReporter->FileError(mSrcPath, strerror(errno), errno, "failed");
        }
        return tl::unexpected(StackError(
            fmt::format("Failed to open/create destination file: {}, errno: {}, errstr: {}", mDstPath, errno, strerror(errno)), errno));
    }

    // Fast-path for CksumCopy: if dst exists with same size and mtime,
    // skip block-level checksum entirely (rsync-style optimization).
    if (mCksum && !mCksumOnly && mDstFd >= 0)
    {
        struct stat dstStat;
        if (fstat(mDstFd, &dstStat) == 0)
        {
            if (mSrcStat.st_size == dstStat.st_size)
            {
#ifdef __APPLE__
                auto srcMtime = mSrcStat.st_mtimespec;
                auto dstMtime = dstStat.st_mtimespec;
#else
                auto srcMtime = mSrcStat.st_mtim;
                auto dstMtime = dstStat.st_mtim;
#endif
                if (srcMtime.tv_sec == dstMtime.tv_sec && srcMtime.tv_nsec == dstMtime.tv_nsec)
                {
                    close(mSrcFd); mSrcFd = -1;
                    close(mDstFd); mDstFd = -1;
                    mSkipBlockCksum = true;
                    mReadBytes = static_cast<size_t>(mSrcStat.st_size);
                    mWrittenBytes = static_cast<size_t>(mSrcStat.st_size);
                    mStartTime = std::chrono::steady_clock::now();
                    if (mReporter)
                    {
                        mReporter->FileStart(mSrcPath, mDstPath, GetSrcFileSize());
                    }
                    RecordCksumContent("size_mtime_match");
                    return {};
                }
            }
        }
    }

    if (mReporter)
    {
        mReporter->FileStart(mSrcPath, mDstPath, GetSrcFileSize());
    }
    mStartTime = std::chrono::steady_clock::now();
    return {};
}

// DoDstState: 在写入完成后对目标文件执行 fstat，更新 mDstStat 用于后续元数据比较。
// 注意返回类型为 std::string 而非 StackError，是历史债务，不影响功能。
tl::expected<void, std::string> CPFilePair::DoDstState()
{
    if (fstat(mDstFd, &mDstStat) < 0)
    {
        return tl::unexpected(fmt::format("Failed to fstat destination file: {}, errno: {}, errstr: {}",
                                          mDstPath, errno, strerror(errno)));
    }

    return {};
}

// GetNextReadIO: 从 FPChannel 取出下一个可读的文件对，初始化未就绪的文件。
// 核心逻辑：Peek 队首 → 未初始化则 CheckAndInit → ENOTSUP 则跳过并报告 → 否则返回文件对指针。
// 返回 nullptr 表示 Channel 已关闭且无更多工作。
tl::expected<std::shared_ptr<CPFilePair>, StackError> CPFilePairMgr::GetNextReadIO()
{
    while (true)
    {
        auto front = mChannel.PeekFront();
        if (!front)
        {
            mLogger->trace("GetNextReadIO() ended");
            return nullptr;
        }

        if (!front->IsInitialized())
        {
            auto init_res = front->CheckAndInit();
            if (!init_res)
            {
                // ENOTSUP 表示不支持的文件类型（如 FIFO），跳过而非失败，保持复制流程继续
                if (init_res.error().Code() == ENOTSUP)
                {
                    if (mReporter)
                    {
                        mReporter->FileUnsupported(front->GetSrcPath(), init_res.error().ToString(), "skipped");
                    }
                    mChannel.PopFront();
                    continue;
                }
                init_res.error().Append("front->CheckAndInit(), err: ");
                return Unexpt(init_res.error());
            }
        }

        if (front->IsDir() || front->IsSymlink())
        {
            if (mOptions.CopyMode == "CksumOnly")
            {
                auto read_complete_res = CheckReadCompleteNoLock(front);
                if (!read_complete_res)
                {
                    return tl::unexpected(read_complete_res.error());
                }
                if (read_complete_res.value())
                {
                    continue;
                }
            }
            mChannel.PopFront();
            continue;
        }

        auto read_complete_res = CheckReadCompleteNoLock(front);
        if (!read_complete_res)
        {
            return tl::unexpected(read_complete_res.error());
        }
        if (read_complete_res.value())
        {
            continue;
        }
        if (mReporter)
        {
            mReporter->SetCurrentFile(front->GetSrcPath());
        }
        mLogger->trace("GetNextReadIO() return, src: {}, dst: {}, read offset: {}",
                       front->GetSrcPath(), front->GetDstPath(), front->GetReadOffset());
        return front;
    }
}

// CheckReadComplete: 检测指定文件对的读取是否全部完成。
// 场景：IOSlotMgr 在 HandleReadCompletion 中调用，用于判断是否可以触发该文件的写入阶段。
tl::expected<void, StackError> CPFilePairMgr::CheckReadComplete(std::shared_ptr<CPFilePair> pFP)
{
    auto res = CheckReadCompleteNoLock(pFP);
    if (!res)
    {
        return tl::unexpected(res.error());
    }
    return {};
}

// CheckReadCompleteNoLock: 无锁检查读取完成状态。
// 设计意图：mReadBytes >= src_file_size 表示所有 block 已读完，可进入写入阶段。
// 零字节文件或 SkipBlockCksum 文件在此处直接短路到 CheckWriteComplete，避免无意义的 IO 等待。
tl::expected<bool, StackError> CPFilePairMgr::CheckReadCompleteNoLock(std::shared_ptr<CPFilePair> pFP)
{
    mLogger->trace("CheckReadCompleteNoLock(): src: {}, dst: {}, preparedReadBytes: {}, totalBytes: {}, IsReadFinished: {}",
                   pFP->GetSrcPath(), pFP->GetDstPath(),
                   pFP->GetReadOffset(), pFP->GetSrcFileSize(), pFP->IsReadFinished());

    if (pFP->IsReadFinished())
    {
        mChannel.MoveToInflight(pFP);

        if (pFP->IsInitialized() &&
            (pFP->GetSrcFileSize() == 0 || pFP->IsSkipBlockCksum()))
        {
            mLogger->debug("Source file size is 0 or skip block cksum, directly checking write completion for src: {}, dst: {}",
                           pFP->GetSrcPath(), pFP->GetDstPath());
            auto write_res = CheckWriteComplete(pFP);
            if (!write_res)
            {
                return tl::unexpected(StackError("CheckWriteComplete() for 0-size or skip-cksum file, err: ", write_res.error()));
            }
        }

        return true;
    }
    return false;
}

// CheckWriteComplete: 检测指定文件对的写入是否全部完成。
// 完成后的处理链：Truncate → DoDstState(fstat dst) → PreserveMetadata → fsync → 关闭 fd → FileComplete 报告。
// 与 CheckReadComplete 不同，写入完成会触发副作用（元数据保留、资源释放、Reporter 事件）。
tl::expected<void, StackError> CPFilePairMgr::CheckWriteComplete(std::shared_ptr<CPFilePair> pFP)
{
    assert(pFP != nullptr);
    mLogger->debug("Checking write completion for file pair: src: {}, dst: {}",
                   pFP->GetSrcPath(), pFP->GetDstPath());

    if (pFP->IsWriteFinished())
    {
        if (!pFP->IsSkipBlockCksum() && (mOptions.DirectIO || pFP->HasHoles() || mOptions.CopyMode == "CksumCopy"))
        {
            auto truncate_res = pFP->TruncateDstToSrcSize();
            if (!truncate_res)
            {
                return tl::unexpected(StackError("pFP->TruncateDstToSrcSize(), err: ", truncate_res.error()));
            }
        }
        if (mOptions.SyncWrites && pFP->GetSrcFileSize() > 0 && !pFP->IsSkipBlockCksum())
        {
            auto fsync_res = pFP->FsyncDst();
            if (!fsync_res)
            {
                return tl::unexpected(StackError("pFP->FsyncDst(), err: ", fsync_res.error()));
            }
        }

        if (mOptions.PreserveMeta)
        {
            if (mOptions.CopyMode == "CksumOnly")
            {
                // Emit block-level checksum result for CksumOnly:
                // - If block checksum was performed and all blocks matched, emit content_match.
                // - If block checksum was skipped (symlink/dir/dst_missing), skip this.
                // - Mismatches are already emitted per-block in HandleReadCompletion.
                if (!pFP->IsSkipBlockCksum() && !pFP->GetCksumError())
                {
                    pFP->RecordCksumContent("match");
                }
                else if (pFP->GetCksumError())
                {
                    pFP->RecordCksumContent("mismatch");
                }
                else if (pFP->GetCksumContentResult().empty())
                {
                    pFP->RecordCksumContent("not_applicable");
                }
                auto diff_res = pFP->CompareMetadata();
                if (!diff_res)
                {
                    mLogger->warn("CompareMetadata failed for {}: {}",
                                  pFP->GetSrcPath(), diff_res.error().ToString());
                }
                pFP->FlushCksumResult();
            }
            else if (!pFP->IsSkipBlockCksum())
            {
                auto meta_res = pFP->PreserveMetadata();
                if (!meta_res)
                {
                    mLogger->warn("PreserveMetadata failed for {}: {}",
                                  pFP->GetSrcPath(), meta_res.error().ToString());
                }
            }
        }

        mChannel.RemoveFromInflight(pFP);

        if (mReporter)
        {
            mReporter->FileComplete(pFP->GetSrcPath(), pFP->GetDstPath(), pFP->GetSrcFileSize(), pFP->GetElapsedMs());
            mReporter->IncrementFilesDone();
        }
    }
    return {};
}

// === PreserveMeta 实现 ===

tl::expected<void, StackError> CPFilePair::PreserveMetadata()
{
    if (!mPreserveMeta)
        return {};

    if (auto r = PreserveMode(); !r)
        EmitMetaWarning("mode", errno);
    if (auto r = PreserveOwnership(); !r)
        EmitMetaWarning("ownership", errno);
    if (auto r = PreserveTimestamps(); !r)
        EmitMetaWarning("mtime", errno);
    if (auto r = PreserveXattr(); !r)
        EmitMetaWarning("xattr", errno);
    if (auto r = PreserveAcl(); !r)
        EmitMetaWarning("acl", errno);

    return {};
}

void CPFilePair::EmitMetaWarning(const std::string &metaType, int err)
{
    if (mReporter)
    {
        mReporter->FileMetaWarning(mSrcPath, metaType, strerror(err), err);
    }
}

tl::expected<void, StackError> CPFilePair::PreserveMode()
{
    mode_t mode = mSrcStat.st_mode & 07777;
    if (mIsSymlink)
    {
#ifdef __APPLE__
        if (lchmod(mDstPath.c_str(), mode) < 0)
            return tl::unexpected(StackError("lchmod failed", errno));
#else
        // Linux: symlink mode is always 0777, nothing to preserve
        (void)mode;
#endif
    }
    else if (mDstFd >= 0)
    {
        if (fchmod(mDstFd, mode) < 0)
            return tl::unexpected(StackError("fchmod failed", errno));
    }
    else
    {
        if (chmod(mDstPath.c_str(), mode) < 0)
            return tl::unexpected(StackError("chmod failed", errno));
    }
    return {};
}

tl::expected<void, StackError> CPFilePair::PreserveOwnership()
{
    int rc;
    if (mIsSymlink)
    {
        rc = lchown(mDstPath.c_str(), mSrcStat.st_uid, mSrcStat.st_gid);
    }
    else if (mDstFd >= 0)
    {
        rc = fchown(mDstFd, mSrcStat.st_uid, mSrcStat.st_gid);
    }
    else
    {
        rc = chown(mDstPath.c_str(), mSrcStat.st_uid, mSrcStat.st_gid);
    }

    if (rc < 0)
    {
        if (errno == EPERM)
            return {}; // 非 root 用户预期行为，静默
        return tl::unexpected(StackError("chown failed", errno));
    }
    return {};
}

tl::expected<void, StackError> CPFilePair::PreserveTimestamps()
{
    struct timespec times[2];
#ifdef __APPLE__
    times[0] = mSrcStat.st_atimespec;
    times[1] = mSrcStat.st_mtimespec;
#else
    times[0] = mSrcStat.st_atim;
    times[1] = mSrcStat.st_mtim;
#endif

    int rc;
    if (mIsSymlink)
    {
        rc = utimensat(AT_FDCWD, mDstPath.c_str(), times, AT_SYMLINK_NOFOLLOW);
    }
    else if (mDstFd >= 0)
    {
        rc = futimens(mDstFd, times);
    }
    else
    {
        rc = utimensat(AT_FDCWD, mDstPath.c_str(), times, 0);
    }

    if (rc < 0)
        return tl::unexpected(StackError("utimens failed", errno));
    return {};
}

tl::expected<void, StackError> CPFilePair::PreserveXattr()
{
#ifdef __APPLE__
    // macOS: fgetxattr/fsetxattr 签名: (fd, name, value, size, position, options)
    int srcFd = mSrcFd;
    int dstFd = mDstFd;
    if (mIsSymlink)
    {
        // symlink 用 path-based API
        srcFd = -1;
        dstFd = -1;
    }

    ssize_t listLen = (srcFd >= 0)
                          ? flistxattr(srcFd, nullptr, 0, 0)
                          : listxattr(mSrcPath.c_str(), nullptr, 0, XATTR_NOFOLLOW);
    if (listLen <= 0)
        return {};

    std::vector<char> nameBuf(listLen);
    listLen = (srcFd >= 0)
                  ? flistxattr(srcFd, nameBuf.data(), listLen, 0)
                  : listxattr(mSrcPath.c_str(), nameBuf.data(), listLen, XATTR_NOFOLLOW);
    if (listLen <= 0)
        return {};

    for (char *name = nameBuf.data(); name < nameBuf.data() + listLen;
         name += strlen(name) + 1)
    {
        // 跳过 macOS 系统 xattr（系统通过 fcopyfile() 自动管理）
        if (strncmp(name, "com.apple.", 10) == 0)
            continue;

        ssize_t valLen = (srcFd >= 0)
                             ? fgetxattr(srcFd, name, nullptr, 0, 0, 0)
                             : getxattr(mSrcPath.c_str(), name, nullptr, 0, 0, XATTR_NOFOLLOW);
        if (valLen < 0)
            continue;

        std::vector<char> valBuf(valLen);
        valLen = (srcFd >= 0)
                     ? fgetxattr(srcFd, name, valBuf.data(), valLen, 0, 0)
                     : getxattr(mSrcPath.c_str(), name, valBuf.data(), valLen, 0, XATTR_NOFOLLOW);
        if (valLen < 0)
            continue;

        int rc = (dstFd >= 0)
                     ? fsetxattr(dstFd, name, valBuf.data(), valLen, 0, 0)
                     : setxattr(mDstPath.c_str(), name, valBuf.data(), valLen, 0, XATTR_NOFOLLOW);
        if (rc < 0)
        {
            mLogger->warn("setxattr failed for {} on {}: {}",
                          name, mDstPath, strerror(errno));
        }
    }
#elif defined(__linux__)
    int srcFd = mIsSymlink ? -1 : mSrcFd;
    const char *srcPath = mSrcPath.c_str();
    const char *dstPath = mDstPath.c_str();

    ssize_t listLen = (srcFd >= 0)
                          ? flistxattr(srcFd, nullptr, 0)
                          : llistxattr(srcPath, nullptr, 0);
    if (listLen <= 0)
        return {};

    std::vector<char> nameBuf(listLen);
    listLen = (srcFd >= 0)
                  ? flistxattr(srcFd, nameBuf.data(), listLen)
                  : llistxattr(srcPath, nameBuf.data(), listLen);
    if (listLen <= 0)
        return {};

    for (char *name = nameBuf.data(); name < nameBuf.data() + listLen;
         name += strlen(name) + 1)
    {
        // 跳过 SELinux 上下文
        if (strcmp(name, "security.selinux") == 0)
            continue;

        ssize_t valLen = (srcFd >= 0)
                             ? fgetxattr(srcFd, name, nullptr, 0)
                             : lgetxattr(srcPath, name, nullptr, 0);
        if (valLen < 0)
            continue;

        std::vector<char> valBuf(valLen);
        valLen = (srcFd >= 0)
                     ? fgetxattr(srcFd, name, valBuf.data(), valLen)
                     : lgetxattr(srcPath, name, valBuf.data(), valLen);
        if (valLen < 0)
            continue;

        int rc = (mDstFd >= 0 && !mIsSymlink)
                     ? fsetxattr(mDstFd, name, valBuf.data(), valLen, 0)
                     : lsetxattr(dstPath, name, valBuf.data(), valLen, 0);
        if (rc < 0)
        {
            mLogger->warn("setxattr failed for {} on {}: {}",
                          name, dstPath, strerror(errno));
        }
    }
#endif
    return {};
}

tl::expected<void, StackError> CPFilePair::PreserveAcl()
{
#ifdef HAS_LIBACL
    if (mSrcFd < 0 || mDstFd < 0)
        return {};

    acl_t acl = acl_get_fd(mSrcFd);
    if (!acl)
        return {};

    int rc = acl_set_fd(mDstFd, acl);
    acl_free(acl);
    if (rc < 0)
        return tl::unexpected(StackError("acl_set_fd failed", errno));
#endif
    return {};
}

// === CompareMetadata 实现（CksumOnly 专用） ===

void CPFilePair::RecordCksumContent(const std::string &contentValue)
{
    mCksumContentResult = contentValue;
    mCksumMetaResult.clear();
    mCksumMetaDetails.clear();
    // For CksumCopy size_mtime_match fast path, flush immediately (no meta comparison)
    if (contentValue == "size_mtime_match")
    {
        mCksumMetaResult = "not_applicable";
        FlushCksumResult();
    }
}

void CPFilePair::RecordCksumMetaMismatch(const std::string &reason, const std::string &detail)
{
    mMetaMismatchEmitted = true;
    mCksumMetaResult = "mismatch";
    if (!detail.empty())
        mCksumMetaDetails.push_back(reason + ":" + detail);
    else
        mCksumMetaDetails.push_back(reason);
}

void CPFilePair::RecordCksumMetaDstMissing(const std::string &reason)
{
    mCksumMetaResult = "dst_missing";
    mCksumMetaDetails.push_back(reason);
}

void CPFilePair::RecordCksumMetaMatch()
{
    mCksumMetaResult = "match";
}

void CPFilePair::FlushCksumResult()
{
    if (!mReporter)
        return;

    // Derive combined result: only "match" or "mismatch"
    // - dst_missing on either dimension → mismatch
    // - mismatch on either dimension → mismatch
    // - otherwise → match (including "not_applicable" which means N/A, not a problem)
    std::string combinedResult;
    if (mCksumContentResult == "mismatch" || mCksumContentResult == "dst_missing" ||
        mCksumMetaResult == "mismatch" || mCksumMetaResult == "dst_missing")
        combinedResult = "mismatch";
    else
        combinedResult = "match";

    mReporter->FileCksumResult(mSrcPath, mDstPath, combinedResult,
                               mCksumContentResult, mCksumMetaResult,
                               mCksumMetaDetails);
}

tl::expected<void, StackError> CPFilePair::CompareMetadata()
{
    mMetaMismatchEmitted = false;
    mCksumMetaDetails.clear();
    mCksumMetaResult.clear();

    struct stat dstStat;
    if (lstat(mDstPath.c_str(), &dstStat) < 0)
    {
        RecordCksumMetaDstMissing("dst_stat_failed");
        return tl::unexpected(StackError("lstat dst failed", errno));
    }

    CompareMode(dstStat);
    CompareOwnership(dstStat);
    CompareTimestamps(dstStat);
    CompareXattr(dstStat);
    CompareAcl(dstStat);

    if (!mMetaMismatchEmitted)
    {
        RecordCksumMetaMatch();
    }

    return {};
}

tl::expected<void, StackError> CPFilePair::CompareMode(const struct stat &dstStat)
{
    mode_t srcMode = mSrcStat.st_mode & 07777;
    mode_t dstMode = dstStat.st_mode & 07777;
    if (srcMode != dstMode)
    {
        RecordCksumMetaMismatch("mode_mismatch",
                        fmt::format("src={:04o} dst={:04o}", srcMode, dstMode));
    }
    return {};
}

tl::expected<void, StackError> CPFilePair::CompareOwnership(const struct stat &dstStat)
{
    if (mSrcStat.st_uid != dstStat.st_uid)
    {
        RecordCksumMetaMismatch("owner_mismatch",
                        fmt::format("uid src={} dst={}", mSrcStat.st_uid, dstStat.st_uid));
    }
    if (mSrcStat.st_gid != dstStat.st_gid)
    {
        RecordCksumMetaMismatch("owner_mismatch",
                        fmt::format("gid src={} dst={}", mSrcStat.st_gid, dstStat.st_gid));
    }
    return {};
}

tl::expected<void, StackError> CPFilePair::CompareTimestamps(const struct stat &dstStat)
{
#ifdef __APPLE__
    auto srcMtime = mSrcStat.st_mtimespec;
    auto dstMtime = dstStat.st_mtimespec;
#else
    auto srcMtime = mSrcStat.st_mtim;
    auto dstMtime = dstStat.st_mtim;
#endif

    if (srcMtime.tv_sec != dstMtime.tv_sec || srcMtime.tv_nsec != dstMtime.tv_nsec)
    {
        RecordCksumMetaMismatch("timestamp_mismatch", "mtime");
    }
    // atime is intentionally not compared: it changes on every read access
    // and would cause false positives in CksumOnly mode
    return {};
}

tl::expected<void, StackError> CPFilePair::CompareXattr([[maybe_unused]] const struct stat &dstStat)
{
#ifdef __APPLE__
    int srcFd = mSrcFd;
    int dstFd = mDstFd;
    if (mIsSymlink)
    {
        srcFd = -1;
        dstFd = -1;
    }

    // Collect src xattrs
    std::set<std::string> srcNames;
    std::map<std::string, std::vector<char>> srcValues;

    ssize_t listLen = (srcFd >= 0)
                          ? flistxattr(srcFd, nullptr, 0, 0)
                          : listxattr(mSrcPath.c_str(), nullptr, 0, XATTR_NOFOLLOW);
    if (listLen > 0)
    {
        std::vector<char> nameBuf(listLen);
        listLen = (srcFd >= 0)
                      ? flistxattr(srcFd, nameBuf.data(), listLen, 0)
                      : listxattr(mSrcPath.c_str(), nameBuf.data(), listLen, XATTR_NOFOLLOW);
        if (listLen > 0)
        {
            for (char *name = nameBuf.data(); name < nameBuf.data() + listLen;
                 name += strlen(name) + 1)
            {
                if (strncmp(name, "com.apple.", 10) == 0)
                    continue;
                srcNames.insert(name);
                ssize_t valLen = (srcFd >= 0)
                                     ? fgetxattr(srcFd, name, nullptr, 0, 0, 0)
                                     : getxattr(mSrcPath.c_str(), name, nullptr, 0, 0, XATTR_NOFOLLOW);
                if (valLen >= 0)
                {
                    std::vector<char> valBuf(valLen);
                    valLen = (srcFd >= 0)
                                 ? fgetxattr(srcFd, name, valBuf.data(), valLen, 0, 0)
                                 : getxattr(mSrcPath.c_str(), name, valBuf.data(), valLen, 0, XATTR_NOFOLLOW);
                    if (valLen >= 0)
                        srcValues[name] = std::move(valBuf);
                }
            }
        }
    }

    // Collect dst xattrs
    std::set<std::string> dstNames;
    std::map<std::string, std::vector<char>> dstValues;

    listLen = (dstFd >= 0)
                  ? flistxattr(dstFd, nullptr, 0, 0)
                  : listxattr(mDstPath.c_str(), nullptr, 0, XATTR_NOFOLLOW);
    if (listLen > 0)
    {
        std::vector<char> nameBuf(listLen);
        listLen = (dstFd >= 0)
                      ? flistxattr(dstFd, nameBuf.data(), listLen, 0)
                      : listxattr(mDstPath.c_str(), nameBuf.data(), listLen, XATTR_NOFOLLOW);
        if (listLen > 0)
        {
            for (char *name = nameBuf.data(); name < nameBuf.data() + listLen;
                 name += strlen(name) + 1)
            {
                if (strncmp(name, "com.apple.", 10) == 0)
                    continue;
                dstNames.insert(name);
                ssize_t valLen = (dstFd >= 0)
                                     ? fgetxattr(dstFd, name, nullptr, 0, 0, 0)
                                     : getxattr(mDstPath.c_str(), name, nullptr, 0, 0, XATTR_NOFOLLOW);
                if (valLen >= 0)
                {
                    std::vector<char> valBuf(valLen);
                    valLen = (dstFd >= 0)
                                 ? fgetxattr(dstFd, name, valBuf.data(), valLen, 0, 0)
                                 : getxattr(mDstPath.c_str(), name, valBuf.data(), valLen, 0, XATTR_NOFOLLOW);
                    if (valLen >= 0)
                        dstValues[name] = std::move(valBuf);
                }
            }
        }
    }

    // Compare
    for (const auto &name : srcNames)
    {
        if (dstNames.find(name) == dstNames.end())
        {
            RecordCksumMetaMismatch("xattr_mismatch", "src_has:" + name);
        }
        else if (srcValues[name] != dstValues[name])
        {
            RecordCksumMetaMismatch("xattr_mismatch", "diff:" + name);
        }
    }
    for (const auto &name : dstNames)
    {
        if (srcNames.find(name) == srcNames.end())
        {
            RecordCksumMetaMismatch("xattr_mismatch", "dst_has:" + name);
        }
    }
#elif defined(__linux__)
    int srcFd = mIsSymlink ? -1 : mSrcFd;
    int dstFd = mIsSymlink ? -1 : mDstFd;
    const char *srcPath = mSrcPath.c_str();
    const char *dstPath = mDstPath.c_str();

    std::set<std::string> srcNames;
    std::map<std::string, std::vector<char>> srcValues;

    ssize_t listLen = (srcFd >= 0)
                          ? flistxattr(srcFd, nullptr, 0)
                          : llistxattr(srcPath, nullptr, 0);
    if (listLen > 0)
    {
        std::vector<char> nameBuf(listLen);
        listLen = (srcFd >= 0)
                      ? flistxattr(srcFd, nameBuf.data(), listLen)
                      : llistxattr(srcPath, nameBuf.data(), listLen);
        if (listLen > 0)
        {
            for (char *name = nameBuf.data(); name < nameBuf.data() + listLen;
                 name += strlen(name) + 1)
            {
                if (strcmp(name, "security.selinux") == 0)
                    continue;
                srcNames.insert(name);
                ssize_t valLen = (srcFd >= 0)
                                     ? fgetxattr(srcFd, name, nullptr, 0)
                                     : lgetxattr(srcPath, name, nullptr, 0);
                if (valLen >= 0)
                {
                    std::vector<char> valBuf(valLen);
                    valLen = (srcFd >= 0)
                                 ? fgetxattr(srcFd, name, valBuf.data(), valLen)
                                 : lgetxattr(srcPath, name, valBuf.data(), valLen);
                    if (valLen >= 0)
                        srcValues[name] = std::move(valBuf);
                }
            }
        }
    }

    std::set<std::string> dstNames;
    std::map<std::string, std::vector<char>> dstValues;

    listLen = (dstFd >= 0)
                  ? flistxattr(dstFd, nullptr, 0)
                  : llistxattr(dstPath, nullptr, 0);
    if (listLen > 0)
    {
        std::vector<char> nameBuf(listLen);
        listLen = (dstFd >= 0)
                      ? flistxattr(dstFd, nameBuf.data(), listLen)
                      : llistxattr(dstPath, nameBuf.data(), listLen);
        if (listLen > 0)
        {
            for (char *name = nameBuf.data(); name < nameBuf.data() + listLen;
                 name += strlen(name) + 1)
            {
                if (strcmp(name, "security.selinux") == 0)
                    continue;
                dstNames.insert(name);
                ssize_t valLen = (dstFd >= 0)
                                     ? fgetxattr(dstFd, name, nullptr, 0)
                                     : lgetxattr(dstPath, name, nullptr, 0);
                if (valLen >= 0)
                {
                    std::vector<char> valBuf(valLen);
                    valLen = (dstFd >= 0)
                                 ? fgetxattr(dstFd, name, valBuf.data(), valLen)
                                 : lgetxattr(dstPath, name, valBuf.data(), valLen);
                    if (valLen >= 0)
                        dstValues[name] = std::move(valBuf);
                }
            }
        }
    }

    for (const auto &name : srcNames)
    {
        if (dstNames.find(name) == dstNames.end())
        {
            RecordCksumMetaMismatch("xattr_mismatch", "src_has:" + name);
        }
        else if (srcValues[name] != dstValues[name])
        {
            RecordCksumMetaMismatch("xattr_mismatch", "diff:" + name);
        }
    }
    for (const auto &name : dstNames)
    {
        if (srcNames.find(name) == srcNames.end())
        {
            RecordCksumMetaMismatch("xattr_mismatch", "dst_has:" + name);
        }
    }
#endif
    return {};
}

tl::expected<void, StackError> CPFilePair::CompareAcl([[maybe_unused]] const struct stat &dstStat)
{
#ifdef HAS_LIBACL
    if (mSrcFd < 0)
        return {};

    acl_t srcAcl = acl_get_fd(mSrcFd);
    if (!srcAcl)
        return {};

    char *srcText = acl_to_text(srcAcl, nullptr);
    acl_free(srcAcl);

    acl_t dstAcl = nullptr;
    char *dstText = nullptr;
    if (mDstFd >= 0)
    {
        dstAcl = acl_get_fd(mDstFd);
    }
    if (!dstAcl)
    {
        dstAcl = acl_get_file(mDstPath.c_str(), ACL_TYPE_ACCESS);
    }
    if (dstAcl)
    {
        dstText = acl_to_text(dstAcl, nullptr);
        acl_free(dstAcl);
    }

    if (srcText)
    {
        if (!dstText || strcmp(srcText, dstText) != 0)
        {
            RecordCksumMetaMismatch("acl_mismatch",
                            fmt::format("src={} dst={}",
                                        srcText ? srcText : "none",
                                        dstText ? dstText : "none"));
        }
        acl_free(srcText);
    }
    if (dstText)
    {
        acl_free(dstText);
    }
#endif
    return {};
}

// SkipWriteAsHole: 跳过全零块的写入，保持稀疏文件的洞结构（不实际写入零字节）。
// 设计意图：当 IsAllZeros 检测到某 block 全为零时，不派发写 IO，而是直接更新 written bytes，
// 最终通过 ftruncate 将目标文件扩展到正确大小，让文件系统按需分配块，对齐 cp --sparse=auto 行为。
tl::expected<void, StackError> CPFilePair::SkipWriteAsHole(size_t bytes)
{
    mHasHoles = true;
    UpdateWrittenBytes(bytes);
    if (mReporter)
    {
        mReporter->AddBytesDone(bytes);
    }

    if (IsWriteFinished())
    {
        if (ftruncate(mDstFd, mSrcStat.st_size) < 0)
        {
            return tl::unexpected(StackError(
                fmt::format("ftruncate for sparse file failed: {}, errno: {}", mDstPath, errno)));
        }
    }
    return {};
}