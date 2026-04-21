#include "lib/mainlib.hpp"

#include <fstream>

#include "base/event_reporter.hpp"

#ifdef __APPLE__
#include "base/fsevents.hpp"
#else
#include "base/inotify.hpp"
#endif

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

        options.ProgramLogLevel = data.value("ProgramLogLevel", std::string("info"));
        options.ProgramLogMode = data.value("ProgramLogMode", std::string("console"));
        options.ProgramLogFilePath = data.value("ProgramLogFilePath", std::string("/tmp/acp_program.log"));
        options.FileLogEnabled = data.value("FileLogEnabled", false);
        options.FileLogMode = data.value("FileLogMode", std::string("file"));
        options.FileLogIntervalSec = data.value("FileLogIntervalSec", 5);
        options.FileLogPath = data.value("FileLogPath", std::string("/tmp/acp_file_info.json"));
        options.CopyEngine = data.at("CopyEngine").get<std::string>();
        options.CopyMode = data.at("CopyMode").get<std::string>();
        options.CksumAlgorithm = data.value("CksumAlgorithm", std::string("xxhash64"));
        options.CopyParallelism = data.at("CopyParallelism").get<int>();
        options.CopyChanSize = data.at("CopyChanSize").get<int>();
        options.DirectIO = data.value("DirectIO", false);
        options.EnableInotify = data.value("EnableInotify", false);
        options.PreserveSparseFiles = data.value("PreserveSparseFiles", true);
        options.PreserveMeta = data.value("PreserveMeta", true);

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

std::shared_ptr<ILogger> InitLogger(const RWCombinedCopyOptions &options)
{
    std::shared_ptr<ILogger> myLogger;
    if (options.ProgramLogMode == "file")
    {
        auto logger = spdlog::get("file");
        if (!logger)
        {
            logger = spdlog::basic_logger_mt("file", options.ProgramLogFilePath);
        }
        logger->set_level(spdlog::level::from_str("trace"));
        logger->set_pattern("%v");
        myLogger = std::make_shared<SpdLogger>(logger);
        myLogger->set_level(options.ProgramLogLevel);
        return myLogger;
    }
    else
    {
        auto logger = spdlog::get("console");
        if (!logger)
        {
            logger = spdlog::stdout_color_mt("console");
        }
        logger->set_level(spdlog::level::from_str("trace"));
        logger->set_pattern("%v");
        myLogger = std::make_shared<SpdLogger>(logger);
        myLogger->set_level(options.ProgramLogLevel);
        return myLogger;
    }
}

int CopyDir(const fs::path src_p, const fs::path dst_p, const RWCombinedCopyOptions &options, std::shared_ptr<ILogger> logger, std::atomic<bool> *stopFlag)
{
#ifdef __APPLE__
    if (options.DirectIO)
    {
        std::cerr << "DirectIO is not supported on macOS" << std::endl;
        return 1;
    }
#endif
    // deal with inotify if enabled
    InotifyChannel iChan;
#ifdef __APPLE__
    FSEventsWatcher inotifyWatcher(src_p.string(), logger);
#else
    Inotify inotifyWatcher(src_p.string(), logger);
#endif
    std::jthread inotifyThread;
    if (options.EnableInotify)
    {

        auto add_watch_res = inotifyWatcher.Init();
        if (!add_watch_res)
        {
            std::cerr << fmt::format("Failed to add inotify init for path: {}, err: {}", src_p.string(), add_watch_res.error().ToString());
            return 1;
        }
        // start a thread to read inotify events and push to copyChannel
        inotifyThread = std::jthread(
            [&inotifyWatcher, &iChan, logger](std::stop_token st)
            {
                while (!st.stop_requested())
                {
                    auto read_res = inotifyWatcher.ReadEventToChannel(iChan);
                    if (!read_res)
                    {
                        if (inotifyWatcher.IsClosed())
                            break;
                        logger->error("Inotify read event failed: {}", read_res.error().ToString());
                    }
                }
            });
    }
    // 开始创建各种对象，注入依赖
    Channel<CopyEntry> copyChannel(options.CopyChanSize);
    auto funcDurationStat = std::make_shared<FuncDurationStat>(logger);
    auto reporter = std::make_unique<FileLogReporter>(options.FileLogEnabled, options.FileLogMode, options.FileLogIntervalSec, options.FileLogPath, options.FileLogPath);
    auto file_copier = std::make_unique<CopyEngine>(options, logger, funcDurationStat, reporter.get());

    // start a thread to run CopyEngine
    std::thread file_copy_thread(
        [fc = std::move(file_copier), &copyChannel, logger]()
        {
            auto copy_res = fc->RunChannel(copyChannel);
            if (!copy_res)
            {
                logger->error("File copy failed: {}", copy_res.error().ToString());
            }
        });

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

    // recursively walk through source directory and prepare file pairs
    size_t filesSeen = 0;
    size_t dirsSeen = 0;
    size_t symlinksSeen = 0;
    size_t bytesSeen = 0;
    size_t filesRegular = 0;
    size_t filesUnsupported = 0;
    auto scanStart = std::chrono::steady_clock::now();

    for (const auto &entry : fs::recursive_directory_iterator(src_p))
    {
        fs::path relative_path = fs::relative(entry.path(), src_p);
        fs::path dst_file_path = dst_p / relative_path;

        auto status = entry.symlink_status();
        if (fs::is_symlink(status))
        {
            symlinksSeen++;
        }
        else if (fs::is_directory(status))
        {
            dirsSeen++;
        }
        else if (fs::is_regular_file(status))
        {
            filesRegular++;
            bytesSeen += fs::file_size(entry.path());
        }
        else
        {
            filesUnsupported++;
        }
        filesSeen++;

        auto copy_entry = std::make_unique<CopyEntry>();
        copy_entry->srcPath = entry.path().string();
        copy_entry->dstPath = dst_file_path.string();
        copyChannel.Push(copy_entry);

        // emit scanning progress every 1000 files
        if (reporter && filesSeen % 1000 == 0)
        {
            reporter->CopyPlan("scanning", filesSeen, dirsSeen, symlinksSeen, bytesSeen, filesRegular, filesUnsupported);
        }
    }

    if (reporter)
    {
        reporter->CopyPlan("completed", filesSeen, dirsSeen, symlinksSeen, bytesSeen, filesRegular, filesUnsupported);
        reporter->SetFilesTotal(filesSeen);
        reporter->SetBytesTotal(bytesSeen);
    }

    if (options.EnableInotify)
    {
        auto copyDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - scanStart)
                                .count();
        if (reporter)
        {
            reporter->CopyComplete(copyDuration);
            reporter->FinalizeStateFile();
        }
        logger->warn("Done copying from: {} to: {}, now monitoring for changes...", src_p.string(), dst_p.string());
        // read from iChan and push to copyChannel
        while (true)
        {
            if (stopFlag && stopFlag->load())
            {
                inotifyWatcher.Close();
                iChan.Close();
                break;
            }
            auto pop_res = iChan.Pop();
            if (!pop_res)
            {
                logger->error("Inotify copyChannel pop failed: {}", pop_res.error().ToString());
                continue;
            }
            std::string changed_path = pop_res.value();
            fs::path changed_rel_path = fs::relative(changed_path, src_p);
            fs::path changed_dst_path = dst_p / changed_rel_path;
            logger->warn("[inotify] Detected change: src_rel={}, dst={}, full={}", changed_rel_path.string(), changed_dst_path.string(), changed_path);

            auto copy_entry = std::make_unique<CopyEntry>();
            copy_entry->srcPath = changed_path;
            copy_entry->dstPath = changed_dst_path.string();
            copyChannel.Push(copy_entry);
        }
        // main() never exits normally when inotify is enabled
        inotifyThread.join();
    }

    copyChannel.Close();

    file_copy_thread.join();

    auto copyDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - scanStart)
                            .count();
    if (reporter)
    {
        reporter->CopyComplete(copyDuration);
        reporter->FinalizeStateFile();
    }

    funcDurationStat->PrintStats();
    return 0;
}

int CopyFile(const fs::path src_file, const fs::path dst_file, const RWCombinedCopyOptions &options, std::shared_ptr<ILogger> logger)
{
#ifdef __APPLE__
    if (options.DirectIO)
    {
        std::cerr << "DirectIO is not supported on macOS" << std::endl;
        return 1;
    }
#endif
    Channel<CopyEntry> copyChannel(options.CopyChanSize);
    auto funcDurationStat = std::make_shared<FuncDurationStat>(logger);
    auto reporter = std::make_unique<FileLogReporter>(options.FileLogEnabled, options.FileLogMode, options.FileLogIntervalSec, options.FileLogPath, options.FileLogPath);
    auto file_copier = std::make_unique<CopyEngine>(options, logger, funcDurationStat, reporter.get());

    // gather file info for copy_plan
    size_t fileSize = 0;
    if (fs::exists(src_file) && fs::is_regular_file(src_file))
    {
        fileSize = fs::file_size(src_file);
    }
    if (reporter)
    {
        reporter->CopyPlan("completed", 1, 0, 0, fileSize, 1, 0);
        reporter->SetFilesTotal(1);
        reporter->SetBytesTotal(fileSize);
    }

    // start a thread to run CopyEngine
    auto copyStart = std::chrono::steady_clock::now();
    std::thread file_copy_thread(
        [fc = std::move(file_copier), &copyChannel, logger]()
        {
            auto copy_res = fc->RunChannel(copyChannel);
            if (!copy_res)
            {
                logger->error("File copy failed: {}", copy_res.error().ToString());
            }
        });

    auto copy_entry = std::make_unique<CopyEntry>();
    copy_entry->srcPath = src_file.string();
    copy_entry->dstPath = dst_file.string();
    copyChannel.Push(copy_entry);

    copyChannel.Close();

    file_copy_thread.join();

    auto copyDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - copyStart)
                            .count();
    if (reporter)
    {
        reporter->CopyComplete(copyDuration);
        reporter->FinalizeStateFile();
    }

    funcDurationStat->PrintStats();

    return 0;
}