#include "lib/mainlib.hpp"
#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

TEST_CASE("Watchdog timing verification with large file copy", "[watchdog]")
{
    fs::path src_dir = fs::path("testdata") / "watchdog_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_watchdog_dst_" + std::to_string(::getpid()));

    std::error_code ec;
    if (fs::exists(src_dir, ec)) fs::remove_all(src_dir, ec);
    if (fs::exists(dst_dir, ec)) fs::remove_all(dst_dir, ec);
    fs::create_directories(src_dir, ec);
    fs::create_directories(dst_dir, ec);
    REQUIRE(!ec);

    // Create a 256MB file
    fs::path src_file = src_dir / "big.bin";
    int fd = ::open(src_file.c_str(), O_CREAT | O_WRONLY, 0644);
    REQUIRE(fd >= 0);
    std::vector<char> buf(1024 * 1024, 'X');
    for (int i = 0; i < 256; ++i)
    {
        ssize_t n = ::write(fd, buf.data(), buf.size());
        REQUIRE(n == static_cast<ssize_t>(buf.size()));
    }
    ::close(fd);
    REQUIRE(fs::file_size(src_file) == 256 * 1024 * 1024LL);

    RWCombinedCopyOptions options;
    options.ProgramLogLevel = "info";
    options.ProgramLogMode = "file";
    options.ProgramLogFilePath = "/tmp/acp_program.log";
    options.FileLogEnabled = false;
#ifdef __APPLE__
    options.CopyEngine = "gcd";
#else
    options.CopyEngine = "libaio";
#endif
    options.CopyMode = "CopyOnly";
    options.CksumAlgorithm = "xxhash64";
    options.CopyParallelism = 1;
    options.CopyChanSize = 10;
    options.EnableInotify = false;
    options.PreserveSparseFiles = false;
    options.PreserveMeta = false;
    options.DirectIO = false;
    options.SyncWrites = false;
    options.IoSize = 1 * 1024 * 1024;
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;
    options.IOStuckTimeout = 10;  // 10 seconds per code logic

    // Clear previous log
    std::ofstream ofs("/tmp/acp_program.log", std::ios::trunc);
    ofs.close();

    auto logger = InitLogger(options);
    auto start = std::chrono::steady_clock::now();
    int rc = CopyDir(src_dir, dst_dir, options, logger);
    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Parse log for watchdog messages
    std::ifstream log("/tmp/acp_program.log");
    std::string line;
    bool watchdogTriggered = false;
    while (std::getline(log, line))
    {
        if (line.find("Watchdog: IO stuck") != std::string::npos)
        {
            watchdogTriggered = true;
            INFO("Watchdog triggered: " << line);
            INFO("Elapsed time: " << elapsed_ms << " ms");
        }
    }

    INFO("Copy result: rc=" << rc << ", elapsed=" << elapsed_ms << "ms");

    // The copy should succeed (256MB on SSD < 10s)
    REQUIRE(rc == 0);
    REQUIRE_FALSE(watchdogTriggered);

    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
