#include <filesystem>
#include "lib/combined/combined.hpp"

CPFilePair::CPFilePair(std::string_view src_path,
                       std::string_view dst_path,
                       size_t io_size,
                       bool direct_io,
                       bool sync_writes,
                       bool cksum,
                       std::shared_ptr<ILogger> logger)
    : mSrcPath(src_path),
      mDstPath(dst_path),
      mIOSize(io_size),
      mDirectIO(direct_io),
      mSyncWrites(sync_writes),
      mCksum(cksum),
      mLogger(logger)
{
    bzero(&mSrcStat, sizeof(struct stat));
    bzero(&mDstStat, sizeof(struct stat));
}

tl::expected<void, StackError> CPFilePair::CheckAndInit()
{
    namespace fs = std::filesystem;
    // check if source file is symlink, if so, create a coresponding symlink on dst path
    if (fs::is_symlink(mSrcPath))
    {
        mIsSymlink = true;

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
        mLogger->debug("Source is a symlink, created destination symlink: {} -> {}",
                       mDstPath, target_path.string());
        return {};
    }

    // check if source file is directory, create destination directory if so
    if (fs::is_directory(mSrcPath))
    {
        mIsDir = true;
        std::error_code ec;
        fs::create_directories(mDstPath, ec);
        if (ec)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to create destination directory: {}, errstr: {}", mDstPath, ec.message())));
        }
        mLogger->debug("Source is a directory, created destination directory: {}", mDstPath);
        return {};
    }

    // check if source file is not regular file, return error
    if (!fs::is_regular_file(mSrcPath))
    {
        // std::filesystem::file_type is not directly formattable by fmt, cast to int for diagnostic
        return tl::unexpected(StackError(
            fmt::format("src: {} is not supported file type: {}", mSrcPath, static_cast<int>(fs::status(mSrcPath).type()))));
    }

    // check source file
    mLogger->trace("Opening source file: {}", mSrcPath);
    if (mDirectIO)
    {
        mSrcFd = open(mSrcPath.c_str(), O_RDONLY | O_DIRECT);
    }
    else
    {
        mSrcFd = open(mSrcPath.c_str(), O_RDONLY);
    }

    if (mSrcFd < 0)
    {
        return tl::unexpected(StackError(
            fmt::format("open src file failed: {}, errno: {}, errstr: {}", mSrcPath, errno, strerror(errno))));
    }
    if (fstat(mSrcFd, &mSrcStat) < 0)
    {
        return tl::unexpected(StackError(
            fmt::format("fstat() src file failed: {}, errno: {}, errstr: {}", mSrcPath, errno, strerror(errno))));
    }
    if (!S_ISREG(mSrcStat.st_mode))
    {
        return tl::unexpected(StackError("Source file is not a regular file: " + mSrcPath));
    }

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
    int dstOpenFlags = O_CREAT;
    if (mCksum)
    {
        dstOpenFlags |= O_RDWR;
    }
    else
    {
        dstOpenFlags |= O_WRONLY | O_TRUNC;
    }
    if (mDirectIO)
    {
        dstOpenFlags |= O_DIRECT;
    }
    mDstFd = open(mDstPath.c_str(), dstOpenFlags, 0644);

    if (mDstFd < 0)
    {
        return tl::unexpected(StackError(
            fmt::format("Failed to open/create destination file: {}, errno: {}, errstr: {}", mDstPath, errno, strerror(errno))));
    }

    mLogger->debug("Initialized file pair: src: {}, dst: {}, size: {}", mSrcPath, mDstPath, GetSrcFileSize());
    return {};
}

tl::expected<void, std::string> CPFilePair::DoDstState()
{
    if (fstat(mDstFd, &mDstStat) < 0)
    {
        return tl::unexpected(fmt::format("Failed to fstat destination file: {}, errno: {}, errstr: {}",
                                          mDstPath, errno, strerror(errno)));
    }

    return {};
}

tl::expected<std::shared_ptr<CPFilePair>, StackError> CPFilePairMgr::GetNextReadIO()
{
    std::lock_guard<std::mutex> lock(mMutex);

AGAIN:
    if (mReadPtr == mFilePairs.end())
    {
        mLogger->trace("GetNextReadIO() ended");
        return nullptr; // already at end
    }
    // now mReadPtr points to a valid file pair

    if (!(*mReadPtr)->IsInitialized())
    {
        auto init_res = (*mReadPtr)->CheckAndInit();
        if (!init_res)
        {
            // 检查如果报错含有“not supported”字样，就简单跳过这次copy，取下一个mReadPtr即可
            if (std::string(init_res.error().ToString()).find("not supported") != std::string::npos)
            {
                mLogger->warn("Skipping unsupported file pair, src: {}, dst: {}. Error: {}",
                              (*mReadPtr)->GetSrcPath(), (*mReadPtr)->GetDstPath(), init_res.error().ToString());
                ReadPtrAdvance(*mReadPtr);
                goto AGAIN;
            }
            init_res.error().Append("(*mReadPtr)->CheckAndInit(), err: ");
            return Unexpt(init_res.error());
        }
    }

    if ((*mReadPtr)->IsDir() || (*mReadPtr)->IsSymlink())
    {
        // CheckAndInit()已经处理好了目录和Symlink，这里直接跳过

        // namespace fs = std::filesystem;
        // // make dirs at dest, return error if failed
        // mLogger->trace("mkdir for src: {}, dst: {}",
        //                (*mReadPtr)->GetSrcPath(), (*mReadPtr)->GetDstPath());

        // std::error_code ec;
        // fs::create_directories((*mReadPtr)->GetDstPath(), ec);
        // if (ec)
        // {
        //     return tl::unexpected(StackError(
        //         fmt::format("Failed to create destination directory: {}, errstr: {}",
        //                     (*mReadPtr)->GetDstPath(), ec.message())));
        // }

        // TODO: call CheckWriteComplete(mReadPtr) in future to 统一做copy收尾工作

        ReadPtrAdvance(*mReadPtr);
        goto AGAIN;
    }

    if (CheckReadCompleteNoLock(*mReadPtr))
    {
        goto AGAIN;
    }
    mLogger->trace("GetNextReadIO() return, src: {}, dst: {}, read offset: {}",
                   (*mReadPtr)->GetSrcPath(), (*mReadPtr)->GetDstPath(), (*mReadPtr)->GetReadOffset());
    return *mReadPtr;
}

void CPFilePairMgr::CheckReadComplete(std::shared_ptr<CPFilePair> pFP)
{
    std::lock_guard<std::mutex> lock(mMutex);
    CheckReadCompleteNoLock(pFP);
}

bool CPFilePairMgr::CheckReadCompleteNoLock(std::shared_ptr<CPFilePair> pFP)
{
    mLogger->trace("CheckReadCompleteNoLock(): src: {}, dst: {}, preparedReadBytes: {}, totalBytes: {}, IsReadFinished: {}",
                   pFP->GetSrcPath(), pFP->GetDstPath(),
                   pFP->GetReadOffset(), pFP->GetSrcFileSize(), pFP->IsReadFinished());

    if (pFP->IsReadFinished())
    {
        mInflightFPs.Push(pFP, pFP->GetSrcPath());

        // 如果是0-size文件，它是(*mReadPtr)->IsInitialized()==true的，说明是经由AGAIN标签过来的
        if ((pFP)->IsInitialized() && (pFP)->GetSrcFileSize() == 0)
        {
            mLogger->debug("Source file size is 0, directly checking write completion for src: {}, dst: {}",
                           (pFP)->GetSrcPath(), (pFP)->GetDstPath());
            CheckWriteComplete(pFP); // directly check write complete for 0-size files
        }

        ReadPtrAdvance(pFP);
        return true;
    }
    return false;
}

tl::expected<void, StackError> CPFilePairMgr::CheckWriteComplete(std::shared_ptr<CPFilePair> pFP)
{
    assert(pFP != nullptr);
    mLogger->debug("Checking write completion for file pair: src: {}, dst: {}",
                   pFP->GetSrcPath(), pFP->GetDstPath());

    if (pFP->IsWriteFinished())
    {
        if (mOptions.DirectIO)
        {
            auto truncate_res = pFP->TruncateDstToSrcSize();
            if (!truncate_res)
            {
                return tl::unexpected(StackError("pFP->TruncateDstToSrcSize(), err: ", truncate_res.error()));
            }
        }
        if (mOptions.SyncWrites && pFP->GetDstFileSize() > 0)
        {
            auto fsync_res = pFP->FsyncDst();
            if (!fsync_res)
            {
                return tl::unexpected(StackError("pFP->FsyncDst(), err: ", fsync_res.error()));
            }
        }

        // 如果有其他结尾要做的事情，比如copy file attributes，可以在这里做

        mLogger->debug("Completed writing file pair: src: {}, dst: {}, size: {}, inflight pairs remaining: {}",
                       pFP->GetSrcPath(), pFP->GetDstPath(), pFP->GetSrcFileSize(), mInflightFPs.Size());
        if (mInflightFPs.Size() > 0)
        {
            mLogger->debug("Inflight file pairs:");
            for (const auto &fp : mInflightFPs.GetList())
            {
                mLogger->debug("  src: {}, dst: {}, read_offset: {}, write_offset: {}",
                               fp->GetSrcPath(), fp->GetDstPath(),
                               fp->GetReadOffset(), fp->GetWriteOffset());
            }
        }
        // 从inflight列表中移除
        mInflightFPs.Remove(pFP, pFP->GetSrcPath());
    }
    return {};
}