#include "lib/mainlib.hpp"

#include <dirent.h>
#include <fstream>
#include <sys/stat.h>

#include "base/event_reporter.hpp"

#ifdef __APPLE__
#include "base/fsevents.hpp"
#else
#include "base/inotify.hpp"
#endif

std::optional<RWCombinedCopyOptions> MergeCopyOptions(
    const RWCombinedCopyOptions &base,
    const std::string &config_path)
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
        RWCombinedCopyOptions options = base;

        if (data.contains("ProgramLogLevel"))
            options.ProgramLogLevel = data["ProgramLogLevel"].get<std::string>();
        if (data.contains("ProgramLogMode"))
            options.ProgramLogMode = data["ProgramLogMode"].get<std::string>();
        if (data.contains("ProgramLogFilePath"))
            options.ProgramLogFilePath = data["ProgramLogFilePath"].get<std::string>();
        if (data.contains("FileLogEnabled"))
            options.FileLogEnabled = data["FileLogEnabled"].get<bool>();
        if (data.contains("FileLogMode"))
            options.FileLogMode = data["FileLogMode"].get<std::string>();
        if (data.contains("FileLogIntervalSec"))
            options.FileLogIntervalSec = data["FileLogIntervalSec"].get<int>();
        if (data.contains("FileLogPath"))
            options.FileLogPath = data["FileLogPath"].get<std::string>();
        if (data.contains("CopyEngine"))
            options.CopyEngine = data["CopyEngine"].get<std::string>();
        if (data.contains("CopyMode"))
            options.CopyMode = data["CopyMode"].get<std::string>();
        if (data.contains("CksumAlgorithm"))
            options.CksumAlgorithm = data["CksumAlgorithm"].get<std::string>();
        if (data.contains("CopyParallelism"))
            options.CopyParallelism = data["CopyParallelism"].get<int>();
        if (data.contains("CopyChanSize"))
            options.CopyChanSize = data["CopyChanSize"].get<int>();
        if (data.contains("DirectIO"))
            options.DirectIO = data["DirectIO"].get<bool>();
        if (data.contains("EnableInotify"))
            options.EnableInotify = data["EnableInotify"].get<bool>();
        if (data.contains("PreserveSparseFiles"))
            options.PreserveSparseFiles = data["PreserveSparseFiles"].get<bool>();
        if (data.contains("PreserveMeta"))
            options.PreserveMeta = data["PreserveMeta"].get<bool>();
        if (data.contains("SyncWrites"))
            options.SyncWrites = data["SyncWrites"].get<bool>();

        if (data.contains("CopyOptions"))
        {
            auto &co = data["CopyOptions"];
            if (co.contains("IOSize"))
                options.IoSize = co["IOSize"].get<size_t>();
            if (co.contains("QueueDepth"))
                options.QueueDepth = co["QueueDepth"].get<size_t>();
            if (co.contains("Batch"))
                options.Batch = co["Batch"].get<int>();
            if (co.contains("IOReapWait"))
                options.IOReapWait = co["IOReapWait"].get<int>();
            if (co.contains("IOStuckTimeout"))
                options.IOStuckTimeout = co["IOStuckTimeout"].get<int>();
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

// Scan a single source directory and push all entries into the shared channel.
// Stats are accumulated into the caller-provided counters.
static int ScanDirIntoChannel(const fs::path &src_p,
                              const fs::path &dst_p,
                              Channel<CopyEntry> &copyChannel,
                              FileLogReporter *reporter,
                              std::shared_ptr<ILogger> logger,
                              size_t &filesSeen,
                              size_t &dirsSeen,
                              size_t &symlinksSeen,
                              size_t &bytesSeen,
                              size_t &filesRegular,
                              size_t &filesUnsupported)
{
    struct ScanFrame
    {
        std::string srcPath;
        std::string dstPath;
        DIR *dir = nullptr;
        bool childrenProcessed = false;
    };

    DIR *rootDir = opendir(src_p.c_str());
    if (!rootDir)
    {
        logger->error("opendir failed for source directory: {}, errno={}", src_p.string(), errno);
        return 1;
    }

    std::deque<ScanFrame> scanStack;
    scanStack.push_back({src_p.string(), dst_p.string(), rootDir, false});

    while (!scanStack.empty())
    {
        auto &frame = scanStack.back();

        if (!frame.childrenProcessed)
        {
            frame.childrenProcessed = true;

            struct dirent *entry;
            while ((entry = readdir(frame.dir)) != nullptr)
            {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;

                std::string srcChild = frame.srcPath + "/" + std::string(entry->d_name);
                std::string dstChild = frame.dstPath + "/" + std::string(entry->d_name);

                struct stat st;
                if (lstat(srcChild.c_str(), &st) != 0)
                {
                    logger->warn("lstat failed for {}: errno={}", srcChild, errno);
                    filesSeen++;
                    continue;
                }

                if (S_ISLNK(st.st_mode))
                {
                    auto ce = std::make_unique<CopyEntry>();
                    ce->srcPath = srcChild;
                    ce->dstPath = dstChild;
                    ce->srcStat = st;
                    copyChannel.Push(ce);
                    symlinksSeen++;
                    filesSeen++;
                }
                else if (S_ISDIR(st.st_mode))
                {
                    mkdir(dstChild.c_str(), st.st_mode & 0777);

                    DIR *childDir = opendir(srcChild.c_str());
                    if (!childDir)
                    {
                        logger->warn("opendir failed for {}: errno={}", srcChild, errno);
                        filesSeen++;
                        continue;
                    }
                    scanStack.push_back({srcChild, dstChild, childDir, false});
                }
                else if (S_ISREG(st.st_mode))
                {
                    auto ce = std::make_unique<CopyEntry>();
                    ce->srcPath = srcChild;
                    ce->dstPath = dstChild;
                    ce->srcStat = st;
                    copyChannel.Push(ce);
                    filesSeen++;
                    bytesSeen += st.st_size;
                    filesRegular++;
                }
                else
                {
                    filesUnsupported++;
                    filesSeen++;
                }
            }

            if (reporter && filesSeen % 1000 == 0)
            {
                reporter->CopyPlan("scanning", filesSeen, dirsSeen, symlinksSeen, bytesSeen, filesRegular, filesUnsupported);
            }
        }
        else
        {
            auto ce = std::make_unique<CopyEntry>();
            ce->srcPath = frame.srcPath;
            ce->dstPath = frame.dstPath;
            lstat(ce->srcPath.c_str(), &ce->srcStat);
            copyChannel.Push(ce);
            dirsSeen++;

            closedir(frame.dir);
            scanStack.pop_back();
        }
    }

    return 0;
}

int CopyDir(const fs::path src_p, const fs::path dst_p, const RWCombinedCopyOptions &options, std::shared_ptr<ILogger> logger, std::atomic<bool> *stopFlag)
{
#ifdef __APPLE__
    if (options.DirectIO)
    {
        logger->error("DirectIO is not supported on macOS");
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
    std::thread inotifyThread;
    std::atomic<bool> inotifyStopRequested{false};
    if (options.EnableInotify)
    {
        auto add_watch_res = inotifyWatcher.Init();
        if (!add_watch_res)
        {
            logger->error("Failed to add inotify init for path: {}, err: {}", src_p.string(), add_watch_res.error().ToString());
            return 1;
        }
        // start a thread to read inotify events and push to copyChannel
        inotifyThread = std::thread(
            [&inotifyWatcher, &iChan, logger, &inotifyStopRequested]()
            {
                while (!inotifyStopRequested.load())
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

    std::atomic<bool> copyFailed{false};

    // start a thread to run CopyEngine
    std::thread file_copy_thread(
        [fc = std::move(file_copier), &copyChannel, logger, &copyFailed]()
        {
            auto copy_res = fc->RunChannel(copyChannel);
            if (!copy_res)
            {
                logger->error("File copy failed: {}", copy_res.error().ToString());
                copyFailed.store(true);
            }
        });

    // create dst_dir if not exist
    if (!fs::exists(dst_p))
    {
        std::error_code ec;
        fs::create_directories(dst_p, ec);
        if (ec)
        {
            logger->error("Failed to create destination directory: {}, errstr: {}", dst_p.string(), ec.message());
            return 1;
        }
    }

    // recursively walk through source directory and prepare file pairs
    // using post-order DFS with explicit stack (readdir + lstat, each path stat once)
    size_t filesSeen = 0;
    size_t dirsSeen = 0;
    size_t symlinksSeen = 0;
    size_t bytesSeen = 0;
    size_t filesRegular = 0;
    size_t filesUnsupported = 0;
    auto scanStart = std::chrono::steady_clock::now();

    int scanRc = ScanDirIntoChannel(src_p, dst_p, copyChannel, reporter.get(), logger,
                                    filesSeen, dirsSeen, symlinksSeen,
                                    bytesSeen, filesRegular, filesUnsupported);
    if (scanRc != 0)
    {
        copyChannel.Close();
        file_copy_thread.join();
        return 1;
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
            lstat(copy_entry->srcPath.c_str(), &copy_entry->srcStat);
            copyChannel.Push(copy_entry);
        }
        // main() never exits normally when inotify is enabled
        inotifyStopRequested.store(true);
        inotifyThread.join();
    }

    copyChannel.Close();

    file_copy_thread.join();

    if (copyFailed.load())
    {
        return 1;
    }

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
        logger->error("DirectIO is not supported on macOS");
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
    std::atomic<bool> copyFailed{false};
    auto copyStart = std::chrono::steady_clock::now();
    std::thread file_copy_thread(
        [fc = std::move(file_copier), &copyChannel, logger, &copyFailed]()
        {
            auto copy_res = fc->RunChannel(copyChannel);
            if (!copy_res)
            {
                logger->error("File copy failed: {}", copy_res.error().ToString());
                copyFailed.store(true);
            }
        });

    auto copy_entry = std::make_unique<CopyEntry>();
    copy_entry->srcPath = src_file.string();
    copy_entry->dstPath = dst_file.string();
    lstat(copy_entry->srcPath.c_str(), &copy_entry->srcStat);
    copyChannel.Push(copy_entry);

    copyChannel.Close();

    file_copy_thread.join();

    if (copyFailed.load())
    {
        return 1;
    }

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

int CopyBatch(const std::vector<std::pair<fs::path, fs::path>> &srcDstPairs,
              const RWCombinedCopyOptions &options,
              std::shared_ptr<ILogger> logger)
{
#ifdef __APPLE__
    if (options.DirectIO)
    {
        logger->error("DirectIO is not supported on macOS");
        return 1;
    }
#endif

    Channel<CopyEntry> copyChannel(options.CopyChanSize);
    auto funcDurationStat = std::make_shared<FuncDurationStat>(logger);
    auto reporter = std::make_unique<FileLogReporter>(options.FileLogEnabled, options.FileLogMode,
                                                      options.FileLogIntervalSec, options.FileLogPath, options.FileLogPath);
    auto fileCopier = std::make_unique<CopyEngine>(options, logger, funcDurationStat, reporter.get());

    std::atomic<bool> copyFailed{false};
    std::thread fileCopyThread(
        [fc = std::move(fileCopier), &copyChannel, logger, &copyFailed]()
        {
            auto copyRes = fc->RunChannel(copyChannel);
            if (!copyRes)
            {
                logger->error("File copy failed: {}", copyRes.error().ToString());
                copyFailed.store(true);
            }
        });

    size_t filesSeen = 0;
    size_t dirsSeen = 0;
    size_t symlinksSeen = 0;
    size_t bytesSeen = 0;
    size_t filesRegular = 0;
    size_t filesUnsupported = 0;
    auto batchStart = std::chrono::steady_clock::now();

    for (const auto &[src, dst] : srcDstPairs)
    {
        if (fs::is_directory(src))
        {
            if (!fs::exists(dst))
            {
                std::error_code ec;
                fs::create_directories(dst, ec);
                if (ec)
                {
                    logger->error("Failed to create destination directory: {}, errstr: {}", dst.string(), ec.message());
                    continue;
                }
            }

            int scanRc = ScanDirIntoChannel(src, dst, copyChannel, reporter.get(), logger,
                                            filesSeen, dirsSeen, symlinksSeen,
                                            bytesSeen, filesRegular, filesUnsupported);
            if (scanRc != 0)
            {
                logger->error("Scan failed for source: {}", src.string());
            }
        }
        else if (fs::is_regular_file(src))
        {
            struct stat st;
            if (lstat(src.c_str(), &st) == 0)
            {
                auto ce = std::make_unique<CopyEntry>();
                ce->srcPath = src.string();
                ce->dstPath = dst.string();
                ce->srcStat = st;
                copyChannel.Push(ce);

                filesSeen++;
                filesRegular++;
                bytesSeen += st.st_size;

                if (reporter)
                {
                    reporter->FileStart(src.string(), dst.string(), st.st_size);
                }
            }
            else
            {
                logger->warn("lstat failed for {}: errno={}", src.string(), errno);
                filesSeen++;
            }
        }
        else
        {
            logger->warn("Unsupported source type in batch: {}", src.string());
            filesUnsupported++;
            filesSeen++;
        }
    }

    if (reporter)
    {
        reporter->CopyPlan("completed", filesSeen, dirsSeen, symlinksSeen,
                           bytesSeen, filesRegular, filesUnsupported);
        reporter->SetFilesTotal(filesSeen);
        reporter->SetBytesTotal(bytesSeen);
    }

    copyChannel.Close();
    fileCopyThread.join();

    if (copyFailed.load())
    {
        return 1;
    }

    auto copyDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - batchStart)
                            .count();
    if (reporter)
    {
        reporter->CopyComplete(copyDuration);
        reporter->FinalizeStateFile();
    }

    funcDurationStat->PrintStats();
    return 0;
}