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

class UIOSlotMgr : public IOSlotMgr<IOSlot>
{
public:
    UIOSlotMgr(const RWCombinedCopyOptions &options,
               CPFilePairMgr *file_pair_mgr, std::shared_ptr<ILogger> logger)
        : IOSlotMgr<IOSlot>(options, file_pair_mgr, logger)
    {
    }
    ~UIOSlotMgr() override
    {
        io_uring_queue_exit(&mRing);
    }

private:
    tl::expected<void, StackError> Init() override;
    void DoPrepareOneRead(IOSlot *slot, int fd, void *buf, size_t ioSize, off_t offset) override;
    void PrepareOneWrite(IOSlot *slot) override;
    tl::expected<void, StackError> SubmitOneRead(IOSlot *slot) override;
    tl::expected<void, StackError> SubmitOneWrite(IOSlot *slot) override;
    tl::expected<void, StackError> IOReap() override;
    tl::expected<void, StackError> ReapRead(IOSlot *slot, io_uring_cqe *cqe);
    tl::expected<void, StackError> ReapWrite(IOSlot *slot, io_uring_cqe *cqe);

private:
    struct io_uring mRing;
};
