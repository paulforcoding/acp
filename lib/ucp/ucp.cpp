#include "lib/ucp/ucp.hpp"

#include <memory>
#include <fmt/format.h>

tl::expected<void, StackError> UIOSlotMgr::Init()
{
    // 当前 flags 为 0，未启用 IORING_SETUP_SQPOLL；如需内核线程轮询可在此传入对应 flags
    int ret = io_uring_queue_init(static_cast<int>(mRWSlots.size() + mCksumSlots.size()), &mRing, 0);
    if (ret < 0)
    {
        return tl::unexpected(StackError(fmt::format("io_uring_queue_init() failed, errno: {}", -ret)));
    }
    return {};
}

void UIOSlotMgr::DoPrepareOneRead(IOSlot *slot, int fd, void *buf, size_t ioSize, off_t offset)
{
    // 从 ring 的提交队列获取一个 sqe，绑定读参数与 slot 上下文；实际提交延迟到 SubmitBatchRead
    struct io_uring_sqe *sqe = io_uring_get_sqe(&mRing);
    io_uring_prep_read(sqe, fd, buf, ioSize, offset);
    io_uring_sqe_set_data(sqe, slot);
}

void UIOSlotMgr::PrepareOneWrite(IOSlot *slot)
{
    // 写准备与读对称：获取 sqe 后填充写参数，DirectIO 时同样按 IoSize 对齐写入
    struct io_uring_sqe *sqe = io_uring_get_sqe(&mRing);
    auto currCPFPIt = slot->GetCPFPPtr();

    auto [offset, ioSize] = slot->GetIOInfo();
    if (mOptions.DirectIO)
    {
        ioSize = mOptions.IoSize;
    }
    io_uring_prep_write(sqe, currCPFPIt->GetDstFd(), slot->GetBuf(), ioSize, offset);
    mLogger->debug("Preparing write IO for slot ID: {}, offset: {}, io_size: {}, src: {}, dst: {}",
                   slot->GetID(), offset, ioSize, currCPFPIt->GetSrcPath(), currCPFPIt->GetDstPath());

    io_uring_sqe_set_data(sqe, slot);

    slot->SetStatus(IOSlot::Status::WritePrepared);
}

tl::expected<int, StackError> UIOSlotMgr::SubmitBatchRead(std::vector<IOSlot*> &slots)
{
    if (slots.empty()) return 0;

    // io_uring 的批量提交只需一次 io_uring_submit，所有已填充的 sqe 会一次性刷入内核
#ifndef NDEBUG
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_uring_submit(&mRing);
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_uring_submit(read)", start, end);
#else
    int ret = io_uring_submit(&mRing);
#endif

    if (ret < 0)
    {
        // 与 libaio 一致，EAGAIN 表示 ring 满，需收割后再重试
        if (ret == -EAGAIN)
        {
            mLogger->warn("io_uring_submit() for read batch got EAGAIN, will try later.");
            return tl::unexpected(StackError::FromErrno(-EAGAIN));
        }
        return tl::unexpected(StackError(fmt::format("io_uring_submit() failed, errno: {}, errstr: {}", -ret, strerror(-ret))));
    }

    for (auto *slot : slots)
    {
        slot->SetStatus(IOSlot::Status::ReadSubmitted);
    }
    return static_cast<int>(slots.size());
}

tl::expected<int, StackError> UIOSlotMgr::SubmitBatchWrite(std::vector<IOSlot*> &slots)
{
    if (slots.empty()) return 0;

    // 写提交与读提交共用同一个 ring，io_uring_submit 会刷出所有 pending 的 sqe
#ifndef NDEBUG
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_uring_submit(&mRing);
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_uring_submit(write)", start, end);
#else
    int ret = io_uring_submit(&mRing);
#endif

    if (ret < 0)
    {
        if (ret == -EAGAIN)
        {
            mLogger->warn("io_uring_submit() for write batch got EAGAIN, will try later.");
            return tl::unexpected(StackError::FromErrno(-EAGAIN));
        }
        return tl::unexpected(StackError(fmt::format("io_uring_submit() failed, errno: {}, errstr: {}", -ret, strerror(-ret))));
    }

    for (auto *slot : slots)
    {
        slot->SetStatus(IOSlot::Status::WriteSubmitted);
    }
    return static_cast<int>(slots.size());
}

tl::expected<void, StackError> UIOSlotMgr::ReapRead(IOSlot *slot, io_uring_cqe *cqe)
{
    ssize_t res = cqe->res;
    assert(slot->GetStatus() == IOSlot::Status::ReadSubmitted);

    return HandleReadCompletion(slot, res);
}

tl::expected<void, StackError> UIOSlotMgr::ReapWrite(IOSlot *slot, io_uring_cqe *cqe)
{
    ssize_t res = cqe->res;
    assert(slot->GetStatus() == IOSlot::Status::WriteSubmitted);

    return HandleWriteCompletion(slot, res);
}

tl::expected<void, StackError> UIOSlotMgr::IOReap()
{
    const int max_events = static_cast<int>(mRWSlots.size());

    // 使用 io_uring_peek_cqe 非阻塞轮询，与 libaio 的 io_getevents 超时等待不同
    // 若当前无完成事件则立即返回，由上层 RunQueue 循环控制调度节奏
    for (int i = 0; i < max_events; ++i)
    {
        mLogger->trace("Waiting for completion events to reap...");
        struct io_uring_cqe *cqe = nullptr;
        int ret = io_uring_peek_cqe(&mRing, &cqe);
        if (ret == -EAGAIN || cqe == nullptr)
        {
            mLogger->trace("No more completion events to reap.");
            break; // no more events
        }
        if (ret < 0)
        {
            return tl::unexpected(StackError(fmt::format("io_uring_peek_cqe failed, errno: {}", -ret)));
        }

        IOSlot *slot = static_cast<IOSlot *>(io_uring_cqe_get_data(cqe));
        if (slot->GetStatus() == IOSlot::Status::ReadSubmitted)
        {
            mLogger->trace("Reaping read completion for slot ID: {}", slot->GetID());
            auto reap_res = ReapRead(slot, cqe);
            if (!reap_res)
            {
                io_uring_cqe_seen(&mRing, cqe);
                return tl::unexpected(reap_res.error());
            }
        }
        else if (slot->GetStatus() == IOSlot::Status::WriteSubmitted)
        {
            mLogger->trace("Reaping write for slot ID: {}", slot->GetID());
            auto reap_res = ReapWrite(slot, cqe);
            if (!reap_res)
            {
                io_uring_cqe_seen(&mRing, cqe);
                return tl::unexpected(reap_res.error());
            }
        }

        io_uring_cqe_seen(&mRing, cqe);
    }

    return {};
}
