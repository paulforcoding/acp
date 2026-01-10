#include "lib/ucp/ucp.hpp"

#include <memory>
#include <fmt/format.h>

tl::expected<void, StackError> UIOSlotMgr::Init()
{
    int ret = io_uring_queue_init(static_cast<int>(mRWSlots.size()), &m_ring, 0);
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
            return tl::unexpected(StackError::FromErrno(-EAGAIN));
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
            return tl::unexpected(StackError::FromErrno(-EAGAIN));
        }
        return tl::unexpected(StackError(fmt::format("io_uring_submit() failed, errno: {}, errstr: {}", -ret, strerror(-ret))));
    }
    slot->SetStatus(IOSlot::Status::WriteSubmitted);

    return {};
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

