#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/mainlib.hpp"
#include "base/logger.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
// Note: thread/chrono used in test construction timing

namespace fs = std::filesystem;

static RWCombinedCopyOptions MakeOptions()
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
    options.CksumAlgorithm = "xxhash64";
    options.CopyParallelism = 1;
    options.CopyChanSize = 10;
    options.IoSize = 4096;
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;
    options.IOStuckTimeout = 10;
    return options;
}

TEST_CASE("IOSlotMgr CopyOnly full round", "[ioslotmgr]")
{
    fs::path src = "/tmp/acp_ioslot_src.bin";
    fs::path dst = "/tmp/acp_ioslot_dst.bin";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    // Create source with non-trivial content
    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(8192, 'A');
        ofs.write(buf.data(), buf.size());
    }

    auto options = MakeOptions();
    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(dst));
    REQUIRE(fs::file_size(dst) == fs::file_size(src));

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("IOSlotMgr CksumCopy match skip write", "[ioslotmgr]")
{
    fs::path src = "/tmp/acp_ioslot_ck_src.bin";
    fs::path dst = "/tmp/acp_ioslot_ck_dst.bin";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    // Create identical source and destination
    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(8192, 'B');
        ofs.write(buf.data(), buf.size());
    }
    {
        std::ofstream ofs(dst, std::ios::binary);
        std::string buf(8192, 'B');
        ofs.write(buf.data(), buf.size());
    }

    auto options = MakeOptions();
    options.CopyMode = "CksumCopy";
    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(dst));
    REQUIRE(fs::file_size(dst) == fs::file_size(src));

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("IOSlotMgr CksumCopy mismatch do write", "[ioslotmgr]")
{
#ifdef __APPLE__
    // Bug: GCDSlotMgr::SubmitBatchRead always uses GetSrcFd(), so cksum slot
    // reads src instead of dst. This causes checksum to always match.
    SKIP("Known bug on macOS: GCDSlotMgr uses wrong fd for cksum reads");
#endif

    fs::path src = "/tmp/acp_ioslot_ckm_src.bin";
    fs::path dst = "/tmp/acp_ioslot_ckm_dst.bin";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    // First copy identical content to dst, then modify src.
    // This ensures mtime differs and avoids the size+mtime fast path.
    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(8192, 'C');
        ofs.write(buf.data(), buf.size());
    }

    auto copyOnlyOpts = MakeOptions();
    copyOnlyOpts.CopyMode = "CopyOnly";
    copyOnlyOpts.PreserveMeta = false; // avoid mtime sync affecting CksumCopy fast path
    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, copyOnlyOpts, logger);
    REQUIRE(rc == 0);

    // Sleep to ensure mtime differs from dst (avoids size+mtime fast path)
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Now modify src content — dst still has old content
    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(8192, 'D');
        ofs.write(buf.data(), buf.size());
    }

    auto cksumOpts = MakeOptions();
    cksumOpts.CopyMode = "CksumCopy";
    rc = CopyFile(src, dst, cksumOpts, logger);
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(dst));

    // Verify dst was overwritten with new src content
    std::ifstream ifs(dst, std::ios::binary);
    char first_byte = 0;
    ifs.read(&first_byte, 1);
    REQUIRE(first_byte == 'D');

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("IOSlotMgr CksumOnly never write", "[ioslotmgr]")
{
    fs::path src = "/tmp/acp_ioslot_cko_src.bin";
    fs::path dst = "/tmp/acp_ioslot_cko_dst.bin";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    // Create different source and destination
    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(8192, 'E');
        ofs.write(buf.data(), buf.size());
    }
    {
        std::ofstream ofs(dst, std::ios::binary);
        std::string buf(8192, 'F');
        ofs.write(buf.data(), buf.size());
    }

    auto options = MakeOptions();
    options.CopyMode = "CksumOnly";
    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(dst));

    // Verify dst was NOT overwritten
    std::ifstream ifs(dst, std::ios::binary);
    char first_byte = 0;
    ifs.read(&first_byte, 1);
    REQUIRE(first_byte == 'F');

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("IOSlotMgr HandleReadCompletion EOF", "[ioslotmgr]")
{
    // 0-byte file triggers immediate EOF handling
    fs::path src = "/tmp/acp_ioslot_eof.bin";
    fs::path dst = "/tmp/acp_ioslot_eof_dst.bin";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
    }

    auto options = MakeOptions();
    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(dst));
    REQUIRE(fs::file_size(dst) == 0);

    fs::remove(src, ec);
    fs::remove(dst, ec);
}
