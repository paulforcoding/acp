#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/mainlib.hpp"
#include "base/logger.hpp"
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

static RWCombinedCopyOptions MakeBackendOptions()
{
    RWCombinedCopyOptions options;
    options.ProgramLogLevel = "error";
    options.ProgramLogMode = "console";
    options.FileLogEnabled = false;
#ifdef __APPLE__
    options.CopyEngine = "gcd";
#else
    options.CopyEngine = "libaio";
#endif
    options.CopyMode = "CopyOnly";
    options.CopyParallelism = 1;
    options.CopyChanSize = 10;
    options.IoSize = 4096;
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;
    options.IOStuckTimeout = 10;
    return options;
}

#ifdef __APPLE__

TEST_CASE("GCDSlotMgr read error propagation", "[backend][gcd]")
{
    // Bug #6: GCDSlotMgr IOReap ignores HandleReadCompletion errors.
    // We test by copying a normal file and verifying the operation succeeds
    // (this establishes baseline); the bug is that if an error were to occur,
    // it would be silently logged rather than propagated.
    fs::path src = "/tmp/acp_backend_gcd_src.bin";
    fs::path dst = "/tmp/acp_backend_gcd_dst.bin";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(8192, 'G');
        ofs.write(buf.data(), buf.size());
    }

    auto options = MakeBackendOptions();
    options.CopyEngine = "gcd";
    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(dst));
    REQUIRE(fs::file_size(dst) == fs::file_size(src));

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("GCDSlotMgr write error propagation", "[backend][gcd]")
{
    // Bug #6: GCDSlotMgr IOReap ignores HandleWriteCompletion errors.
    // Baseline test: normal copy should succeed.
    fs::path src = "/tmp/acp_backend_gcdw_src.bin";
    fs::path dst = "/tmp/acp_backend_gcdw_dst.bin";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(8192, 'H');
        ofs.write(buf.data(), buf.size());
    }

    auto options = MakeBackendOptions();
    options.CopyEngine = "gcd";
    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(dst));

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("Backend DirectIO unaligned size", "[backend]")
{
    // DirectIO requires sector-aligned buffers and file sizes.
    // On macOS DirectIO is not supported, but the option should be rejected.
    fs::path src = "/tmp/acp_backend_dio_src.bin";
    fs::path dst = "/tmp/acp_backend_dio_dst.bin";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        // Size not aligned to 512 bytes
        std::string buf(100, 'I');
        ofs.write(buf.data(), buf.size());
    }

    auto options = MakeBackendOptions();
#ifdef __APPLE__
    options.DirectIO = true;
    auto logger = std::make_shared<ConsoleLogger>();
    // macOS rejects DirectIO at entry point
    int rc = CopyFile(src, dst, options, logger);
    REQUIRE(rc == 1);
#else
    // On Linux with DirectIO, unaligned final block should still work
    // (last block is handled with aligned buffer)
    options.DirectIO = true;
    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(dst));
#endif

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

#else // Linux

TEST_CASE("AIOSlotMgr io_setup failure", "[backend][libaio]")
{
    // QueueDepth too large should cause io_setup to fail
    fs::path src = "/tmp/acp_backend_aio_src.bin";
    fs::path dst = "/tmp/acp_backend_aio_dst.bin";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(4096, 'J');
        ofs.write(buf.data(), buf.size());
    }

    auto options = MakeBackendOptions();
    options.CopyEngine = "libaio";
    options.QueueDepth = 1000000; // Unrealistically large
    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, options, logger);
    // Should fail during Init() due to io_setup failure
    REQUIRE(rc == 1);

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("Backend DirectIO unaligned size", "[backend]")
{
    fs::path src = "/tmp/acp_backend_dio_src.bin";
    fs::path dst = "/tmp/acp_backend_dio_dst.bin";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(100, 'I');
        ofs.write(buf.data(), buf.size());
    }

    auto options = MakeBackendOptions();
    options.DirectIO = true;
    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(dst));

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

#ifdef ENABLE_LIBURING

TEST_CASE("UIOSlotMgr io_uring_queue_init failure", "[backend][liburing]")
{
    fs::path src = "/tmp/acp_backend_uring_src.bin";
    fs::path dst = "/tmp/acp_backend_uring_dst.bin";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(4096, 'K');
        ofs.write(buf.data(), buf.size());
    }

    auto options = MakeBackendOptions();
    options.CopyEngine = "liburing";
    options.QueueDepth = 1000000; // Unrealistically large
    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, options, logger);
    // Should fail during Init() due to io_uring_queue_init failure
    REQUIRE(rc == 1);

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

#endif // ENABLE_LIBURING
#endif // __APPLE__
