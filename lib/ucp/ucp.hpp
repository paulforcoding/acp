#pragma once

#include <liburing.h>
#include <stddef.h>
#include <vector>
#include <tl/expected.hpp>
#include <string>
#include <memory>
#include "base/base.hpp"
#include "lib/combined/combined.hpp"
#include "base/chan.hpp"

// Linux io_uring 后端：通过单一 ring 提交与收割，减少系统调用次数
// 支持 SQPOLL 等高级模式（当前使用默认 0 flags），peek_cqe 实现非阻塞轮询
class UIOSlotMgr : public IOSlotMgr<IOSlot>
{
public:
    UIOSlotMgr(const RWCombinedCopyOptions &options,
               CPFilePairMgr *file_pair_mgr, std::shared_ptr<ILogger> logger,
               FileLogReporter *reporter)
        : IOSlotMgr<IOSlot>(options, file_pair_mgr, logger, reporter)
    {
    }
    ~UIOSlotMgr() override
    {
        io_uring_queue_exit(&mRing);
    }

private:
    // 实现 IOSlotMgr 的纯虚接口，适配 io_uring 的 sqe / cqe 模型
    tl::expected<void, StackError> Init() override;
    void DoPrepareOneRead(IOSlot *slot, int fd, void *buf, size_t ioSize, off_t offset) override;
    void PrepareOneWrite(IOSlot *slot) override;
    tl::expected<int, StackError> SubmitBatchRead(std::vector<IOSlot*> &slots) override;
    tl::expected<int, StackError> SubmitBatchWrite(std::vector<IOSlot*> &slots) override;
    tl::expected<void, StackError> IOReap() override;
    tl::expected<void, StackError> ReapRead(IOSlot *slot, io_uring_cqe *cqe);
    tl::expected<void, StackError> ReapWrite(IOSlot *slot, io_uring_cqe *cqe);

private:
    struct io_uring mRing;
};
