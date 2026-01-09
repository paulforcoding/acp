#include <filesystem>
#include "lib/combined/combined.hpp"

CPFilePair::CPFilePair(std::string_view src_path,
                       std::string_view dst_path,
                       size_t io_size,
                       bool direct_io,
                       bool sync_writes,
                       std::shared_ptr<ILogger> logger)
    : m_src_path(src_path), m_dst_path(dst_path), mIOSize(io_size), mDirectIO(direct_io), mSyncWrites(sync_writes), mLogger(logger)
{
    bzero(&m_src_stat, sizeof(struct stat));
    bzero(&m_dst_stat, sizeof(struct stat));
}

tl::expected<void, StackError> CPFilePair::CheckAndInit()
{
    namespace fs = std::filesystem;
    // check if source file is symlink, if so, create a coresponding symlink on dst path
    if (fs::is_symlink(m_src_path))
    {
        std::error_code ec;
        auto target_path = fs::read_symlink(m_src_path, ec);
        if (ec)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to read symlink target of source file: {}, errstr: {}", m_src_path, ec.message())));
        }
        fs::create_symlink(target_path, m_dst_path, ec);
        if (ec)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to create symlink at destination: {}, pointing to: {}, errstr: {}",
                            m_dst_path, target_path.string(), ec.message())));
        }
        mLogger->debug("Source is a symlink, created destination symlink: {} -> {}",
                       m_dst_path, target_path.string());
        return {};
    }

    // check if source file is directory, create destination directory if so
    if (fs::is_directory(m_src_path))
    {
        mIsDir = true;
        std::error_code ec;
        fs::create_directories(m_dst_path, ec);
        if (ec)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to create destination directory: {}, errstr: {}", m_dst_path, ec.message())));
        }
        mLogger->debug("Source is a directory, created destination directory: {}", m_dst_path);
        return {};
    }

    // check if source file is not regular file, return error
    if (!fs::is_regular_file(m_src_path))
    {
        // std::filesystem::file_type is not directly formattable by fmt, cast to int for diagnostic
        return tl::unexpected(StackError(
            fmt::format("src: {} is not supported file type: {}", m_src_path, static_cast<int>(fs::status(m_src_path).type()))));
    }

    // check source file
    mLogger->trace("Opening source file: {}", m_src_path);
    if (mDirectIO)
    {
        m_src_fd = open(m_src_path.c_str(), O_RDONLY | O_DIRECT);
    }
    else
    {
        m_src_fd = open(m_src_path.c_str(), O_RDONLY);
    }

    if (m_src_fd < 0)
    {
        return tl::unexpected(StackError(
            fmt::format("open src file failed: {}, errno: {}, errstr: {}", m_src_path, errno, strerror(errno))));
    }
    if (fstat(m_src_fd, &m_src_stat) < 0)
    {
        return tl::unexpected(StackError(
            fmt::format("fstat() src file failed: {}, errno: {}, errstr: {}", m_src_path, errno, strerror(errno))));
    }
    if (!S_ISREG(m_src_stat.st_mode))
    {
        return tl::unexpected(StackError("Source file is not a regular file: " + m_src_path));
    }

    // check dst file path, create if not exist
    std::string dst_dir = fs::path(m_dst_path).parent_path().string();
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

    mLogger->trace("Opening/creating destination file: {}", m_dst_path);
    // check destination file
    if (mDirectIO)
    {
        m_dst_fd = open(m_dst_path.c_str(), O_WRONLY | O_CREAT | O_DIRECT | O_TRUNC, 0644);
    }
    else
    {
        m_dst_fd = open(m_dst_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }

    if (m_dst_fd < 0)
    {
        return tl::unexpected(StackError(
            fmt::format("Failed to open/create destination file: {}, errno: {}, errstr: {}", m_dst_path, errno, strerror(errno))));
    }

    mLogger->debug("Initialized file pair: src: {}, dst: {}, size: {}", m_src_path, m_dst_path, GetSrcFileSize());
    return {};
}

tl::expected<void, std::string> CPFilePair::DoDstState()
{
    if (fstat(m_dst_fd, &m_dst_stat) < 0)
    {
        return tl::unexpected(fmt::format("Failed to fstat destination file: {}, errno: {}, errstr: {}",
                                          m_dst_path, errno, strerror(errno)));
    }

    return {};
}

tl::expected<std::shared_ptr<CPFilePair>, StackError> CPFilePairMgr::GetNextReadIO()
{
    std::lock_guard<std::mutex> lock(mMutex);

AGAIN:
    if (mReadPtr == mFilePairs.end())
    {
        mLogger->debug("GetNextReadIO() ended");
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
                mFilePairs.erase(mReadPtr);
                mReadPtr = mFilePairs.begin();
                goto AGAIN;
            }

            return tl::unexpected(StackError("(*mReadPtr)->CheckAndInit(), err: ", init_res.error()));
        }
    }

    if ((*mReadPtr)->IsDir())
    {
        namespace fs = std::filesystem;
        // make dirs at dest, return error if failed
        mLogger->trace("mkdir for src: {}, dst: {}",
                       (*mReadPtr)->GetSrcPath(), (*mReadPtr)->GetDstPath());

        std::error_code ec;
        fs::create_directories((*mReadPtr)->GetDstPath(), ec);
        if (ec)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to create destination directory: {}, errstr: {}",
                            (*mReadPtr)->GetDstPath(), ec.message())));
        }

        // call CheckWriteComplete(mReadPtr) in future to 统一做copy收尾工作

        // remove directory entries from mFilePairs
        mFilePairs.erase(mReadPtr);
        mReadPtr = mFilePairs.begin();
        goto AGAIN;
    }

    if ((*mReadPtr)->IsReadFinished())
    {
        CheckReadCompleteNoLock(*mReadPtr);

        goto AGAIN;
        // make sure to return a initialized and not-finished file pair,
        // and make sure 0-size files are get popped from mFilePairs to mInflightFPs
    }
    mLogger->debug("GetNextReadIO() return, src: {}, dst: {}, read offset: {}",
                   (*mReadPtr)->GetSrcPath(), (*mReadPtr)->GetDstPath(), (*mReadPtr)->GetReadOffset());
    return *mReadPtr;
}

void CPFilePairMgr::CheckReadComplete(std::shared_ptr<CPFilePair> pFP)
{
    std::lock_guard<std::mutex> lock(mMutex);

    if (pFP->IsReadFinished())
    {
        CheckReadCompleteNoLock(pFP);
    }
}

void CPFilePairMgr::CheckReadCompleteNoLock(std::shared_ptr<CPFilePair> pFP)
{
    mInflightFPs.Push(pFP, pFP->GetSrcPath());

    mLogger->debug("Completed reading file pair: src: {}, dst: {}, size: {}, inflight pairs: {}",
                   pFP->GetSrcPath(), pFP->GetDstPath(), pFP->GetSrcFileSize(), mInflightFPs.Size());

    // 如果是0-size文件，它是(*mReadPtr)->IsInitialized()==true的，说明是经由AGAIN标签过来的
    if ((pFP)->IsInitialized() && (pFP)->GetSrcFileSize() == 0)
    {
        mLogger->debug("Source file size is 0, directly checking write completion for src: {}, dst: {}",
                       (pFP)->GetSrcPath(), (pFP)->GetDstPath());
        CheckWriteComplete(pFP); // directly check write complete for 0-size files
    }

    mFilePairs.erase(mReadPtr); // mReadPtr指向的元素被移除后，mReadPtr就不再准确了，需要重新指向begin()
    mReadPtr = mFilePairs.begin();
}

tl::expected<void, StackError> CPFilePairMgr::CheckWriteComplete(std::shared_ptr<CPFilePair> pFP)
{
    assert(pFP != nullptr);
    mLogger->debug("Checking write completion for file pair: src: {}, dst: {}",
                   pFP->GetSrcPath(), pFP->GetDstPath());

    if (pFP->IsWriteFinished())
    {
        if (m_options.DirectIO)
        {
            auto truncate_res = pFP->TrucateDstToSrcSize();
            if (!truncate_res)
            {
                return tl::unexpected(StackError("pFP->TrucateDstToSrcSize(), err: ", truncate_res.error()));
            }
        }
        if (m_options.SyncWrites && pFP->GetDstFileSize() > 0)
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
        // 从inflight列表中移除
        mInflightFPs.Remove(pFP, pFP->GetSrcPath());
    }
    return {};
}