#pragma once

#include <any>
#include <cstddef> // size_t
#include <tl/expected.hpp>
#include "base/base.hpp"

struct CopyEntry
{
    std::string srcPath; // full path
    std::string dstPath;
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
        : m_buf{zplib::AllocBytes(SECTORSIZE, buf_size)}, m_status{Status::Init}, m_id{id}
    {
    }
    // dtor
    virtual ~IOSlot() { zplib::FreeBytes(m_buf); }

    virtual void Reset()
    {
        m_status = Status::Init;
        // bzero(m_buf, SECTORSIZE);
        m_user_data = {};
    }
    // getters
    char *GetBuf() const { return m_buf; }
    Status GetStatus() const { return m_status; }
    int GetID() const { return m_id; }
    std::any GetUserData() const { return m_user_data; }
    // setters
    void SetStatus(Status s) { m_status = s; }
    void SetUserData(const std::any &data) { m_user_data = data; }

private:
    // data fields
    char *m_buf = nullptr;
    Status m_status = Status::Init;
    int m_id = -1;        // its id, also index in IOSlotMgr's m_slots
    std::any m_user_data; // user data field
};

struct RWCombinedCopyOptions
{
    std::string LogLevel;
    std::string LogMode;
    std::string LogFilePath;
    std::string CopyEngine;
    int CopyParallelism;
    bool CopyDirMTime;
    bool FullCopyBeforeInotify;
    bool PreserveSparseFiles;
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
    CPFilePair(std::string_view src_path, std::string_view dst_path, size_t io_size); // full path expected
    ~CPFilePair()
    {
        if (m_src_fd >= 0)
        {
            close(m_src_fd);
            m_src_fd = -1;
        }
        if (m_dst_fd >= 0)
        {
            close(m_dst_fd);
            m_dst_fd = -1;
        }
    }
    tl::expected<void, zplib::StackError> CheckAndInit();
    tl::expected<void, zplib::StackError> TrucateDstToSrcSize()
    {
        if (ftruncate(m_dst_fd, m_src_stat.st_size) < 0)
        {
            return tl::unexpected(zplib::StackError(
                fmt::format("Failed to truncate destination file: {}, errno: {}, errstr: {}", m_dst_path, errno, strerror(errno))));
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
        m_logger->debug("Checking IsWriteFinished: written_bytes: {}, src_file_size: {}, src: {}",
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

    bool mIsDir = false;

    std::shared_ptr<spdlog::logger> m_logger = zplib::GetGlobalLogger();
};

class CPFilePairMgr
{
public:
    // make sure at lease we got one elem in mFilePairs
    CPFilePairMgr(size_t ioSize) : mIOSize(ioSize)
    {
        mReadPtr = mFilePairs.begin();
    }

    tl::expected<void, zplib::StackError> AddFilePair(std::string_view src_path, std::string_view dst_path)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mFilePairs.push_back(std::make_shared<CPFilePair>(src_path, dst_path, mIOSize));
        if (mFilePairs.size() == 1 && !mStartFlag.load()) // 下面的动作只在第一个文件对加入时执行
        {
            mReadPtr = mFilePairs.begin();
            mStartFlag.store(true);
        }

        return {};
    }

    tl::expected<std::shared_ptr<CPFilePair>, zplib::StackError> GetNextReadIO();

    tl::expected<void, zplib::StackError> CheckWriteComplete(std::shared_ptr<CPFilePair> writeIt);
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
        auto should = (mStopFlag.load() && mReadPtr == mFilePairs.end() && mStartFlag.load() && mFilePairs.empty() && mInflightFPs.empty());
        if (!should)
        {
            m_logger->debug("ShouldStopCopy() == false: StopFlag: {}, ReadPtr at end: {}, StartFlag: {}, FilePairs empty: {}, InflightFPs empty: {}",
                            mStopFlag.load(),
                            (mReadPtr == mFilePairs.end()) ? "true" : "false",
                            mStartFlag.load(),
                            mFilePairs.empty() ? "true" : "false",
                            mInflightFPs.empty() ? "true" : "false");
        }
        return should;
    }

private:
    std::list<std::shared_ptr<CPFilePair>> mFilePairs;
    std::list<std::shared_ptr<CPFilePair>> mInflightFPs;

    size_t mIOSize = 0;
    std::mutex mMutex; // to protect mFilePairs, mReadPtr, mWritePtr in multithreaded scenarios
    std::list<std::shared_ptr<CPFilePair>>::iterator mReadPtr;

    std::atomic_bool mStartFlag = false; // whether file pairs have been filled
    std::atomic_bool mStopFlag = false;

    std::shared_ptr<spdlog::logger> m_logger = zplib::GetGlobalLogger();
};