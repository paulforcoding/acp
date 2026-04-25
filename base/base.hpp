#pragma once
#define FMT_HEADER_ONLY
#include <cstddef> // size_t
#include <stdexcept>
#include <list>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h> // 需要包含此头文件以支持彩色控制台输出
#include <spdlog/sinks/basic_file_sink.h>
#include <fmt/format.h>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <cstring> // strerror
#include <cerrno>
#include <tl/expected.hpp>
#include "base/logger.hpp"

#define SECTORSIZE 512
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))
#define ALIGN(x, a) ALIGN_MASK(x, (decltype(x))(a) - 1)

class AcpException : public std::runtime_error
{
private:
    /* data */
public:
    AcpException(std::string_view msg) : std::runtime_error(msg.data()) {};
};

char *AllocBytes(size_t align, size_t bytes);

template <typename T, typename = typename std::is_pointer<T>>
void FreeBytes(T &p)
{
    delete[] p;
    p = nullptr;
}

template <typename... Args>
using format_string_t = fmt::format_string<Args...>;

// 此class是结合tl::expected使用的，用于串联函数调用栈各函数的返回值，not thread-safe
class StackError
{
public:
    StackError(std::string_view msg, int code = 0) : mCode(code)
    {
        mStack.emplace_back(std::string(msg));
        BuildFullMsg();
    }

    StackError(std::string_view msg, const StackError &prev, int code = 0)
        : mStack(prev.mStack), mCode(code ? code : prev.mCode)
    {
        mStack.emplace_back(std::string(msg));
        BuildFullMsg();
    }

    // convenience static methods
    static StackError FromErrno(int errnum)
    {
        return StackError(fmt::format("{}: {}", strerror(errnum), errnum), errnum);
    }

    template <typename... Args>
    static StackError FromFormat(format_string_t<Args...> fmt_str, Args &&...args)
    {
        auto msg = fmt::format(fmt_str, std::forward<Args>(args)...);
        return StackError(msg);
    }

    template <typename... Args>
    void Append(format_string_t<Args...> fmt_str, Args &&...args)
    {
        auto msg = fmt::format(fmt_str, std::forward<Args>(args)...);
        mStack.emplace_back(msg);
        BuildFullMsg();
    }

    int Code() const noexcept { return mCode; }

    const char *ToString() const noexcept
    {
        return mFullMsg.c_str();
    }

private:
    void BuildFullMsg()
    {
        mFullMsg.clear();
        for (auto it = mStack.rbegin(); it != mStack.rend(); ++it)
        {
            if (it == mStack.rbegin())
                mFullMsg += *it;
            else
                mFullMsg += std::string("  caused by: ") + *it;
            mFullMsg += '\n';
        }
    }

    bool operator==(const StackError &other) const
    {
        return mCode == other.mCode && mStack == other.mStack;
    }

private:
    std::vector<std::string> mStack;
    int mCode = 0;                 // lastest error code, 0 if not applicable
    std::string mFullMsg; // cached what() result
};

inline tl::unexpected<StackError> Unexpt(const StackError &se)
{
    return tl::unexpected(se);
}

// // Logger functions
// void InitGlobalConsoleLogger(spdlog::level::level_enum log_level);
// void InitGlobalFileLogger(const std::string &filePath, spdlog::level::level_enum log_level);
// std::shared_ptr<spdlog::logger> GetGlobalLogger();

// 通用的，记录各个函数运行时间的类
class FuncDurationStat
{
public:
    FuncDurationStat(std::shared_ptr<ILogger> logger) : mLogger(logger) {}
    // duration in microseconds
    void AddDuration(std::string_view func_name, int64_t duration)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mStats[std::string(func_name)].push_back(duration);
    };
    void AddDuration(std::string_view func_name, std::chrono::system_clock::time_point start,
                     std::chrono::system_clock::time_point end)
    {
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        AddDuration(func_name, duration);
    };
    void PrintStats()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mLogger->info("Function Duration Statistics:");
        for (const auto &pair : mStats)
        {
            const std::string &func_name = pair.first;
            const std::list<int64_t> &durations = pair.second;
            if (durations.empty())
            {
                continue;
            }
            int64_t total = 0;
            int64_t min_duration = INT64_MAX;
            int64_t max_duration = INT64_MIN;
            for (const auto &d : durations)
            {
                total += d;
                if (d < min_duration)
                {
                    min_duration = d;
                }
                if (d > max_duration)
                {
                    max_duration = d;
                }
            }
            double avg_duration = static_cast<double>(total) / durations.size();
            mLogger->info("Function: {}, Count: {}, Avg: {:.2f} us, Min: {} us, Max: {} us",
                          func_name, durations.size(), avg_duration, min_duration, max_duration);
        }
    }

private:
    // key: function name, value: list of durations (unit in user-defined, e.g., microseconds)
    std::unordered_map<std::string, std::list<int64_t>> mStats;
    std::mutex mMutex;
    std::shared_ptr<ILogger> mLogger;
};
