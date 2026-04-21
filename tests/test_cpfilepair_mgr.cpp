#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/combined/combined.hpp"
#include "base/logger.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static void ensure_clean_dir(const fs::path &p)
{
    std::error_code ec;
    if (fs::exists(p, ec)) fs::remove_all(p, ec);
    fs::create_directories(p, ec);
}

static void write_file(const fs::path &p, const std::string &content)
{
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

TEST_CASE("CPFilePairMgr PeekFront after AddFilePair", "[cpfilepairmgr]")
{
    auto logger = std::make_shared<ConsoleLogger>();
    RWCombinedCopyOptions opts;
    opts.IoSize = 4096;
    opts.DirectIO = false;
    opts.SyncWrites = false;
    opts.ProgramLogMode = "file";
    opts.ProgramLogFilePath = "/tmp/acp_program.log";

    CPFilePairMgr mgr(opts, logger, nullptr);
    // Before AddFilePair, channel is empty but not closed
    REQUIRE(mgr.IsStopRequested() == false);

    auto add_res = mgr.AddFilePair("/tmp/a", "/tmp/b");
    REQUIRE(add_res.has_value());
    // After AddFilePair, HasPendingWork should be true
    REQUIRE(mgr.HasPendingWork());
}

TEST_CASE("CPFilePairMgr GetNextReadIO single file", "[cpfilepairmgr]")
{
    std::string src_dir = "tests/tmp_mgr_single_src";
    std::string dst_dir = "tests/tmp_mgr_single_dst";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    std::string src_file = src_dir + "/file.txt";
    write_file(src_file, "hello world");

    auto logger = std::make_shared<ConsoleLogger>();
    RWCombinedCopyOptions opts;
    opts.IoSize = 4096;
    opts.DirectIO = false;
    opts.SyncWrites = false;
    opts.ProgramLogMode = "file";
    opts.ProgramLogFilePath = "/tmp/acp_program.log";

    CPFilePairMgr mgr(opts, logger, nullptr);
    auto add_res = mgr.AddFilePair(src_file, dst_dir + "/file.txt");
    REQUIRE(add_res.has_value());
    mgr.SetStopFlag();

    auto next = mgr.GetNextReadIO();
    REQUIRE(next.has_value());
    REQUIRE(next.value() != nullptr);
    REQUIRE(next.value()->GetSrcPath() == src_file);

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CPFilePairMgr GetNextReadIO returns null at end", "[cpfilepairmgr]")
{
    auto logger = std::make_shared<ConsoleLogger>();
    RWCombinedCopyOptions opts;
    opts.IoSize = 4096;
    opts.DirectIO = false;
    opts.SyncWrites = false;
    opts.ProgramLogMode = "file";
    opts.ProgramLogFilePath = "/tmp/acp_program.log";

    CPFilePairMgr mgr(opts, logger, nullptr);
    mgr.SetStopFlag();

    auto next = mgr.GetNextReadIO();
    REQUIRE(next.has_value());
    REQUIRE(next.value() == nullptr);
}

TEST_CASE("CPFilePairMgr WaitForWorkOrClose lifecycle", "[cpfilepairmgr]")
{
    auto logger = std::make_shared<ConsoleLogger>();
    RWCombinedCopyOptions opts;
    opts.IoSize = 4096;
    opts.DirectIO = false;
    opts.SyncWrites = false;
    opts.ProgramLogMode = "file";
    opts.ProgramLogFilePath = "/tmp/acp_program.log";

    CPFilePairMgr mgr(opts, logger, nullptr);
    // Empty and not closed: should wait then return true (timeout)
    REQUIRE(mgr.WaitForWorkOrClose(std::chrono::milliseconds(10)));

    mgr.SetStopFlag();
    // Empty and closed: should return false immediately
    REQUIRE_FALSE(mgr.WaitForWorkOrClose(std::chrono::milliseconds(10)));

    // Reset for next test: add a file pair before closing
    CPFilePairMgr mgr2(opts, logger, nullptr);
    auto add_res = mgr2.AddFilePair("/tmp/a", "/tmp/b");
    REQUIRE(add_res.has_value());
    mgr2.SetStopFlag();
    // Pending work exists but closed: should return true (has pending)
    REQUIRE(mgr2.WaitForWorkOrClose(std::chrono::milliseconds(10)));
}

TEST_CASE("CPFilePairMgr CheckWriteComplete with finished file", "[cpfilepairmgr]")
{
    std::string src_dir = "tests/tmp_mgr_write_src";
    std::string dst_dir = "tests/tmp_mgr_write_dst";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    std::string src_file = src_dir + "/file.txt";
    write_file(src_file, "abc");

    auto logger = std::make_shared<ConsoleLogger>();
    RWCombinedCopyOptions opts;
    opts.IoSize = 4096;
    opts.DirectIO = false;
    opts.SyncWrites = false;
    opts.ProgramLogMode = "file";
    opts.ProgramLogFilePath = "/tmp/acp_program.log";

    CPFilePairMgr mgr(opts, logger, nullptr);
    auto add_res = mgr.AddFilePair(src_file, dst_dir + "/file.txt");
    REQUIRE(add_res.has_value());
    mgr.SetStopFlag();

    auto next = mgr.GetNextReadIO();
    REQUIRE(next.has_value());
    REQUIRE(next.value() != nullptr);

    // Simulate write completion
    next.value()->UpdateWrittenBytes(next.value()->GetSrcFileSize());
    REQUIRE(next.value()->IsWriteFinished());

    auto check_res = mgr.CheckWriteComplete(next.value());
    REQUIRE(check_res.has_value());

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CPFilePairMgr multiple files round robin", "[cpfilepairmgr]")
{
    std::string src_dir = "tests/tmp_mgr_multi_src";
    std::string dst_dir = "tests/tmp_mgr_multi_dst";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file(src_dir + "/a.txt", "aaa");
    write_file(src_dir + "/b.txt", "bbb");

    auto logger = std::make_shared<ConsoleLogger>();
    RWCombinedCopyOptions opts;
    opts.IoSize = 4096;
    opts.DirectIO = false;
    opts.SyncWrites = false;
    opts.ProgramLogMode = "file";
    opts.ProgramLogFilePath = "/tmp/acp_program.log";

    CPFilePairMgr mgr(opts, logger, nullptr);
    auto r1 = mgr.AddFilePair(src_dir + "/a.txt", dst_dir + "/a.txt");
    REQUIRE(r1.has_value());
    auto r2 = mgr.AddFilePair(src_dir + "/b.txt", dst_dir + "/b.txt");
    REQUIRE(r2.has_value());
    mgr.SetStopFlag();

    auto n1 = mgr.GetNextReadIO();
    REQUIRE(n1.has_value());
    REQUIRE(n1.value() != nullptr);
    // First file should be a.txt (FIFO order from list)
    REQUIRE(n1.value()->GetSrcPath() == src_dir + "/a.txt");

    // Simulate it being read-finished
    n1.value()->SetReadFinished();

    auto n2 = mgr.GetNextReadIO();
    REQUIRE(n2.has_value());
    REQUIRE(n2.value() != nullptr);
    REQUIRE(n2.value()->GetSrcPath() == src_dir + "/b.txt");

    n2.value()->SetReadFinished();

    auto n3 = mgr.GetNextReadIO();
    REQUIRE(n3.has_value());
    REQUIRE(n3.value() == nullptr);

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
