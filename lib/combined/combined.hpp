#pragma once

#include <any>
#include <cstddef> // size_t
#ifdef __linux__
#include <libaio.h>
#endif
#include <sys/stat.h>
#include <tl/expected.hpp>
#include <vector>
#include <map>
#include <chrono>
#include <cassert>
#include <string>
#include <memory>
#include <fstream>

#include "base/base.hpp"
#include "base/chan.hpp"
#include "base/digest.hpp"
#include "base/event_reporter.hpp"

struct CopyEntry
{
    std::string srcPath; // full path
    std::string dstPath;
    struct stat srcStat{};

    // 重载一个等号操作符用于DedupQueue的去重功能
    bool operator==(const CopyEntry &other) const
    {
        return (srcPath == other.srcPath) && (dstPath == other.dstPath);
    }
};

// class CPFilePair;

struct RWCombinedCopyOptions
{
    std::string ProgramLogLevel = "error";
    std::string ProgramLogMode = "console";
    std::string ProgramLogFilePath = "/tmp/acp_program.log";
    bool FileLogEnabled = false;
    std::string FileLogMode = "console";
    int FileLogIntervalSec = 5;
    std::string FileLogPath = "/tmp/acp_file_info.json";
    std::string CopyEngine = "liburing";
    std::string CopyMode = "CopyOnly";
    std::string CksumAlgorithm = "xxhash64";
    int CopyParallelism = 1;
    int CopyChanSize = 100;
    bool EnableInotify = false;
    bool PreserveSparseFiles = true;
    bool PreserveMeta = true;
    bool DirectIO = false;
    bool SyncWrites = true;
    size_t IoSize = 131072;
    size_t QueueDepth = 16;
    int Batch = 8;
    int IOReapWait = 1;
    int IOStuckTimeout = 10;
};

// 这个类提供源文件和目标文件的信息，并且IO计数的功能，并且负责打开和关闭文件描述符、复制attr extended-attributes等
// 将来还要负责权限、时间戳等的复制、创建目录等功能
class CPFilePair
{
public:
    CPFilePair(std::string_view src_path,
               std::string_view dst_path,
               bool direct_io,
               bool cksum,
               bool cksum_only,
               bool preserve_meta,
               std::shared_ptr<ILogger> logger,
               FileLogReporter* reporter); // full path expected
    ~CPFilePair()
    {
        if (mSrcFd >= 0)
        {
            close(mSrcFd);
            mSrcFd = -1;
        }
        if (mDstFd >= 0)
        {
            close(mDstFd);
            mDstFd = -1;
        }
    }
    tl::expected<void, StackError> CheckAndInit();
    tl::expected<void, StackError> PreserveMetadata();
    tl::expected<void, StackError> CompareMetadata();
    tl::expected<void, StackError> TruncateDstToSrcSize()
    {
        if (ftruncate(mDstFd, mSrcStat.st_size) < 0)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to truncate destination file: {}, errno: {}, errstr: {}", mDstPath, errno, strerror(errno))));
        }
        return {};
    }
    tl::expected<void, StackError> FsyncDst()
    {
        mLogger->trace("Fsyncing destination file: {}", mDstPath);
        if (fsync(mDstFd) < 0)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to fsync destination file: {}, errno: {}, errstr: {}", mDstPath, errno, strerror(errno))));
        }
        return {};
    }

    int GetSrcFd() const { return mSrcFd; }
    int GetDstFd() const { return mDstFd; }
    size_t GetSrcFileSize() const { return static_cast<size_t>(mSrcStat.st_size); }
    size_t GetDstFileSize() const { return static_cast<size_t>(mDstStat.st_size); } // not used yet
    std::string GetSrcPath() const { return mSrcPath; }
    std::string GetDstPath() const { return mDstPath; }
    tl::expected<void, std::string> DoDstState();
    struct stat *GetSrcStatPtr() { return &mSrcStat; } // not used yet
    struct stat *GetDstStatPtr() { return &mDstStat; } // not used yet
    size_t GetReadOffset() const { return mReadBytes; }
    size_t GetWriteOffset() const { return mWrittenBytes; }
    void UpdatePrepareReadBytes(size_t n) { mReadBytes += n; }
    void UpdateWrittenBytes(size_t n) { mWrittenBytes += n; }
    bool IsReadFinished() const { return mReadBytes >= GetSrcFileSize(); }
    bool IsWriteFinished() const
    {
        mLogger->debug("Checking IsWriteFinished: written_bytes: {}, src_file_size: {}, src: {}",
                       mWrittenBytes, GetSrcFileSize(), mSrcPath);
        return mWrittenBytes >= GetSrcFileSize();
    }
    void SetReadFinished() { mReadBytes = GetSrcFileSize(); }
    void SetWriteFinished() { mWrittenBytes = GetSrcFileSize(); }
    bool IsInitialized() const { return (mSrcFd >= 0 && mDstFd >= 0) || IsDir() || IsSymlink() || mSkipBlockCksum; }
    bool IsDir() const { return mIsDir; }
    bool IsSymlink() const { return mIsSymlink; }
    bool IsSkipBlockCksum() const { return mSkipBlockCksum; }
    bool IsProbablySparse() const { return mIsProbablySparse; }
    bool HasHoles() const { return mHasHoles; }
    tl::expected<void, StackError> SkipWriteAsHole(size_t bytes);

    void SetCksumError(bool err) { mIsChksumError = err; }
    bool GetCksumError() const { return mIsChksumError; }

    void RecordCksumContent(const std::string &contentValue);
    void RecordCksumMetaMismatch(const std::string &reason, const std::string &detail = "");
    void RecordCksumMetaDstMissing(const std::string &reason);
    void RecordCksumMetaMatch();
    void FlushCksumResult();
    const std::string &GetCksumContentResult() const { return mCksumContentResult; }

    int64_t GetElapsedMs() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - mStartTime)
            .count();
    }

    void MarkAsInflight() { mIsInflight = true; }
    bool IsInflight() const { return mIsInflight; }
    void MarkAsCompleted() { mIsCompleted = true; }
    bool IsCompleted() const { return mIsCompleted; }

private:
    int mSrcFd = -1;
    int mDstFd = -1;
    struct stat mSrcStat;
    struct stat mDstStat;
    std::string mSrcPath; // full path of source file
    std::string mDstPath; // full path of destination file

    size_t mReadBytes = 0;
    size_t mWrittenBytes = 0;
#ifdef O_DIRECT
    bool mDirectIO = false;
#endif
    bool mCksum = false;
    bool mCksumOnly = false;
    bool mPreserveMeta = false;

    bool mIsDir = false;
    bool mIsSymlink = false;
    bool mSkipBlockCksum = false;

    bool mIsChksumError = false;
    bool mMetaMismatchEmitted = false;
    std::string mCksumContentResult;
    std::string mCksumMetaResult;
    std::vector<std::string> mCksumMetaDetails;
    bool mIsProbablySparse = false;
    bool mHasHoles = false;

    bool mIsInflight = false;
    bool mIsCompleted = false;

    std::shared_ptr<ILogger> mLogger;
    FileLogReporter* mReporter = nullptr;
    std::chrono::steady_clock::time_point mStartTime;

    tl::expected<void, StackError> PreserveMode();
    tl::expected<void, StackError> PreserveOwnership();
    tl::expected<void, StackError> PreserveTimestamps();
    tl::expected<void, StackError> PreserveXattr();
    tl::expected<void, StackError> PreserveAcl();
    void EmitMetaWarning(const std::string &metaType, int err);

    tl::expected<void, StackError> CompareMode(const struct stat &dstStat);
    tl::expected<void, StackError> CompareOwnership(const struct stat &dstStat);
    tl::expected<void, StackError> CompareTimestamps(const struct stat &dstStat);
    tl::expected<void, StackError> CompareXattr(const struct stat &dstStat);
    tl::expected<void, StackError> CompareAcl(const struct stat &dstStat);
};

// Helper: check if a buffer is entirely zero bytes (used by sparse copy).
inline bool IsAllZeros(const char *buf, size_t len)
{
    const uint64_t *p64 = reinterpret_cast<const uint64_t *>(buf);
    size_t n64 = len / 8;
    for (size_t i = 0; i < n64; ++i)
    {
        if (p64[i] != 0)
            return false;
    }
    for (size_t i = n64 * 8; i < len; ++i)
    {
        if (buf[i] != 0)
            return false;
    }
    return true;
}

// CPFilePairMgr: 管理单个工作线程的文件对队列（FPChannel）及 IO 完成检测。
// 设计意图：每个 CopyEngine 线程拥有一个实例，负责从 Channel 取出文件对、
// 跟踪读取/写入完成状态，并协调与 IOSlotMgr 的三阶段循环（SubmitReads → IOReap → SubmitWrites）。
class CPFilePairMgr
{
public:
    CPFilePairMgr(const RWCombinedCopyOptions &options,
                  std::shared_ptr<ILogger> logger,
                  FileLogReporter* reporter)
        : mOptions(options), mLogger(logger), mReporter(reporter) {}

    tl::expected<void, StackError> AddFilePair(std::string_view src_path, std::string_view dst_path)
    {
        mChannel.Push(std::make_shared<CPFilePair>(src_path,
                       dst_path,
                       mOptions.DirectIO,
                       (mOptions.CopyMode == "CksumCopy" || mOptions.CopyMode == "CksumOnly"),
                       (mOptions.CopyMode == "CksumOnly"),
                       mOptions.PreserveMeta,
                       mLogger,
                       mReporter));
        return {};
    }

    tl::expected<std::shared_ptr<CPFilePair>, StackError> GetNextReadIO();
    tl::expected<void, StackError> CheckWriteComplete(std::shared_ptr<CPFilePair> pFP);
    tl::expected<void, StackError> CheckReadComplete(std::shared_ptr<CPFilePair> pFP);

    void SetStopFlag() { mChannel.Close(); }

    bool HasPendingWork() { return mChannel.HasPendingWork(); }
    bool WaitForWorkOrClose(std::chrono::milliseconds timeout) { return mChannel.WaitForWorkOrClose(timeout); }
    bool IsStopRequested() { return mChannel.IsClosed(); }

    // Dump internal state for hang diagnosis (always at warn level so it's captured)
    void DumpState() const
    {
        mLogger->warn("CPFilePairMgr state: pending={}, inflight={}, closed={}",
                       mChannel.PendingCount(), mChannel.InflightCount(), mChannel.IsClosed());
    }

private:
    tl::expected<bool, StackError> CheckReadCompleteNoLock(std::shared_ptr<CPFilePair> pFP);

private:
    RWCombinedCopyOptions mOptions;
    std::shared_ptr<ILogger> mLogger;
    FileLogReporter* mReporter = nullptr;
    FPChannel mChannel;
};

// IOSlot: 单个异步 IO 缓冲区的状态机载体。
// 设计意图：每个 slot 代表一个 inflight IO，状态从 Init 依次流转到 WriteReaped。
// libaio 使用 iocb 结构体，io_uring 使用 sqe/cqe，GCD 使用 mReadFd —— 各后端按需使用各自字段。
class IOSlot
{
public:
    enum class Status
    {
        Init,
        ReadPrepared,
        ReadSubmitted,
        ReadReaped,
        // CksumReadPrepared,
        // CksumReadSubmitted,
        // CksumReadReaped,
        // 由于cksum IO是在独立的slots上，不共用IO slot，无需独立的状态记录
        WritePrepared,
        WriteSubmitted,
        WriteReaped
    };
    static const char *StatusToStr(Status s)
    {
        switch (s)
        {
        case Status::Init:
            return "Init";
        case Status::ReadPrepared:
            return "ReadPrepared";
        case Status::ReadSubmitted:
            return "ReadSubmitted";
        case Status::ReadReaped:
            return "ReadReaped";
        case Status::WritePrepared:
            return "WritePrepared";
        case Status::WriteSubmitted:
            return "WriteSubmitted";
        case Status::WriteReaped:
            return "WriteReaped";
        }
        return "UnknownStatus";
    }
    // ctor
    IOSlot(size_t buf_size, int id, std::string_view ty)
        : mBuf{AllocBytes(SECTORSIZE, buf_size)},
          mStatus{Status::Init},
          mId{id},
          mType{ty}
    {
    }
    // dtor
    virtual ~IOSlot() { FreeBytes(mBuf); }

    // disable copy and move to avoid accidental double-free or ownership transfer
    IOSlot(const IOSlot &) = delete;
    IOSlot &operator=(const IOSlot &) = delete;
    IOSlot(IOSlot &&) = delete;
    IOSlot &operator=(IOSlot &&) = delete;

    virtual void Reset()
    {
        mStatus = Status::Init;
        // bzero(mBuf, SECTORSIZE);
        mUserData = {};
        // clear IO tracking
        mIOInfo = IOInfo{};
        mReadFd = -1;
    }

    char *GetBuf() const { return mBuf; }
    Status GetStatus() const { return mStatus; }
    int GetID() const { return mId; }
    void SetStatus(Status s) { mStatus = s; }

    // IOInfo fields, used by ucp, in future may be used by acp
    struct IOInfo
    {
        off_t offset = 0;
        size_t io_size = 0;
    };
    // IOInfo，先是被读使用，之后被写使用
    void SetIOInfo(off_t offset, size_t io_size)
    {
        mIOInfo.offset = offset;
        mIOInfo.io_size = io_size;
    }
    IOInfo GetIOInfo() const
    {
        return mIOInfo;
    }

    // getters for iocb, used by acp
#ifdef __linux__
    struct iocb *GetReadIOCB() { return &mIocbRead; }
    struct iocb *GetWriteIOCB() { return &mIocbWrite; }

    struct iocb *InitReadIOCB()
    {
        memset(&mIocbRead, 0, sizeof(struct iocb));
        return &mIocbRead;
    }
    struct iocb *InitWriteIOCB()
    {
        memset(&mIocbWrite, 0, sizeof(struct iocb));
        return &mIocbWrite;
    }
#endif

    // UserData field, not used yet
    const std::any &GetUserData() const { return mUserData; }    // not used yet
    void SetUserData(const std::any &data) { mUserData = data; } // not used yet

    std::shared_ptr<CPFilePair> GetCPFPPtr() { return mCPFPIt; }
    std::shared_ptr<CPFilePair> GetCPFPPtr() const { return mCPFPIt; }
    void SetCPFPPtr(std::shared_ptr<CPFilePair> it) { mCPFPIt = it; }

    std::string GetType() const { return mType; }

    void SetAssociatedSlot(IOSlot *slot) { mAssociatedSlot = slot; }
    IOSlot *GetAssociatedSlot() const { return mAssociatedSlot; }

    // Read fd set by DoPrepareOneRead (used by GCD backend)
    void SetReadFd(int fd) { mReadFd = fd; }
    int GetReadFd() const { return mReadFd; }

private:
    // data fields
    char *mBuf = nullptr;
    Status mStatus = Status::Init;
    int mId = -1;        // its id, also index in IOSlotMgr's m_slots
    std::any mUserData; // user data field
    std::shared_ptr<CPFilePair> mCPFPIt;

    IOInfo mIOInfo;
    int mReadFd = -1;   // set by DoPrepareOneRead, consumed by GCD SubmitBatchRead

#ifdef __linux__
    struct iocb mIocbRead;  // 读iocb
    struct iocb mIocbWrite; // 写iocb
#endif

    std::string mType;
    IOSlot *mAssociatedSlot = nullptr;
};

// IOSlotMgr: 异步 IO 管理器的抽象模板基类，三阶段主循环（SubmitReads → IOReap → SubmitWrites）。
// 设计意图：解耦后端差异（libaio / io_uring / GCD）与通用逻辑（状态机推进、校验和比较、完成检测）。
// RunQueue() 是各工作线程的核心循环，直到 FPChannel 关闭且所有 inflight IO 完成才退出。
template <typename SlotType = IOSlot>
class IOSlotMgr
{
public:
    IOSlotMgr(const RWCombinedCopyOptions &options,
              CPFilePairMgr *file_pair_mgr,
              std::shared_ptr<ILogger> logger,
              FileLogReporter* reporter)
        : mOptions(options), mCPFPMgr(file_pair_mgr), mLogger(logger), mReporter(reporter)
    {
        size_t slot_count = options.QueueDepth;
        size_t buf_size = options.IoSize;
        mRWSlots.reserve(slot_count);

        mLogger->debug("IOSlotMgr created with QueueDepth: {}, IoSize: {}", slot_count, buf_size);

        assert(file_pair_mgr != nullptr);

        for (size_t i = 0; i < slot_count; ++i)
        {
            mRWSlots.emplace_back(std::make_unique<SlotType>(buf_size, i, "rw"));
        }
        if (options.CopyMode == "CksumCopy" || options.CopyMode == "CksumOnly")
        {
            mCksumSlots.reserve(slot_count);
            for (size_t i = 0; i < slot_count; ++i)
            {
                mCksumSlots.emplace_back(std::make_unique<SlotType>(buf_size, i, "cksum"));
            }
            if (options.CksumAlgorithm == "md5")
            {
                mDigest = std::make_unique<MD5Digest>();
            }
            else if (options.CksumAlgorithm == "sha256")
            {
                mDigest = std::make_unique<SHA256Digest>();
            }
            else
            {
                mDigest = std::make_unique<XXHash64Digest>();
            }

        }
    }
    virtual ~IOSlotMgr() = default;
    SlotType *GetSlot(int id)
    {
        if (id < 0 || static_cast<size_t>(id) >= mRWSlots.size())
        {
            return nullptr;
        }
        return mRWSlots[id].get();
    }

//     tl::expected<void, StackError> RunCopyQueue()
//     {
//         while (!mCPFPMgr->ShouldStartCopy())
//         {
//             std::this_thread::sleep_for(std::chrono::milliseconds(100));
//             if (mCPFPMgr->ShouldStopCopy())
//             {
//                 mLogger->debug("Received stop signal before starting copy queue.");
//                 return {};
//             }
//         }

//         auto init_res = Init();
//         if (!init_res)
//         {
//             return tl::unexpected(init_res.error());
//         }

//         long round = 0;
//         while (!mCPFPMgr->ShouldStopCopy())
//         {

// #ifndef NDEBUG
//             auto start = std::chrono::high_resolution_clock::now();
//             auto submit_res = SubmitReads();
//             auto end = std::chrono::high_resolution_clock::now();
//             AddDuration("SubmitReads()", start, end);
// #else
//             auto submit_res = SubmitReads();
// #endif

//             if (!submit_res)
//             {
//                 return tl::unexpected(StackError("SubmitReads(), err: ", submit_res.error()));
//             }

//             mLogger->debug("Submitted {} read IOs in round: {}.", submit_res.value(), round);

//             auto reap_res = IOReap();
//             if (!reap_res)
//             {
//                 return tl::unexpected(StackError("IOReap(), err: ", reap_res.error()));
//             }

//             auto write_res = SubmitWrites();
//             if (!write_res)
//             {
//                 return tl::unexpected(StackError("SubmitWrites(), err: ", write_res.error()));
//             }
//             mLogger->debug("Submitted {} write IOs in round: {}.", write_res.value(), round);

//             round++;
//         }

//         Reset();

//         return {};
//     }

    tl::expected<void, StackError> RunQueue()
    {
        auto init_res = Init();
        if (!init_res)
        {
            return tl::unexpected(init_res.error());
        }

        mLogger->warn("RunQueue: starting, QueueDepth={}, IoSize={}, CopyMode={}",
                       mOptions.QueueDepth, mOptions.IoSize, mOptions.CopyMode);

        long round = 0;

        // === Watchdog state ===
        size_t lastFilesDone = 0;
        size_t lastBytesDone = 0;
        auto lastProgressTime = std::chrono::steady_clock::now();
        const bool watchdogEnabled = (mOptions.IOStuckTimeout > 0 && !mOptions.EnableInotify);
        auto lastStateDumpTime = std::chrono::steady_clock::now();

        while (true)
        {
            mLogger->debug("Starting IO round: {}", round);

#ifndef NDEBUG
            auto start = std::chrono::high_resolution_clock::now();
            auto read_submitted = SubmitReads();
            auto end = std::chrono::high_resolution_clock::now();
            AddDuration("SubmitReads()", start, end);
#else
            auto read_submitted = SubmitReads();
#endif

            if (!read_submitted)
            {
                return tl::unexpected(StackError("SubmitReads(), err: ", read_submitted.error()));
            }

            mLogger->debug("Submitted {} read IOs in round: {}.", read_submitted.value(), round);

            int cksum_submitted = 0;
            if (mCksumQueue.size() > 0)
            {
                auto cksum_res = SubmitCksumReads();
                if (!cksum_res)
                {
                    return tl::unexpected(StackError("SubmitCksumReads(), err: ", cksum_res.error()));
                }
                cksum_submitted = cksum_res.value();
                mLogger->debug("Submitted {} checksum read IOs in round: {}.", cksum_submitted, round);
            }

            auto reap_res = IOReap();
            if (!reap_res)
            {
                return tl::unexpected(StackError("IOReap(), err: ", reap_res.error()));
            }

            int write_submitted = 0;
            if (mOptions.CopyMode != "CksumOnly")
            {
                auto write_res = SubmitWrites();
                if (!write_res)
                {
                    return tl::unexpected(StackError("SubmitWrites(), err: ", write_res.error()));
                }
                write_submitted = write_res.value();
                mLogger->debug("Submitted {} write IOs in round: {}.", write_submitted, round);
            }

            if (read_submitted.value() == 0 && write_submitted == 0 && cksum_submitted == 0)
            {
                bool hasWork = mCPFPMgr->WaitForWorkOrClose(std::chrono::milliseconds(100));
                if (!hasWork)
                {
                    mLogger->warn("RunQueue: exiting at round={}, channel closed and no work", round);
                    break;
                }
            }

            if (mReporter)
            {
                mReporter->MaybeEmitProgressSummary();
                mReporter->UpdateStateFile();
            }

            // === Watchdog detection ===
            if (watchdogEnabled && mReporter)
            {
                size_t filesDone = mReporter->GetFilesDone();
                size_t bytesDone = mReporter->GetBytesDone();

                bool hasSubmitted = false;
                for (auto &slot_up : mRWSlots)
                {
                    auto st = slot_up->GetStatus();
                    if (st == IOSlot::Status::ReadSubmitted ||
                        st == IOSlot::Status::WriteSubmitted)
                    {
                        hasSubmitted = true;
                        break;
                    }
                }
                // Also check cksum slots for submitted IOs
                if (!hasSubmitted)
                {
                    for (auto &slot_up : mCksumSlots)
                    {
                        auto st = slot_up->GetStatus();
                        if (st == IOSlot::Status::ReadSubmitted ||
                            st == IOSlot::Status::WriteSubmitted)
                        {
                            hasSubmitted = true;
                            break;
                        }
                    }
                }

                if (filesDone != lastFilesDone || bytesDone != lastBytesDone || !hasSubmitted)
                {
                    lastProgressTime = std::chrono::steady_clock::now();
                }
                else if (hasSubmitted)
                {
                    auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::steady_clock::now() - lastProgressTime)
                                          .count();
                    if (elapsedSec >= mOptions.IOStuckTimeout)
                    {
                        mReporter->StuckDetected(round, static_cast<int>(elapsedSec));
                        mLogger->error("Watchdog: IO stuck for {}s, "
                                       "files_done={}, bytes_done={}",
                                       elapsedSec,
                                       filesDone, bytesDone);
                        DumpState();
                        return tl::unexpected(
                            StackError(fmt::format(
                                "IO stuck: no progress for {}s "
                                "(threshold={}s)",
                                elapsedSec,
                                mOptions.IOStuckTimeout)));
                    }
                }
                lastFilesDone = filesDone;
                lastBytesDone = bytesDone;
            }

            // === Periodic state dump for hang diagnosis (every 30s) ===
            {
                auto now = std::chrono::steady_clock::now();
                auto dumpElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastStateDumpTime).count();
                if (dumpElapsed >= 30)
                {
                    mLogger->warn("RunQueue periodic state dump: round={}, files_done={}, bytes_done={}",
                                   round,
                                   mReporter ? mReporter->GetFilesDone() : 0,
                                   mReporter ? mReporter->GetBytesDone() : 0);
                    DumpState();
                    lastStateDumpTime = now;
                }
            }

            round++;
        }

        Reset();

        return {};
    }

    void SetFuncDurationStat(std::shared_ptr<FuncDurationStat> stat)
    {
        mFuncDurationStat = stat;
    }

    // Dump internal state for hang diagnosis (always at warn level)
    void DumpState() const
    {
        std::map<IOSlot::Status, int> rwStatusCount;
        for (auto &slot_up : mRWSlots)
            rwStatusCount[slot_up->GetStatus()]++;

        mLogger->warn("IOSlotMgr state: rw_slots={}", mRWSlots.size());
        for (const auto &[status, count] : rwStatusCount)
            mLogger->warn("  rw slot: {} x{}", IOSlot::StatusToStr(status), count);

        if (!mCksumSlots.empty())
        {
            std::map<IOSlot::Status, int> cksumStatusCount;
            for (auto &slot_up : mCksumSlots)
                cksumStatusCount[slot_up->GetStatus()]++;

            mLogger->warn("  cksum_slots={}, cksum_queue={}", mCksumSlots.size(), mCksumQueue.size());
            for (const auto &[status, count] : cksumStatusCount)
                mLogger->warn("  cksum slot: {} x{}", IOSlot::StatusToStr(status), count);
        }

        // Log details of non-Init slots for deeper diagnosis
        for (auto &slot_up : mRWSlots)
        {
            auto st = slot_up->GetStatus();
            if (st != IOSlot::Status::Init)
            {
                auto ioInfo = slot_up->GetIOInfo();
                std::string srcPath;
                if (slot_up->GetCPFPPtr())
                    srcPath = slot_up->GetCPFPPtr()->GetSrcPath();
                mLogger->warn("  rw[{}]: status={}, offset={}, io_size={}, src={}",
                               slot_up->GetID(), IOSlot::StatusToStr(st),
                               ioInfo.offset, ioInfo.io_size, srcPath);
            }
        }

        mCPFPMgr->DumpState();
    }

protected:
    SlotType *GetOneFreeCksumSlot()
    {
        for (auto &slot_up : mCksumSlots)
        {
            auto slot = slot_up.get();
            if (slot->GetStatus() == IOSlot::Status::Init)
            {
                return slot;
            }
        }
        return nullptr;
    }

    tl::expected<int, StackError> SubmitReads()
    {
        std::vector<IOSlot *> batch;
        batch.reserve(mOptions.Batch);
        int submitted = 0;

        for (auto &slot_up : mRWSlots)
        {
            auto slot = slot_up.get();
            // prepare read io
            if (slot->GetStatus() == IOSlot::Status::Init)
            {

#ifndef NDEBUG
                // 打印io_submit()所用时间
                auto start = std::chrono::high_resolution_clock::now();
                auto next_res = mCPFPMgr->GetNextReadIO();
                auto end = std::chrono::high_resolution_clock::now();
                AddDuration("GetNextReadIO()", start, end);
#else
                auto next_res = mCPFPMgr->GetNextReadIO();
#endif

                if (!next_res)
                {
                    return tl::unexpected(StackError("mCPFPMgr->GetNextReadIO(), err: ", next_res.error()));
                }

                auto nextIO = next_res.value();

                if (nextIO == nullptr)
                {
                    mLogger->debug("SubmitReads(): All read IOs have been submitted.");
                    break; // all io submitted
                }

                size_t offset = nextIO->GetReadOffset();
                PrepareOneRead(slot, offset, nextIO);
                if ((mOptions.CopyMode == "CksumCopy" || mOptions.CopyMode == "CksumOnly") &&
                    slot->GetType() == "rw")
                {
                    mCksumQueue.push(slot);
                }
            }

            // collect prepared read io into batch
            if (slot->GetStatus() == IOSlot::Status::ReadPrepared)
            {
                batch.push_back(slot);
                if (static_cast<int>(batch.size()) >= mOptions.Batch)
                {
                    auto res = SubmitBatchRead(batch);
                    if (!res)
                    {
                        if (res.error().Code() == -EAGAIN)
                        {
                            mLogger->debug("SubmitBatchRead() got EAGAIN, will try later.");
                            PrtSlots();
                            break;
                        }
                        return tl::unexpected(res.error());
                    }
                    submitted += res.value();
                    batch.clear();
                }
            }
        }

        // submit remaining slots in the final batch
        if (!batch.empty())
        {
            auto res = SubmitBatchRead(batch);
            if (!res)
            {
                if (res.error().Code() == -EAGAIN)
                {
                    mLogger->debug("SubmitBatchRead() final batch got EAGAIN, will try later.");
                }
                else
                {
                    return tl::unexpected(res.error());
                }
            }
            else
            {
                submitted += res.value();
            }
        }

        return submitted;
    }
    tl::expected<int, StackError> SubmitWrites()
    {
        std::vector<IOSlot *> batch;
        batch.reserve(mOptions.Batch);
        int submitted = 0;

        for (auto &slot_up : mRWSlots)
        {
            auto slot = slot_up.get();
            if (slot->GetStatus() == IOSlot::Status::WritePrepared)
            {
                batch.push_back(slot);
                if (static_cast<int>(batch.size()) >= mOptions.Batch)
                {
                    auto res = SubmitBatchWrite(batch);
                    if (!res)
                    {
                        if (res.error().Code() == -EAGAIN)
                        {
                            mLogger->debug("SubmitBatchWrite() got EAGAIN, will try later.");
                            PrtSlots();
                            break;
                        }
                        return tl::unexpected(res.error());
                    }
                    submitted += res.value();
                    batch.clear();
                }
            }
        }

        if (!batch.empty())
        {
            auto res = SubmitBatchWrite(batch);
            if (!res)
            {
                if (res.error().Code() == -EAGAIN)
                {
                    mLogger->debug("SubmitBatchWrite() final batch got EAGAIN, will try later.");
                }
                else
                {
                    return tl::unexpected(res.error());
                }
            }
            else
            {
                submitted += res.value();
            }
        }
        return submitted;
    }

    void PrepareOneRead(IOSlot *slot, off_t offset, std::shared_ptr<CPFilePair> currCPFPIt)
    {
        auto io_size = mOptions.IoSize;
        slot->SetCPFPPtr(currCPFPIt);
        slot->SetIOInfo(offset, io_size);

        mLogger->trace("PrepareOneRead() slot ID: {}, type: {}, offset: {}, io_size: {}, src: {}, dst: {}",
                       slot->GetID(), slot->GetType(), offset, io_size,
                       currCPFPIt->GetSrcPath(), currCPFPIt->GetDstPath());

        if (slot->GetType() == "rw")
        {
            DoPrepareOneRead(slot,
                           currCPFPIt->GetSrcFd(),
                           slot->GetBuf(),
                           io_size,
                           offset);
            currCPFPIt->UpdatePrepareReadBytes(io_size);
        }
        else
        {
            DoPrepareOneRead(slot,
                           currCPFPIt->GetDstFd(),
                           slot->GetBuf(),
                           io_size,
                           offset);
        }

        slot->SetStatus(IOSlot::Status::ReadPrepared);

        if (slot->GetType() == "rw")
        {
            auto check_res = mCPFPMgr->CheckReadComplete(slot->GetCPFPPtr());
            if (!check_res)
            {
                mLogger->error("CheckReadComplete failed in PrepareOneRead: {}", check_res.error().ToString());
            }
        }
    }

    tl::expected<int, StackError> SubmitCksumReads()
    {
        int submitted = 0;
        if (mCksumQueue.empty())
        {
            return submitted;
        }

        std::vector<IOSlot *> batch;
        batch.reserve(mOptions.Batch);

        for (auto &slot_up : mCksumSlots)
        {
            auto slot = slot_up.get();
            // prepare cksum read io
            if (slot->GetStatus() == IOSlot::Status::Init)
            {
                if (mCksumQueue.empty())
                {
                    break;
                }
                auto ioSlot = mCksumQueue.front();
                mCksumQueue.pop();
                // associate ioSlot with cksum slot
                slot->SetAssociatedSlot(ioSlot);
                ioSlot->SetAssociatedSlot(slot);

                auto cksum_read_offset = ioSlot->GetIOInfo().offset;
                PrepareOneRead(slot, cksum_read_offset, ioSlot->GetCPFPPtr());
            }

            // collect prepared cksum read io into batch
            if (slot->GetStatus() == IOSlot::Status::ReadPrepared)
            {
                batch.push_back(slot);
                if (static_cast<int>(batch.size()) >= mOptions.Batch)
                {
                    auto res = SubmitBatchRead(batch);
                    if (!res)
                    {
                        if (res.error().Code() == -EAGAIN)
                        {
                            mLogger->debug("SubmitBatchRead() for cksum got EAGAIN, will try later.");
                            PrtSlots();
                            break;
                        }
                        return tl::unexpected(res.error());
                    }
                    submitted += res.value();
                    batch.clear();
                }
            }
        }

        if (!batch.empty())
        {
            auto res = SubmitBatchRead(batch);
            if (!res)
            {
                if (res.error().Code() == -EAGAIN)
                {
                    mLogger->debug("SubmitBatchRead() for cksum final batch got EAGAIN, will try later.");
                }
                else
                {
                    return tl::unexpected(res.error());
                }
            }
            else
            {
                submitted += res.value();
            }
        }
        return submitted;
    }

    void PrtSlots()
    {
        mLogger->warn("Current IOSlot statuses:");
        // 按状态统计slot数量，并打印每个状态slot的数量
        std::map<IOSlot::Status, int> status_count;
        for (auto &slot_up : mRWSlots)
        {
            auto slot = slot_up.get();
            status_count[slot->GetStatus()]++;
        }

        for (const auto &pair : status_count)
        {
            mLogger->warn("Slot Status: {}, Count: {}", IOSlot::StatusToStr(pair.first), pair.second);
        }
    }
    void Reset()
    {
        for (auto &slot_up : mRWSlots)
        {
            if (slot_up)
            {
                slot_up->Reset();
            }
        }

        for (auto &slot_up : mCksumSlots)
        {
            if (slot_up)
            {
                slot_up->Reset();
            }
        }
    }

    void AddDuration(std::string_view func_name, std::chrono::high_resolution_clock::time_point start,
                     std::chrono::high_resolution_clock::time_point end)
    {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        if (duration > 100)
        {
            mLogger->warn("Function {} took too long: {} ms", func_name, duration);
        }

        if (mFuncDurationStat)
        {
            mFuncDurationStat->AddDuration(func_name, duration);
        }
    }
    tl::expected<void, StackError> CheckOneCompleted(SlotType *slot)
    {
        if (slot->GetStatus() == IOSlot::Status::WriteReaped)
        {
            auto check_res = mCPFPMgr->CheckWriteComplete(slot->GetCPFPPtr());
            if (!check_res)
            {
                return tl::unexpected(StackError("mCPFPMgr->CheckWriteComplete(), err: ", check_res.error()));
            }
            slot->Reset();
        }
        return {};
    };

    bool bothSlotsReadReaped(SlotType *slot1, SlotType *slot2)
    {
        assert(slot1 != nullptr && slot2 != nullptr);
        assert(slot1 != slot2);
        return (slot1->GetStatus() == IOSlot::Status::ReadReaped) &&
               (slot2->GetStatus() == IOSlot::Status::ReadReaped);
    }

    // 验证两个slot的内容是否一致，返回true表示一致，false表示不一致
    bool cksum(SlotType *slot1, SlotType *slot2)
    {
        assert(slot1 != nullptr && slot2 != nullptr);
        assert(slot1 != slot2);
        assert(mDigest != nullptr);
        // 获取slot1和slot2的buf内容
        const char *buf1 = slot1->GetBuf();
        const char *buf2 = slot2->GetBuf();
        auto size1 = slot1->GetIOInfo().io_size;
        auto size2 = slot2->GetIOInfo().io_size;
        if (size1 != size2)
        {
            return false;
        }

        std::string cksum1 = mDigest->Do(buf1, size1);
        std::string cksum2 = mDigest->Do(buf2, size2);
        return cksum1 == cksum2;
    }

    // Shared completion handlers for backends to call after extracting event data.
    // These encapsulate the common logic for handling read/write completions so
    // that backend implementations (libaio / io_uring) don't duplicate it.
    tl::expected<void, StackError> HandleReadCompletion(SlotType *slot, ssize_t io_ret)
    {
        assert(slot != nullptr);
        // mark as reaped
        slot->SetStatus(IOSlot::Status::ReadReaped);
        auto [offset, io_size] = slot->GetIOInfo();

        if (io_ret < 0)
        {
            if (io_ret == -EAGAIN)
            {
                mLogger->warn("AIO read got EAGAIN, resubmitting for slot ID: {}, offset: {}, io_size: {}",
                              slot->GetID(), offset, io_size);
                PrtSlots();
                // ask backend to re-prepare this read (derived class implements PrepareOneRead)
                PrepareOneRead(slot, offset, slot->GetCPFPPtr());
                return {};
            }
            if (mReporter)
            {
                mReporter->FileError(slot->GetCPFPPtr()->GetSrcPath(), strerror(-io_ret), -io_ret, "failed");
            }
            return tl::unexpected(StackError(
                fmt::format("read failed, errno: {}, errstr: {}, slotType: {}", -io_ret, strerror(-io_ret), slot->GetType())));
        }

        mLogger->debug("read completed for slot ID: {}, type: {}, offset: {}, bytes read: {}, slot addr: {:p}",
                       slot->GetID(), slot->GetType(), offset, io_ret, static_cast<void *>(slot));

        if (io_ret == 0) // EOF, 特殊处理
        {
            if (slot->GetType() == "rw")
            {
                mLogger->debug("Reached EOF on read for slot ID: {}, offset: {}, src: {}, dst: {}",
                               slot->GetID(), offset,
                               slot->GetCPFPPtr()->GetSrcPath(),
                               slot->GetCPFPPtr()->GetDstPath());
                slot->Reset(); // this will cancel the write IO
                return {};
            }
            else // cksum slot read EOF, set cksum error on associated rw slot
            {
                slot->GetCPFPPtr()->SetCksumError(true);
                // don't return here
            }
        }

        slot->SetIOInfo(offset, static_cast<size_t>(io_ret));

        if (mOptions.CopyMode == "CopyOnly")
        {
            if (mOptions.PreserveSparseFiles &&
                slot->GetCPFPPtr()->IsProbablySparse() &&
                IsAllZeros(slot->GetBuf(), static_cast<size_t>(io_ret)))
            {
                mLogger->debug("Sparse hole detected at offset: {} size: {} for src: {}, dst: {}",
                               offset, io_ret,
                               slot->GetCPFPPtr()->GetSrcPath(),
                               slot->GetCPFPPtr()->GetDstPath());
                auto skip_res = slot->GetCPFPPtr()->SkipWriteAsHole(static_cast<size_t>(io_ret));
                if (!skip_res)
                {
                    return tl::unexpected(StackError("SkipWriteAsHole() err: ", skip_res.error()));
                }
                auto check_res = mCPFPMgr->CheckWriteComplete(slot->GetCPFPPtr());
                if (!check_res)
                {
                    return tl::unexpected(StackError("CheckWriteComplete after SkipWriteAsHole, err: ", check_res.error()));
                }
                slot->Reset();
            }
            else
            {
                // prepare write io using the actual bytes read
                PrepareOneWrite(slot);
            }
        }
        else if (mOptions.CopyMode == "CksumCopy")
        {
            if (bothSlotsReadReaped(slot, slot->GetAssociatedSlot()))
            {
                // both read slots are reaped, do cksum comparison
                auto ioSlot = (slot->GetType() == "rw") ? slot : slot->GetAssociatedSlot();
                bool match = false;
                if (ioSlot->GetCPFPPtr()->GetCksumError())
                {
                    match = false; // if already marked as cksum error, treat as mismatch
                }
                else
                {
                    match = cksum(slot, slot->GetAssociatedSlot());
                    if (!match)
                    {
                        mLogger->debug("Checksum mismatch detected at offset: {} for src: {}, dst: {}",
                                       offset,
                                       slot->GetCPFPPtr()->GetSrcPath(),
                                       slot->GetCPFPPtr()->GetDstPath());
                        // then we do write using the rw slot

                        ioSlot->GetCPFPPtr()->SetCksumError(true);
                    }
                }

                if (!match) // mismatch, act like we've done the write using rw slot
                {
                    if (mOptions.PreserveSparseFiles &&
                        ioSlot->GetCPFPPtr()->IsProbablySparse() &&
                        IsAllZeros(ioSlot->GetBuf(), ioSlot->GetIOInfo().io_size))
                    {
                        mLogger->debug("Sparse hole detected at offset: {} size: {} for src: {}, dst: {}",
                                       offset, ioSlot->GetIOInfo().io_size,
                                       ioSlot->GetCPFPPtr()->GetSrcPath(),
                                       ioSlot->GetCPFPPtr()->GetDstPath());
                        auto skip_res = ioSlot->GetCPFPPtr()->SkipWriteAsHole(ioSlot->GetIOInfo().io_size);
                        if (!skip_res)
                        {
                            return tl::unexpected(StackError("SkipWriteAsHole() after cksum mismatch, err: ", skip_res.error()));
                        }
                        auto check_res = mCPFPMgr->CheckWriteComplete(ioSlot->GetCPFPPtr());
                        if (!check_res)
                        {
                            return tl::unexpected(StackError("CheckWriteComplete after SkipWriteAsHole, err: ", check_res.error()));
                        }
                        ioSlot->Reset();
                    }
                    else
                    {
                        PrepareOneWrite(ioSlot);
                    }
                }
                else // match , act just like we've done the write
                {
                    mLogger->debug("Checksum matched at offset: {} for src: {}, dst: {}",
                                   offset,
                                   slot->GetCPFPPtr()->GetSrcPath(),
                                   slot->GetCPFPPtr()->GetDstPath());
                    auto write_res = HandleWriteCompletion(ioSlot, ioSlot->GetIOInfo().io_size);
                    if (!write_res)
                    {
                        return tl::unexpected(StackError("HandleWriteCompletion() after cksum match, err: ", write_res.error()));
                    }
                }

                // reset cksum slot after pair processing
                auto cksumSlot = (slot->GetType() == "cksum") ? slot : slot->GetAssociatedSlot();
                cksumSlot->Reset();
            }
        }
        else // CksumOnly
        {
            if (bothSlotsReadReaped(slot, slot->GetAssociatedSlot()))
            {
                auto ioSlot = (slot->GetType() == "rw") ? slot : slot->GetAssociatedSlot();
                bool match = cksum(slot, slot->GetAssociatedSlot());

                if (!match)
                {
                    mLogger->debug("Checksum mismatch detected at offset: {} for src: {}, dst: {}",
                                   offset,
                                   slot->GetCPFPPtr()->GetSrcPath(),
                                   slot->GetCPFPPtr()->GetDstPath());
                    ioSlot->GetCPFPPtr()->SetCksumError(true);
                    ioSlot->GetCPFPPtr()->RecordCksumContent("mismatch");
                }

                if (ioSlot->GetCPFPPtr()->GetCksumError())
                {
                    ioSlot->GetCPFPPtr()->SetReadFinished(); // mark read as finished to avoid further reads
                }

                // cksum normally done, act like we've done the write
                auto write_res = HandleWriteCompletion(ioSlot, ioSlot->GetIOInfo().io_size);
                if (!write_res)
                {
                    return tl::unexpected(StackError("HandleWriteCompletion() after CksumOnly, err: ", write_res.error()));
                }

                // reset cksum slot after pair processing
                auto cksumSlot = (slot->GetType() == "cksum") ? slot : slot->GetAssociatedSlot();
                cksumSlot->Reset();
            }
        }

        return {};
    }

    tl::expected<void, StackError> HandleWriteCompletion(SlotType *slot, ssize_t io_ret)
    {
        assert(slot != nullptr);
        slot->SetStatus(IOSlot::Status::WriteReaped);
        auto [offset, io_size] = slot->GetIOInfo();

        if (io_ret < 0)
        {
            if (io_ret == -EAGAIN)
            {
                mLogger->warn("AIO write got EAGAIN, resubmitting for slot ID: {}, offset: {}, io_size: {}",
                              slot->GetID(), offset, io_size);
                PrtSlots();
                PrepareOneWrite(slot);
                return {};
            }
            if (mReporter)
            {
                mReporter->FileError(slot->GetCPFPPtr()->GetSrcPath(), strerror(-io_ret), -io_ret, "failed");
            }
            return tl::unexpected(StackError(
                fmt::format("AIO write failed, errno: {}, errstr: {}, offset: {}, io_size: {}",
                            io_ret, strerror(-io_ret), offset, io_size)));
        }

        if (io_ret == 0)
        {
            return tl::unexpected(StackError("AIO write returned 0 bytes written, unexpected."));
        }

        mLogger->debug("AIO write completed for slot ID: {}, offset: {}, bytes written: {}, src/dst info unknown", slot->GetID(), offset, io_ret);

        // update written bytes and check completion
        slot->GetCPFPPtr()->UpdateWrittenBytes(io_ret);
        if (mReporter)
        {
            mReporter->AddBytesDone(static_cast<size_t>(io_ret));
        }

        auto check_res = CheckOneCompleted(slot);
        if (!check_res)
        {
            return tl::unexpected(StackError("CheckOneCompleted() failed after write reap, err: ", check_res.error()));
        }

        return {};
    }

private:
    virtual tl::expected<void, StackError> Init() = 0;
    virtual void DoPrepareOneRead(SlotType *slot, int fd, void *buf, size_t ioSize, off_t offset) = 0;
    virtual void PrepareOneWrite(SlotType *slot) = 0;
    virtual tl::expected<int, StackError> SubmitBatchRead(std::vector<SlotType*> &slots) = 0;
    virtual tl::expected<int, StackError> SubmitBatchWrite(std::vector<SlotType*> &slots) = 0;
    virtual tl::expected<void, StackError> IOReap() = 0;

protected:
    std::vector<std::unique_ptr<SlotType>> mRWSlots;
    std::vector<std::unique_ptr<SlotType>> mCksumSlots;
    std::queue<SlotType *> mCksumQueue;

    RWCombinedCopyOptions mOptions;
    CPFilePairMgr *mCPFPMgr;

    std::shared_ptr<ILogger> mLogger;
    FileLogReporter* mReporter = nullptr;
    std::shared_ptr<FuncDurationStat> mFuncDurationStat;

    std::unique_ptr<Digest> mDigest;
};