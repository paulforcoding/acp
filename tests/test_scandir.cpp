#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/mainlib.hpp"
#include "base/logger.hpp"
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

static void ensure_clean_dir(const fs::path &p)
{
    std::error_code ec;
    if (fs::exists(p, ec)) fs::remove_all(p, ec);
    fs::create_directories(p, ec);
}

TEST_CASE("ScanDirIntoChannel flat files", "[scandir]")
{
    fs::path src = "/tmp/acp_scan_flat_src";
    fs::path dst = "/tmp/acp_scan_flat_dst";
    ensure_clean_dir(src);
    ensure_clean_dir(dst);

    for (int i = 0; i < 5; ++i)
    {
        std::ofstream(src / ("f" + std::to_string(i) + ".txt")) << "data" << i;
    }

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

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyDir(src, dst / src.filename(), options, logger);
    REQUIRE(rc == 0);

    for (int i = 0; i < 5; ++i)
    {
        REQUIRE(fs::exists(dst / src.filename() / ("f" + std::to_string(i) + ".txt")));
    }

    std::error_code ec;
    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
}

TEST_CASE("ScanDirIntoChannel nested dirs", "[scandir]")
{
    fs::path src = "/tmp/acp_scan_nested_src";
    fs::path dst = "/tmp/acp_scan_nested_dst";
    ensure_clean_dir(src);
    ensure_clean_dir(dst);

    fs::create_directories(src / "a" / "b" / "c");
    std::ofstream(src / "a" / "b" / "c" / "deep.txt") << "deep";
    std::ofstream(src / "top.txt") << "top";

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

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyDir(src, dst / src.filename(), options, logger);
    REQUIRE(rc == 0);

    REQUIRE(fs::exists(dst / src.filename() / "a" / "b" / "c" / "deep.txt"));
    REQUIRE(fs::exists(dst / src.filename() / "top.txt"));

    std::error_code ec;
    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
}

TEST_CASE("ScanDirIntoChannel symlink to file", "[scandir]")
{
    fs::path src = "/tmp/acp_scan_symlink_src";
    fs::path dst = "/tmp/acp_scan_symlink_dst";
    ensure_clean_dir(src);
    ensure_clean_dir(dst);

    std::ofstream(src / "target.txt") << "hello";
    std::error_code ec;
    fs::create_symlink("target.txt", src / "link.txt", ec);
    REQUIRE(!ec);

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

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyDir(src, dst / src.filename(), options, logger);
    REQUIRE(rc == 0);

    REQUIRE(fs::is_symlink(dst / src.filename() / "link.txt"));
    auto target = fs::read_symlink(dst / src.filename() / "link.txt", ec);
    REQUIRE(target == "target.txt");

    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
}

TEST_CASE("ScanDirIntoChannel dangling symlink", "[scandir]")
{
    fs::path src = "/tmp/acp_scan_dangle_src";
    fs::path dst = "/tmp/acp_scan_dangle_dst";
    ensure_clean_dir(src);
    ensure_clean_dir(dst);

    std::error_code ec;
    fs::create_symlink("nonexistent", src / "dangling", ec);
    REQUIRE(!ec);

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

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyDir(src, dst / src.filename(), options, logger);
    REQUIRE(rc == 0);

    REQUIRE(fs::is_symlink(dst / src.filename() / "dangling"));

    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
}

TEST_CASE("ScanDirIntoChannel circular symlink", "[scandir]")
{
    fs::path src = "/tmp/acp_scan_circular_src";
    fs::path dst = "/tmp/acp_scan_circular_dst";
    ensure_clean_dir(src);
    ensure_clean_dir(dst);

    std::error_code ec;
    fs::create_symlink(".", src / "loop", ec);
    REQUIRE(!ec);

    std::ofstream(src / "real.txt") << "real";

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

    auto logger = std::make_shared<ConsoleLogger>();
    // Should not hang — circular symlink to directory should be treated as symlink
    // (lstat sees it as S_ISLNK), so it should be copied as symlink, not followed
    int rc = CopyDir(src, dst / src.filename(), options, logger);
    REQUIRE(rc == 0);

    REQUIRE(fs::exists(dst / src.filename() / "real.txt"));
    // The circular symlink itself should also be copied
    REQUIRE(fs::is_symlink(dst / src.filename() / "loop"));

    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
}

TEST_CASE("ScanDirIntoChannel unsupported type fifo", "[scandir]")
{
    fs::path src = "/tmp/acp_scan_fifo_src";
    fs::path dst = "/tmp/acp_scan_fifo_dst";
    ensure_clean_dir(src);
    ensure_clean_dir(dst);

    std::ofstream(src / "real.txt") << "real";
    int rc = mkfifo((src / "pipe").c_str(), 0644);
    REQUIRE(rc == 0);

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

    auto logger = std::make_shared<ConsoleLogger>();
    int copy_rc = CopyDir(src, dst / src.filename(), options, logger);
    REQUIRE(copy_rc == 0);

    // Regular file should still be copied
    REQUIRE(fs::exists(dst / src.filename() / "real.txt"));
    // FIFO should be skipped (unsupported), not copied
    REQUIRE_FALSE(fs::exists(dst / src.filename() / "pipe"));

    std::error_code ec;
    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
}
