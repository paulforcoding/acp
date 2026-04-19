#include <filesystem>
#include "base/event_reporter.hpp"
#include "lib/combined/combined.hpp"

CPFilePair::CPFilePair(std::string_view src_path,
                       std::string_view dst_path,
                       size_t io_size,
                       bool direct_io,
                       bool sync_writes,
                       bool cksum,
                       std::shared_ptr<ILogger> logger,
                       FileLogReporter* reporter)
    : mSrcPath(src_path),
      mDstPath(dst_path),
      mIOSize(io_size),
      mDirectIO(direct_io),
      mSyncWrites(sync_writes),
      mCksum(cksum),
      mLogger(logger),
      mReporter(reporter)
{
    memset(&mSrcStat, 0, sizeof(struct stat));
    memset(&mDstStat, 0, sizeof(struct stat));
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
        std::error_code ec;
        fs::create_directories(mDstPath, ec);
        if (ec)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to create destination directory: {}, errstr: {}", mDstPath, ec.message())));
        }
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
#ifdef O_DIRECT
    if (mDirectIO)
    {
        dstOpenFlags |= O_DIRECT;
    }
#endif
    mDstFd = open(mDstPath.c_str(), dstOpenFlags, 0644);

    if (mDstFd < 0)
    {
        if (mReporter)
        {
            mReporter->FileError(mSrcPath, strerror(errno), errno, "failed");
        }
        return tl::unexpected(StackError(
            fmt::format("Failed to open/create destination file: {}, errno: {}, errstr: {}", mDstPath, errno, strerror(errno))));
    }

    if (mReporter)
    {
        mReporter->FileStart(mSrcPath, mDstPath, GetSrcFileSize());
    }
    mStartTime = std::chrono::steady_clock::now();
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

tl::expected<void, StackError> CPFilePairMgr::CheckReadComplete(std::shared_ptr<CPFilePair> pFP)
{
    auto res = CheckReadCompleteNoLock(pFP);
    if (!res)
    {
        return tl::unexpected(res.error());
    }
    return {};
}

tl::expected<bool, StackError> CPFilePairMgr::CheckReadCompleteNoLock(std::shared_ptr<CPFilePair> pFP)
{
    mLogger->trace("CheckReadCompleteNoLock(): src: {}, dst: {}, preparedReadBytes: {}, totalBytes: {}, IsReadFinished: {}",
                   pFP->GetSrcPath(), pFP->GetDstPath(),
                   pFP->GetReadOffset(), pFP->GetSrcFileSize(), pFP->IsReadFinished());

    if (pFP->IsReadFinished())
    {
        mChannel.MoveToInflight(pFP);

        if (pFP->IsInitialized() && pFP->GetSrcFileSize() == 0)
        {
            mLogger->debug("Source file size is 0, directly checking write completion for src: {}, dst: {}",
                           pFP->GetSrcPath(), pFP->GetDstPath());
            auto write_res = CheckWriteComplete(pFP);
            if (!write_res)
            {
                return tl::unexpected(StackError("CheckWriteComplete() for 0-size file, err: ", write_res.error()));
            }
        }

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

        mChannel.RemoveFromInflight(pFP);

        if (mReporter)
        {
            mReporter->FileComplete(pFP->GetSrcPath(), pFP->GetDstPath(), pFP->GetSrcFileSize(), pFP->GetElapsedMs());
            mReporter->IncrementFilesDone();
            mReporter->AddBytesDone(pFP->GetSrcFileSize());
        }
    }
    return {};
}

// === FPChannel 实现 ===

void FPChannel::Push(std::shared_ptr<CPFilePair> item)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mPending.push_back(std::move(item));
    mCv.notify_one();
}

void FPChannel::Close()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mClosed = true;
    mCv.notify_all();
}

std::shared_ptr<CPFilePair> FPChannel::PeekFront()
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mPending.empty())
        return nullptr;
    return mPending.front();
}

void FPChannel::PopFront()
{
    std::lock_guard<std::mutex> lock(mMutex);
    assert(!mPending.empty());
    mPending.pop_front();
}

void FPChannel::MoveToInflight(std::shared_ptr<CPFilePair> pFP)
{
    std::lock_guard<std::mutex> lock(mMutex);
    assert(!mPending.empty() && mPending.front() == pFP);
    mPending.pop_front();
    mInflight.push_back(pFP);
    pFP->MarkAsInflight();
}

void FPChannel::RemoveFromInflight(std::shared_ptr<CPFilePair> pFP)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mInflight.remove(pFP);
    pFP->MarkAsCompleted();
    mCv.notify_all();
}

bool FPChannel::WaitForWorkOrClose(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mMutex);
    mCv.wait_for(lock, timeout, [this]
                 { return mClosed || !mPending.empty(); });
    return !(mClosed && mPending.empty() && mInflight.empty());
}

bool FPChannel::HasPendingWork() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return !mPending.empty() || !mInflight.empty();
}

bool FPChannel::Empty() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mPending.empty();
}

bool FPChannel::IsClosed() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mClosed;
}

size_t FPChannel::PendingCount() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mPending.size();
}

size_t FPChannel::InflightCount() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mInflight.size();
}