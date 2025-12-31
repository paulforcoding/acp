#include "lib/acp/acp.hpp"

#include <memory> // std::make_unique
// #include <linux/aio_abi.h> // don't include this, it may cause conflict with libaio
#include <linux/fs.h> // RWF_NOWAIT

tl::expected<void, StackError> AIOSlotMgr::Init()
{

    io_context_t ctx = 0;
    int ret = io_setup(static_cast<unsigned>(m_slots.size()), &ctx);
    if (ret < 0)
    {
        return tl::unexpected(StackError("io_setup() failed, errno: " + std::to_string(-ret)));
    }
    m_io_ctx = ctx;

    return {};
}

void AIOSlotMgr::PrepareOneRead(IOSlot *slot, off_t offset, std::shared_ptr<CPFilePair> currCPFPIt)
{
    auto iocb{slot->InitReadIOCB()};
    size_t io_size = m_options.IoSize;

    m_logger->trace("Preparing read IO for slot ID: {}, offset: {}, io_size: {}, iocb addr: {:p}, src: {}, dst: {}",
                    slot->GetID(), offset, io_size, static_cast<void *>(iocb),
                    currCPFPIt->GetSrcPath(), currCPFPIt->GetDstPath());
    io_prep_pread(iocb, currCPFPIt->GetSrcFd(), slot->GetBuf(), io_size, offset);
    iocb->data = slot; // associate slot with this iocb
    // iocb->aio_rw_flags |= RWF_NOWAIT;
    slot->SetCPFPPtr(currCPFPIt);
    slot->SetStatus(IOSlot::Status::ReadPrepared);
    currCPFPIt->UpdateReadBytes(io_size);
    mCPFPMgr->CheckReadComplete(slot->GetCPFPPtr());
    return;
}
tl::expected<void, StackError> AIOSlotMgr::SubmitOneRead(IOSlot *slot)
{
    // m_logger->debug("Submitting read IO for slot ID: {}", slot->GetID());

    auto iocb = slot->GetReadIOCB();
    assert(iocb != nullptr);

    struct iocb *iocbs[1];
    iocbs[0] = iocb;

    // special handling for zero-size source file
    // auto currCPFPIt = slot->GetCPFPPtr();
    // if (currCPFPIt->GetSrcFileSize() == 0)
    // {
    //     m_logger->warn("Source file size is 0, skipping read submission for slot ID: {}", slot->GetID());
    //     slot->SetStatus(IOSlot::Status::WriteReaped); // directly mark as write reaped

    //     auto check_res = CheckOneCompleted(slot);
    //     if (!check_res)
    //     {
    //         return tl::unexpected(StackError("CheckOneCompleted() failed after write reap, err: ", check_res.error()));
    //     }

    //     // 不能在这里直接 CheckOneCompleted，因为此时currCPFPIt还没有被加入到inflight列表中
    //     // 要等到下一次调用GetNextReadIO时，才能保证currCPFPIt已经在inflight列表中
    //     return {};
    // }

#ifndef NDEBUG
    // 打印io_submit()所用时间
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_submit(m_io_ctx, 1, iocbs);
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_submit(read)", start, end);
#else
    int ret = io_submit(m_io_ctx, 1, iocbs);
#endif
    if (ret < 0)
    {
        if (ret == -EAGAIN) // cannot submit more IO now
        {
            m_logger->warn("io_submit() for read got EAGAIN, slot: {}, will try later.", slot->GetID());
            // PrtSlots();
            return tl::unexpected(StackError("EAGAIN"));
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

void AIOSlotMgr::PrepareOneWrite(IOSlot *slot, off_t offset)
{
    auto iocb{slot->InitWriteIOCB()};
    auto currCPFPIt = slot->GetCPFPPtr();
    m_logger->debug("Preparing write IO for slot ID: {}, offset: {}, io_size: {}, src: {}, dst: {}",
                    slot->GetID(), offset, m_options.IoSize, currCPFPIt->GetSrcPath(), currCPFPIt->GetDstPath());
    io_prep_pwrite(iocb, currCPFPIt->GetDstFd(), slot->GetBuf(), m_options.IoSize,
                   offset);
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

    m_logger->debug("Submitting write IO for slot ID: {}, offset: {}, io_size: {}, src: {}, dst: {}",
                    slot->GetID(), iocb->u.c.offset, iocb->u.c.nbytes,
                    slot->GetCPFPPtr()->GetSrcPath(), slot->GetCPFPPtr()->GetDstPath());

#ifndef NDEBUG
    // 打印io_submit()所用时间
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_submit(m_io_ctx, 1, iocbs);
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_submit(write)", start, end); // warn if >100ms
#else
    int ret = io_submit(m_io_ctx, 1, iocbs);
#endif
    if (ret < 0)
    {
        // EAGAIN happens here
        if (ret == -EAGAIN)
        {
            m_logger->warn("io_submit() for write got EAGAIN, slot: {}, will try later.", slot->GetID());
            return tl::unexpected(StackError("EAGAIN"));
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
    struct iocb *cb = ev->obj;
    IOSlot *slot = static_cast<IOSlot *>(ev->data);
    assert(slot->GetStatus() == IOSlot::Status::ReadSubmitted);
    slot->SetStatus(IOSlot::Status::ReadReaped);

    if (io_ret < 0)
    {
        // if we set RWF_NOWAIT, we'll got EAGAIN here, submit this read again
        if (io_ret == -EAGAIN)
        {
            m_logger->warn("AIO read got EAGAIN, resubmitting for slot ID: {}, offset: {}, io_size: {}",
                           slot->GetID(), cb->u.c.offset, cb->u.c.nbytes);
            PrtSlots();
            PrepareOneRead(slot, cb->u.c.offset, slot->GetCPFPPtr());
            return {}; // let SubmitReads handle resubmission in next round
        }

        return tl::unexpected(StackError("AIO read failed, errno: " + std::to_string(-io_ret)));
    }

    if (io_ret == 0)
    {
        return tl::unexpected(StackError("AIO read returned 0 bytes read, unexpected."));
    }

    

    m_logger->debug("AIO read completed for slot ID: {}, offset: {}, bytes read: {}, iocb addr: {:p}",
                    slot->GetID(), cb->u.c.offset, io_ret, static_cast<void *>(cb));

    // prepare write io
    off_t write_offset = cb->u.c.offset; // same offset as read
    PrepareOneWrite(slot, write_offset);

    return {};
}

tl::expected<void, StackError> AIOSlotMgr::ReapWrite(struct io_event *ev)
{

    ssize_t io_ret = ev->res;
    IOSlot *slot = static_cast<IOSlot *>(ev->data);
    struct iocb *cb = ev->obj;
    auto currCPFPIt = slot->GetCPFPPtr();

    assert(slot->GetStatus() == IOSlot::Status::WriteSubmitted);

    slot->SetStatus(IOSlot::Status::WriteReaped);

    if (io_ret < 0)
    {
        //  if we set RWF_NOWAIT, we'll got EAGAIN here, submit this write again
        if (io_ret == -EAGAIN)
        {
            m_logger->warn("AIO write got EAGAIN, resubmitting for slot ID: {}, offset: {}, io_size: {}",
                           slot->GetID(), cb->u.c.offset, cb->u.c.nbytes);
            PrtSlots();
            PrepareOneWrite(slot, cb->u.c.offset);
            return {}; // let SubmitWrites handle resubmission in next round, this will let read IO go first
        }
        return tl::unexpected(StackError(
            fmt::format("AIO write failed, errno: {}, errstr: {}, offset: {}, io_size: {}",
                        io_ret, strerror(-io_ret), cb->u.c.offset, cb->u.c.nbytes)));
    }
    if (io_ret == 0)
    {
        return tl::unexpected(StackError("AIO write returned 0 bytes written, unexpected."));
    }

    m_logger->debug("AIO write completed for slot ID: {}, offset: {}, bytes written: {}, src: {}, dst: {}",
                    slot->GetID(), cb->u.c.offset, io_ret, currCPFPIt->GetSrcPath(), currCPFPIt->GetDstPath());

    currCPFPIt->UpdateWrittenBytes(io_ret);

    auto check_res = CheckOneCompleted(slot);
    if (!check_res)
    {
        return tl::unexpected(StackError("CheckOneCompleted() failed after write reap, err: ", check_res.error()));
    }

    return {};
}

tl::expected<void, StackError> AIOSlotMgr::IOReap()
{
    const int max_events = static_cast<int>(m_slots.size());
    struct io_event events[max_events];
    struct timespec timeout;
    timeout.tv_sec = 10;
    timeout.tv_nsec = 0;

#ifndef NDEBUG
    // 打印io_getevents()所用时间
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_getevents(m_io_ctx, 1, max_events, events,
                           &timeout);
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_getevents()", start, end); // warn if >10ms
#else
    int ret = io_getevents(m_io_ctx, 1, max_events, events, &timeout);
#endif
    if (ret < 0)
    {
        return tl::unexpected(StackError("io_getevents() failed, errno: " + std::to_string(-ret)));
    }
    m_logger->debug("Reaped {} IO events.", ret);
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

// tl::expected<void, StackError> AIOSlotMgr::CheckOneCompleted(AIOSlot *slot)
// {
//     if (slot->GetStatus() == IOSlot::Status::WriteReaped)
//     {
//         auto check_res = mCPFPMgr->CheckWriteComplete(slot->GetCPFPPtr());
//         if (!check_res)
//         {
//             return tl::unexpected(StackError("mCPFPMgr->CheckWriteComplete(), err: ", check_res.error()));
//         }
//         slot->SetStatus(IOSlot::Status::Init);
//     }
//     return {};
// }
