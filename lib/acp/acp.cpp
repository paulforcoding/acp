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

void AIOSlotMgr::PrepareOneRead(AIOSlot *slot, off_t offset, std::shared_ptr<CPFilePair> currCPFPIt)
{
    auto iocb{slot->InitReadIOCB()};
    size_t io_size = m_options.IoSize;

    m_logger->debug("Preparing read IO for slot ID: {}, offset: {}, io_size: {}, iocb addr: {:p}, src: {}, dst: {}",
                    slot->GetID(), offset, io_size, static_cast<void *>(iocb),
                    currCPFPIt->GetSrcPath(), currCPFPIt->GetDstPath());
    io_prep_pread(iocb, currCPFPIt->GetSrcFd(), slot->GetBuf(), io_size, offset);
    iocb->data = slot; // associate slot with this iocb
    // iocb->aio_rw_flags |= RWF_NOWAIT;
    slot->SetCPFPPtr(currCPFPIt);
    slot->SetStatus(IOSlot::Status::ReadPrepared);
    currCPFPIt->UpdateReadBytes(io_size);
    return;
}
tl::expected<void, StackError> AIOSlotMgr::SubmitOneRead(AIOSlot *slot)
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
    AddDurationWarn("io_submit(read)", start, end, 100); // warn if >100ms
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

tl::expected<int, StackError> AIOSlotMgr::SubmitReads()
{
    // TODO: change to batch submit later
    int submitted = 0;

    for (auto slot : m_slots)
    {
        auto next_res = mCPFPMgr->GetNextReadIO();

        if (!next_res)
        {
            return tl::unexpected(StackError("mCPFPMgr->GetNextReadIO(), err: ", next_res.error()));
        }

        auto nextIO = next_res.value();

        if (nextIO == nullptr)
        {
            m_logger->debug("All read IOs have been submitted.");
            break; // all io submitted
        }

        // prepare read io
        if (slot->GetStatus() == IOSlot::Status::Init)
        {
            size_t offset = nextIO->GetReadOffset();
            PrepareOneRead(slot, offset, nextIO);
        }

        // submit prepared read io
        if (slot->GetStatus() == IOSlot::Status::ReadPrepared)
        {
            auto res = SubmitOneRead(slot);
            if (!res)
            {
                // if EAGAIN, break and try again later
                if (res.error() == StackError("EAGAIN"))
                {
                    m_logger->debug("io_submit() for read got EAGAIN, slot: {}, will try later.", slot->GetID());
                    PrtSlots();
                    break;
                }
                return tl::unexpected(res.error());
            }
            else
            {
                submitted++;
            }
        }
    }

    return submitted;
}

void AIOSlotMgr::PrepareOneWrite(AIOSlot *slot, off_t offset)
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

tl::expected<void, StackError> AIOSlotMgr::SubmitOneWrite(AIOSlot *slot)
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
    AddDurationWarn("io_submit(write)", start, end, 100); // warn if >100ms
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

// 使用RWF_NOWAIT标志提交写请求会多此一个步骤
tl::expected<int, StackError> AIOSlotMgr::SubmitWrites()
{
    // TODO: change to batch submit later
    int submitted = 0;

    // 找出所有可以提交写请求的slot并提交写请求
    for (auto slot : m_slots)
    {
        // prepare write io
        if (slot->GetStatus() == IOSlot::Status::WritePrepared)
        {

            auto res = SubmitOneWrite(slot);
            if (!res)
            {
                // if EAGAIN, break and try again later
                if (res.error() == StackError("EAGAIN"))
                {
                    m_logger->debug("SubmitOneWrite() in SubmitWrites() got EAGAIN, slot: {}, will try later.", slot->GetID());
                    PrtSlots();
                    break;
                }
                return tl::unexpected(res.error());
            }
            else
            {
                submitted++;
            }
        }
    }
    return submitted;
}

// reap read IO and submit write IO with current IOSlot's buffer
tl::expected<void, StackError> AIOSlotMgr::ReapRead(struct io_event *ev)
{

    ssize_t io_ret = ev->res;
    struct iocb *cb = ev->obj;
    AIOSlot *slot = static_cast<AIOSlot *>(ev->data);
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
    AIOSlot *slot = static_cast<AIOSlot *>(ev->data);
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
    slot->SetStatus(IOSlot::Status::WriteReaped);

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
    AddDurationWarn("io_getevents()", start, end, 10); // warn if >10ms
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

tl::expected<void, StackError> AIOSlotMgr::CheckOneCompleted(AIOSlot *slot)
{
    if (slot->GetStatus() == IOSlot::Status::WriteReaped)
    {
        auto check_res = mCPFPMgr->CheckWriteComplete(slot->GetCPFPPtr());
        if (!check_res)
        {
            return tl::unexpected(StackError("mCPFPMgr->CheckWriteComplete(), err: ", check_res.error()));
        }
        slot->SetStatus(IOSlot::Status::Init);
    }
    return {};
}

// 现在只有src file size == 0的情况会触发这个函数
tl::expected<void, StackError> AIOSlotMgr::CheckCompleteds()
{
    for (auto slot : m_slots)
    {
        m_logger->debug("Checking slot ID: {} with status: {}",
                        slot->GetID(), IOSlot::StatusToStr(slot->GetStatus()));
        if (slot->GetStatus() == IOSlot::Status::WriteReaped)
        {
            auto check_res = mCPFPMgr->CheckWriteComplete(slot->GetCPFPPtr());
            if (!check_res)
            {
                return tl::unexpected(StackError("mCPFPMgr->CheckWriteComplete(), err: ", check_res.error()));
            }
            slot->SetStatus(IOSlot::Status::Init);
            m_logger->debug("Slot ID: {} reset to Init status after write completion.", slot->GetID());
        }
        m_logger->debug("Slot ID: {} final status after CheckCompleted: {}",
                        slot->GetID(), IOSlot::StatusToStr(slot->GetStatus()));
    }
    m_logger->debug("Completed checking all slots.");
    return {};
}

tl::expected<void, StackError> AIOSlotMgr::RunCopyQueue()
{

    while (!mCPFPMgr->ShouldStartCopy())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (mCPFPMgr->ShouldStopCopy())
        {
            m_logger->debug("Received stop signal before starting copy queue.");
            return {};
        }
    }

    auto init_res = Init();
    if (!init_res)
    {
        return tl::unexpected(init_res.error());
    }

    long round = 0;
    while (!mCPFPMgr->ShouldStopCopy())
    {
        auto submit_res = SubmitReads();
        if (!submit_res)
        {
            return tl::unexpected(StackError("SubmitReads(), err: ", submit_res.error()));
        }

        m_logger->debug("Submitted {} read IOs in round: {}.", submit_res.value(), round);

        auto check_stuck_res = CheckStuck();
        if (!check_stuck_res)
        {
            return tl::unexpected(check_stuck_res.error());
        }

        auto reap_res = IOReap();
        if (!reap_res)
        {
            return tl::unexpected(StackError("IOReap(), err: ", reap_res.error()));
        }

        auto write_res = SubmitWrites();
        if (!write_res)
        {
            return tl::unexpected(StackError("SubmitWrites(), err: ", write_res.error()));
        }
        m_logger->debug("Submitted {} write IOs in round: {}.", write_res.value(), round);

        // auto check_completed_res = CheckCompleteds();
        // if (!check_completed_res)
        // {
        //     return tl::unexpected(StackError("CheckCompleted(), err: ", check_completed_res.error()));
        // }

        round++;
    }

    Reset();

    return {};
}

tl::expected<void, StackError> AIOSlotMgr::CheckStuck()
{
    // 检查是否只少有一个slot处于ReadSubmitted或者WriteSubmitted状态
    bool isStuck = true;
    for (auto slot : m_slots)
    {
        if (slot->GetStatus() == IOSlot::Status::ReadSubmitted || slot->GetStatus() == IOSlot::Status::WriteSubmitted)
        {
            isStuck = false;
            break;
        }
    }

    if (isStuck && !mCPFPMgr->ShouldStopCopy() && !m_options.EnableInotify)
    {
        m_logger->warn("Detected stuck AIO operations.");
        return tl::unexpected(StackError("Detected stuck AIO operations."));
    }

    return {};
}

void AIOSlotMgr::PrtSlots()
{
    m_logger->warn("Current IOSlot statuses:");
    // 按状态统计slot数量，并打印每个状态slot的数量
    std::map<IOSlot::Status, int> status_count;
    for (auto slot : m_slots)
    {
        status_count[slot->GetStatus()]++;
    }

    for (const auto &pair : status_count)
    {
        m_logger->warn("Slot Status: {}, Count: {}", IOSlot::StatusToStr(pair.first), pair.second);
    }
}

void AIOSlotMgr::AddDurationWarn(std::string_view func_name, std::chrono::_V2::system_clock::time_point start,
                                 std::chrono::_V2::system_clock::time_point end, int64_t warn_threshold)
{
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    if (duration > warn_threshold)
    {

        m_logger->warn("Function {} took {} ms, exceeding threshold {} ms",
                       func_name, duration, warn_threshold);
        PrtSlots();
    }
    if (m_func_duration_stat)
    {
        m_func_duration_stat->AddDuration(func_name, duration);
    }

    if (func_name == "io_submit(read)")
    {
        last_read_submit_duration = duration;
    }
    else if (func_name == "io_submit(write)")
    {
        last_write_submit_duration = duration;
    }
};
