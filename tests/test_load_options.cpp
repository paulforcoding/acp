#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/mainlib.hpp"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

static void write_json(const std::string &path, const std::string &content)
{
    std::ofstream ofs(path);
    REQUIRE(ofs.good());
    ofs << content;
}

TEST_CASE("LoadCopyOptions missing field", "[config]")
{
    std::string cfg = "tests/tmp_bad_config.json";
    write_json(cfg, R"({
        "LogLevel": "info",
        "LogMode": "console",
        "CopyEngine": "libaio",
        "CopyMode": "CopyOnly",
        "CopyParallelism": 2,
        "CopyChanSize": 10
    })");

    auto opt = LoadCopyOptions(cfg);
    REQUIRE_FALSE(opt.has_value());

    fs::remove(cfg);
}

TEST_CASE("LoadCopyOptions invalid IOSize", "[config]")
{
    std::string cfg = "tests/tmp_bad_io.json";
    write_json(cfg, R"({
        "LogLevel": "info",
        "LogMode": "console",
        "CopyEngine": "libaio",
        "CopyMode": "CopyOnly",
        "CopyParallelism": 2,
        "CopyChanSize": 10,
        "CopyOptions": {
            "IOSize": 0,
            "QueueDepth": 8,
            "Batch": 4,
            "IOReapWait": 1
        }
    })");

    auto opt = LoadCopyOptions(cfg);
    REQUIRE_FALSE(opt.has_value());

    fs::remove(cfg);
}

TEST_CASE("LoadCopyOptions invalid CopyChanSize", "[config]")
{
    std::string cfg = "tests/tmp_bad_chan.json";
    write_json(cfg, R"({
        "LogLevel": "info",
        "LogMode": "console",
        "CopyEngine": "libaio",
        "CopyMode": "CopyOnly",
        "CopyParallelism": 2,
        "CopyChanSize": 0,
        "CopyOptions": {
            "IOSize": 1048576,
            "QueueDepth": 8,
            "Batch": 4,
            "IOReapWait": 1
        }
    })");

    auto opt = LoadCopyOptions(cfg);
    REQUIRE_FALSE(opt.has_value());

    fs::remove(cfg);
}

TEST_CASE("LoadCopyOptions defaults", "[config]")
{
    std::string cfg = "tests/tmp_good_config.json";
    write_json(cfg, R"({
        "LogLevel": "info",
        "LogMode": "console",
        "CopyEngine": "libaio",
        "CopyMode": "CopyOnly",
        "CopyParallelism": 2,
        "CopyChanSize": 10,
        "CopyOptions": {
            "IOSize": 1048576,
            "QueueDepth": 8,
            "Batch": 4,
            "IOReapWait": 1
        }
    })");

    auto opt = LoadCopyOptions(cfg);
    REQUIRE(opt.has_value());
    REQUIRE(opt->IoSize == 1048576);
    REQUIRE(opt->QueueDepth == 8);
    REQUIRE(opt->Batch == 4);
    REQUIRE(opt->CopyParallelism == 2);
    REQUIRE(opt->CopyChanSize == 10);
    REQUIRE(opt->CksumAlgorithm == "xxhash64"); // default
    REQUIRE(opt->DirectIO == false);            // default
    REQUIRE(opt->EnableInotify == false);       // default
    REQUIRE(opt->PreserveSparseFiles == true);  // default (matches GNU cp --sparse=auto)

    fs::remove(cfg);
}
