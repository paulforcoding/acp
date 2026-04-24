#include <iostream>
#include <string>
#include <filesystem>
#include <algorithm>
#include "lib/mainlib.hpp"

constexpr const char* kVersion = "0.4.0";

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
        "    " << program_name << " <src_path> <dst_path>\n"
        "\n"
        "POSITIONAL ARGUMENTS\n"
        "    src_path    Source path (regular file or directory)\n"
        "    dst_path    Destination path (regular file or directory)\n"
        "\n"
        "CONFIG\n"
        "    Loaded from ./acp_config.json or /etc/acp_config.json (first found wins).\n"
        "\n"
        "CONFIGURATION FIELDS\n"
        "─────────────────────────────────────────────────────────────────────────\n"
        "LOGGING\n"
        "    Note: ProgramLog* controls general application diagnostics (normal/abnormal\n"
        "    program behavior); FileLog* controls per-file copy progress telemetry\n"
        "    (what files are being copied and their status).\n"
        "\n"
        "    ProgramLogLevel       string   default: \"info\"\n"
        "                          Values: \"trace\", \"debug\", \"info\", \"warn\", \"error\", \"critical\"\n"
        "\n"
        "    ProgramLogMode        string   default: \"console\"\n"
        "                          Values: \"console\", \"file\"\n"
        "\n"
        "    ProgramLogFilePath    string   default: \"/tmp/acp_program.log\"\n"
        "                          Effective when ProgramLogMode == \"file\".\n"
        "\n"
        "    FileLogEnabled        bool     default: false\n"
        "                          If true, enable file_info event output.\n"
        "\n"
        "    FileLogMode           string   default: \"file\"\n"
        "                          Values: \"console\", \"file\"\n"
        "                          console — output NDJSON events to stdout.\n"
        "                          file    — append NDJSON events to FileLogPath.\n"
        "\n"
        "    FileLogIntervalSec    int      default: 5\n"
        "                          Progress summary flush interval in seconds.\n"
        "\n"
        "    FileLogPath           string   default: \"/tmp/acp_file_info.json\"\n"
        "                          NDJSON event log path (file mode).\n"
        "                          Also used for state snapshot when FileLogMode==\"console\".\n"
        "\n"
        "COPY ENGINE\n"
        "    CopyEngine            string   required\n"
        "                          Values: \"libaio\", \"liburing\" (Linux); \"libaio\" on macOS\n"
        "                          \"liburing\" requires ENABLE_LIBURING=ON at build time.\n"
        "                          macOS always uses GCD; CopyEngine value is ignored but must be present.\n"
        "\n"
        "    CopyMode              string   required\n"
        "                          Values: \"CopyOnly\", \"CksumCopy\", \"CksumOnly\"\n"
        "                          CopyOnly   — standard read/write copy.\n"
        "                          CksumCopy  — verify checksum before writing; skip matched blocks.\n"
        "                          CksumOnly  — verify checksum only; write nothing; log mismatches\n"
        "                                       to ./cksum_result.log.\n"
        "\n"
        "    CksumAlgorithm        string   default: \"xxhash64\"\n"
        "                          Values: \"xxhash64\", \"md5\", \"sha256\"\n"
        "\n"
        "    CopyParallelism       int      required, min: 1\n"
        "                          Number of concurrent copy worker threads.\n"
        "\n"
        "    CopyChanSize          int      required, min: 1\n"
        "                          Bounded channel capacity for file pair dispatch.\n"
        "\n"
        "    EnableInotify         bool     default: false\n"
        "                          If true, monitor source directory for changes after initial copy.\n"
        "                          On Linux: inotify; on macOS: FSEvents.\n"
        "                          The process runs indefinitely until interrupted.\n"
        "\n"
        "IO OPTIONS (nested under \"CopyOptions\")\n"
        "    IOSize                size_t   required, default: 1048576\n"
        "                          Single I/O unit size in bytes.\n"
        "\n"
        "    QueueDepth            size_t   required, min: 1\n"
        "                          Max in-flight asynchronous I/O requests per worker.\n"
        "\n"
        "    Batch                 int      required\n"
        "                          Number of I/Os to submit in one batch.\n"
        "\n"
        "    IOReapWait            int      required, unit: seconds\n"
        "                          Max wait time for I/O completion events.\n"
        "\n"
        "    IOStuckTimeout        int      default: 0 (disabled)\n"
        "                          Max seconds without progress before declaring stuck.\n"
        "                          Only effective when EnableInotify=false.\n"
        "                          If >0, RunQueue exits with error when IO hangs.\n"
        "\n"
        "ADVANCED\n"
        "    DirectIO              bool     default: false\n"
        "                          Use O_DIRECT for unbuffered I/O.\n"
        "\n"
        "    SyncWrites            bool     default: false\n"
        "                          Sync data to disk after each write (fsync per write).\n"
        "\n"
        "    PreserveSparseFiles   bool     default: true\n"
        "                          Preserve sparse file holes (detected heuristically, like cp --sparse=auto).\n"
        "\n"
        "CONFIG EXAMPLE\n"
        "─────────────────────────────────────────────────────────────────────────\n"
        "{\n"
        "  \"ProgramLogLevel\": \"info\",\n"
        "  \"ProgramLogMode\": \"console\",\n"
        "  \"ProgramLogFilePath\": \"/tmp/acp_program.log\",\n"
        "  \"FileLogEnabled\": false,\n"
        "  \"FileLogMode\": \"file\",\n"
        "  \"FileLogIntervalSec\": 5,\n"
        "  \"FileLogPath\": \"/tmp/acp_file_info.json\",\n"
        "  \"CopyEngine\": \"libaio\",\n"
        "  \"CopyMode\": \"CopyOnly\",\n"
        "  \"CksumAlgorithm\": \"xxhash64\",\n"
        "  \"CopyParallelism\": 1,\n"
        "  \"CopyChanSize\": 10,\n"
        "  \"EnableInotify\": false,\n"
        "  \"PreserveSparseFiles\": true,\n"
        "  \"DirectIO\": false,\n"
        "  \"SyncWrites\": false,\n"
        "  \"CopyOptions\": {\n"
        "    \"QueueDepth\": 8,\n"
        "    \"IOSize\": 1048576,\n"
        "    \"Batch\": 8,\n"
        "    \"IOReapWait\": 1,\n"
        "    \"IOStuckTimeout\": 0\n"
        "  }\n"
        "}\n"
        "\n"
        "UNSUPPORTED FILE TYPES\n"
        "─────────────────────────────────────────────────────────────────────────\n"
        "    acp only copies regular files, directories, and symbolic links.\n"
        "    The following file types are logged as 'skipped' and the copy continues:\n"
        "\n"
        "        • Block devices\n"
        "        • Character devices\n"
        "        • FIFOs (named pipes)\n"
        "        • Sockets\n"
        "        • Whiteout files\n"
        "\n"
        "    When FileLogEnabled is true, skipped files appear in the NDJSON status\n"
        "    stream with event_type 'file_error' or 'file_unsupported'.\n"
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