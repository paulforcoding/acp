#include "base/base.hpp"
#include <new> // std::align_val_t, std::nothrow

char *AllocBytes(size_t align, size_t bytes)
{
    auto *buf = new (std::align_val_t(align), std::nothrow) char[bytes]{0};
    if (buf == nullptr)
    {
        throw AcpException("AllocBytes() failed");
    }
    return buf;
}

// std::string g_global_log_name;
// void InitGlobalConsoleLogger(spdlog::level::level_enum log_level)
// {
//     g_global_log_name = "console";
//     auto logger = spdlog::stdout_color_mt(g_global_log_name);
//     logger->set_level(log_level);
// }
// void InitGlobalFileLogger(const std::string &filePath, spdlog::level::level_enum log_level)
// {
//     g_global_log_name = "file";
//     auto logger = spdlog::basic_logger_mt(g_global_log_name, filePath);
//     logger->set_level(log_level);
// }
// std::shared_ptr<spdlog::logger> GetGlobalLogger()
// {
//     return spdlog::get(g_global_log_name);
// }