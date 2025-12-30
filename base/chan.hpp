#pragma once
#include "base/base.hpp"
#include <condition_variable>
#include <mutex>
#include <queue>
#include <atomic>

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

template <typename T, typename QType = std::queue<std::unique_ptr<T>>>
class Channel
{
private:
    static constexpr int CHANNEL_SIZE_DEFAULT = 1;
    static constexpr int MICRO_SLEEP_TIME = 1; // ms

    std::mutex m_mutex;
    std::condition_variable m_cond_var;
    // std::queue<std::unique_ptr<T>> m_queue;
    QType m_queue;
    const int m_len;
    // initialize atomic_flag for C++17 (no default ctor prior to C++20)
    std::atomic_flag m_done = ATOMIC_FLAG_INIT;

public:
    explicit Channel(int len = CHANNEL_SIZE_DEFAULT) : m_len(len) {}

    int Size() const
    {
        return m_queue.size();
    }
    bool IsClosed() const
    {
        return m_done.test();
    }
    void Close()
    {
        m_done.test_and_set();
        m_cond_var.notify_one();
    }

    tl::expected<std::unique_ptr<T>, zplib::StackError> Pop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond_var.wait(lock, [this]()
                        { return !m_queue.empty() || IsClosed(); });

        if (!m_queue.empty())
        {
            auto item = std::move(m_queue.front());
            m_queue.pop();
            assert(item != nullptr);
            return std::move(item);
        }
        else if (IsClosed())
        {
            return tl::unexpected(zplib::StackError("Channel is closed."));
        }
        else
        {
            return tl::unexpected(zplib::StackError("Unknown error in Channel::Pop()."));
        }
    }

    void Push(std::unique_ptr<T> &item)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        CondVarGuard cv_guard(m_cond_var);
        while (m_queue.size() >= static_cast<size_t>(m_len) && !IsClosed())
        {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(MICRO_SLEEP_TIME));
            lock.lock();
        }
        if (!IsClosed())
        {
            m_queue.push(std::move(item));
        }
        else
        {
            throw zplib::StackError("Channel closed");
        }
    }

    // Push with deduplication, ignore queue size limit, and won't block
    void PushUnique(std::unique_ptr<T> &item)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        CondVarGuard cv_guard(m_cond_var);
        // deduplication check
        for (auto &existing_item : m_queue)
        {
            if (*existing_item == *item)
            {
                return; // item already exists, do not add
            }
        }
        if (!IsClosed())
        {
            m_queue.push_front(std::move(item));
        }
        else
        {
            throw zplib::StackError("Channel closed");
        }
    }

    tl::expected<std::unique_ptr<T>, zplib::StackError> PopUnique()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond_var.wait(lock, [this]()
                        { return !m_queue.empty() || IsClosed(); });

        if (!m_queue.empty())
        {
            auto item = std::move(m_queue.front());
            m_queue.pop_back();
            assert(item != nullptr);
            return std::move(item);
        }
        else if (IsClosed())
        {
            return tl::unexpected(zplib::StackError("Channel is closed."));
        }
        else
        {
            return tl::unexpected(zplib::StackError("Unknown error in Channel::Pop()."));
        }
    }
};