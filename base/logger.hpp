#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <atomic>

#define FMT_HEADER_ONLY
#include <fmt/core.h>
// spdlog adapter
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

template <typename... Args>
using format_string_t = fmt::format_string<Args...>;

// 日志级别
enum class Level : int
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
};

inline const char *LevelToStr(Level lvl)
{
    switch (lvl)
    {
    case Level::Trace:
        return "trace";
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info";
    case Level::Warn:
        return "warn";
    case Level::Error:
        return "error";
    case Level::Fatal:
        return "fatal";
    }
    return "unknown";
}

// JSON string escaping: handles ", \, \b, \f, \n, \r, \t
inline std::string EscapeJsonString(std::string_view s)
{
    std::string result;
    result.reserve(s.size() + s.size() / 4);
    for (char c : s)
    {
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                result += buf;
            }
            else
            {
                result += c;
            }
        }
    }
    return result;
}

inline std::string CurrentIsoTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

// 通用 Logger 基类：提供级别管理和模板包装器。
struct ILogger
{
    virtual ~ILogger() = default;

    // 设置/获取级别
    void set_level(Level l) noexcept { mLevel.store(static_cast<int>(l)); }
    void set_level(std::string_view level_str) noexcept
    {
        std::string lowered;
        lowered.reserve(level_str.size());
        std::transform(level_str.begin(), level_str.end(), std::back_inserter(lowered), ::tolower);
        if (lowered == "trace")
            set_level(Level::Trace);
        else if (lowered == "debug")
            set_level(Level::Debug);
        else if (lowered == "info")
            set_level(Level::Info);
        else if (lowered == "warn" || lowered == "warning")
            set_level(Level::Warn);
        else if (lowered == "error")
            set_level(Level::Error);
        else if (lowered == "fatal")
            set_level(Level::Fatal);
    }
    Level level() const noexcept { return static_cast<Level>(mLevel.load()); }

    bool enabled(Level l) const noexcept { return static_cast<int>(l) >= mLevel.load(); }

    // 模板包装器：先检查级别，只有在启用时才进行格式化（避免不必要开销）
    template <typename... Args>
    void log(Level l, format_string_t<Args...> fmt_str, Args &&...args)
    {
        if (!enabled(l))
            return;
        log_impl(l, fmt::format(fmt_str, std::forward<Args>(args)...));
    }

    // 方便方法
    template <typename... Args>
    void trace(format_string_t<Args...> fmt, Args &&...a) { log(Level::Trace, fmt, std::forward<Args>(a)...); }
    template <typename... Args>
    void debug(format_string_t<Args...> fmt, Args &&...a) { log(Level::Debug, fmt, std::forward<Args>(a)...); }
    template <typename... Args>
    void info(format_string_t<Args...> fmt, Args &&...a) { log(Level::Info, fmt, std::forward<Args>(a)...); }
    template <typename... Args>
    void warn(format_string_t<Args...> fmt, Args &&...a) { log(Level::Warn, fmt, std::forward<Args>(a)...); }
    template <typename... Args>
    void error(format_string_t<Args...> fmt, Args &&...a) { log(Level::Error, fmt, std::forward<Args>(a)...); }
    template <typename... Args>
    void fatal(format_string_t<Args...> fmt, Args &&...a) { log(Level::Fatal, fmt, std::forward<Args>(a)...); }

protected:
    // 派生类实现实际的输出（接收已格式化的字符串）
    virtual void log_impl(Level lvl, const std::string &msg) = 0;

private:
    std::atomic<int> mLevel{static_cast<int>(Level::Info)};
};

// 控制台实现 — 统一输出 NDJSON
struct ConsoleLogger : ILogger
{
    void log_impl(Level lvl, const std::string &msg) override
    {
        std::cerr << fmt::format("{{\"type\":\"program_log\",\"level\":\"{}\",\"msg\":\"{}\",\"timestamp\":\"{}\"}}\n",
                                 LevelToStr(lvl), EscapeJsonString(msg), CurrentIsoTimestamp());
    }
};

// Spdlog 适配器 — 统一输出 NDJSON
struct SpdLogger : ILogger
{
    explicit SpdLogger(std::shared_ptr<spdlog::logger> lg)
        : mLg(std::move(lg))
    {
    }

    void log_impl(Level lvl, const std::string &msg) override
    {
        if (!mLg)
            return;
        std::string json = fmt::format("{{\"type\":\"program_log\",\"level\":\"{}\",\"msg\":\"{}\",\"timestamp\":\"{}\"}}",
                                         LevelToStr(lvl), EscapeJsonString(msg), CurrentIsoTimestamp());
        mLg->log(spdlog::level::info, json);
    }

private:
    std::shared_ptr<spdlog::logger> mLg;
};
