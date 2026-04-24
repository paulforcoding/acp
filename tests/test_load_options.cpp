#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/mainlib.hpp"
#include "lib/combined/combined.hpp"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

static void write_json(const std::string &path, const std::string &content)
{
    std::ofstream ofs(path);
    REQUIRE(ofs.good());
    ofs << content;
}

TEST_CASE("MergeCopyOptions missing CopyOptions", "[config]")
{
    std::string cfg = "tests/tmp_bad_config.json";
    write_json(cfg, R"({
        "CopyEngine": "libaio",
        "CopyMode": "CopyOnly",
        "CopyParallelism": 2,
        "CopyChanSize": 10
    })");

    RWCombinedCopyOptions base;
    auto opt = MergeCopyOptions(base, cfg);
    REQUIRE(opt.has_value());
    // CopyOptions missing -> keep base values
    REQUIRE(opt->IoSize == base.IoSize);
    REQUIRE(opt->QueueDepth == base.QueueDepth);

    fs::remove(cfg);
}

TEST_CASE("MergeCopyOptions invalid JSON", "[config]")
{
    std::string cfg = "tests/tmp_invalid.json";
    write_json(cfg, "{ invalid json }");

    RWCombinedCopyOptions base;
    auto opt = MergeCopyOptions(base, cfg);
    REQUIRE_FALSE(opt.has_value());

    fs::remove(cfg);
}

TEST_CASE("MergeCopyOptions partial override", "[config]")
{
    std::string cfg = "tests/tmp_partial.json";
    write_json(cfg, R"({
        "CopyEngine": "libaio",
        "CopyMode": "CksumCopy",
        "CopyParallelism": 8,
        "CopyOptions": {
            "QueueDepth": 32
        }
    })");

    RWCombinedCopyOptions base;
    base.CopyEngine = "liburing";
    base.CopyMode = "CopyOnly";
    base.CopyParallelism = 1;
    base.QueueDepth = 16;
    base.IoSize = 131072;

    auto opt = MergeCopyOptions(base, cfg);
    REQUIRE(opt.has_value());
    REQUIRE(opt->CopyEngine == "libaio");
    REQUIRE(opt->CopyMode == "CksumCopy");
    REQUIRE(opt->CopyParallelism == 8);
    REQUIRE(opt->QueueDepth == 32);
    // Unspecified fields keep base values
    REQUIRE(opt->IoSize == 131072);
    REQUIRE(opt->Batch == base.Batch);

    fs::remove(cfg);
}

TEST_CASE("MergeCopyOptions full override", "[config]")
{
    std::string cfg = "tests/tmp_full.json";
    write_json(cfg, R"({
        "ProgramLogLevel": "debug",
        "ProgramLogMode": "file",
        "ProgramLogFilePath": "/tmp/test.log",
        "FileLogEnabled": true,
        "FileLogMode": "console",
        "FileLogIntervalSec": 10,
        "FileLogPath": "/tmp/test.json",
        "CopyEngine": "libaio",
        "CopyMode": "CksumOnly",
        "CksumAlgorithm": "md5",
        "CopyParallelism": 4,
        "CopyChanSize": 50,
        "EnableInotify": true,
        "PreserveSparseFiles": false,
        "PreserveMeta": false,
        "DirectIO": true,
        "SyncWrites": false,
        "CopyOptions": {
            "IOSize": 65536,
            "QueueDepth": 64,
            "Batch": 16,
            "IOReapWait": 2,
            "IOStuckTimeout": 30
        }
    })");

    RWCombinedCopyOptions base;
    auto opt = MergeCopyOptions(base, cfg);
    REQUIRE(opt.has_value());
    REQUIRE(opt->ProgramLogLevel == "debug");
    REQUIRE(opt->ProgramLogMode == "file");
    REQUIRE(opt->ProgramLogFilePath == "/tmp/test.log");
    REQUIRE(opt->FileLogEnabled == true);
    REQUIRE(opt->FileLogMode == "console");
    REQUIRE(opt->FileLogIntervalSec == 10);
    REQUIRE(opt->FileLogPath == "/tmp/test.json");
    REQUIRE(opt->CopyEngine == "libaio");
    REQUIRE(opt->CopyMode == "CksumOnly");
    REQUIRE(opt->CksumAlgorithm == "md5");
    REQUIRE(opt->CopyParallelism == 4);
    REQUIRE(opt->CopyChanSize == 50);
    REQUIRE(opt->EnableInotify == true);
    REQUIRE(opt->PreserveSparseFiles == false);
    REQUIRE(opt->PreserveMeta == false);
    REQUIRE(opt->DirectIO == true);
    REQUIRE(opt->SyncWrites == false);
    REQUIRE(opt->IoSize == 65536);
    REQUIRE(opt->QueueDepth == 64);
    REQUIRE(opt->Batch == 16);
    REQUIRE(opt->IOReapWait == 2);
    REQUIRE(opt->IOStuckTimeout == 30);

    fs::remove(cfg);
}
