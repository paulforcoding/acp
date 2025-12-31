#pragma once
#include "lib/ucp/ucp.hpp"
#include "lib/acp/acp.hpp"

class CopyEngine
{
public:
    CopyEngine(const RWCombinedCopyOptions &options) : m_options(options) {};

    tl::expected<void, StackError> RunChannel(FuncDurationStat *stat,
                                              Channel<CopyEntry> &channel)
    {
        // 根据options.CopyParallelism启动多个RunCopyQueue线程
        m_logger->debug("CopyEngine: Starting {} RunCopyQueue threads.", m_options.CopyParallelism);
        std::vector<std::thread> threads;
        threads.reserve(m_options.CopyParallelism);
        std::vector<std::unique_ptr<CPFilePairMgr>> cpfpMgrs;
        cpfpMgrs.reserve(m_options.CopyParallelism);

        for (int i = 0; i < m_options.CopyParallelism; ++i)
        {
            cpfpMgrs.emplace_back(std::make_unique<CPFilePairMgr>(m_options));
            threads.emplace_back(&CopyEngine::startCopyThread, this, cpfpMgrs.back().get(), stat, m_options);
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
            m_logger->debug("CopyEngine: Adding file pair: src: {}, dst: {}",
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
        m_logger->debug("CopyEngine: All file pairs added, signaling stop to CPFilePairMgrs.");
        for (auto &cpfpMgr : cpfpMgrs)
        {
            cpfpMgr->SetStopFlag();
        }

        m_logger->debug("CopyEngine: Waiting for RunCopyQueue thread to finish...");
        for (auto &t : threads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        m_logger->debug("CopyEngine: All RunCopyQueue threads have finished.");

        return {};
    }

private:
    void startCopyThread(CPFilePairMgr *cpfpMgr, FuncDurationStat *stat, RWCombinedCopyOptions options)
    {
        // std::variant<std::unique_ptr<UIOSlotMgr>, std::unique_ptr<AIOSlotMgr>> slotMgr;

        // if (options.CopyEngine == "liburing")
        // {
        //     slotMgr = std::make_unique<UIOSlotMgr>(options, cpfpMgr);
        // }
        // else
        // {
        //     slotMgr = std::make_unique<AIOSlotMgr>(options, cpfpMgr);
        // }

        // std::visit([&stat](auto &mgr)
        //            { mgr->SetFuncDurationStat(stat); }, slotMgr);

        // auto run_res = std::visit([](auto &mgr)
        //                           { return mgr->RunCopyQueue(); }, slotMgr);
        // if (!run_res)
        // {
        //     m_logger->error("CopyEngine::RunChannel: RunCopyQueue() failed, err: {}", run_res.error().what());
        // }

        std::unique_ptr<IOSlotMgr<IOSlot>> slotMgr;
        if (options.CopyEngine == "liburing")
        {
            slotMgr = std::make_unique<UIOSlotMgr>(options, cpfpMgr);
        }
        else
        {
            slotMgr = std::make_unique<AIOSlotMgr>(options, cpfpMgr);
        }

        slotMgr->SetFuncDurationStat(stat);
        auto run_res = slotMgr->RunCopyQueue();
        if (!run_res)
        {
            m_logger->error("CopyEngine::RunChannel: RunCopyQueue() failed, err: {}", run_res.error().what());
        }
    };

private:
    RWCombinedCopyOptions m_options;
    std::shared_ptr<spdlog::logger> m_logger = GetGlobalLogger();
};