#pragma once
#define FMT_HEADER_ONLY
#include <cstddef> // size_t
#include <stdexcept>
#include <list>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h> // 需要包含此头文件以支持彩色控制台输出
#include <spdlog/sinks/basic_file_sink.h>
#include <fmt/format.h>

#define SECTORSIZE 512
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))
#define ALIGN(x, a) ALIGN_MASK(x, (typeof(x))(a) - 1)

namespace zplib
{

    class ZPExcetion : public std::runtime_error
    {
    private:
        /* data */
    public:
        ZPExcetion(std::string_view msg) : std::runtime_error(msg.data()) {};
    };

    char *AllocBytes(size_t align, size_t bytes);

    template <typename T, typename = typename std::is_pointer<T>>
    void FreeBytes(T &p)
    {
        delete[] p;
        p = nullptr;
    }

    class StackError
    {
    public:
        StackError(std::string_view msg) { m_stack.push_back(std::string(msg)); }
        StackError(std::string_view msg, const StackError &prev)
        {
            m_stack = prev.m_stack;
            m_stack.push_back(std::string(msg));
        }
        const char *what() const
        {
            static std::string full_msg;
            for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it)
            {
                if (it == m_stack.rbegin())
                {
                    full_msg += *it + "\n";
                }
                else
                {
                    full_msg += "  caused by: " + *it + "\n";
                }
                // full_msg += *it + "\n";
            }
            return full_msg.c_str();
        }

        // 实现 == 运算符重载， 判断StackError是否相等
        bool operator==(const StackError &other) const
        {
            if (m_stack.size() != other.m_stack.size())
            {
                return false;
            }
            auto it1 = m_stack.begin();
            auto it2 = other.m_stack.begin();
            while (it1 != m_stack.end() && it2 != other.m_stack.end())
            {
                if (*it1 != *it2)
                {
                    return false;
                }
                ++it1;
                ++it2;
            }
            return true;
        }

    private:
        std::list<std::string> m_stack;
    };

    // Logger functions
    void InitGlobalConsoleLogger(spdlog::level::level_enum log_level);
    void InitGlobalFileLogger(const std::string &filePath, spdlog::level::level_enum log_level);
    std::shared_ptr<spdlog::logger> GetGlobalLogger();

    // 通用的，记录各个函数运行时间的类
    class FuncDurationStat
    {
    public:
        FuncDurationStat()
        {
            m_logger = zplib::GetGlobalLogger();
        };
        // duration in ms
        void AddDuration(std::string_view func_name, int64_t duration)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stats[std::string(func_name)].push_back(duration);
        };
        void AddDuration(std::string_view func_name, std::chrono::_V2::system_clock::time_point start,
                         std::chrono::_V2::system_clock::time_point end)
        {
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            AddDuration(func_name, duration);
        };
        void PrintStats()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_logger->info("Function Duration Statistics:");
            for (const auto &pair : m_stats)
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
                m_logger->info("Function: {}, Count: {}, Avg: {:.2f} ms, Min: {} ms, Max: {} ms",
                               func_name, durations.size(), avg_duration, min_duration, max_duration);
            }
        }

    private:
        // key: function name, value: list of durations (unit in user-defined, e.g., microseconds)
        std::unordered_map<std::string, std::list<int64_t>> m_stats;
        std::mutex m_mutex;
        std::shared_ptr<spdlog::logger> m_logger;
    };
} // namespace zplib


