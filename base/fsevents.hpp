#pragma once

#ifdef __APPLE__

#include "base/base.hpp"
#include "base/chan.hpp"
#include <CoreServices/CoreServices.h>
#include <filesystem>
#include <tl/expected.hpp>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>

namespace fs = std::filesystem;

class FSEventsWatcher
{
public:
    FSEventsWatcher(const std::string &path, std::shared_ptr<ILogger> logger)
        : mRootPath(path), mLogger(logger)
    {
    }
    ~FSEventsWatcher()
    {
        Close();
    }

    tl::expected<void, StackError> Init()
    {
        CFStringRef pathRef = CFStringCreateWithCString(
            kCFAllocatorDefault, mRootPath.c_str(), kCFStringEncodingUTF8);
        if (!pathRef)
        {
            return tl::unexpected(StackError("Failed to create CFString for path"));
        }

        CFArrayCallBacks callbacks = kCFTypeArrayCallBacks;
        CFArrayRef pathsToWatch = CFArrayCreate(
            kCFAllocatorDefault, (const void **)&pathRef, 1, &callbacks);
        CFRelease(pathRef);
        if (!pathsToWatch)
        {
            return tl::unexpected(StackError("Failed to create CFArray for paths"));
        }

        FSEventStreamContext context = {0, this, nullptr, nullptr, nullptr};

        mStream = FSEventStreamCreate(
            kCFAllocatorDefault,
            &FSEventsWatcher::FSEventsCallback,
            &context,
            pathsToWatch,
            kFSEventStreamEventIdSinceNow,
            0.5, // latency in seconds
            kFSEventStreamCreateFlagFileEvents);

        CFRelease(pathsToWatch);

        if (!mStream)
        {
            return tl::unexpected(StackError("FSEventStreamCreate failed"));
        }

        mDispatchQueue = dispatch_queue_create("acp.fsevents", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(mStream, mDispatchQueue);

        if (!FSEventStreamStart(mStream))
        {
            FSEventStreamRelease(mStream);
            mStream = nullptr;
            dispatch_release(mDispatchQueue);
            mDispatchQueue = nullptr;
            return tl::unexpected(StackError("FSEventStreamStart failed"));
        }

        return {};
    }

    tl::expected<void, StackError> AddWatch(const std::string &path)
    {
        // FSEvents 天然支持递归监控，无需手动添加子目录
        (void)path;
        return {};
    }

    tl::expected<void, StackError> ReadEventToChannel(InotifyChannel &channel)
    {
        if (mClosed)
        {
            return tl::unexpected(StackError("FSEventsWatcher is closed", EBADF));
        }

        std::vector<std::string> events;
        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(1);
            mQueueCV.wait_until(lock, deadline, [this] {
                return !mEventQueue.empty() || mClosed;
            });

            while (!mEventQueue.empty())
            {
                events.push_back(mEventQueue.front());
                mEventQueue.pop();
            }
        }

        for (auto &ev : events)
        {
            mLogger->debug("FSEvents push: {}", ev);
            channel.Push(std::move(ev));
        }
        return {};
    }

    void Close()
    {
        if (mClosed)
            return;
        mClosed = true;

        if (mStream)
        {
            FSEventStreamStop(mStream);
            FSEventStreamInvalidate(mStream);
            FSEventStreamRelease(mStream);
            mStream = nullptr;
        }

        if (mDispatchQueue)
        {
            dispatch_release(mDispatchQueue);
            mDispatchQueue = nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            mQueueCV.notify_all();
        }
    }

    bool IsClosed() const { return mClosed; }

private:
    static void FSEventsCallback(
        ConstFSEventStreamRef streamRef,
        void *clientCallBackInfo,
        size_t numEvents,
        void *eventPaths,
        const FSEventStreamEventFlags eventFlags[],
        const FSEventStreamEventId eventIds[])
    {
        (void)streamRef;
        (void)eventFlags;
        (void)eventIds;
        FSEventsWatcher *watcher = static_cast<FSEventsWatcher *>(clientCallBackInfo);
        char **paths = static_cast<char **>(eventPaths);

        for (size_t i = 0; i < numEvents; ++i)
        {
            if (paths[i])
            {
                std::string path(paths[i]);
                watcher->mLogger->debug("FSEvents detected file change: {}", path);
                std::lock_guard<std::mutex> lock(watcher->mQueueMutex);
                watcher->mEventQueue.push(std::move(path));
                watcher->mQueueCV.notify_one();
            }
        }
    }

private:
    std::string mRootPath;
    std::shared_ptr<ILogger> mLogger;
    FSEventStreamRef mStream = nullptr;
    dispatch_queue_t mDispatchQueue = nullptr;

    std::mutex mQueueMutex;
    std::condition_variable mQueueCV;
    std::queue<std::string> mEventQueue;
    bool mClosed = false;
};

#endif // __APPLE__
