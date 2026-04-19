#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <algorithm>
#include "lib/mainlib.hpp"

std::optional<RWCombinedCopyOptions> LoadCopyOptions(const std::string &config_path)
{
    std::ifstream f(config_path);
    if (!f.is_open())
    {
        return std::nullopt;
    }

    using json = nlohmann::json;
    try
    {
        json data = json::parse(f);

        RWCombinedCopyOptions options;

        auto copyOpts = data.at("CopyOptions");
        options.IoSize = copyOpts.at("IOSize").get<size_t>();
        options.QueueDepth = copyOpts.at("QueueDepth").get<size_t>();
        options.Batch = copyOpts.at("Batch").get<int>();
        options.IOReapWait = copyOpts.at("IOReapWait").get<int>();

        options.LogLevel = data.at("LogLevel").get<std::string>();
        options.LogMode = data.at("LogMode").get<std::string>();
        options.LogFilePath = data.value("LogFilePath", std::string());
        options.CopyEngine = data.at("CopyEngine").get<std::string>();
        options.CopyMode = data.at("CopyMode").get<std::string>();
        options.CksumAlgorithm = data.value("CksumAlgorithm", std::string("xxhash64"));
        options.CopyParallelism = data.at("CopyParallelism").get<int>();
        options.CopyChanSize = data.at("CopyChanSize").get<int>();
        options.DirectIO = data.value("DirectIO", false);
        options.EnableInotify = data.value("EnableInotify", false);
        options.PreserveSparseFiles = data.value("PreserveSparseFiles", false);

        if (options.IoSize == 0)
        {
            std::cerr << "Configuration error: IOSize must be greater than 0" << std::endl;
            return std::nullopt;
        }
        if (options.QueueDepth == 0)
        {
            std::cerr << "Configuration error: QueueDepth must be greater than 0" << std::endl;
            return std::nullopt;
        }
        if (options.CopyParallelism <= 0)
        {
            std::cerr << "Configuration error: CopyParallelism must be greater than 0" << std::endl;
            return std::nullopt;
        }
        if (options.CopyChanSize <= 0)
        {
            std::cerr << "Configuration error: CopyChanSize must be greater than 0" << std::endl;
            return std::nullopt;
        }

        return options;
    }
    catch (const json::exception &e)
    {
        std::cerr << "Configuration error in " << config_path << ": " << e.what() << std::endl;
        return std::nullopt;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Configuration error in " << config_path << ": " << e.what() << std::endl;
        return std::nullopt;
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

    auto logger = InitLogger(options);

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

    // recursively walk through source directory and prepare file pairs
    if (fs::is_directory(src_path) && fs::is_directory(dst_path))
    {
        // 要找出src_path的最后一级目录，以便在dst_path下创建同名目录
        fs::path src_last_level = fs::path(src_p).filename();
        dst_p = dst_p / src_last_level;

        logger->warn("begin to copy directory: {} to directory: {}", src_p.string(), dst_p.string());

        return CopyDir(src_p, dst_p, options, logger);
    }
    else if (fs::is_regular_file(src_path) && fs::is_directory(dst_path)) // copy single file into directory
    {
        fs::path src_last_level = fs::path(src_p).filename();
        dst_p = dst_p / src_last_level;
        logger->info("begin to copy file: {} to file: {}", src_p.string(), dst_p.string());

        return CopyFile(src_p, dst_p, options, logger);
    }
    else if (fs::is_regular_file(src_path) && fs::is_regular_file(dst_path)) // single file copy
    {
        logger->info("begin to copy file: {} to file: {}", src_p.string(), dst_p.string());

        return CopyFile(src_p, dst_p, options, logger);
    }
    else
    {
        std::cerr << "Unsupported source and destination path types." << std::endl;
        return 1;
    }

    return 0;
}