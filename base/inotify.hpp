#pragma once

#include "base/base.hpp"
#include "base/chan.hpp"
#include <sys/inotify.h>
#include <filesystem>
#include <tl/expected.hpp>
#include <memory>

namespace fs = std::filesystem;

class Inotify
{
public:
    Inotify(const std::string &path, std::shared_ptr<ILogger> logger):m_logger(logger)
    {
        mRootPath = path;
        mFD = inotify_init();
    };
    ~Inotify()
    {
        if (mFD >= 0)
        {
            close(mFD);
        }
    };
    tl::expected<void, StackError> Init()
    {
        if (mFD < 0)
        {
            return tl::unexpected(StackError("inotify_init failed"));
        }
        return AddWatch(mRootPath);
    }
    tl::expected<void, StackError> AddWatch(const std::string &path)
    {

        // resursively add all directories under path
        int wd = inotify_add_watch(mFD, path.c_str(), mMask);
        if (wd < 0)
        {
            throw StackError(fmt::format("inotify_add_watch failed for path {}", path));
        }
        // begin recursive add
        for (const auto &entry : fs::recursive_directory_iterator(path))
        {
            if (entry.is_directory())
            {
                int cwd = inotify_add_watch(mFD, entry.path().c_str(), mMask);
                if (cwd < 0)
                {
                    throw StackError(
                        fmt::format("inotify_add_watch failed for path {}", entry.path().string()));
                }
            }
        }
        return {};
    }

    tl::expected<void, StackError> ReadEventToChannel(InotifyChannel &channel)
    {
        constexpr size_t EVENT_BUF_LEN = 1024 * (sizeof(struct inotify_event) + 16);
        char buffer[EVENT_BUF_LEN];
        ssize_t length = read(mFD, buffer, EVENT_BUF_LEN);
        if (length < 0)
        {
            return tl::unexpected(StackError("inotify read failed"));
        }
        size_t i = 0;
        while (i < static_cast<size_t>(length))
        {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];
            if (event->len > 0)
            {
                m_logger->debug("Inotify event: wd: {}, mask: {}, cookie: {}, len: {}, name: {}",
                                event->wd, event->mask, event->cookie, event->len, event->name);
                std::string entry(event->name);
                auto file_path = std::filesystem::canonical(fs::path(mRootPath) / entry);
                m_logger->debug("Inotify detected file change: {}", file_path.string());

                // 如果是目录，要添加到AddWatch中
                if ((event->mask & IN_ISDIR) && (event->mask & IN_CREATE))
                {
                    // 新建目录，添加watch
                    auto add_watch_res = AddWatch(file_path);
                    if (!add_watch_res)
                    {
                        m_logger->error("AddWatch failed for new directory {}", file_path.string());
                    }
                }
                m_logger->debug("Inotify push: {}", file_path.string());
                channel.Push(file_path.string());
            }
            i += sizeof(struct inotify_event) + event->len;
        }
        return {};
    }

private:
    int mFD = -1;
    std::string mRootPath;
    int mMask = IN_CLOSE_WRITE | IN_CREATE;
    std::shared_ptr<ILogger> m_logger;
};