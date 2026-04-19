#pragma once
#include "base/base.hpp"
#include <tl/expected.hpp>
#include <condition_variable>
#include <mutex>
#include <deque>
#include <queue>
#include <atomic>
#include <unordered_set>
#include <list>
#include <chrono>

class CondVarGuard
{
public:
    explicit CondVarGuard(std::condition_variable &cv) : mCv(cv) {}

    ~CondVarGuard()
    {
        mCv.notify_one();
    }

private:
    std::condition_variable &mCv;
};

template <typename ElemType>
class Channel
{
protected:
    static constexpr int CHANNEL_SIZE_DEFAULT = 1;
    static constexpr int MICRO_SLEEP_TIME = 1; // ms

    mutable std::mutex mMutex;
    std::condition_variable mCondVar;
    std::queue<std::unique_ptr<ElemType>> mQueue;

    const int mSize;
    // initialize atomic_flag for C++17 (no default ctor prior to C++20)
    std::atomic_flag mDone = ATOMIC_FLAG_INIT;

public:
    explicit Channel(int len = CHANNEL_SIZE_DEFAULT) : mSize(len)
    {
        if (len <= 0)
        {
            throw std::invalid_argument("Channel size must be greater than 0");
        }
    }

    int Size() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
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

    mutable std::mutex mMutex;
    std::condition_variable mCondVar;
    std::deque<std::string> mQueue;
    std::unordered_set<std::string> mQueueIdx; // for deduplication

    // initialize atomic_flag for C++17 (no default ctor prior to C++20)
    std::atomic_flag mDone = ATOMIC_FLAG_INIT;

public:
    explicit InotifyChannel() {}

    int Size() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
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
    mutable std::mutex mMutex;
    std::list<std::shared_ptr<ElemType>> mList;
    std::unordered_set<std::string> mQueueIdx; // for deduplication

public:
    explicit DedupList() {}

    int Size() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mList.size();
    }
    bool Empty() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mList.empty();
    }

    // Push with deduplication, ignore queue size limit, and won't block
    void Push(std::shared_ptr<ElemType> &item, std::string_view key)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        // deduplication check, using unordered_set for O(1) lookup
        if (mQueueIdx.find(std::string(key)) != mQueueIdx.end())
        {
            // std::cout << "Deduplication: " << std::string(key) << std::endl;
            return; // item already exists, do not add
        }

        mQueueIdx.insert(std::string(key));
        mList.emplace_back(item);
    }

    void Remove(std::shared_ptr<ElemType> &item, std::string_view key)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQueueIdx.erase(std::string(key));
        mList.remove(item);
    }

    const std::list<std::shared_ptr<ElemType>> &GetList()
    {
        return mList;
    }
};

class CPFilePair;

// 专用于 CPFilePairMgr 的文件对通道
// 管理 CPFilePair 从"待读"→"读完成等写"→"全部完成"的完整生命周期
class FPChannel
{
public:
    FPChannel() = default;

    // === 生产者 ===
    void Push(std::shared_ptr<CPFilePair> item);
    void Close(); // 标记关闭，唤醒所有等待线程

    // === 消费者：读取阶段 ===
    std::shared_ptr<CPFilePair> PeekFront(); // 查看队首（不弹出），空返回 nullptr
    void PopFront();                         // 确认队首处理完毕，弹出

    // === 消费者：状态转换 ===
    void MoveToInflight(std::shared_ptr<CPFilePair> pFP);      // pending → inflight
    void RemoveFromInflight(std::shared_ptr<CPFilePair> pFP);  // inflight → 完成

    // === 阻塞等待 ===
    // 返回 true：有新工作来了或还有 inflight，继续干活
    // 返回 false：通道已关闭且没有待读/无 inflight，该退出了
    bool WaitForWorkOrClose(std::chrono::milliseconds timeout);

    // === 查询 ===
    bool HasPendingWork() const; // pending 非空 或 inflight 非空
    bool Empty() const;           // pending 为空
    bool IsClosed() const;
    size_t PendingCount() const;
    size_t InflightCount() const;

private:
    mutable std::mutex mMutex;
    std::condition_variable mCv;
    std::deque<std::shared_ptr<CPFilePair>> mPending;
    std::list<std::shared_ptr<CPFilePair>> mInflight;
    bool mClosed = false;
};