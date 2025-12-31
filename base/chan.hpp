#pragma once
#include "base/base.hpp"
#include <tl/expected.hpp>
#include <condition_variable>
#include <mutex>
#include <deque>
#include <queue>
#include <atomic>
#include <unordered_set>

class CondVarGuard
{
public:
    explicit CondVarGuard(std::condition_variable &cv) : m_cv(cv) {}

    ~CondVarGuard()
    {
        m_cv.notify_one();
    }

private:
    std::condition_variable &m_cv;
};

template <typename ElemType>
class Channel
{
protected:
    static constexpr int CHANNEL_SIZE_DEFAULT = 1;
    static constexpr int MICRO_SLEEP_TIME = 1; // ms

    std::mutex mMutex;
    std::condition_variable mCondVar;
    std::queue<std::unique_ptr<ElemType>> mQueue;

    const int mSize;
    // initialize atomic_flag for C++17 (no default ctor prior to C++20)
    std::atomic_flag mDone = ATOMIC_FLAG_INIT;

public:
    explicit Channel(int len = CHANNEL_SIZE_DEFAULT) : mSize(len) {}

    int Size() const
    {
        return mQueue.size();
    }
    bool IsClosed() const
    {
        return mDone.test();
    }
    void Close()
    {
        mDone.test_and_set();
        mCondVar.notify_one();
    }

    tl::expected<std::unique_ptr<ElemType>, StackError> Pop()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mCondVar.wait(lock, [this]()
                      { return !mQueue.empty() || IsClosed(); });

        if (!mQueue.empty())
        {
            auto item = std::move(mQueue.front());
            mQueue.pop();
            assert(item != nullptr);
            return std::move(item);
        }
        else if (IsClosed())
        {
            return tl::unexpected(StackError("Channel is closed."));
        }
        else
        {
            return tl::unexpected(StackError("Unknown error in Channel::Pop()."));
        }
    }

    void Push(std::unique_ptr<ElemType> &item)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        CondVarGuard cv_guard(mCondVar);
        while (mQueue.size() >= static_cast<size_t>(mSize) && !IsClosed())
        {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(MICRO_SLEEP_TIME));
            lock.lock();
        }
        if (!IsClosed())
        {
            mQueue.push(std::move(item));
        }
        else
        {
            throw StackError("Channel closed");
        }
    }
};

class InotifyChannel
{
protected:
    static constexpr int MICRO_SLEEP_TIME = 1000; // ms

    std::mutex mMutex;
    std::condition_variable mCondVar;
    std::deque<std::string> mQueue;
    std::unordered_set<std::string> mQueueIdx; // for deduplication

    // initialize atomic_flag for C++17 (no default ctor prior to C++20)
    std::atomic_flag mDone = ATOMIC_FLAG_INIT;

public:
    explicit InotifyChannel() {}

    int Size() const
    {
        return mQueue.size();
    }
    bool IsClosed() const
    {
        return mDone.test();
    }
    void Close()
    {
        mDone.test_and_set();
        mCondVar.notify_one();
    }
    // Push with deduplication, ignore queue size limit, and won't block
    void Push(std::string_view item)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        CondVarGuard cv_guard(mCondVar);
        // deduplication check, using unordered_set for O(1) lookup
        if (mQueueIdx.find(std::string(item)) != mQueueIdx.end())
        {
            return; // item already exists, do not add
        }

        if (!IsClosed())
        {
            mQueueIdx.insert(std::string(item));
            mQueue.emplace_back(item);
        }
        else
        {
            throw StackError("Channel closed");
        }
    }

    tl::expected<std::string, StackError> Pop()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mCondVar.wait(lock, [this]()
                      { return !mQueue.empty() || IsClosed(); });

        if (!mQueue.empty())
        {
            auto item = std::move(mQueue.front());
            mQueue.pop_front();
            mQueueIdx.erase(item); // remove from deduplication set
            return item;
        }
        else if (IsClosed())
        {
            return tl::unexpected(StackError("Channel is closed."));
        }
        else
        {
            return tl::unexpected(StackError("Unknown error in Channel::Pop()."));
        }
    }
};

template <typename ElemType>
class DedupList
{
protected:
    std::list<std::shared_ptr<ElemType>> mList;
    std::unordered_set<std::string> mQueueIdx; // for deduplication

public:
    explicit DedupList() {}

    int Size() const
    {
        return mList.size();
    }
    bool Empty() const
    {
        return mList.empty();
    }

    // Push with deduplication, ignore queue size limit, and won't block
    void Push(std::shared_ptr<ElemType> &item, std::string_view key)
    {
        // deduplication check, using unordered_set for O(1) lookup
        if (mQueueIdx.find(std::string(key)) != mQueueIdx.end())
        {
            return; // item already exists, do not add
        }

        mQueueIdx.insert(std::string(key));
        mList.emplace_back(item);
    }

    void Remove(std::shared_ptr<ElemType> &item, std::string_view key)
    {
        mQueueIdx.erase(std::string(key));
        mList.remove(item);
    }

    const std::list<std::shared_ptr<ElemType>> &GetList()
    {
        return mList;
    }
};