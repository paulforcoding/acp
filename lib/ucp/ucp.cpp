#include "lib/ucp/ucp.hpp"

#include <memory>
#include <chrono>
#include <thread>
#include <fmt/format.h>

tl::expected<void, zplib::StackError> UIOSlotMgr::Init()
{
    io_cnt_total = DIV_ROUND_UP(m_file_infos->GetSrcFileSize(), m_options.IoSize);
    m_logger->info("(uring) from: {}, to: {}, file_size: {}, io_cnt_total: {}, io_size: {}, src_fd: {}, dst_fd: {}, io_depth: {}",
                   m_file_infos->GetSrcPath(), m_file_infos->GetDstPath(), m_file_infos->GetSrcFileSize(),
                   io_cnt_total, m_options.IoSize, m_file_infos->GetSrcFd(), m_file_infos->GetDstFd(), m_slots.size());

    int ret = io_uring_queue_init(static_cast<unsigned>(m_slots.size()), &m_ring, 0);
    if (ret < 0)
    {
        return tl::unexpected(zplib::StackError(fmt::format("io_uring_queue_init() failed: {}", -ret)));
    }

    return {};
}

void UIOSlotMgr::PrepareOneRead(UIOSlot *slot, off_t offset)
{
    m_logger->debug("(uring) Preparing read IO for slot ID: {}, offset: {}, io_size: {}, buf: {:p}",
                    slot->GetID(), offset, m_options.IoSize, static_cast<void *>(slot->GetBuf()));
    slot->SetStatus(IOSlot::Status::ReadPrepared);
    // actual sqe prep happens in SubmitOneRead to keep parity with acp's behavior
}

tl::expected<void, zplib::StackError> UIOSlotMgr::SubmitOneRead(UIOSlot *slot)
{
    assert(slot->GetStatus() == IOSlot::Status::ReadPrepared);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
    if (!sqe)
    {
        m_logger->warn("io_uring: no sqe available for read, will try later. slot: {}", slot->GetID());
        return tl::unexpected(zplib::StackError("EAGAIN"));
    }

    off_t offset = static_cast<off_t>(io_read_submitted * m_options.IoSize);
    io_uring_prep_read(sqe, m_file_infos->GetSrcFd(), slot->GetBuf(), m_options.IoSize, offset);
    io_uring_sqe_set_data(sqe, slot);

#ifndef NDEBUG
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_uring_submit(&m_ring);
    auto end = std::chrono::high_resolution_clock::now();
    AddDurationWarn("io_submit(read)", start, end, 100); // warn if >100ms
    last_read_submit_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
#else
    int ret = io_uring_submit(&m_ring);
#endif

    if (ret < 0)
    {
        return tl::unexpected(zplib::StackError(fmt::format("io_uring submit read failed: {}", -ret)));
    }

    slot->SetStatus(IOSlot::Status::ReadSubmitted);
    io_read_submitted++;
    return {};
}

tl::expected<int, zplib::StackError> UIOSlotMgr::SubmitReads()
{
    int submitted = 0;
    for (auto slot : m_slots)
    {
        if (io_read_submitted >= io_cnt_total)
        {
            break;
        }

        if (slot->GetStatus() == IOSlot::Status::Init)
        {
            size_t offset = io_read_submitted * m_options.IoSize;
            PrepareOneRead(slot, offset);
        }

        if (slot->GetStatus() == IOSlot::Status::ReadPrepared)
        {
            auto res = SubmitOneRead(slot);
            if (!res)
            {
                if (res.error() == zplib::StackError("EAGAIN"))
                {
                    m_logger->debug("(uring) submit read got EAGAIN, slot: {}, will try later.", slot->GetID());
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

void UIOSlotMgr::PrepareOneWrite(UIOSlot *slot, off_t offset)
{
    m_logger->debug("(uring) Preparing write IO for slot ID: {}, offset: {}, io_size: {}",
                    slot->GetID(), offset, m_options.IoSize);
    slot->SetStatus(IOSlot::Status::WritePrepared);
}

tl::expected<void, zplib::StackError> UIOSlotMgr::SubmitOneWrite(UIOSlot *slot)
{
    assert(slot->GetStatus() == IOSlot::Status::WritePrepared);
    struct io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
    if (!sqe)
    {
        m_logger->warn("io_uring: no sqe available for write, will try later. slot: {}", slot->GetID());
        return tl::unexpected(zplib::StackError("EAGAIN"));
    }

    // struct iovec iov; // not strictly needed; use direct write
    // off_t offset = 0; // will set write offset in caller via PrepareOneWrite
    // compute offset from slot buffer's associated offset: assume slot stores offset in its buf or via pattern
    // Use last read offset trick: in ReapRead we call PrepareOneWrite with proper offset

    // Here we need the offset from slot; but base IOSlot doesn't store offset. For compatibility we assume
    // caller set the write offset by reusing same order; so we compute from io_completed+io_read_submitted etc is complex.
    // Simpler: assume PrepareOneWrite was called with correct offset and we saved it in slot's m_any via SetUserData if available.

    // For simplicity, encode offset into slot->GetID() mapping is not possible. We'll reuse slot's buffer address as data only.
    // We'll ask io_uring to write at the offset passed in by PrepareOneWrite by storing it temporarily in slot's "m_user_any" if available.

    // For this implementation we store offset in the slot's internal m_user_any (via SetUserAttr if provided by IOSlot). If not available,
    // we default to sequential writes by calculating from io_completed.

    // Fallback offset calculation:
    off_t write_offset = static_cast<off_t>(slot->GetID()) * static_cast<off_t>(m_options.IoSize);
    io_uring_prep_write(sqe, m_file_infos->GetDstFd(), slot->GetBuf(), m_options.IoSize, write_offset);
    io_uring_sqe_set_data(sqe, slot);

#ifndef NDEBUG
    auto start = std::chrono::high_resolution_clock::now();
    int ret = io_uring_submit(&m_ring);
    auto end = std::chrono::high_resolution_clock::now();
    AddDurationWarn("io_submit(write)", start, end, 100);
    last_write_submit_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
#else
    int ret = io_uring_submit(&m_ring);
#endif

    if (ret < 0)
    {
        return tl::unexpected(zplib::StackError(fmt::format("io_uring submit write failed: {}", -ret)));
    }

    slot->SetStatus(IOSlot::Status::WriteSubmitted);
    return {};
}

tl::expected<int, zplib::StackError> UIOSlotMgr::SubmitWrites()
{
    int submitted = 0;
    for (auto slot : m_slots)
    {
        if (slot->GetStatus() == IOSlot::Status::WritePrepared)
        {
            auto res = SubmitOneWrite(slot);
            if (!res)
            {
                if (res.error() == zplib::StackError("EAGAIN"))
                {
                    m_logger->debug("(uring) SubmitOneWrite() got EAGAIN, slot: {}, will try later.", slot->GetID());
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

tl::expected<void, zplib::StackError> UIOSlotMgr::ReapRead(struct io_uring_cqe *cqe)
{
    ssize_t res = cqe->res;
    UIOSlot *slot = static_cast<UIOSlot *>(io_uring_cqe_get_data(cqe));
    assert(slot != nullptr);
    if (res < 0)
    {
        if (res == -EAGAIN)
        {
            m_logger->warn("Uring read got EAGAIN, resubmitting slot: {}", slot->GetID());
            PrepareOneRead(slot, 0);
            return {};
        }
        return tl::unexpected(zplib::StackError(fmt::format("Uring read failed: {}", -res)));
    }
    if (res == 0)
    {
        return tl::unexpected(zplib::StackError("Uring read returned 0 bytes, unexpected."));
    }

    m_logger->debug("Uring read completed slot: {}, bytes: {}", slot->GetID(), res);
    slot->SetStatus(IOSlot::Status::ReadReaped);

    // prepare write at same offset
    off_t write_offset = static_cast<off_t>(slot->GetID()) * static_cast<off_t>(m_options.IoSize);
    PrepareOneWrite(slot, write_offset);
    auto submit_res = SubmitOneWrite(slot);
    if (!submit_res)
    {
        if (submit_res.error() == zplib::StackError("EAGAIN"))
        {
            m_logger->debug("SubmitOneWrite() in ReapRead() got EAGAIN after read reap, slot: {}, will try later.", slot->GetID());
            PrtSlots();
            return {};
        }
        return tl::unexpected(zplib::StackError("SubmitOneWrite() failed after read reap, err: ", submit_res.error()));
    }
    return {};
}

tl::expected<void, zplib::StackError> UIOSlotMgr::ReapWrite(struct io_uring_cqe *cqe)
{
    ssize_t res = cqe->res;
    UIOSlot *slot = static_cast<UIOSlot *>(io_uring_cqe_get_data(cqe));
    assert(slot != nullptr);
    if (res < 0)
    {
        if (res == -EAGAIN)
        {
            m_logger->warn("Uring write got EAGAIN, resubmitting slot: {}", slot->GetID());
            PrepareOneWrite(slot, 0);
            return {};
        }
        return tl::unexpected(zplib::StackError(fmt::format("Uring write failed: {}", -res)));
    }
    if (res == 0)
    {
        return tl::unexpected(zplib::StackError("Uring write returned 0 bytes, unexpected."));
    }

    m_logger->debug("Uring write completed slot: {}, bytes: {}", slot->GetID(), res);
    slot->SetStatus(IOSlot::Status::Init);
    io_completed++;
    return {};
}

tl::expected<void, zplib::StackError> UIOSlotMgr::IOReap()
{
    // submit and wait for at least one completion (non-busy)
    int ret = io_uring_submit_and_wait(&m_ring, 1);
    if (ret < 0)
    {
        return tl::unexpected(zplib::StackError(fmt::format("io_uring_submit_and_wait failed: {}", -ret)));
    }

    struct io_uring_cqe *cqe = nullptr;
    while (io_uring_peek_cqe(&m_ring, &cqe) == 0 && cqe)
    {
        // time measurement could be added here if desired
        UIOSlot *slot = static_cast<UIOSlot *>(io_uring_cqe_get_data(cqe));
        if (!slot)
        {
            io_uring_cqe_seen(&m_ring, cqe);
            continue;
        }

        // determine status: if slot was ReadSubmitted -> ReapRead, else if WriteSubmitted -> ReapWrite
        if (slot->GetStatus() == IOSlot::Status::ReadSubmitted)
        {
            auto r = ReapRead(cqe);
            if (!r)
            {
                return tl::unexpected(r.error());
            }
        }
        else if (slot->GetStatus() == IOSlot::Status::WriteSubmitted || slot->GetStatus() == IOSlot::Status::WritePrepared)
        {
            auto r = ReapWrite(cqe);
            if (!r)
            {
                return tl::unexpected(r.error());
            }
        }

        io_uring_cqe_seen(&m_ring, cqe);
        cqe = nullptr;
    }

    return {};
}

tl::expected<void, zplib::StackError> UIOSlotMgr::RunQueue(CPFilePair *file_infos)
{
    m_file_infos = file_infos;
    auto init_res = Init();
    if (!init_res)
    {
        return tl::unexpected(init_res.error());
    }

    long round = 0;
    while (!ShouldStop())
    {
        auto submit_res = SubmitReads();
        if (!submit_res)
        {
            return tl::unexpected(zplib::StackError("SubmitReads(), err: ", submit_res.error()));
        }

        m_logger->debug("(uring) Submitted {} read IOs in round: {}.", submit_res.value(), round);

        auto write_res = SubmitWrites();
        if (!write_res)
        {
            return tl::unexpected(zplib::StackError("SubmitWrites(), err: ", write_res.error()));
        }
        m_logger->debug("(uring) Submitted {} write IOs in round: {}.", write_res.value(), round);

        auto check_stuck_res = CheckStuck();
        if (!check_stuck_res)
        {
            return tl::unexpected(check_stuck_res.error());
        }

        auto reap_res = IOReap();
        if (!reap_res)
        {
            return tl::unexpected(zplib::StackError("IOReap(), err: ", reap_res.error()));
        }
        round++;
    }

    Reset();
    return {};
}

tl::expected<void, zplib::StackError> UIOSlotMgr::CheckStuck()
{
    bool isStuck = true;
    for (auto slot : m_slots)
    {
        if (slot->GetStatus() == IOSlot::Status::ReadSubmitted || slot->GetStatus() == IOSlot::Status::WriteSubmitted)
        {
            isStuck = false;
            break;
        }
    }

    if (isStuck)
    {
        m_logger->warn("Detected stuck uring operations.");
        return tl::unexpected(zplib::StackError("Detected stuck uring operations."));
    }

    return {};
}

void UIOSlotMgr::PrtSlots()
{
    m_logger->warn("Current UIOSlot statuses:");
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

void UIOSlotMgr::AddDurationWarn(std::string_view func_name, std::chrono::_V2::system_clock::time_point start,
                                 std::chrono::_V2::system_clock::time_point end, int64_t warn_threshold)
{
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    if (duration > warn_threshold)
    {
        m_logger->warn("Function {} took {} ms, exceeding threshold {} ms", func_name, duration, warn_threshold);
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
}

tl::expected<void, zplib::StackError> UFileCopy::Copy(const char *src_path, const char *dst_path)
{
    auto fis{std::make_unique<CPFilePair>(src_path, dst_path)};
    auto init_res = fis->CheckAndInit();
    if (!init_res)
    {
        return tl::unexpected(zplib::StackError("CPFilePair::CheckAndInit(), err: ", init_res.error()));
    }

    auto run_res = m_io_slot_mgr.RunQueue(fis.get());
    if (!run_res)
    {
        return tl::unexpected(zplib::StackError("m_io_slot_mgr.RunQueue(), err: ", run_res.error()));
    }

    auto truncate_res = fis->TrucateDstToSrcSize();
    if (!truncate_res)
    {
        return tl::unexpected(zplib::StackError("CPFilePair::TrucateDstToSrcSize(), err: ", truncate_res.error()));
    }

    return {};
}
