#pragma once

#include "lib/combined/combined.hpp"

#ifdef __APPLE__

#include <dispatch/dispatch.h>
#include <mutex>
#include <condition_variable>
#include <queue>

// macOS GCD 后端：利用 dispatch_group_async 在线程池上执行 pread/pwrite 模拟异步 IO
// 无内核原生异步 IO 支持，因此不需要显式 SubmitBatch，提交即派发；通过线程安全队列收割完成事件
class GCDSlotMgr : public IOSlotMgr<IOSlot>
{
public:
    GCDSlotMgr(const RWCombinedCopyOptions &options,
               CPFilePairMgr *filePairMgr,
               std::shared_ptr<ILogger> logger,
               FileLogReporter *reporter)
        : IOSlotMgr<IOSlot>(options, filePairMgr, logger, reporter)
    {
    }
    ~GCDSlotMgr() override
    {
        if (mGroup != nullptr)
        {
            dispatch_group_wait(mGroup, DISPATCH_TIME_FOREVER);
            dispatch_release(mGroup);
        }
    }

private:
    tl::expected<void, StackError> Init() override
    {
        mQueue = dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
        mGroup = dispatch_group_create();
        return {};
    }

    void DoPrepareOneRead(IOSlot *slot, int fd, void *buf, size_t ioSize, off_t offset) override
    {
        // GCD 无 iocb 概念，仅保存 fd，实际的 buf/ioSize/offset 在 SubmitBatchRead 时从 slot 重新获取
        // 这样 SubmitBatchRead 无需区分 rw 与 cksum 的传参差异
        slot->SetReadFd(fd);
        (void)buf;
        (void)ioSize;
        (void)offset;
    }

    void PrepareOneWrite(IOSlot *slot) override
    {
        auto currCPFPIt = slot->GetCPFPPtr();
        auto [offset, ioSize] = slot->GetIOInfo();
        mLogger->debug("Preparing write IO for slot ID: {}, offset: {}, io_size: {}, src: {}, dst: {}",
                       slot->GetID(), offset, ioSize, currCPFPIt->GetSrcPath(), currCPFPIt->GetDstPath());
        slot->SetStatus(IOSlot::Status::WritePrepared);
    }

    tl::expected<int, StackError> SubmitBatchRead(std::vector<IOSlot*> &slots) override
    {
        for (auto *slot : slots)
        {
            auto ioInfo = slot->GetIOInfo();
            off_t offset = ioInfo.offset;
            size_t ioSize = ioInfo.io_size;
            int fd = slot->GetReadFd();
            void *buf = slot->GetBuf();

            // 每次 dispatch_group_async 即将任务加入全局并发队列，提交即执行，无需显式 flush
            // 与 libaio/io_uring 的批量提交不同，GCD 的“批量”只是连续派发多个 block
            dispatch_group_async(mGroup, mQueue, ^{
                ssize_t ret = pread(fd, buf, ioSize, offset);
                if (ret < 0)
                    ret = -errno;
                EnqueueCompletion(slot, ret, true);
            });

            slot->SetStatus(IOSlot::Status::ReadSubmitted);
        }
        return static_cast<int>(slots.size());
    }

    tl::expected<int, StackError> SubmitBatchWrite(std::vector<IOSlot*> &slots) override
    {
        for (auto *slot : slots)
        {
            auto ioInfo = slot->GetIOInfo();
            off_t offset = ioInfo.offset;
            size_t ioSize = ioInfo.io_size;
            int fd = slot->GetCPFPPtr()->GetDstFd();
            void *buf = slot->GetBuf();

            // 写派发与读对称：block 内同步执行 pwrite，完成后将结果推入线程安全队列
            dispatch_group_async(mGroup, mQueue, ^{
                ssize_t ret = pwrite(fd, buf, ioSize, offset);
                if (ret < 0)
                    ret = -errno;
                EnqueueCompletion(slot, ret, false);
            });

            slot->SetStatus(IOSlot::Status::WriteSubmitted);
        }
        return static_cast<int>(slots.size());
    }

    tl::expected<void, StackError> IOReap() override
    {
        std::vector<CompletionEvent> events;
        {
            // 使用条件变量 + 互斥锁实现线程安全的完成队列，与 libaio/io_uring 的内核完成事件不同
            // 超时由 IOReapWait 控制，避免空转；若超时无事件则返回空，由上层继续循环
            std::unique_lock<std::mutex> lock(mQueueMutex);
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(mOptions.IOReapWait);
            mQueueCV.wait_until(lock, deadline, [this] {
                return !mCompletionQueue.empty();
            });

            while (!mCompletionQueue.empty())
            {
                events.push_back(mCompletionQueue.front());
                mCompletionQueue.pop();
            }
        }

        for (auto &ev : events)
        {
            if (ev.isRead)
            {
                auto readRes = HandleReadCompletion(ev.slot, ev.result);
                if (!readRes)
                {
                    // GCD 后端目前仅记录日志，不通过 tl::unexpected 中断 RunQueue，与 Linux 后端的错误传播策略不同
                    mLogger->error("HandleReadCompletion failed: {}", readRes.error().ToString());
                }
            }
            else
            {
                auto writeRes = HandleWriteCompletion(ev.slot, ev.result);
                if (!writeRes)
                {
                    mLogger->error("HandleWriteCompletion failed: {}", writeRes.error().ToString());
                }
            }
        }
        return {};
    }

    void EnqueueCompletion(IOSlot *slot, ssize_t result, bool isRead)
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mCompletionQueue.push({slot, result, isRead});
        mQueueCV.notify_one();
    }

private:
    dispatch_queue_t mQueue = nullptr;
    dispatch_group_t mGroup = nullptr;

    struct CompletionEvent
    {
        IOSlot *slot;
        ssize_t result;
        bool isRead;
    };
    std::queue<CompletionEvent> mCompletionQueue;
    std::mutex mQueueMutex;
    std::condition_variable mQueueCV;
};

#endif // __APPLE__
