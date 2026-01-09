#include "lib/ucp/ucp.hpp"

#include <memory>
#include <fmt/format.h>

tl::expected<void, StackError> UIOSlotMgr::Init()
{
    int ret = io_uring_queue_init(static_cast<int>(m_slots.size()), &m_ring, 0);
    if (ret < 0)
    {
        return tl::unexpected(StackError(fmt::format("io_uring_queue_init() failed, errno: {}", -ret)));
    }
    return {};
}

void UIOSlotMgr::PrepareOneRead(IOSlot *slot, off_t offset, std::shared_ptr<CPFilePair> currCPFPIt)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
    size_t io_size = m_options.IoSize;
    mLogger->debug("Preparing read IO for slot ID: {}, offset: {}, io_size: {}, src: {}, dst: {}",
                   slot->GetID(), offset, io_size, currCPFPIt->GetSrcPath(), currCPFPIt->GetDstPath());

    io_uring_prep_read(sqe, currCPFPIt->GetSrcFd(), slot->GetBuf(), io_size, offset);
    io_uring_sqe_set_data(sqe, slot);
    slot->SetCPFPPtr(currCPFPIt);
    slot->SetIOInfo(offset, io_size);

    slot->SetStatus(IOSlot::Status::ReadPrepared);
    currCPFPIt->UpdateReadBytes(io_size);
    mCPFPMgr->CheckReadComplete(slot->GetCPFPPtr());
}

tl::expected<void, StackError> UIOSlotMgr::SubmitOneRead(IOSlot *slot)
{
#ifndef NDEBUG
    // 打印io_submit()所用时间
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_uring_submit(&m_ring);
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_submit(read)", start, end);
#else
    int ret = io_uring_submit(&m_ring);
#endif

    if (ret < 0)
    {
        if (ret == -EAGAIN)
        {
            mLogger->warn("io_uring_submit() for read got EAGAIN, slot: {}, will try later.", slot->GetID());
            return tl::unexpected(StackError("EAGAIN"));
        }
        return tl::unexpected(StackError(fmt::format("io_uring_submit() failed, errno: {}, errstr: {}", -ret, strerror(-ret))));
    }
    slot->SetStatus(IOSlot::Status::ReadSubmitted);
    return {};
}

void UIOSlotMgr::PrepareOneWrite(IOSlot *slot)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
    auto currCPFPIt = slot->GetCPFPPtr();

    auto [offset, ioSize] = slot->GetIOInfo();
    if (m_options.DirectIO)
    {
        ioSize = m_options.IoSize;
    }
    io_uring_prep_write(sqe, currCPFPIt->GetDstFd(), slot->GetBuf(), ioSize, offset);
    mLogger->debug("Preparing write IO for slot ID: {}, offset: {}, io_size: {}, src: {}, dst: {}",
                   slot->GetID(), offset, ioSize, currCPFPIt->GetSrcPath(), currCPFPIt->GetDstPath());

    io_uring_sqe_set_data(sqe, slot);

    slot->SetStatus(IOSlot::Status::WritePrepared);
}

tl::expected<void, StackError> UIOSlotMgr::SubmitOneWrite(IOSlot *slot)
{
#ifndef NDEBUG
    // 打印io_submit()所用时间
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_uring_submit(&m_ring);
    auto end = std::chrono::high_resolution_clock::now();
    AddDuration("io_submit(write)", start, end);
#else
    int ret = io_uring_submit(&m_ring);
#endif

    if (ret < 0)
    {
        if (ret == -EAGAIN)
        {
            mLogger->warn("io_uring_submit() for write got EAGAIN, slot: {}, will try later.", slot->GetID());
            return tl::unexpected(StackError("EAGAIN"));
        }
        return tl::unexpected(StackError(fmt::format("io_uring_submit() failed, errno: {}, errstr: {}", -ret, strerror(-ret))));
    }
    slot->SetStatus(IOSlot::Status::WriteSubmitted);

    return {};
}

tl::expected<void, StackError> UIOSlotMgr::ReapRead(IOSlot *slot, io_uring_cqe *cqe)
{
    ssize_t res = cqe->res;
    auto [offset, ioSize] = slot->GetIOInfo();

    mLogger->trace("UIO read completed for slot ID: {}, offset: {}, bytes read: {}",
                   slot->GetID(), offset, res);
    // assert(ioSize == static_cast<size_t>(res));
    assert(slot->GetStatus() == IOSlot::Status::ReadSubmitted);

    slot->SetStatus(IOSlot::Status::ReadReaped);

    if (res < 0)
    {
        if (res == -EAGAIN)
        {
            mLogger->warn("UIO read got EAGAIN, resubmitting for slot ID: {}, res: {}", slot->GetID(), res);
            // re-prepare read for this slot
            PrepareOneRead(slot, /*offset*/ 0, slot->GetCPFPPtr());
            return {};
        }
        return tl::unexpected(StackError(fmt::format("UIO read failed, errno: {}", -res)));
    }

    if (res == 0)
    {
        return tl::unexpected(StackError("UIO read returned 0 bytes read, unexpected."));
    }

    mLogger->debug("UIO read completed for slot ID: {}, bytes read: {}", slot->GetID(), res);
    // prepare write with same offset
    // extract offset from cqe not available; use slot's IOInfo
    slot->SetIOInfo(offset, static_cast<size_t>(res));
    PrepareOneWrite(slot);
    return {};
}

tl::expected<void, StackError> UIOSlotMgr::ReapWrite(IOSlot *slot, io_uring_cqe *cqe)
{
    ssize_t res = cqe->res;

    assert(slot->GetStatus() == IOSlot::Status::WriteSubmitted);
    slot->SetStatus(IOSlot::Status::WriteReaped);

    if (res < 0)
    {
        if (res == -EAGAIN)
        {
            mLogger->warn("UIO write got EAGAIN, resubmitting for slot ID: {}, res: {}", slot->GetID(), res);
            PrepareOneWrite(slot);
            return {};
        }
        return tl::unexpected(StackError(fmt::format("UIO write failed, errno: {}, errstr: {}", res, strerror(-res))));
    }

    if (res == 0)
    {
        return tl::unexpected(StackError(
            fmt::format("UIO write returned 0 bytes written, unexpected. src: {}, dst: {}",
                        slot->GetCPFPPtr()->GetSrcPath(), slot->GetCPFPPtr()->GetDstPath())));
    }

    auto currCPFPIt = slot->GetCPFPPtr();
    mLogger->debug("UIO write completed for slot ID: {}, bytes written: {}, src: {}, dst: {}",
                   slot->GetID(), res, currCPFPIt->GetSrcPath(), currCPFPIt->GetDstPath());
    currCPFPIt->UpdateWrittenBytes(res);

    auto check_res = CheckOneCompleted(slot);
    if (!check_res)
    {
        return tl::unexpected(StackError("CheckOneCompleted() failed after write reap, err: ", check_res.error()));
    }

    return {};
}

tl::expected<void, StackError> UIOSlotMgr::IOReap()
{
    const int max_events = static_cast<int>(m_slots.size());

    for (int i = 0; i < max_events; ++i)
    {
        mLogger->trace("Waiting for completion events to reap...");
        struct io_uring_cqe *cqe = nullptr;
        int ret = io_uring_peek_cqe(&m_ring, &cqe);
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
                io_uring_cqe_seen(&m_ring, cqe);
                return tl::unexpected(reap_res.error());
            }
        }
        else if (slot->GetStatus() == IOSlot::Status::WriteSubmitted)
        {
            mLogger->trace("Reaping write for slot ID: {}", slot->GetID());
            auto reap_res = ReapWrite(slot, cqe);
            if (!reap_res)
            {
                io_uring_cqe_seen(&m_ring, cqe);
                return tl::unexpected(reap_res.error());
            }
        }

        io_uring_cqe_seen(&m_ring, cqe);
    }

    return {};
}

// tl::expected<void, StackError> UIOSlotMgr::CheckOneCompleted(IOSlot *slot)
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
