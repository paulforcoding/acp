#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/combined/combined.hpp"
#include "base/logger.hpp"
#include <filesystem>

TEST_CASE("CPFilePair CheckAndInit with real file", "[integration][cpfilepair]")
{
    namespace fs = std::filesystem;
    std::string src = "testdata/hello.txt";
    REQUIRE(fs::exists(src));

    std::string dst_dir = "tests/tmp_out";
    std::string dst = dst_dir + "/hello_copy.txt";

    // cleanup
    std::error_code ec;
    fs::remove_all(dst_dir, ec);
    fs::create_directories(dst_dir, ec);

    auto logger = std::make_shared<ConsoleLogger>();
    RWCombinedCopyOptions opts;
    opts.IoSize = 4096;
    opts.DirectIO = false;
    opts.SyncWrites = false;

    CPFilePair p(src, dst, opts.IoSize, opts.DirectIO, opts.SyncWrites, logger);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    // check src fd valid
    REQUIRE(p.GetSrcFd() >= 0);
    // dst file should be created (CheckAndInit creates it)
    REQUIRE(fs::exists(dst));

    // cleanup
    fs::remove_all(dst_dir, ec);
}

