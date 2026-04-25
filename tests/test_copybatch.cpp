#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/mainlib.hpp"
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

TEST_CASE("CopyBatch multiple files", "[integration][batch]")
{
    fs::path src_dir = "/tmp/acp_batch_src";
    fs::path dst_dir = "/tmp/acp_batch_dst";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Create multiple source files
    for (int i = 0; i < 5; ++i)
    {
        std::ofstream ofs(src_dir / ("file" + std::to_string(i) + ".txt"));
        ofs << "content" << i;
    }

    std::vector<std::pair<fs::path, fs::path>> pairs;
    for (int i = 0; i < 5; ++i)
    {
        pairs.emplace_back(src_dir / ("file" + std::to_string(i) + ".txt"),
                           dst_dir / ("file" + std::to_string(i) + ".txt"));
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
    options.CopyParallelism = 2;
    options.CopyChanSize = 10;
    options.IoSize = 4096;
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;
    options.IOStuckTimeout = 10;

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyBatch(pairs, options, logger);
    REQUIRE(rc == 0);

    for (int i = 0; i < 5; ++i)
    {
        fs::path dst_file = dst_dir / ("file" + std::to_string(i) + ".txt");
        REQUIRE(fs::exists(dst_file));
        std::ifstream ifs(dst_file);
        std::string content;
        std::getline(ifs, content);
        REQUIRE(content == "content" + std::to_string(i));
    }

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CopyBatch mixed files and dirs", "[integration][batch]")
{
    fs::path src_dir = "/tmp/acp_batch_mix_src";
    fs::path src_file = "/tmp/acp_batch_mix_file.txt";
    fs::path dst_dir = "/tmp/acp_batch_mix_dst";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Create a source directory with nested file
    fs::create_directories(src_dir / "nested");
    {
        std::ofstream(src_dir / "nested" / "a.txt") << "nested content";
    }
    // Create a standalone source file
    {
        std::ofstream ofs(src_file);
        ofs << "standalone";
    }

    std::vector<std::pair<fs::path, fs::path>> pairs;
    pairs.emplace_back(src_dir, dst_dir / src_dir.filename());
    pairs.emplace_back(src_file, dst_dir / src_file.filename());

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
    int rc = CopyBatch(pairs, options, logger);
    REQUIRE(rc == 0);

    REQUIRE(fs::exists(dst_dir / src_dir.filename() / "nested" / "a.txt"));
    REQUIRE(fs::exists(dst_dir / src_file.filename()));

    std::ifstream ifs(dst_dir / src_dir.filename() / "nested" / "a.txt");
    std::string content;
    std::getline(ifs, content);
    REQUIRE(content == "nested content");

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove(src_file, ec);
    fs::remove_all(dst_dir, ec);
}
