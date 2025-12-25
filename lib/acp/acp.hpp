#pragma once

#include <cstring> // bzero
#include <libaio.h>
#include <stddef.h> // size_t
#include <vector>
#include <any>
#include <tl/expected.hpp>
#include <sys/stat.h>
#include <fcntl.h>  // open
#include <unistd.h> // close
#include <string>
#include <list>
#include "base/base.hpp"
#include "lib/combined/combined.hpp"
#include "base/chan.hpp"

class AIOSlot : public IOSlot
{
public:
    using IOSlot::IOSlot; // 继承构造函数
    AIOSlot(size_t buf_size, int id)
        : IOSlot(buf_size, id)
    {
    }
    ~AIOSlot() override = default;

    // getters for iocb
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

    std::shared_ptr<CPFilePair> GetCPFPPtr() { return mCPFPIt; }
    void SetCPFPPtr(std::shared_ptr<CPFilePair> it) { mCPFPIt = it; }

private:
    struct iocb m_iocb_read;  // 读iocb
    struct iocb m_iocb_write; // 写iocb
    std::shared_ptr<CPFilePair> mCPFPIt;
};

class AIOSlotMgr
{
public:
    AIOSlotMgr(const RWCombinedCopyOptions &options, CPFilePairMgr *file_pair_mgr)
        : m_options(options), mCPFPMgr(file_pair_mgr), m_logger(zplib::GetGlobalLogger())
    {
        size_t slot_count = options.QueueDepth;
        size_t buf_size = options.IoSize;
        m_slots.reserve(slot_count);

        m_logger->debug("AIOSlotMgr created with QueueDepth: {}, IoSize: {}", slot_count, buf_size);

        for (size_t i = 0; i < slot_count; ++i)
        {
            m_slots.push_back(new AIOSlot(buf_size, i));
        }
    }
    ~AIOSlotMgr()
    {
        for (auto slot : m_slots)
        {
            delete slot;
        }
        m_slots.clear();
        io_destroy(m_io_ctx);
    }
    IOSlot *GetSlot(int id)
    {
        if (id < 0 || static_cast<size_t>(id) >= m_slots.size())
        {
            return nullptr;
        }
        return m_slots[id];
    }

    tl::expected<void, zplib::StackError> RunCopyQueue();
    tl::expected<void, zplib::StackError> CheckStuck();
    void SetFuncDurationStat(zplib::FuncDurationStat *stat)
    {
        m_func_duration_stat = stat;
    }

private:
    tl::expected<void, zplib::StackError> Init();
    void PrepareOneRead(AIOSlot *slot, off_t offset, std::shared_ptr<CPFilePair> currCPFPIt);
    void PrepareOneWrite(AIOSlot *slot, off_t offset);
    tl::expected<void, zplib::StackError> SubmitOneRead(AIOSlot *slot);
    tl::expected<void, zplib::StackError> SubmitOneWrite(AIOSlot *slot);
    tl::expected<int, zplib::StackError> SubmitReads();
    tl::expected<int, zplib::StackError> SubmitWrites();
    tl::expected<void, zplib::StackError> IOReap();
    tl::expected<void, zplib::StackError> ReapRead(struct io_event *ev);
    tl::expected<void, zplib::StackError> ReapWrite(struct io_event *ev);
    tl::expected<void, zplib::StackError> CheckOneCompleted(AIOSlot *slot);
    tl::expected<void, zplib::StackError> CheckCompleteds();
    void PrtSlots();
    void Reset()
    {
        for (auto slot : m_slots)
        {
            slot->Reset();
        }
    }

    void AddDurationWarn(std::string_view func_name, std::chrono::_V2::system_clock::time_point start,
                         std::chrono::_V2::system_clock::time_point end, int64_t warn_threshold);

private:
    std::vector<AIOSlot *> m_slots;
    io_context_t m_io_ctx = 0;

    RWCombinedCopyOptions m_options;
    CPFilePairMgr *mCPFPMgr;

    std::shared_ptr<spdlog::logger> m_logger;
    zplib::FuncDurationStat *m_func_duration_stat;

    // flow control
    int64_t last_read_submit_duration = 0;
    int64_t last_write_submit_duration = 0;
};

class AIOFileCopy
{
public:
    AIOFileCopy(const RWCombinedCopyOptions &options) : m_options(options) {};

    tl::expected<void, zplib::StackError> RunChannel(zplib::FuncDurationStat *stat,
                                                     Channel<CopyEntry> &channel)
    {
        CPFilePairMgr cpfpMgr(m_options.IoSize);
        auto sMgr = AIOSlotMgr(m_options, &cpfpMgr);
        sMgr.SetFuncDurationStat(stat);

        // 启动RunCopyQueue线程
        std::thread th([&sMgr, &cpfpMgr, this]()
                       {
        auto run_res = sMgr.RunCopyQueue();
        if (!run_res)
        {
            m_logger->error("AIOFileCopy::RunChannel: RunCopyQueue() failed, err: {}", run_res.error().what());
        } });

        while (true)
        {
            auto pop_res = channel.Pop();
            if (!pop_res)
            {
                m_logger->debug("Channel is closed or empty, finishing adding file pairs.");
                break; // exit loop
            }
            auto copy_entry = std::move(pop_res.value());
            m_logger->debug("AIOFileCopy: Adding file pair: src: {}, dst: {}",
                            copy_entry->srcPath, copy_entry->dstPath);
            auto add_file_pair_res = cpfpMgr.AddFilePair(copy_entry->srcPath, copy_entry->dstPath);
            if (!add_file_pair_res)
            {
                return tl::unexpected(zplib::StackError("cpfpMgr.AddFilePair(), err: ", add_file_pair_res.error()));
            }
        }

        cpfpMgr.SetStopFlag();

        m_logger->debug("AIOFileCopy: Waiting for RunCopyQueue thread to finish...");
        th.join();

        return {};
    }

private:
    RWCombinedCopyOptions m_options;
    std::shared_ptr<spdlog::logger> m_logger = zplib::GetGlobalLogger();
};