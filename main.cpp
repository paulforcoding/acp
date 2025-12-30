#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <algorithm>
#include "lib/acp/acp.hpp"
#include "lib/thirdparty/json.hpp"
#include "base/base.hpp"
#include "base/chan.hpp"
#include "base/inotify.hpp"

std::optional<RWCombinedCopyOptions> LoadCopyOptions(const std::string &config_path)
{
    std::ifstream f(config_path);
    if (!f.is_open())
    {
        return std::nullopt;
    }

    using json = nlohmann::json;
    json data = json::parse(f);

    // TODO: 如果写错配置项，这几行代码会直接coredump，要研究一下如何报错
    RWCombinedCopyOptions options;
    options.IoSize = data["CopyOptions"]["IOSize"];
    options.QueueDepth = data["CopyOptions"]["QueueDepth"];
    options.Batch = data["CopyOptions"]["Batch"];
    options.IOReapWait = data["CopyOptions"]["IOReapWait"];
    options.LogLevel = data["LogLevel"];
    options.LogMode = data["LogMode"];
    options.LogFilePath = data["LogFilePath"];
    options.CopyEngine = data["CopyEngine"];
    options.CopyParallelism = data["CopyParallelism"];
    options.EnableInotify = data["EnableInotify"];
    options.PreserveSparseFiles = data["PreserveSparseFiles"];

    return options;
}

void InitGlobalLogger(const RWCombinedCopyOptions &options)
{
    if (options.LogMode == "console")
    {
        zplib::InitGlobalConsoleLogger(spdlog::level::from_str(options.LogLevel));
    }
    else if (options.LogMode == "file")
    {
        zplib::InitGlobalFileLogger(options.LogFilePath, spdlog::level::from_str(options.LogLevel));
    }
    else
    {
        // default to console
        zplib::InitGlobalConsoleLogger(spdlog::level::from_str(options.LogLevel));
    }
}

int main(int argc, char *argv[])
{
    namespace fs = std::filesystem;

    auto options_opt = LoadCopyOptions("./acp_config.json");
    if (!options_opt)
    {
        options_opt = LoadCopyOptions("/etc/acp_config.json");
        if (!options_opt)
        {
            std::cerr << "Failed to load configuration from ./acp_config.json or /etc/acp_config.json" << std::endl;
            return 1;
        }
    }
    auto options = options_opt.value();

    InitGlobalLogger(options);

    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <src_path> <dst_path>" << std::endl;
        std::cerr << "\t All IO option should be written in ./acp_config.json or /etc/acp_config.json" << std::endl;
        return 1;
    }

    const char *src_path = argv[1];
    const char *dst_path = argv[2];

    // check source path and destination path, if source path or destination path not exist, exit directly
    if (!fs::exists(src_path))
    {
        std::cerr << "Source path does not exist: " << src_path << std::endl;
        return 1;
    }
    if (!fs::exists(dst_path) && !fs::is_regular_file(src_path))
    {
        std::cerr << "Destination path does not exist: " << dst_path << std::endl;
        return 1;
    }

    // if (fs::is_directory(src_path) && fs::is_regular_file(dst_path))
    // {
    //     std::cerr << "When source path is a directory, destination path cannot be a file." << std::endl;
    //     return 1;
    // }

    // check source path and destination path are not the same
    if (fs::equivalent(src_path, dst_path))
    {
        std::cerr << "Source path and destination path cannot be the same." << std::endl;
        return 1;
    }
    // check destination path is writable
    std::error_code ec;
    fs::perms p = fs::status(dst_path, ec).permissions();
    if (ec)
    {
        std::cerr << "Failed to get permissions of destination path: " << dst_path << ", errstr: " << ec.message() << std::endl;
        return 1;
    }
    if ((p & fs::perms::owner_write) == fs::perms::none &&
        (p & fs::perms::group_write) == fs::perms::none &&
        (p & fs::perms::others_write) == fs::perms::none)
    {
        std::cerr << "Destination path is not writable: " << dst_path << std::endl;
        return 1;
    }

    // convert src and dst path to canonical path
    fs::path src_p = fs::canonical(src_path);
    fs::path dst_p = fs::canonical(dst_path);
    // check src and dst are not subdirectory of each other when both are directories
    if (fs::is_directory(src_path) && fs::is_directory(dst_path))
    {
        // use the 4-iterator overload of std::mismatch to compare path components safely
        // src_is_prefix_of_dst == true means dst is inside src (or equal)
        bool src_is_prefix_of_dst = std::mismatch(src_p.begin(), src_p.end(), dst_p.begin(), dst_p.end()).first == src_p.end();
        bool dst_is_prefix_of_src = std::mismatch(dst_p.begin(), dst_p.end(), src_p.begin(), src_p.end()).first == dst_p.end();

        if (src_is_prefix_of_dst || dst_is_prefix_of_src)
        {
            std::cerr << "Source path and destination path cannot be subdirectory of each other." << std::endl;
            std::cerr << "Source path: " << src_p.string() << std::endl;
            std::cerr << "Destination path: " << dst_p.string() << std::endl;
            return 1;
        }
    }

    auto logger = zplib::GetGlobalLogger();

    // deal with inotify if enabled
    InotifyChannel iChan;
    Inotify inotifyWatcher(src_p.string());
    std::jthread inotifyThread;
    if (options.EnableInotify && fs::is_directory(src_path) && fs::is_directory(dst_path))
    {

        auto add_watch_res = inotifyWatcher.Init();
        if (!add_watch_res)
        {
            std::cerr << fmt::format("Failed to add inotify init for path: {}, err: {}", src_p.string(), add_watch_res.error().what());
            return 1;
        }
        // start a thread to read inotify events and push to copyChannel
        inotifyThread = std::jthread(
            [&inotifyWatcher, &iChan]()
            {
                auto logger = zplib::GetGlobalLogger();
                while (true)
                {
                    auto read_res = inotifyWatcher.ReadEventToChannel(iChan);
                    if (!read_res)
                    {

                        logger->error("Inotify read event failed: {}", read_res.error().what());
                    }
                }
            });
    }

    Channel<CopyEntry> copyChannel(1024);

    auto funcDurationStat = zplib::FuncDurationStat{};

    AIOFileCopy file_copier(options);
    // start a thread to run AIOFileCopy
    std::thread file_copy_thread(
        [&file_copier, &copyChannel, &funcDurationStat]()
        { 
    auto copy_res = file_copier.RunChannel(&funcDurationStat, copyChannel); 
    if (!copy_res)
    {
        auto logger = zplib::GetGlobalLogger();
        // std::cerr << "File copy failed: " << copy_res.error().what() << std::endl;
        logger->error("File copy failed: {}", copy_res.error().what());        
    } });

    // recursively walk through source directory and prepare file pairs
    if (fs::is_directory(src_path) && fs::is_directory(dst_path))
    {
        // 要找出src_path的最后一级目录，以便在dst_path下创建同名目录
        fs::path src_last_level = fs::path(src_p).filename();
        dst_p = dst_p / src_last_level;

        logger->warn("begin to copy directory: {} to directory: {}", src_p.string(), dst_p.string());

        // create dst_dir if not exist
        if (!fs::exists(dst_p))
        {
            std::error_code ec;
            fs::create_directories(dst_p, ec);
            if (ec)
            {
                std::cerr << "Failed to create destination directory: " << dst_p.string()
                          << ", errstr: " << ec.message() << std::endl;
                return 1;
            }
        }
        for (const auto &entry : fs::recursive_directory_iterator(src_p))
        {
            fs::path relative_path = fs::relative(entry.path(), src_p);
            fs::path dst_file_path = dst_p / relative_path;

            auto copy_entry = std::make_unique<CopyEntry>();
            copy_entry->srcPath = entry.path().string();
            copy_entry->dstPath = dst_file_path.string();
            logger->debug("pushed file pair: src: {}, dst: {}", copy_entry->srcPath, copy_entry->dstPath);
            copyChannel.Push(copy_entry);
        }

        if (options.EnableInotify)
        {
            logger->warn("Done copying from: {} to: {}, now monitoring for changes...", src_p.string(), dst_p.string());
            // read from iChan and push to copyChannel
            while (true)
            {
                auto pop_res = iChan.Pop();
                if (!pop_res)
                {
                    logger->error("Inotify copyChannel pop failed: {}", pop_res.error().what());
                    continue;
                }
                std::string changed_path = pop_res.value();
                fs::path changed_rel_path = fs::relative(changed_path, src_p);
                fs::path changed_dst_path = dst_p / changed_rel_path;
                logger->debug("Detected change in file: {}, scheduling copy to: {}", changed_path, changed_dst_path.string());

                auto copy_entry = std::make_unique<CopyEntry>();
                copy_entry->srcPath = changed_path;
                copy_entry->dstPath = changed_dst_path.string();
                logger->debug("pushed file pair: src: {}, dst: {}", copy_entry->srcPath, copy_entry->dstPath);
                copyChannel.Push(copy_entry);
            }
            // main() never exits normally when inotify is enabled
            inotifyThread.join();
        }
    }
    else if (fs::is_regular_file(src_path) && fs::is_directory(dst_path)) // copy single file into directory
    {
        fs::path src_last_level = fs::path(src_p).filename();
        dst_p = dst_p / src_last_level;
        logger->info("begin to copy file: {} to file: {}", src_p.string(), dst_p.string());

        auto copy_entry = std::make_unique<CopyEntry>();
        copy_entry->srcPath = src_p.string();
        copy_entry->dstPath = dst_p.string();
        copyChannel.Push(copy_entry);
    }
    else if (fs::is_regular_file(src_path) && fs::is_regular_file(dst_path)) // single file copy
    {
        logger->info("begin to copy file: {} to file: {}", src_p.string(), dst_p.string());

        auto copy_entry = std::make_unique<CopyEntry>();
        copy_entry->srcPath = src_p.string();
        copy_entry->dstPath = dst_p.string();
        copyChannel.Push(copy_entry);
    }
    else
    {
        std::cerr << "Unsupported source and destination path types." << std::endl;
        return 1;
    }

    copyChannel.Close();

    file_copy_thread.join();

    funcDurationStat.PrintStats();
    return 0;
}