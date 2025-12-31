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



class AIOSlotMgr : public IOSlotMgr<IOSlot>
{
public:
    AIOSlotMgr(const RWCombinedCopyOptions &options, CPFilePairMgr *file_pair_mgr)
        : IOSlotMgr<IOSlot>(options, file_pair_mgr)
    {
    }
    ~AIOSlotMgr() override
    {
        io_destroy(m_io_ctx);
    }

private:
    // implement virtual functions from IOSlotMgr
    tl::expected<void, StackError> Init() override;
    void PrepareOneRead(IOSlot *slot, off_t offset, std::shared_ptr<CPFilePair> currCPFPIt) override;
    void PrepareOneWrite(IOSlot *slot, off_t offset) override;
    tl::expected<void, StackError> SubmitOneRead(IOSlot *slot) override;
    tl::expected<void, StackError> SubmitOneWrite(IOSlot *slot) override;
    tl::expected<void, StackError> IOReap() override;    
    // tl::expected<void, StackError> CheckOneCompleted(AIOSlot *slot) override;

    // reap helpers
    tl::expected<void, StackError> ReapRead(struct io_event *ev);
    tl::expected<void, StackError> ReapWrite(struct io_event *ev);

private:
    io_context_t m_io_ctx = 0;
};

