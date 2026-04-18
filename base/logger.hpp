#pragma once

#include <iostream>
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

// 通用 Logger 基类：提供级别管理和模板包装器。
struct ILogger
{
    virtual ~ILogger() = default;

    // 设置/获取级别
    void set_level(Level l) noexcept { mLevel.store(static_cast<int>(l)); }
    void set_level(std::string_view level_str) noexcept
    {
        std::transform(level_str.begin(), level_str.end(), std::string().begin(), ::tolower);
        if (level_str == "trace")
            set_level(Level::Trace);
        else if (level_str == "debug")
            set_level(Level::Debug);
        else if (level_str == "info")
            set_level(Level::Info);
        else if (level_str == "warn" || level_str == "warning")
            set_level(Level::Warn);
        else if (level_str == "error")
            set_level(Level::Error);
        else if (level_str == "fatal")
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

// 控制台实现
struct ConsoleLogger : ILogger
{
    void log_impl(Level lvl, const std::string &msg) override
    {
        const char *prefix = nullptr;
        switch (lvl)
        {
        case Level::Trace:
            prefix = "[TRACE] ";
            break;
        case Level::Debug:
            prefix = "[DEBUG] ";
            break;
        case Level::Info:
            prefix = "[INFO]  ";
            break;
        case Level::Warn:
            prefix = "[WARN]  ";
            break;
        case Level::Error:
            prefix = "[ERROR] ";
            break;
        case Level::Fatal:
            prefix = "[FATAL] ";
            break;
        }
        std::cerr << prefix << msg << '\n';
    }
};

// Spdlog 适配器：将 ILogger 的调用转发到 std::shared_ptr<spdlog::logger>
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
        switch (lvl)
        {
        case Level::Trace:
            mLg->trace(msg);
            break;
        case Level::Debug:
            mLg->debug(msg);
            break;
        case Level::Info:
            mLg->info(msg);
            break;
        case Level::Warn:
            mLg->warn(msg);
            break;
        case Level::Error:
            mLg->error(msg);
            break;
        case Level::Fatal:
            mLg->critical(msg);
            break;
        }
    }

private:
    std::shared_ptr<spdlog::logger> mLg;
};