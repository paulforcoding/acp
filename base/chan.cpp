#include "base/chan.hpp"
#include "lib/combined/combined.hpp"
#include <cassert>

void FPChannel::Push(std::shared_ptr<CPFilePair> item)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mPending.push_back(std::move(item));
    mCv.notify_one();
}

void FPChannel::Close()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mClosed = true;
    mCv.notify_all();
}

std::shared_ptr<CPFilePair> FPChannel::PeekFront()
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mPending.empty())
        return nullptr;
    return mPending.front();
}

void FPChannel::PopFront()
{
    std::lock_guard<std::mutex> lock(mMutex);
    assert(!mPending.empty());
    mPending.pop_front();
}

void FPChannel::MoveToInflight(std::shared_ptr<CPFilePair> pFP)
{
    std::lock_guard<std::mutex> lock(mMutex);
    assert(!mPending.empty() && mPending.front() == pFP);
    mPending.pop_front();
    mInflight.push_back(pFP);
    pFP->MarkAsInflight();
}

void FPChannel::RemoveFromInflight(std::shared_ptr<CPFilePair> pFP)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mInflight.remove(pFP);
    pFP->MarkAsCompleted();
    mCv.notify_all();
}

bool FPChannel::WaitForWorkOrClose(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mMutex);
    mCv.wait_for(lock, timeout, [this]
                 { return mClosed || !mPending.empty(); });
    return !(mClosed && mPending.empty() && mInflight.empty());
}

bool FPChannel::HasPendingWork() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return !mPending.empty() || !mInflight.empty();
}

bool FPChannel::Empty() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mPending.empty();
}

bool FPChannel::IsClosed() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mClosed;
}

size_t FPChannel::PendingCount() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mPending.size();
}

size_t FPChannel::InflightCount() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mInflight.size();
}
