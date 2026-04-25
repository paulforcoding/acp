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

// Linux libaio 后端：基于 io_setup / io_submit / io_getevents 的内核原生异步 IO
// 与 io_uring 不同，libaio 的提交与收割是分离的系统调用，需显式维护 iocb 数组
class AIOSlotMgr : public IOSlotMgr<IOSlot>
{
public:
    AIOSlotMgr(const RWCombinedCopyOptions &options, CPFilePairMgr *file_pair_mgr,
               std::shared_ptr<ILogger> logger, FileLogReporter *reporter)
        : IOSlotMgr<IOSlot>(options, file_pair_mgr, logger, reporter)
    {
    }
    ~AIOSlotMgr() override
    {
        if (mIoCtx != 0)
        {
            io_destroy(mIoCtx);
        }
    }

private:
    // 实现 IOSlotMgr 的纯虚接口，适配 libaio 的 iocb / io_event 模型
    tl::expected<void, StackError> Init() override;
    void DoPrepareOneRead(IOSlot *slot, int fd, void *buf, size_t ioSize, off_t offset) override;
    void PrepareOneWrite(IOSlot *slot) override;
    tl::expected<int, StackError> SubmitBatchRead(std::vector<IOSlot *> &slots) override;
    tl::expected<int, StackError> SubmitBatchWrite(std::vector<IOSlot *> &slots) override;
    tl::expected<void, StackError> IOReap() override;
    // tl::expected<void, StackError> CheckOneCompleted(AIOSlot *slot) override;

    // reap helpers
    tl::expected<void, StackError> ReapRead(struct io_event *ev);
    tl::expected<void, StackError> ReapWrite(struct io_event *ev);

private:
    io_context_t mIoCtx = 0;
};
