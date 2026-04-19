#include "lib/acp/acp.hpp"

#include <memory> // std::make_unique
// #include <linux/aio_abi.h> // don't include this, it may cause conflict with libaio
#include <linux/fs.h> // RWF_NOWAIT

tl::expected<void, StackError> AIOSlotMgr::Init()
{
    io_context_t ctx = 0;
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
    iocb->data = slot; // associate slot with this iocb
    // iocb->aio_rw_flags |= RWF_NOWAIT;

    return;
}
tl::expected<void, StackError> AIOSlotMgr::SubmitOneRead(IOSlot *slot)
{
    // m_logger->debug("Submitting read IO for slot ID: {}", slot->GetID());

    auto iocb = slot->GetReadIOCB();
    assert(iocb != nullptr);

    struct iocb *iocbs[1];
    iocbs[0] = iocb;

#ifndef NDEBUG
    // 打印io_submit()所用时间
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_submit(mIoCtx, 1, iocbs);
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_submit(read)", start, end);
#else
    int ret = io_submit(mIoCtx, 1, iocbs);
#endif
    if (ret < 0)
    {
        if (ret == -EAGAIN) // cannot submit more IO now
        {
            mLogger->warn("io_submit() for read got EAGAIN, slot: {}, will try later.", slot->GetID());
            // PrtSlots();
            return tl::unexpected(StackError::FromErrno(-EAGAIN));
        }

        return tl::unexpected(StackError(
            fmt::format("io_submit() failed, errno: {}, errstr: {}", -ret, strerror(-ret))));
    }
    else
    {
        slot->SetStatus(IOSlot::Status::ReadSubmitted);
    }
    return {};
}

void AIOSlotMgr::PrepareOneWrite(IOSlot *slot)
{
    auto iocb{slot->InitWriteIOCB()};
    auto currCPFPIt = slot->GetCPFPPtr();

    auto [offset, ioSize] = slot->GetIOInfo();
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

tl::expected<void, StackError> AIOSlotMgr::SubmitOneWrite(IOSlot *slot)
{
    assert(slot->GetStatus() == IOSlot::Status::WritePrepared);
    auto iocb = slot->GetWriteIOCB();
    assert(iocb != nullptr);

    struct iocb *iocbs[1];
    iocbs[0] = iocb;

    mLogger->debug("Submitting write IO for slot ID: {}, offset: {}, io_size: {}, src: {}, dst: {}",
                   slot->GetID(), iocb->u.c.offset, iocb->u.c.nbytes,
                   slot->GetCPFPPtr()->GetSrcPath(), slot->GetCPFPPtr()->GetDstPath());

#ifndef NDEBUG
    // 打印io_submit()所用时间
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_submit(mIoCtx, 1, iocbs);
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_submit(write)", start, end); // warn if >100ms
#else
    int ret = io_submit(mIoCtx, 1, iocbs);
#endif
    if (ret < 0)
    {
        // EAGAIN happens here
        if (ret == -EAGAIN)
        {
            mLogger->warn("io_submit() for write got EAGAIN, slot: {}, will try later.", slot->GetID());
            return tl::unexpected(StackError::FromErrno(-EAGAIN));
        }

        return tl::unexpected(StackError(
            fmt::format("io_submit() failed, errno: {}, errstr: {}", -ret, strerror(-ret))));
    }
    else
    {
        slot->SetStatus(IOSlot::Status::WriteSubmitted);
        return {};
    }

    return {};
}

// reap read IO and submit write IO with current IOSlot's buffer
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
