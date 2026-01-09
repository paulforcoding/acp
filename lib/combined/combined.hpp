#pragma once

#include <any>
#include <cstddef> // size_t
#include <libaio.h>
#include <tl/expected.hpp>
#include <memory>
#include "base/base.hpp"
#include "base/chan.hpp"
#include <vector>
#include <map>
#include <chrono>
#include <cassert>
#include <string>

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

class CPFilePair;

class IOSlot
{
public:
    enum class Status
    {
        Init,
        ReadPrepared,
        ReadSubmitted,
        ReadReaped,
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
    IOSlot(size_t buf_size, int id)
        : m_buf{AllocBytes(SECTORSIZE, buf_size)}, m_status{Status::Init}, m_id{id}
    {
    }
    // dtor
    virtual ~IOSlot() { FreeBytes(m_buf); }

    // disable copy and move to avoid accidental double-free or ownership transfer
    IOSlot(const IOSlot &) = delete;
    IOSlot &operator=(const IOSlot &) = delete;
    IOSlot(IOSlot &&) = delete;
    IOSlot &operator=(IOSlot &&) = delete;

    virtual void Reset()
    {
        m_status = Status::Init;
        // bzero(m_buf, SECTORSIZE);
        m_user_data = {};
        // clear IO tracking
        mIOInfo = IOInfo{};
    }

    char *GetBuf() const { return m_buf; }
    Status GetStatus() const { return m_status; }
    int GetID() const { return m_id; }
    void SetStatus(Status s) { m_status = s; }

    // IOInfo fields, used by ucp, in future may be used by acp
    struct IOInfo
    {
        off_t offset = 0;
        size_t io_size = 0;
    };
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
    struct iocb *GetReadIOCB() { return &m_iocb_read; }
    struct iocb *GetWriteIOCB() { return &m_iocb_write; }

    struct iocb *InitReadIOCB()
    {
        bzero(&m_iocb_read, sizeof(struct iocb));
        return &m_iocb_read;
    }
    struct iocb *InitWriteIOCB()
    {
        bzero(&m_iocb_write, sizeof(struct iocb));
        return &m_iocb_write;
    }

    // UserData field, not used yet
    const std::any &GetUserData() const { return m_user_data; }           // not used yet
    void SetUserData(const std::any &data) { m_user_data = data; }        // not used yet

    std::shared_ptr<CPFilePair> GetCPFPPtr() { return mCPFPIt; }
    std::shared_ptr<CPFilePair> GetCPFPPtr() const { return mCPFPIt; }
    void SetCPFPPtr(std::shared_ptr<CPFilePair> it) { mCPFPIt = it; }

private:
    // data fields
    char *m_buf = nullptr;
    Status m_status = Status::Init;
    int m_id = -1;        // its id, also index in IOSlotMgr's m_slots
    std::any m_user_data; // user data field
    std::shared_ptr<CPFilePair> mCPFPIt;

    IOInfo mIOInfo;

    struct iocb m_iocb_read;  // 读iocb
    struct iocb m_iocb_write; // 写iocb
};

struct RWCombinedCopyOptions
{
    std::string LogLevel;
    std::string LogMode;
    std::string LogFilePath;
    std::string CopyEngine;
    int CopyParallelism;
    bool EnableInotify;
    bool PreserveSparseFiles;
    bool DirectIO = false;
    bool SyncWrites = false;
    size_t IoSize = 1 * 1024 * 1024; // 1MB
    size_t QueueDepth = 8;
    int Batch = 4;
    int IOReapWait = 1;
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
               std::shared_ptr<ILogger> logger); // full path expected
    ~CPFilePair()
    {
        if (m_src_fd >= 0)
        {
            mLogger->trace("Closing source file descriptor: {}, src: {}", m_src_fd, m_src_path);
            close(m_src_fd);
            m_src_fd = -1;
        }
        if (m_dst_fd >= 0)
        {
            mLogger->trace("Closing destination file descriptor: {}, dst: {}", m_dst_fd, m_dst_path);
            close(m_dst_fd);
            m_dst_fd = -1;
        }
    }
    tl::expected<void, StackError> CheckAndInit();
    tl::expected<void, StackError> TrucateDstToSrcSize()
    {
        if (ftruncate(m_dst_fd, m_src_stat.st_size) < 0)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to truncate destination file: {}, errno: {}, errstr: {}", m_dst_path, errno, strerror(errno))));
        }
        return {};
    }
    tl::expected<void, StackError> FsyncDst()
    {
        mLogger->trace("Fsyncing destination file: {}", m_dst_path);
        if (fsync(m_dst_fd) < 0)
        {
            return tl::unexpected(StackError(
                fmt::format("Failed to fsync destination file: {}, errno: {}, errstr: {}", m_dst_path, errno, strerror(errno))));
        }
        return {};
    }

    int GetSrcFd() const { return m_src_fd; }
    int GetDstFd() const { return m_dst_fd; }
    size_t GetSrcFileSize() const { return static_cast<size_t>(m_src_stat.st_size); }
    size_t GetDstFileSize() const { return static_cast<size_t>(m_dst_stat.st_size); } // not used yet
    std::string GetSrcPath() const { return m_src_path; }                             // not used yet
    std::string GetDstPath() const { return m_dst_path; }                             // not used yet
    tl::expected<void, std::string> DoDstState();                                     // not used yet
    struct stat *GetSrcStatPtr() { return &m_src_stat; }                              // not used yet
    struct stat *GetDstStatPtr() { return &m_dst_stat; }                              // not used yet
    size_t GetReadOffset() const { return mReadBytes; }
    size_t GetWriteOffset() const { return mWrittenBytes; }
    void UpdateReadBytes(size_t n) { mReadBytes += n; }
    void UpdateWrittenBytes(size_t n) { mWrittenBytes += n; }
    bool IsReadFinished() const { return mReadBytes >= GetSrcFileSize(); }
    bool IsWriteFinished() const
    {
        mLogger->debug("Checking IsWriteFinished: written_bytes: {}, src_file_size: {}, src: {}",
                       mWrittenBytes, GetSrcFileSize(), m_src_path);
        return mWrittenBytes >= GetSrcFileSize();
    }
    bool IsInitialized() const { return (m_src_fd >= 0 && m_dst_fd >= 0) || IsDir(); }
    bool IsDir() const { return mIsDir; }

private:
    int m_src_fd = -1;
    int m_dst_fd = -1;
    struct stat m_src_stat;
    struct stat m_dst_stat;
    std::string m_src_path; // full path of source file
    std::string m_dst_path; // full path of destination file

    size_t mReadBytes = 0;
    size_t mWrittenBytes = 0;
    size_t mIOSize = 0;
    bool mDirectIO = false;
    bool mSyncWrites = false;

    bool mIsDir = false;

    std::shared_ptr<ILogger> mLogger;
};

class CPFilePairMgr
{
public:
    // make sure at lease we got one elem in mFilePairs
    CPFilePairMgr(const RWCombinedCopyOptions &options,
                  std::shared_ptr<ILogger> logger)
        : m_options(options), mLogger(logger)
    {
        mReadPtr = mFilePairs.begin();
    }

    tl::expected<void, StackError> AddFilePair(std::string_view src_path, std::string_view dst_path)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mFilePairs.push_back(
            std::make_shared<CPFilePair>(src_path,
                                         dst_path,
                                         m_options.IoSize,
                                         m_options.DirectIO,
                                         m_options.SyncWrites,
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
        auto should = (mStopFlag.load() && mReadPtr == mFilePairs.end() && mStartFlag.load() && mFilePairs.empty() && mInflightFPs.Empty());
        if (!should)
        {
            mLogger->debug("ShouldStopCopy() == false: StopFlag: {}, ReadPtr at end: {}, StartFlag: {}, FilePairs empty: {}, InflightFPs empty: {}",
                           mStopFlag.load(),
                           (mReadPtr == mFilePairs.end()) ? "true" : "false",
                           mStartFlag.load(),
                           mFilePairs.empty() ? "true" : "false",
                           mInflightFPs.Empty() ? "true" : "false");
        }
        return should;
    }

private:
    void CheckReadCompleteNoLock(std::shared_ptr<CPFilePair> pFP);

private:
    std::list<std::shared_ptr<CPFilePair>> mFilePairs;
    DedupList<CPFilePair> mInflightFPs;

    RWCombinedCopyOptions m_options;
    std::mutex mMutex; // to protect mFilePairs, mReadPtr, mWritePtr in multithreaded scenarios
    std::list<std::shared_ptr<CPFilePair>>::iterator mReadPtr;

    std::atomic_bool mStartFlag = false; // whether file pairs have been filled
    std::atomic_bool mStopFlag = false;

    std::shared_ptr<ILogger> mLogger;
};

// abstract class for IOSlotMgr, use as interface
template <typename SlotType = IOSlot>
class IOSlotMgr
{
public:
    IOSlotMgr(const RWCombinedCopyOptions &options,
              CPFilePairMgr *file_pair_mgr,
              std::shared_ptr<ILogger> logger)
        : m_options(options), mCPFPMgr(file_pair_mgr), mLogger(logger)
    {
        size_t slot_count = options.QueueDepth;
        size_t buf_size = options.IoSize;
        m_slots.reserve(slot_count);

        mLogger->debug("IOSlotMgr created with QueueDepth: {}, IoSize: {}", slot_count, buf_size);

        assert(file_pair_mgr != nullptr);

        for (size_t i = 0; i < slot_count; ++i)
        {
            m_slots.emplace_back(std::make_unique<SlotType>(buf_size, i));
        }
    }
    virtual ~IOSlotMgr() = default;
    SlotType *GetSlot(int id)
    {
        if (id < 0 || static_cast<size_t>(id) >= m_slots.size())
        {
            return nullptr;
        }
        return m_slots[id].get();
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
    void SetFuncDurationStat(std::shared_ptr<FuncDurationStat> stat)
    {
        m_func_duration_stat = stat;
    }

protected:
    tl::expected<void, StackError> CheckStuck()
    {
        // 检查是否只少有一个slot处于ReadSubmitted或者WriteSubmitted状态
        bool isStuck = true;
        for (auto &slot_up : m_slots)
        {
            auto slot = slot_up.get();
            if (slot->GetStatus() == IOSlot::Status::ReadSubmitted || slot->GetStatus() == IOSlot::Status::WriteSubmitted)
            {
                isStuck = false;
                break;
            }
        }

        if (isStuck && !mCPFPMgr->ShouldStopCopy() && !m_options.EnableInotify)
        {
            mLogger->warn("Detected stuck AIO operations.");
            return tl::unexpected(StackError("Detected stuck AIO operations."));
        }

        return {};
    }

    tl::expected<int, StackError> SubmitReads()
    {
        // TODO: change to batch submit later
        int submitted = 0;

        for (auto &slot_up : m_slots)
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
        for (auto &slot_up : m_slots)
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
    void PrtSlots()
    {
        mLogger->warn("Current IOSlot statuses:");
        // 按状态统计slot数量，并打印每个状态slot的数量
        std::map<IOSlot::Status, int> status_count;
        for (auto &slot_up : m_slots)
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
        for (auto &slot_up : m_slots)
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

        if (m_func_duration_stat)
        {
            m_func_duration_stat->AddDuration(func_name, duration);
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
            slot->SetStatus(IOSlot::Status::Init);
        }
        return {};
    };

private:
    virtual tl::expected<void, StackError> Init() = 0;
    virtual void PrepareOneRead(SlotType *slot, off_t offset, std::shared_ptr<CPFilePair> currCPFPIt) = 0;
    virtual void PrepareOneWrite(SlotType *slot) = 0;
    virtual tl::expected<void, StackError> SubmitOneRead(SlotType *slot) = 0;
    virtual tl::expected<void, StackError> SubmitOneWrite(SlotType *slot) = 0;
    virtual tl::expected<void, StackError> IOReap() = 0;

protected:
    std::vector<std::unique_ptr<SlotType>> m_slots;

    RWCombinedCopyOptions m_options;
    CPFilePairMgr *mCPFPMgr;

    std::shared_ptr<ILogger> mLogger;
    std::shared_ptr<FuncDurationStat> m_func_duration_stat;
};