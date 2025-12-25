#pragma once

#include <cstring> // bzero
#include <liburing.h>
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

class UIOSlot : public IOSlot
{
public:
    using IOSlot::IOSlot;
    UIOSlot(size_t buf_size, int id) : IOSlot(buf_size, id) {}
    ~UIOSlot() override = default;
};

class UIOSlotMgr
{
public:
    UIOSlotMgr(const RWCombinedCopyOptions &options)
        : m_options(options), m_file_infos(nullptr), m_logger(zplib::GetGlobalLogger())
    {
        size_t slot_count = options.QueueDepth;
        size_t buf_size = options.IoSize;
        m_slots.reserve(slot_count);

        m_logger->debug("UIOSlotMgr created with QueueDepth: {}, IoSize: {}", slot_count, buf_size);

        for (size_t i = 0; i < slot_count; ++i)
        {
            m_slots.push_back(new UIOSlot(buf_size, i));
        }
    }
    ~UIOSlotMgr()
    {
        io_uring_queue_exit(&m_ring);
        for (auto slot : m_slots)
        {
            delete slot;
        }
        m_slots.clear();
    }

    tl::expected<void, zplib::StackError> RunQueue(CPFilePair *file_infos);
    tl::expected<void, zplib::StackError> CheckStuck();
    void SetFuncDurationStat(zplib::FuncDurationStat *stat)
    {
        m_func_duration_stat = stat;
    }

private:
    tl::expected<void, zplib::StackError> Init();
    void PrepareOneRead(UIOSlot *slot, off_t offset);
    void PrepareOneWrite(UIOSlot *slot, off_t offset);
    tl::expected<void, zplib::StackError> SubmitOneRead(UIOSlot *slot);
    tl::expected<void, zplib::StackError> SubmitOneWrite(UIOSlot *slot);
    tl::expected<int, zplib::StackError> SubmitReads();
    tl::expected<int, zplib::StackError> SubmitWrites();
    tl::expected<void, zplib::StackError> IOReap();
    tl::expected<void, zplib::StackError> ReapRead(struct io_uring_cqe *cqe);
    tl::expected<void, zplib::StackError> ReapWrite(struct io_uring_cqe *cqe);
    void PrtSlots();
    void Reset()
    {
        for (auto slot : m_slots)
        {
            slot->Reset();
        }
    }
    bool ShouldStop() { return io_completed >= io_cnt_total; }
    void AddDurationWarn(std::string_view func_name, std::chrono::_V2::system_clock::time_point start,
                         std::chrono::_V2::system_clock::time_point end, int64_t warn_threshold);

private:
    std::vector<UIOSlot *> m_slots;
    struct io_uring m_ring;
    size_t io_cnt_total = 0;
    size_t io_completed = 0;
    size_t io_read_submitted = 0;

    RWCombinedCopyOptions m_options;
    CPFilePair *m_file_infos;

    std::shared_ptr<spdlog::logger> m_logger;
    zplib::FuncDurationStat *m_func_duration_stat{
        nullptr};

    // flow control / diagnostics
    int64_t last_read_submit_duration = 0;
    int64_t last_write_submit_duration = 0;
};

class UFileCopy
{
public:
    UFileCopy(const RWCombinedCopyOptions &options) : m_options(options), m_io_slot_mgr(options) {};

    tl::expected<void, zplib::StackError> Copy(const char *src_path, const char *dst_path);
    void SetFuncDurationStat(zplib::FuncDurationStat *stat)
    {
        m_io_slot_mgr.SetFuncDurationStat(stat);
    }

private:
    RWCombinedCopyOptions m_options;
    UIOSlotMgr m_io_slot_mgr;
};
