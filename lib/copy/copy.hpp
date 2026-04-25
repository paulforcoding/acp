#pragma once
#include <optional>
#include <vector>

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

// CopyEngine: 复制引擎，负责将 CopyEntry 分发给多个工作线程并行复制
// 线程模型：CopyParallelism 个线程各拥有一个 CPFilePairMgr，独立执行 RunQueue
// Channel 分发策略：主线程轮询（round-robin）将文件对均匀分配到各 CPFilePairMgr
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

    // RunChannel: 主线程生命周期 — 启动工作线程 → 轮询分发文件对 → 关闭 Channel → join 线程 → 处理延迟目录元数据
    tl::expected<void, StackError> RunChannel(Channel<CopyEntry> &channel)
    {
        // 根据 CopyParallelism 创建对应数量的 CPFilePairMgr 和线程，每个线程独立执行 RunQueue
        mLogger->debug("CopyEngine: Starting {} RunQueue threads.", mOptions.CopyParallelism);
        std::vector<std::thread> threads;
        threads.reserve(mOptions.CopyParallelism);
        std::vector<std::unique_ptr<CPFilePairMgr>> cpfpMgrs;
        cpfpMgrs.reserve(mOptions.CopyParallelism);
        std::vector<std::optional<StackError>> threadErrors(mOptions.CopyParallelism);

        for (int i = 0; i < mOptions.CopyParallelism; ++i)
        {
            cpfpMgrs.emplace_back(std::make_unique<CPFilePairMgr>(mOptions, mLogger, mReporter));
            threads.emplace_back(&CopyEngine::startCopyThread, this, cpfpMgrs.back().get(), std::ref(threadErrors[i]));
        }

        // 多线程且保留元数据时延迟处理目录：避免并发修改同一目录的权限/时间戳导致竞态
        bool deferDirs = (mOptions.CopyParallelism > 1 && mOptions.PreserveMeta);
        std::vector<std::unique_ptr<CopyEntry>> deferredDirs;
        deferredDirs.reserve(100);

        // 主线程轮询从 Channel 取出 CopyEntry，均匀分发到各 CPFilePairMgr
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

            if (deferDirs && S_ISDIR(copy_entry->srcStat.st_mode))
            {
                mLogger->debug("CopyEngine.RunChannel(): Deferring directory metadata: src: {}, dst: {}",
                               copy_entry->srcPath, copy_entry->dstPath);
                deferredDirs.push_back(std::move(copy_entry));
                continue;
            }

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
        // 所有文件对分发完毕，通知各 CPFilePairMgr 停止；工作线程在排空剩余 I/O 后自然退出
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

        for (auto &err : threadErrors)
        {
            if (err)
            {
                return tl::unexpected(*err);
            }
        }

        // 按 LIFO（最深优先）顺序处理延迟目录，确保子目录元数据先于父目录恢复
        if (deferDirs)
        {
            mLogger->debug("CopyEngine: Processing {} deferred directories.", deferredDirs.size());
            for (auto &entry : deferredDirs)
            {
                CPFilePair cpfp(entry->srcPath,
                                entry->dstPath,
                                false,
                                false,
                                false,
                                mOptions.PreserveMeta,
                                mLogger,
                                mReporter);
                *cpfp.GetSrcStatPtr() = entry->srcStat;
                if (mOptions.PreserveMeta)
                {
                    auto meta_res = cpfp.PreserveMetadata();
                    if (!meta_res)
                    {
                        mLogger->warn("PreserveMetadata failed for deferred dir {}: {}",
                                      entry->srcPath, meta_res.error().ToString());
                    }
                }

                if (mReporter)
                {
                    mReporter->FileStart(entry->srcPath, entry->dstPath, 0);
                    mReporter->FileComplete(entry->srcPath, entry->dstPath, 0, 0);
                    mReporter->IncrementFilesDone();
                }
            }
        }

        return {};
    }

private:
    // 每个工作线程的入口：根据平台/配置选择后端（libaio / liburing / GCD），然后执行 RunQueue 主循环
    void startCopyThread(CPFilePairMgr *cpfpMgr, std::optional<StackError> &outError)
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
            outError = run_res.error();
        }
    };

private:
    RWCombinedCopyOptions mOptions;
    std::shared_ptr<ILogger> mLogger;
    std::shared_ptr<FuncDurationStat> mFuncDurationStat;
    FileLogReporter *mReporter = nullptr;
};