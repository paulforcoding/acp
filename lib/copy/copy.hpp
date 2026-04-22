#pragma once
#include "base/logger.hpp"
#include "base/event_reporter.hpp"
#ifdef __APPLE__
#include "lib/gcd/gcd.hpp"
#else
#include "lib/acp/acp.hpp"
#ifdef ENABLE_LIBURING
#include "lib/ucp/ucp.hpp"
#endif
#endif

class CopyEngine
{
public:
    CopyEngine(const RWCombinedCopyOptions &options,
               std::shared_ptr<ILogger> logger,
               std::shared_ptr<FuncDurationStat> funcDurationStat,
               FileLogReporter *reporter)
        : mOptions(options),
          mLogger(logger),
          mFuncDurationStat(funcDurationStat),
          mReporter(reporter) {};

    tl::expected<void, StackError> RunChannel(Channel<CopyEntry> &channel)
    {
        // 根据options.CopyParallelism启动多个RunQueue线程
        mLogger->debug("CopyEngine: Starting {} RunQueue threads.", mOptions.CopyParallelism);
        std::vector<std::thread> threads;
        threads.reserve(mOptions.CopyParallelism);
        std::vector<std::unique_ptr<CPFilePairMgr>> cpfpMgrs;
        cpfpMgrs.reserve(mOptions.CopyParallelism);

        for (int i = 0; i < mOptions.CopyParallelism; ++i)
        {
            cpfpMgrs.emplace_back(std::make_unique<CPFilePairMgr>(mOptions, mLogger, mReporter));
            threads.emplace_back(&CopyEngine::startCopyThread, this, cpfpMgrs.back().get());
        }
        // 主线程负责从channel中取出CopyEntry，分发到各个CPFilePairMgr中
        size_t round_robin_idx = 0;
        while (true)
        {
            auto pop_res = channel.Pop();
            if (!pop_res)
            {
                mLogger->debug("CopyEngine.RunChannel(): finishing pop file pairs from CopyEngine.");
                break; // exit loop
            }
            auto copy_entry = std::move(pop_res.value());
            mLogger->debug("CopyEngine.RunChannel(): Adding file pair: src: {}, dst: {}",
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
        mLogger->debug("CopyEngine: All file pairs added, signaling stop to CPFilePairMgrs.");
        for (auto &cpfpMgr : cpfpMgrs)
        {
            cpfpMgr->SetStopFlag();
        }

        mLogger->debug("CopyEngine: Waiting for RunQueue thread to finish...");
        for (auto &t : threads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        mLogger->debug("CopyEngine: All RunQueue threads have finished.");

        return {};
    }

private:
    void startCopyThread(CPFilePairMgr *cpfpMgr)
    {
        std::unique_ptr<IOSlotMgr<IOSlot>> slotMgr;
#ifdef __APPLE__
        slotMgr = std::make_unique<GCDSlotMgr>(mOptions, cpfpMgr, mLogger, mReporter);
#else
        if (mOptions.CopyEngine == "liburing")
        {
#ifdef ENABLE_LIBURING
            slotMgr = std::make_unique<UIOSlotMgr>(mOptions, cpfpMgr, mLogger, mReporter);
#else
            mLogger->error("liburing support not compiled in, falling back to libaio");
            slotMgr = std::make_unique<AIOSlotMgr>(mOptions, cpfpMgr, mLogger, mReporter);
#endif
        }
        else
        {
            slotMgr = std::make_unique<AIOSlotMgr>(mOptions, cpfpMgr, mLogger, mReporter);
        }
#endif

        slotMgr->SetFuncDurationStat(mFuncDurationStat);
        auto run_res = slotMgr->RunQueue();
        if (!run_res)
        {
            mLogger->error("CopyEngine::RunChannel: RunQueue() failed, err: {}", run_res.error().ToString());
        }
    };

private:
    RWCombinedCopyOptions mOptions;
    std::shared_ptr<ILogger> mLogger;
    std::shared_ptr<FuncDurationStat> mFuncDurationStat;
    FileLogReporter *mReporter = nullptr;
};