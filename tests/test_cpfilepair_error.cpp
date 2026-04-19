#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/combined/combined.hpp"
#include "base/logger.hpp"
#include <filesystem>
#include <sys/stat.h>

namespace fs = std::filesystem;

TEST_CASE("CPFilePair source file does not exist", "[cpfilepair][error]")
{
    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p("/nonexistent/file/path.txt", "/tmp/dst.txt", 4096, false, false, false, logger);
    auto init_res = p.CheckAndInit();
    REQUIRE_FALSE(init_res.has_value());
}

TEST_CASE("CPFilePair source is a fifo", "[cpfilepair][error]")
{
    std::string src_fifo = "tests/tmp_fifo_src";
    std::string dst_dir = "tests/tmp_fifo_dst";

    std::error_code ec;
    fs::remove(src_fifo, ec);
    fs::remove_all(dst_dir, ec);
    fs::create_directories(dst_dir, ec);

    int rc = mkfifo(src_fifo.c_str(), 0644);
    REQUIRE(rc == 0);

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src_fifo, dst_dir + "/out", 4096, false, false, false, logger);
    auto init_res = p.CheckAndInit();
    REQUIRE_FALSE(init_res.has_value());
    REQUIRE(init_res.error().Code() == ENOTSUP);

    fs::remove(src_fifo, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CPFilePair dst directory creation failure", "[cpfilepair][error]")
{
    std::string src = "tests/tmp_badperm_src.dat";
    std::string dst = "/proc/nonexistent/dir/out.dat";

    std::error_code ec;
    fs::remove(src, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        ofs.write("x", 1);
    }

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, 4096, false, false, false, logger);
    auto init_res = p.CheckAndInit();
    // Should fail because parent directory cannot be created under /proc
    REQUIRE_FALSE(init_res.has_value());

    fs::remove(src, ec);
}
