#include "lib/acp/acp.hpp"

#include <memory> // std::make_unique
// #include <linux/aio_abi.h> // don't include this, it may cause conflict with libaio
#include <linux/fs.h> // RWF_NOWAIT

tl::expected<void, StackError> AIOSlotMgr::Init()
{
    io_context_t ctx = 0;
    // 根据 RW slot 与校验和 slot 总数创建 aio context，决定内核队列容量
    int ret = io_setup(static_cast<unsigned>(mRWSlots.size() + mCksumSlots.size()), &ctx);
    if (ret < 0)
    {
        return tl::unexpected(StackError("io_setup() failed, errno: " + std::to_string(-ret)));
    }
    mIoCtx = ctx;

    return {};
}

void AIOSlotMgr::DoPrepareOneRead(IOSlot *slot, int fd, void *buf, size_t ioSize, off_t offset)
{
    auto iocb{slot->InitReadIOCB()};
    io_prep_pread(iocb, fd, buf, ioSize, offset);
    // 将 slot 指针绑定到 iocb->data，以便 io_getevents 返回时通过 io_event.data 还原上下文
    iocb->data = slot; // associate slot with this iocb
    // iocb->aio_rw_flags |= RWF_NOWAIT;

    return;
}
void AIOSlotMgr::PrepareOneWrite(IOSlot *slot)
{
    auto iocb{slot->InitWriteIOCB()};
    auto currCPFPIt = slot->GetCPFPPtr();

    auto [offset, ioSize] = slot->GetIOInfo();
    // DirectIO 要求写入大小与偏移均为扇区对齐；若启用则统一按 IoSize 写入，尾部由上层截断对齐
    if (mOptions.DirectIO)
    {
        ioSize = mOptions.IoSize;
    }
    io_prep_pwrite(iocb, currCPFPIt->GetDstFd(), slot->GetBuf(), ioSize,
                   offset);
    mLogger->debug("Preparing write IO for slot ID: {}, offset: {}, io_size: {}, src: {}, dst: {}",
                   slot->GetID(), offset, ioSize, currCPFPIt->GetSrcPath(), currCPFPIt->GetDstPath());

    iocb->data = slot;
    // iocb->aio_rw_flags |= RWF_NOWAIT;
    slot->SetStatus(IOSlot::Status::WritePrepared);
    return;
}

tl::expected<int, StackError> AIOSlotMgr::SubmitBatchRead(std::vector<IOSlot *> &slots)
{
    if (slots.empty()) return 0;

    // 将各 slot 的 iocb 指针收集为数组，满足 io_submit 的批量提交接口
    std::vector<struct iocb *> iocbs;
    iocbs.reserve(slots.size());
    for (auto *slot : slots)
    {
        iocbs.push_back(slot->GetReadIOCB());
    }

#ifndef NDEBUG
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_submit(mIoCtx, static_cast<int>(iocbs.size()), iocbs.data());
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_submit(read)", start, end);
#else
    int ret = io_submit(mIoCtx, static_cast<int>(iocbs.size()), iocbs.data());
#endif

    if (ret < 0)
    {
        // EAGAIN 表示内核请求队列已满，需由上层退避重试，而非致命错误
        if (ret == -EAGAIN)
        {
            mLogger->warn("io_submit() for read batch got EAGAIN, will try later.");
            return tl::unexpected(StackError::FromErrno(-EAGAIN));
        }
        return tl::unexpected(StackError(
            fmt::format("io_submit() failed, errno: {}, errstr: {}", -ret, strerror(-ret))));
    }

    for (int i = 0; i < ret; ++i)
    {
        slots[i]->SetStatus(IOSlot::Status::ReadSubmitted);
    }
    return ret;
}

tl::expected<int, StackError> AIOSlotMgr::SubmitBatchWrite(std::vector<IOSlot *> &slots)
{
    if (slots.empty()) return 0;

    // 写入批量提交流程与读完全一致：聚合 iocb 后一次 io_submit，减少系统调用次数
    std::vector<struct iocb *> iocbs;
    iocbs.reserve(slots.size());
    for (auto *slot : slots)
    {
        iocbs.push_back(slot->GetWriteIOCB());
    }

#ifndef NDEBUG
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_submit(mIoCtx, static_cast<int>(iocbs.size()), iocbs.data());
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_submit(write)", start, end);
#else
    int ret = io_submit(mIoCtx, static_cast<int>(iocbs.size()), iocbs.data());
#endif

    if (ret < 0)
    {
        // 内核队列满时同样返回 EAGAIN，由上层 IOReap 收割后重试
        if (ret == -EAGAIN)
        {
            mLogger->warn("io_submit() for write batch got EAGAIN, will try later.");
            return tl::unexpected(StackError::FromErrno(-EAGAIN));
        }
        return tl::unexpected(StackError(
            fmt::format("io_submit() failed, errno: {}, errstr: {}", -ret, strerror(-ret))));
    }

    for (int i = 0; i < ret; ++i)
    {
        slots[i]->SetStatus(IOSlot::Status::WriteSubmitted);
    }
    return ret;
}

// 收割读 IO 并触发写提交；通过 io_event.data 还原 slot 上下文
tl::expected<void, StackError> AIOSlotMgr::ReapRead(struct io_event *ev)
{

    ssize_t io_ret = ev->res;
    IOSlot *slot = static_cast<IOSlot *>(ev->data);
    assert(slot->GetStatus() == IOSlot::Status::ReadSubmitted);
    return HandleReadCompletion(slot, io_ret);
}

tl::expected<void, StackError> AIOSlotMgr::ReapWrite(struct io_event *ev)
{

    ssize_t io_ret = ev->res;
    IOSlot *slot = static_cast<IOSlot *>(ev->data);
    assert(slot->GetStatus() == IOSlot::Status::WriteSubmitted);

    return HandleWriteCompletion(slot, io_ret);
}

tl::expected<void, StackError> AIOSlotMgr::IOReap()
{
    const int max_events = static_cast<int>(mRWSlots.size());
    std::vector<struct io_event> events(max_events);
    struct timespec timeout;
    timeout.tv_sec = mOptions.IOReapWait;
    timeout.tv_nsec = 0;

#ifndef NDEBUG
    // 打印io_getevents()所用时间
    auto start = std::chrono::high_resolution_clock::now();
    // 至少等待 1 个事件完成，最多收割 max_events 个；超时由 IOReapWait 控制，避免空转
    int ret = io_getevents(mIoCtx, 1, max_events, events.data(),
                           &timeout);
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_getevents()", start, end); // warn if >10ms
#else
    int ret = io_getevents(mIoCtx, 1, max_events, events.data(), &timeout);
#endif
    if (ret < 0)
    {
        return tl::unexpected(StackError("io_getevents() failed, errno: " + std::to_string(-ret)));
    }
    mLogger->debug("Reaped {} IO events.", ret);
    if (ret == 0)
    {
        return {}; // no events to reap, just wait for next round
    }

    for (int i = 0; i < ret; ++i)
    {
        struct io_event &event = events[i];
        IOSlot *slot = static_cast<IOSlot *>(event.data);
        if (slot->GetStatus() == IOSlot::Status::ReadSubmitted)
        {

            auto reap_read_res = ReapRead(&event);
            if (!reap_read_res)
            {
                return tl::unexpected(reap_read_res.error());
            }
        }
        else if (slot->GetStatus() == IOSlot::Status::WriteSubmitted)
        {
            auto reap_write_res = ReapWrite(&event);
            if (!reap_write_res)
            {
                return tl::unexpected(reap_write_res.error());
            }
        }
    }

    return {};
}
