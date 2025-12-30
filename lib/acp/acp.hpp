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
        : m_options(options), mCPFPMgr(file_pair_mgr), m_logger(GetGlobalLogger())
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

    tl::expected<void, StackError> RunCopyQueue();
    tl::expected<void, StackError> CheckStuck();
    void SetFuncDurationStat(FuncDurationStat *stat)
    {
        m_func_duration_stat = stat;
    }

private:
    tl::expected<void, StackError> Init();
    void PrepareOneRead(AIOSlot *slot, off_t offset, std::shared_ptr<CPFilePair> currCPFPIt);
    void PrepareOneWrite(AIOSlot *slot, off_t offset);
    tl::expected<void, StackError> SubmitOneRead(AIOSlot *slot);
    tl::expected<void, StackError> SubmitOneWrite(AIOSlot *slot);
    tl::expected<int, StackError> SubmitReads();
    tl::expected<int, StackError> SubmitWrites();
    tl::expected<void, StackError> IOReap();
    tl::expected<void, StackError> ReapRead(struct io_event *ev);
    tl::expected<void, StackError> ReapWrite(struct io_event *ev);
    tl::expected<void, StackError> CheckOneCompleted(AIOSlot *slot);
    tl::expected<void, StackError> CheckCompleteds();
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
    FuncDurationStat *m_func_duration_stat;

    // flow control
    int64_t last_read_submit_duration = 0;
    int64_t last_write_submit_duration = 0;
};

class AIOFileCopy
{
public:
    AIOFileCopy(const RWCombinedCopyOptions &options) : m_options(options) {};

    tl::expected<void, StackError> RunChannel(FuncDurationStat *stat,
                                              Channel<CopyEntry> &channel)
    {
        // 根据options.CopyParallelism启动多个RunCopyQueue线程
        m_logger->debug("AIOFileCopy: Starting {} RunCopyQueue threads.", m_options.CopyParallelism);
        std::vector<std::thread> threads;
        threads.reserve(m_options.CopyParallelism);
        std::vector<std::unique_ptr<CPFilePairMgr>> cpfpMgrs;
        cpfpMgrs.reserve(m_options.CopyParallelism);

        for (int i = 0; i < m_options.CopyParallelism; ++i)
        {
            cpfpMgrs.emplace_back(std::make_unique<CPFilePairMgr>(m_options.IoSize));
            threads.emplace_back(&AIOFileCopy::startCopyThread, this, cpfpMgrs.back().get(), stat, m_options);
        }
        // 主线程负责从channel中取出CopyEntry，分发到各个CPFilePairMgr中
        size_t round_robin_idx = 0;
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
            auto &cpfpMgr = cpfpMgrs[round_robin_idx];
            round_robin_idx = (round_robin_idx + 1) % cpfpMgrs.size();
            auto add_file_pair_res = cpfpMgr->AddFilePair(copy_entry->srcPath, copy_entry->dstPath);
            if (!add_file_pair_res)
            {
                return tl::unexpected(StackError("cpfpMgr.AddFilePair(), err: ", add_file_pair_res.error()));
            }
        }
        // 所有文件对添加完毕，通知各个CPFilePairMgr停止
        m_logger->debug("AIOFileCopy: All file pairs added, signaling stop to CPFilePairMgrs.");
        for (auto &cpfpMgr : cpfpMgrs)
        {
            cpfpMgr->SetStopFlag();
        }

        m_logger->debug("AIOFileCopy: Waiting for RunCopyQueue thread to finish...");
        for (auto &t : threads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        m_logger->debug("AIOFileCopy: All RunCopyQueue threads have finished.");

        return {};
    }

private:
    void startCopyThread(CPFilePairMgr *cpfpMgr, FuncDurationStat *stat, RWCombinedCopyOptions options)
    {
        auto sMgr = AIOSlotMgr(options, cpfpMgr);
        sMgr.SetFuncDurationStat(stat);
        auto run_res = sMgr.RunCopyQueue();
        if (!run_res)
        {
            m_logger->error("AIOFileCopy::RunChannel: RunCopyQueue() failed, err: {}", run_res.error().what());
        }
    };

private:
    RWCombinedCopyOptions m_options;
    std::shared_ptr<spdlog::logger> m_logger = GetGlobalLogger();
};