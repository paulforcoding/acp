#pragma once

#include "lib/combined/combined.hpp"

#ifdef __APPLE__

#include <dispatch/dispatch.h>
#include <mutex>
#include <condition_variable>
#include <queue>

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
        // GCD 不需要类似 libaio 的前置准备，所有数据在 Submit 时直接传递
        (void)slot;
        (void)fd;
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

    tl::expected<void, StackError> SubmitOneRead(IOSlot *slot) override
    {
        auto ioInfo = slot->GetIOInfo();
        off_t offset = ioInfo.offset;
        size_t ioSize = ioInfo.io_size;
        int fd = slot->GetCPFPPtr()->GetSrcFd();
        void *buf = slot->GetBuf();

        dispatch_group_async(mGroup, mQueue, ^{
            ssize_t ret = pread(fd, buf, ioSize, offset);
            if (ret < 0)
                ret = -errno;
            EnqueueCompletion(slot, ret, true);
        });

        slot->SetStatus(IOSlot::Status::ReadSubmitted);
        return {};
    }

    tl::expected<void, StackError> SubmitOneWrite(IOSlot *slot) override
    {
        auto ioInfo = slot->GetIOInfo();
        off_t offset = ioInfo.offset;
        size_t ioSize = ioInfo.io_size;
        int fd = slot->GetCPFPPtr()->GetDstFd();
        void *buf = slot->GetBuf();

        dispatch_group_async(mGroup, mQueue, ^{
            ssize_t ret = pwrite(fd, buf, ioSize, offset);
            if (ret < 0)
                ret = -errno;
            EnqueueCompletion(slot, ret, false);
        });

        slot->SetStatus(IOSlot::Status::WriteSubmitted);
        return {};
    }

    tl::expected<void, StackError> IOReap() override
    {
        std::vector<CompletionEvent> events;
        {
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
