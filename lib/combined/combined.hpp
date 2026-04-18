#pragma once

#include <any>
#include <cstddef> // size_t
#include <libaio.h>
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

struct CopyEntry
{
    std::string srcPath; // full path
    std::string dstPath;

    // 重载一个等号操作符用于DedupQueue的去重功能
    bool operator==(const CopyEntry &other) const
    {
        return (srcPath == other.srcPath) && (dstPath == other.dstPath);
    }
};

// class CPFilePair;

struct RWCombinedCopyOptions
{
    std::string LogLevel;
    std::string LogMode;
    std::string LogFilePath;
    std::string CopyEngine;
    std::string CopyMode;       // "CksumCopy", "CopyOnly", "CksumOnly"
    std::string CksumAlgorithm; // "xxhash64", "md5", "sha256"
    int CopyParallelism;
    int CopyChanSize;
    bool EnableInotify;
    bool PreserveSparseFiles;
    bool DirectIO = false;
    bool SyncWrites = false;
    size_t IoSize = 1 * 1024 * 1024; // 1MB
    size_t QueueDepth = 8;
    int Batch = 4;
    int IOReapWait = 1; // seconds
};

// 这个类提供源文件和目标文件的信息，并且IO计数的功能，并且负责打开和关闭文件描述符、复制attr extended-attributes等
// 将来还要负责权限、时间戳等的复制、创建目录等功能
class CPFilePair
{
public:
    CPFilePair(std::string_view src_path,
               std::string_view dst_path,
               size_t io_size,
               bool direct_io,
               bool sync_writes,
               bool cksum,
               std::shared_ptr<ILogger> logger); // full path expected
    ~CPFilePair()
    {
        if (mSrcFd >= 0)
        {
            mLogger->trace("Closing source file descriptor: {}, src: {}", mSrcFd, mSrcPath);
            close(mSrcFd);
            mSrcFd = -1;
        }
        if (mDstFd >= 0)
        {
            mLogger->trace("Closing destination file descriptor: {}, dst: {}", mDstFd, mDstPath);
            close(mDstFd);
            mDstFd = -1;
        }
    }
    tl::expected<void, StackError> CheckAndInit();
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
    bool IsInitialized() const { return (mSrcFd >= 0 && mDstFd >= 0) || IsDir(); }
    bool IsDir() const { return mIsDir; }
    bool IsSymlink() const { return mIsSymlink; }

    void SetCksumError(bool err) { mIsChksumError = err; }
    bool GetCksumError() const { return mIsChksumError; }

private:
    int mSrcFd = -1;
    int mDstFd = -1;
    struct stat mSrcStat;
    struct stat mDstStat;
    std::string mSrcPath; // full path of source file
    std::string mDstPath; // full path of destination file

    size_t mReadBytes = 0;
    size_t mWrittenBytes = 0;
    size_t mIOSize = 0;
    bool mDirectIO = false;
    bool mSyncWrites = false;
    bool mCksum = false;

    bool mIsDir = false;
    bool mIsSymlink = false;

    bool mIsChksumError = false;

    std::shared_ptr<ILogger> mLogger;
};

class CPFilePairMgr
{
public:
    // make sure at lease we got one elem in mFilePairs
    CPFilePairMgr(const RWCombinedCopyOptions &options,
                  std::shared_ptr<ILogger> logger)
        : mOptions(options), mLogger(logger)
    {
        mReadPtr = mFilePairs.begin();
    }

    tl::expected<void, StackError> AddFilePair(std::string_view src_path, std::string_view dst_path)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mFilePairs.push_back(
            std::make_shared<CPFilePair>(src_path,
                                         dst_path,
                                         mOptions.IoSize,
                                         mOptions.DirectIO,
                                         mOptions.SyncWrites,
                                         (mOptions.CopyMode == "CksumCopy" || mOptions.CopyMode == "CksumOnly"),
                                         mLogger));
        if (mFilePairs.size() == 1) // 下面的动作只在第一个文件对加入时执行
        {
            mReadPtr = mFilePairs.begin();
            mStartFlag.store(true);
        }

        return {};
    }

    tl::expected<std::shared_ptr<CPFilePair>, StackError> GetNextReadIO();
    tl::expected<void, StackError> CheckWriteComplete(std::shared_ptr<CPFilePair> writeIt);
    void CheckReadComplete(std::shared_ptr<CPFilePair> pFP);

    void SetStopFlag()
    {
        mStopFlag.store(true);
    }

    bool ShouldStartCopy()
    {
        return mStartFlag.load();
    }
    bool ShouldStopCopy()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto should = (mStopFlag.load() &&
                       mReadPtr == mFilePairs.end() &&
                       mStartFlag.load() &&
                       mFilePairs.empty() &&
                       mInflightFPs.Empty());
        if (!should)
        {
            mLogger->debug("ShouldStopCopy() == false: StopFlag: {}, ReadPtr at end: {}, StartFlag: {}, FilePairs empty: {}, InflightFPs empty: {}",
                           mStopFlag.load(),
                           (mReadPtr == mFilePairs.end()) ? "true" : "false",
                           mStartFlag.load(),
                           mFilePairs.empty() ? "true" : "false",
                           mInflightFPs.Empty() ? "true" : "false");

            if (mReadPtr != mFilePairs.end())
            {
                // print info about remaining file pairs
                mLogger->debug("Remaining file pairs to read:");
                for (auto it = mReadPtr; it != mFilePairs.end(); ++it)
                {
                    mLogger->debug("  src: {}, dst: {}", (*it)->GetSrcPath(), (*it)->GetDstPath());
                }
            }

            if (!mInflightFPs.Empty())
            {
                mLogger->debug("Inflight file pairs:");
                for (const auto &fp : mInflightFPs.GetList())
                {
                    mLogger->debug("  src: {}, dst: {}, read_offset: {}, write_offset: {}",
                                   fp->GetSrcPath(), fp->GetDstPath(),
                                   fp->GetReadOffset(), fp->GetWriteOffset());
                }
            }
        }
        return should;
    }

private:
    bool CheckReadCompleteNoLock(std::shared_ptr<CPFilePair> pFP);
    void ReadPtrAdvance(std::shared_ptr<CPFilePair> pFP)
    {
        assert(pFP == *mReadPtr);
        if (mReadPtr != mFilePairs.end())
        {
            mLogger->debug("ReadPtrAdvance(): advanced from : {}",
                           (*mReadPtr)->GetSrcPath());
        }

        mFilePairs.erase(mReadPtr); // mReadPtr指向的元素被移除后，mReadPtr就不再准确了，需要重新指向begin()
        mReadPtr = mFilePairs.begin();

        if (mReadPtr != mFilePairs.end())
        {
            mLogger->debug("ReadPtrAdvance(): advanced to : {}",
                           (*mReadPtr)->GetSrcPath());
        }
    }

private:
    std::list<std::shared_ptr<CPFilePair>> mFilePairs;
    DedupList<CPFilePair> mInflightFPs;

    RWCombinedCopyOptions mOptions;
    std::mutex mMutex; // to protect mFilePairs, mReadPtr, mWritePtr in multithreaded scenarios
    std::list<std::shared_ptr<CPFilePair>>::iterator mReadPtr;

    std::atomic_bool mStartFlag = false; // whether file pairs have been filled
    std::atomic_bool mStopFlag = false;

    std::shared_ptr<ILogger> mLogger;
};

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
    struct iocb *GetReadIOCB() { return &mIocbRead; }
    struct iocb *GetWriteIOCB() { return &mIocbWrite; }

    struct iocb *InitReadIOCB()
    {
        bzero(&mIocbRead, sizeof(struct iocb));
        return &mIocbRead;
    }
    struct iocb *InitWriteIOCB()
    {
        bzero(&mIocbWrite, sizeof(struct iocb));
        return &mIocbWrite;
    }

    // UserData field, not used yet
    const std::any &GetUserData() const { return mUserData; }    // not used yet
    void SetUserData(const std::any &data) { mUserData = data; } // not used yet

    std::shared_ptr<CPFilePair> GetCPFPPtr() { return mCPFPIt; }
    std::shared_ptr<CPFilePair> GetCPFPPtr() const { return mCPFPIt; }
    void SetCPFPPtr(std::shared_ptr<CPFilePair> it) { mCPFPIt = it; }

    std::string GetType() const { return mType; }

    void SetAssociatedSlot(IOSlot *slot) { mAssociatedSlot = slot; }
    IOSlot *GetAssociatedSlot() const { return mAssociatedSlot; }

private:
    // data fields
    char *mBuf = nullptr;
    Status mStatus = Status::Init;
    int mId = -1;        // its id, also index in IOSlotMgr's m_slots
    std::any mUserData; // user data field
    std::shared_ptr<CPFilePair> mCPFPIt;

    IOInfo mIOInfo;

    struct iocb mIocbRead;  // 读iocb
    struct iocb mIocbWrite; // 写iocb

    std::string mType;
    IOSlot *mAssociatedSlot = nullptr;
};

// abstract class for IOSlotMgr, use as interface
template <typename SlotType = IOSlot>
class IOSlotMgr
{
public:
    IOSlotMgr(const RWCombinedCopyOptions &options,
              CPFilePairMgr *file_pair_mgr,
              std::shared_ptr<ILogger> logger)
        : mOptions(options), mCPFPMgr(file_pair_mgr), mLogger(logger)
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

            if (options.CopyMode == "CksumOnly")
            {
                mCksumResultFile.open("./cksum_result.log", std::ios::out | std::ios::trunc);
                if (!mCksumResultFile.is_open())
                {
                    throw std::runtime_error("Failed to open/create ./cksum_result.log for writing checksum results.");
                }
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

    tl::expected<void, StackError> RunCopyQueue()
    {
        while (!mCPFPMgr->ShouldStartCopy())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (mCPFPMgr->ShouldStopCopy())
            {
                mLogger->debug("Received stop signal before starting copy queue.");
                return {};
            }
        }

        auto init_res = Init();
        if (!init_res)
        {
            return tl::unexpected(init_res.error());
        }

        long round = 0;
        while (!mCPFPMgr->ShouldStopCopy())
        {

#ifndef NDEBUG
            auto start = std::chrono::high_resolution_clock::now();
            auto submit_res = SubmitReads();
            auto end = std::chrono::high_resolution_clock::now();
            AddDuration("SubmitReads()", start, end);
#else
            auto submit_res = SubmitReads();
#endif

            if (!submit_res)
            {
                return tl::unexpected(StackError("SubmitReads(), err: ", submit_res.error()));
            }

            mLogger->debug("Submitted {} read IOs in round: {}.", submit_res.value(), round);

            // auto check_stuck_res = CheckStuck();
            // if (!check_stuck_res)
            // {
            //     return tl::unexpected(check_stuck_res.error());
            // }

            auto reap_res = IOReap();
            if (!reap_res)
            {
                return tl::unexpected(StackError("IOReap(), err: ", reap_res.error()));
            }

            auto write_res = SubmitWrites();
            if (!write_res)
            {
                return tl::unexpected(StackError("SubmitWrites(), err: ", write_res.error()));
            }
            mLogger->debug("Submitted {} write IOs in round: {}.", write_res.value(), round);

            round++;
        }

        Reset();

        return {};
    }

    tl::expected<void, StackError> RunQueue()
    {
        while (!mCPFPMgr->ShouldStartCopy())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (mCPFPMgr->ShouldStopCopy())
            {
                mLogger->debug("Received stop signal before starting copy queue.");
                return {};
            }
        }

        auto init_res = Init();
        if (!init_res)
        {
            return tl::unexpected(init_res.error());
        }

        long round = 0;
        while (!mCPFPMgr->ShouldStopCopy())
        {
            mLogger->debug("Starting IO round: {}", round);

#ifndef NDEBUG
            auto start = std::chrono::high_resolution_clock::now();
            auto submit_res = SubmitReads();
            auto end = std::chrono::high_resolution_clock::now();
            AddDuration("SubmitReads()", start, end);
#else
            auto submit_res = SubmitReads();
#endif

            if (!submit_res)
            {
                return tl::unexpected(StackError("SubmitReads(), err: ", submit_res.error()));
            }

            mLogger->debug("Submitted {} read IOs in round: {}.", submit_res.value(), round);

            if (mCksumQueue.size() > 0)
            {
                auto submit_res = SubmitCksumReads();
                if (!submit_res)
                {
                    return tl::unexpected(StackError("SubmitCksumReads(), err: ", submit_res.error()));
                }
                mLogger->debug("Submitted {} checksum read IOs in round: {}.", submit_res.value(), round);
            }

            auto reap_res = IOReap();
            if (!reap_res)
            {
                return tl::unexpected(StackError("IOReap(), err: ", reap_res.error()));
            }

            if (mOptions.CopyMode != "CksumOnly")
            {
                auto write_res = SubmitWrites();
                if (!write_res)
                {
                    return tl::unexpected(StackError("SubmitWrites(), err: ", write_res.error()));
                }
                mLogger->debug("Submitted {} write IOs in round: {}.", write_res.value(), round);
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

protected:
    tl::expected<void, StackError> CheckStuck()
    {
        // 检查是否只少有一个slot处于ReadSubmitted或者WriteSubmitted状态
        bool isStuck = true;
        for (auto &slot_up : mRWSlots)
        {
            auto slot = slot_up.get();
            if (slot->GetStatus() == IOSlot::Status::ReadSubmitted || slot->GetStatus() == IOSlot::Status::WriteSubmitted)
            {
                isStuck = false;
                break;
            }
        }

        if (isStuck && !mCPFPMgr->ShouldStopCopy() && !mOptions.EnableInotify)
        {
            mLogger->warn("Detected stuck AIO operations.");
            return tl::unexpected(StackError("Detected stuck AIO operations."));
        }

        return {};
    }

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
        // TODO: change to batch submit later
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
                if (mOptions.CopyMode == "CksumCopy" || mOptions.CopyMode == "CksumOnly")
                {
                    mCksumQueue.push(slot);
                }
            }

            // submit prepared read io
            if (slot->GetStatus() == IOSlot::Status::ReadPrepared)
            {
                auto res = SubmitOneRead(slot);
                if (!res)
                {
                    // if EAGAIN, break and try again later
                    if (res.error().Code() == -EAGAIN)
                    {
                        mLogger->debug("io_submit() for read got EAGAIN, slot: {}, will try later.", slot->GetID());
                        PrtSlots();
                        break;
                    }
                    return tl::unexpected(res.error());
                }
                else
                {
                    submitted++;
                }
            }
        }

        return submitted;
    }
    tl::expected<int, StackError> SubmitWrites()
    {
        // TODO: change to batch submit later
        int submitted = 0;

        // 找出所有可以提交写请求的slot并提交写请求
        for (auto &slot_up : mRWSlots)
        {
            auto slot = slot_up.get();
            // prepare write io
            if (slot->GetStatus() == IOSlot::Status::WritePrepared)
            {

                auto res = SubmitOneWrite(slot);
                if (!res)
                {
                    // if EAGAIN, break and try again later
                    if (res.error().Code() == -EAGAIN)
                    {
                        mLogger->debug("SubmitOneWrite() in SubmitWrites() got EAGAIN, slot: {}, will try later.", slot->GetID());
                        PrtSlots();
                        break;
                    }
                    return tl::unexpected(res.error());
                }
                else
                {
                    submitted++;
                }
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
            mCPFPMgr->CheckReadComplete(slot->GetCPFPPtr());
        }
    }

    tl::expected<int, StackError> SubmitCksumReads()
    {
        int submitted = 0;
        if (mCksumQueue.empty())
        {
            return submitted;
        }
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

            // submit prepared cksum read io
            if (slot->GetStatus() == IOSlot::Status::ReadPrepared)
            {
                auto res = SubmitOneRead(slot);
                if (!res)
                {
                    // if EAGAIN, break and try again later
                    if (res.error().Code() == -EAGAIN)
                    {
                        mLogger->debug("io_submit() for cksum read got EAGAIN, slot: {}, will try later.", slot->GetID());
                        PrtSlots();
                        break;
                    }
                    return tl::unexpected(res.error());
                }
                else
                {
                    submitted++;
                }
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

        std::string cksum1 = mDigest->Do(buf1, mOptions.IoSize);
        std::string cksum2 = mDigest->Do(buf2, mOptions.IoSize);
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
            // prepare write io using the actual bytes read
            PrepareOneWrite(slot);
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
                    PrepareOneWrite(ioSlot);
                }
                else // match , act just like we've done the write
                {
                    mLogger->debug("Checksum matched at offset: {} for src: {}, dst: {}",
                                   offset,
                                   slot->GetCPFPPtr()->GetSrcPath(),
                                   slot->GetCPFPPtr()->GetDstPath());
                    HandleWriteCompletion(ioSlot, ioSlot->GetIOInfo().io_size);
                }
            }
        }
        else // CksumOnly
        {
            // write the cksum result to ./cksum_result.log file
            if (bothSlotsReadReaped(slot, slot->GetAssociatedSlot()))
            {
                auto ioSlot = (slot->GetType() == "rw") ? slot : slot->GetAssociatedSlot();
                bool match = cksum(slot, slot->GetAssociatedSlot());
                mCksumResultFile << ioSlot->GetCPFPPtr()->GetSrcPath() << "," << ioSlot->GetCPFPPtr()->GetDstPath()
                                 << "," << offset << "," << (match ? "MATCHED" : "MISMATCH") << std::endl;
                if (!match)
                {
                    mLogger->debug("Checksum mismatch detected at offset: {} for src: {}, dst: {}",
                                   offset,
                                   slot->GetCPFPPtr()->GetSrcPath(),
                                   slot->GetCPFPPtr()->GetDstPath());
                    ioSlot->GetCPFPPtr()->SetCksumError(true);
                }

                if (ioSlot->GetCPFPPtr()->GetCksumError())
                {
                    ioSlot->GetCPFPPtr()->SetReadFinished(); // mark read as finished to avoid further reads
                    ioSlot->Reset();                         // reset the slot for next use
                }

                // cksum normally done, act like we've done the write
                HandleWriteCompletion(ioSlot, ioSlot->GetIOInfo().io_size);
            }
        }

        if (slot->GetType() == "cksum")
        {
            // reset cksum slot after processing
            slot->Reset();
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
    virtual tl::expected<void, StackError> SubmitOneRead(SlotType *slot) = 0;
    virtual tl::expected<void, StackError> SubmitOneWrite(SlotType *slot) = 0;
    virtual tl::expected<void, StackError> IOReap() = 0;

protected:
    std::vector<std::unique_ptr<SlotType>> mRWSlots;
    std::vector<std::unique_ptr<SlotType>> mCksumSlots;
    std::queue<SlotType *> mCksumQueue;

    RWCombinedCopyOptions mOptions;
    CPFilePairMgr *mCPFPMgr;

    std::shared_ptr<ILogger> mLogger;
    std::shared_ptr<FuncDurationStat> mFuncDurationStat;

    std::unique_ptr<Digest> mDigest;
    std::ofstream mCksumResultFile;
};