#include "lib/mainlib.hpp"
#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

static void ensure_clean_dir(const fs::path &p)
{
    std::error_code ec;
    if (fs::exists(p, ec)) fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    REQUIRE(!ec);
}

static bool files_equal(const fs::path &a, const fs::path &b)
{
    if (!fs::exists(a) || !fs::exists(b))
        return false;
    if (fs::file_size(a) != fs::file_size(b))
        return false;

    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    std::vector<char> bufa(4096), bufb(4096);
    while (fa.good() && fb.good())
    {
        fa.read(bufa.data(), bufa.size());
        fb.read(bufb.data(), bufb.size());
        if (fa.gcount() != fb.gcount())
            return false;
        if (std::memcmp(bufa.data(), bufb.data(), static_cast<size_t>(fa.gcount())) != 0)
            return false;
    }
    return fa.eof() && fb.eof();
}

static RWCombinedCopyOptions make_empty_options()
{
    RWCombinedCopyOptions options;
    options.ProgramLogLevel = "info";
    options.ProgramLogMode = "file";
    options.ProgramLogFilePath = "/tmp/acp_program.log";
    options.CopyEngine = "libaio";
    options.CopyMode = "CopyOnly";
    options.CopyParallelism = 1;
    options.EnableInotify = false;
    options.PreserveSparseFiles = false;
    options.DirectIO = false;
    options.SyncWrites = false;
    options.IoSize = 1 << 20;
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;
    return options;
}

TEST_CASE("CopyDir zero byte file", "[integration][empty]")
{
    fs::path src_dir = fs::path("testdata") / "empty_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_empty_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Create a zero-byte file
    {
        std::ofstream ofs(src_dir / "empty.dat", std::ios::binary);
    }
    REQUIRE(fs::file_size(src_dir / "empty.dat") == 0);

    auto options = make_empty_options();
    auto logger = InitLogger(options);

    int rc = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(dst_dir / "empty.dat"));
    REQUIRE(fs::file_size(dst_dir / "empty.dat") == 0);

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CopyDir empty directory", "[integration][empty]")
{
    fs::path src_dir = fs::path("testdata") / "empty_dir_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_empty_dir_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    auto options = make_empty_options();
    auto logger = InitLogger(options);

    int rc = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc == 0);
    // Destination should exist (created by CopyDir) but have no files
    REQUIRE(fs::exists(dst_dir));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CopyDir directory with only empty subdirs", "[integration][empty]")
{
    fs::path src_dir = fs::path("testdata") / "empty_nested_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_empty_nested_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir / "sub1");
    ensure_clean_dir(src_dir / "sub2" / "deep");
    ensure_clean_dir(dst_dir);

    auto options = make_empty_options();
    auto logger = InitLogger(options);

    int rc = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(fs::is_directory(dst_dir / "sub1"));
    REQUIRE(fs::is_directory(dst_dir / "sub2" / "deep"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
