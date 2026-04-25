#include <iostream>
#include <vector>
#include <filesystem>
#include <algorithm>
#include "lib/mainlib.hpp"
#include "lib/thirdparty/CLI11.hpp"

constexpr const char* kVersion = "0.5.0";

#ifdef __APPLE__
constexpr const char* kPlatform = "macOS";
#elif defined(__linux__)
constexpr const char* kPlatform = "Linux";
#else
constexpr const char* kPlatform = "Unknown";
#endif

#if defined(__arm64__) || defined(__aarch64__)
constexpr const char* kArch = "arm64";
#elif defined(__x86_64__)
constexpr const char* kArch = "x86_64";
#else
constexpr const char* kArch = "unknown";
#endif

#ifdef __clang__
constexpr const char* kCompiler = "Clang " __clang_version__;
#elif defined(__GNUC__)
constexpr const char* kCompiler = "GCC " __VERSION__;
#else
constexpr const char* kCompiler = "Unknown";
#endif

static void PrintVersion()
{
    std::cout <<
        "acp version " << kVersion << "\n"
        "\n"
        "BUILD\n"
        "    Platform     " << kPlatform << "\n"
        "    Architecture " << kArch << "\n"
        "    Compiler     " << kCompiler << "\n"
        "\n"
        "SUPPORTED ENGINES\n"
#ifdef __APPLE__
        "    GCD          yes   (default)\n"
        "    libaio       no\n"
        "    liburing     no\n"
#else
        "    libaio       yes   (default)\n"
#ifdef ENABLE_LIBURING
        "    liburing     yes\n"
#else
        "    liburing     no    (build with -DENABLE_LIBURING=ON)\n"
#endif
        "    GCD          no\n"
#endif
        "\n";
}

static void PrintHelp(const char *program_name)
{
    std::cout <<
        "acp — async cp, high-performance file copy for Linux/macOS\n"
        "\n"
        "USAGE\n"
        "    " << program_name << " [OPTIONS] <src_path>... <dst_path>\n"
        "\n"
        "POSITIONAL ARGUMENTS\n"
        "    src_path    One or more source paths (regular files or directories)\n"
        "    dst_path    Destination path (regular file or directory)\n"
        "\n"
        "    Single source:\n"
        "        src may be a file or directory.\n"
        "        dst may be a non-existing path (created), an existing file\n"
        "        (overwritten), or an existing directory (copied inside).\n"
        "\n"
        "    Multiple sources:\n"
        "        All sources are copied into dst, which must be an existing directory.\n"
        "\n"
        "CONFIG LOADING ORDER (later overrides earlier)\n"
        "    1. Built-in defaults\n"
        "    2. /etc/acp_config.json\n"
        "    3. ~/acp_config.json\n"
        "    4. ./acp_config.json\n"
        "    5. Command-line options\n"
        "\n"
        "    Use --help-all to see all available command-line options.\n"
        "\n"
        "EXIT CODES\n"
        "    0   Success\n"
        "    1   Configuration error, path error, or copy failure\n"
        "─────────────────────────────────────────────────────────────────────────\n";
}

int main(int argc, char *argv[])
{
    namespace fs = std::filesystem;

    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0))
    {
        PrintHelp(argv[0]);
        return 0;
    }
    if (argc == 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0))
    {
        PrintVersion();
        return 0;
    }

    // ---------- CLI11 参数定义：所有选项先收集到 optional，后续按优先级覆盖配置 ----------
    CLI::App app{"acp — async cp, high-performance file copy"};

    std::optional<std::string> optLogLevel, optLogMode, optLogFile;
    std::optional<std::string> optFileLogMode, optFileLogPath;
    std::optional<std::string> optEngine, optMode, optCksumAlgo;
    std::optional<int> optFileLogInterval, optParallelism, optChanSize;
    std::optional<size_t> optIoSize, optQueueDepth;
    std::optional<int> optBatch, optIoReapWait, optIoStuckTimeout;

    bool enableFileLog = false, disableFileLog = false;
    bool enableInotify = false, disableInotify = false;
    bool enableSparse = false, disableSparse = false;
    bool enablePreserveMeta = false, disablePreserveMeta = false;
    bool enableDirectIO = false, disableDirectIO = false;
    bool enableSync = false, disableSync = false;

    // String options
    app.add_option("--log-level,-L", optLogLevel, "Program log level (trace/debug/info/warn/error/critical)");
    app.add_option("--log-mode", optLogMode, "Program log mode (console/file)");
    app.add_option("--log-file", optLogFile, "Program log file path");
    app.add_option("--file-log-mode", optFileLogMode, "File log mode (console/file)");
    app.add_option("--file-log-path", optFileLogPath, "File log output path");
    app.add_option("--engine,-e", optEngine, "Copy engine (libaio/liburing)")
        ->check(CLI::IsMember({"libaio", "liburing", "gcd"}));
    app.add_option("--mode,-m", optMode, "Copy mode (CopyOnly/CksumCopy/CksumOnly)")
        ->check(CLI::IsMember({"CopyOnly", "CksumCopy", "CksumOnly"}));
    app.add_option("--cksum-algo,-a", optCksumAlgo, "Checksum algorithm (xxhash64/md5/sha256)")
        ->check(CLI::IsMember({"xxhash64", "md5", "sha256"}));

    // Numeric options
    app.add_option("--file-log-interval", optFileLogInterval, "File log flush interval (sec)")->check(CLI::PositiveNumber);
    app.add_option("--parallelism,-p", optParallelism, "Number of copy worker threads")->check(CLI::PositiveNumber);
    app.add_option("--chan-size", optChanSize, "Channel capacity for file dispatch")->check(CLI::PositiveNumber);
    app.add_option("--io-size", optIoSize, "I/O unit size in bytes")->check(CLI::PositiveNumber);
    app.add_option("--queue-depth,-q", optQueueDepth, "Max in-flight I/Os per worker")->check(CLI::PositiveNumber);
    app.add_option("--batch,-b", optBatch, "I/Os submitted per batch")->check(CLI::PositiveNumber);
    app.add_option("--io-reap-wait,-w", optIoReapWait, "I/O completion wait timeout (sec)")->check(CLI::PositiveNumber);
    app.add_option("--io-stuck-timeout,-t", optIoStuckTimeout, "Stuck I/O detection timeout (sec, 0=off)")->check(CLI::NonNegativeNumber);

    // Boolean flag pairs
    app.add_flag("--enable-file-log", enableFileLog, "Enable file-level NDJSON logging");
    app.add_flag("--disable-file-log", disableFileLog, "Disable file-level NDJSON logging");
    app.add_flag("--enable-inotify", enableInotify, "Enable source directory monitoring");
    app.add_flag("--disable-inotify", disableInotify, "Disable source directory monitoring");
    app.add_flag("--enable-sparse", enableSparse, "Preserve sparse file holes");
    app.add_flag("--disable-sparse", disableSparse, "Do not preserve sparse file holes");
    app.add_flag("--enable-preserve-meta", enablePreserveMeta, "Preserve file metadata");
    app.add_flag("--disable-preserve-meta", disablePreserveMeta, "Do not preserve file metadata");
    app.add_flag("--enable-direct-io", enableDirectIO, "Use O_DIRECT for unbuffered I/O");
    app.add_flag("--disable-direct-io", disableDirectIO, "Use buffered I/O");
    app.add_flag("--enable-sync", enableSync, "Sync data to disk after each write");
    app.add_flag("--disable-sync", disableSync, "Do not force sync after writes");

    // Positional arguments
    std::vector<std::string> positional;
    app.add_option("paths", positional, "Source and destination paths")
       ->required()
       ->expected(-1);

    app.set_help_all_flag("--help-all", "Show all help including configuration options");
    app.set_version_flag("--version,-v", kVersion);

    // ---------- 配置合并优先级：默认值 → /etc/acp_config.json → ~/acp_config.json → ./acp_config.json → CLI 参数 ----------
    RWCombinedCopyOptions options;

    // 分层加载配置文件，后加载的覆盖先加载的
    auto merge = [&](const std::string& path) {
        auto merged = MergeCopyOptions(options, path);
        if (merged) options = *merged;
    };

    merge("/etc/acp_config.json");

    const char* home = std::getenv("HOME");
    if (home) {
        merge(std::string(home) + "/acp_config.json");
    }

    merge("./acp_config.json");

    // ---------- 解析命令行参数（CLI11），其值将在后续覆盖配置文件 ----------
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    // ---------- 命令行参数覆盖配置文件（最高优先级） ----------
    if (optLogLevel)        options.ProgramLogLevel = *optLogLevel;
    if (optLogMode)         options.ProgramLogMode = *optLogMode;
    if (optLogFile)         options.ProgramLogFilePath = *optLogFile;
    if (optFileLogMode)     options.FileLogMode = *optFileLogMode;
    if (optFileLogPath)     options.FileLogPath = *optFileLogPath;
    if (optEngine)          options.CopyEngine = *optEngine;
    if (optMode)            options.CopyMode = *optMode;
    if (optCksumAlgo)       options.CksumAlgorithm = *optCksumAlgo;

    if (optFileLogInterval) options.FileLogIntervalSec = *optFileLogInterval;
    if (optParallelism)     options.CopyParallelism = *optParallelism;
    if (optChanSize)        options.CopyChanSize = *optChanSize;
    if (optIoSize)          options.IoSize = *optIoSize;
    if (optQueueDepth)      options.QueueDepth = *optQueueDepth;
    if (optBatch)           options.Batch = *optBatch;
    if (optIoReapWait)      options.IOReapWait = *optIoReapWait;
    if (optIoStuckTimeout)  options.IOStuckTimeout = *optIoStuckTimeout;

    if (enableFileLog)      options.FileLogEnabled = true;
    if (disableFileLog)     options.FileLogEnabled = false;
    if (enableInotify)      options.EnableInotify = true;
    if (disableInotify)     options.EnableInotify = false;
    if (enableSparse)       options.PreserveSparseFiles = true;
    if (disableSparse)      options.PreserveSparseFiles = false;
    if (enablePreserveMeta) options.PreserveMeta = true;
    if (disablePreserveMeta)options.PreserveMeta = false;
    if (enableDirectIO)     options.DirectIO = true;
    if (disableDirectIO)    options.DirectIO = false;
    if (enableSync)         options.SyncWrites = true;
    if (disableSync)        options.SyncWrites = false;

    // ---------- 统一参数校验 ----------
    if (options.IoSize == 0) {
        std::cerr << "Configuration error: IOSize must be > 0\n";
        return 1;
    }
    if (options.QueueDepth == 0) {
        std::cerr << "Configuration error: QueueDepth must be > 0\n";
        return 1;
    }
    if (options.CopyParallelism <= 0) {
        std::cerr << "Configuration error: CopyParallelism must be > 0\n";
        return 1;
    }
    if (options.CopyChanSize <= 0) {
        std::cerr << "Configuration error: CopyChanSize must be > 0\n";
        return 1;
    }
    if (options.IOStuckTimeout < 0) {
        std::cerr << "Configuration error: IOStuckTimeout must be >= 0\n";
        return 1;
    }

    // IOStuckTimeout 与 EnableInotify 互斥：inotify 模式下主循环永不退出，stuck 检测无意义
    if (options.EnableInotify && options.IOStuckTimeout > 0) {
        std::cerr << "Configuration error: IOStuckTimeout and EnableInotify are mutually exclusive.\n";
        return 1;
    }

#ifdef __APPLE__
    // macOS: validate explicit CopyEngine setting
    if (optEngine && *optEngine != "gcd") {
        std::cerr << "Configuration error: On macOS, --engine must be 'gcd'. Got: " << *optEngine << "\n";
        return 1;
    }
    if (options.DirectIO) {
        std::cerr << "DirectIO is not supported on macOS" << std::endl;
        return 1;
    }
#endif

    auto logger = InitLogger(options);

    std::string dstPath = positional.back();
    std::vector<std::string> srcPaths(positional.begin(), positional.end() - 1);
    bool multiSource = srcPaths.size() > 1;

    // ---------- 路径校验层次 1：存在性 — 所有源路径必须存在 ----------
    for (const auto& src : srcPaths)
    {
        if (!fs::exists(src))
        {
            std::cerr << "Source path does not exist: " << src << std::endl;
            return 1;
        }
    }

    // 多源模式特殊处理：目标必须是已存在的目录，且禁用 inotify（无法同时监控多个源）
    if (multiSource && !fs::is_directory(dstPath))
    {
        std::cerr << "When copying multiple sources, destination must be an existing directory." << std::endl;
        return 1;
    }

    if (multiSource && options.EnableInotify)
    {
        logger->warn("Inotify is disabled when copying multiple sources.");
        options.EnableInotify = false;
    }

    bool anyFailed = false;
    std::vector<std::pair<fs::path, fs::path>> batchPairs;
    batchPairs.reserve(srcPaths.size());

    for (const auto& src_path : srcPaths)
    {
        fs::path src_p;
        try
        {
            src_p = fs::canonical(src_path);
        }
        catch (const fs::filesystem_error& e)
        {
            std::cerr << "Failed to resolve source path: " << src_path << ", " << e.what() << std::endl;
            anyFailed = true;
            continue;
        }

        // ---------- 路径校验层次 2：多源/单源下目的地构造逻辑 ----------
        fs::path dst_p;
        if (multiSource)
        {
            // 多源时：以各源 basename 作为 dst 子目录/文件名（如 cp a b dir/ → dir/a, dir/b）
            try
            {
                dst_p = fs::canonical(dstPath) / src_p.filename();
            }
            catch (const fs::filesystem_error& e)
            {
                std::cerr << "Failed to resolve destination path: " << dstPath << ", " << e.what() << std::endl;
                anyFailed = true;
                continue;
            }
        }
        else
        {
            if (fs::exists(dstPath))
            {
                try
                {
                    dst_p = fs::canonical(dstPath);
                }
                catch (const fs::filesystem_error& e)
                {
                    std::cerr << "Failed to resolve destination path: " << dstPath << ", " << e.what() << std::endl;
                    anyFailed = true;
                    continue;
                }
            }
            else
            {
                dst_p = fs::absolute(dstPath);
            }
        }

        // ---------- 路径校验层次 3：等价性 — 禁止源与目标是同一文件/目录 ----------
        if (fs::exists(dst_p))
        {
            try
            {
                if (fs::equivalent(src_p, dst_p))
                {
                    std::cerr << "Source path and destination path cannot be the same." << std::endl;
                    anyFailed = true;
                    continue;
                }
            }
            catch (const fs::filesystem_error&)
            {
                // equivalent may fail for some path combinations; ignore
            }
        }

        // ---------- 路径校验层次 4：可写性 — 已存在的目的地必须可写 ----------
        if (fs::exists(dst_p))
        {
            std::error_code ec;
            fs::perms p = fs::status(dst_p, ec).permissions();
            if (!ec &&
                (p & fs::perms::owner_write) == fs::perms::none &&
                (p & fs::perms::group_write) == fs::perms::none &&
                (p & fs::perms::others_write) == fs::perms::none)
            {
                std::cerr << "Destination path is not writable: " << dst_p.string() << std::endl;
                anyFailed = true;
                continue;
            }
        }

        // ---------- 路径校验层次 5：子目录关系 — 禁止目录自复制导致无限递归 ----------
        if (fs::is_directory(src_p) && fs::is_directory(dst_p))
        {
            bool src_is_prefix_of_dst = std::mismatch(src_p.begin(), src_p.end(), dst_p.begin(), dst_p.end()).first == src_p.end();
            bool dst_is_prefix_of_src = std::mismatch(dst_p.begin(), dst_p.end(), src_p.begin(), src_p.end()).first == dst_p.end();

            if (src_is_prefix_of_dst || dst_is_prefix_of_src)
            {
                std::cerr << "Source path and destination path cannot be subdirectory of each other." << std::endl;
                std::cerr << "Source path: " << src_p.string() << std::endl;
                std::cerr << "Destination path: " << dst_p.string() << std::endl;
                anyFailed = true;
                continue;
            }
        }

        // ---------- 路由分发：根据源类型和目标类型选择 CopyDir / CopyFile / CopyBatch ----------
        bool dstIsDir = multiSource ? fs::is_directory(dstPath) : (fs::exists(dst_p) && fs::is_directory(dst_p));

        if (fs::is_directory(src_p) && (dstIsDir || !fs::exists(dst_p)))
        {
            fs::path final_dst = multiSource ? dst_p : (dstIsDir ? dst_p / src_p.filename() : dst_p);
            if (multiSource)
            {
                batchPairs.emplace_back(src_p, final_dst);
            }
            else
            {
                logger->warn("begin to copy directory: {} to directory: {}", src_p.string(), final_dst.string());
                int rc = CopyDir(src_p, final_dst, options, logger);
                if (rc != 0)
                    anyFailed = true;
            }
        }
        else if (fs::is_regular_file(src_p) && dstIsDir)
        {
            fs::path final_dst = multiSource ? dst_p : dst_p / src_p.filename();
            if (multiSource)
            {
                batchPairs.emplace_back(src_p, final_dst);
            }
            else
            {
                logger->info("begin to copy file: {} to file: {}", src_p.string(), final_dst.string());
                int rc = CopyFile(src_p, final_dst, options, logger);
                if (rc != 0)
                    anyFailed = true;
            }
        }
        else if (fs::is_regular_file(src_p))
        {
            if (multiSource)
            {
                batchPairs.emplace_back(src_p, dst_p);
            }
            else
            {
                logger->info("begin to copy file: {} to file: {}", src_p.string(), dst_p.string());
                int rc = CopyFile(src_p, dst_p, options, logger);
                if (rc != 0)
                    anyFailed = true;
            }
        }
        else
        {
            std::cerr << "Unsupported source type: " << src_p.string() << std::endl;
            anyFailed = true;
        }
    }

    // 多源模式下统一通过 CopyBatch 执行，避免为每个源单独创建 CopyEngine 线程组
    if (multiSource && !anyFailed && !batchPairs.empty())
    {
        int rc = CopyBatch(batchPairs, options, logger);
        if (rc != 0)
            anyFailed = true;
    }

    return anyFailed ? 1 : 0;
}
