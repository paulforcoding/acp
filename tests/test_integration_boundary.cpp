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
    options.CopyParallelism = 1;
    options.CopyChanSize = 10;
    options.IoSize = 4096;
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;
    options.IOStuckTimeout = 10;
    return options;
}

TEST_CASE("CopyDir file size exactly IOSize", "[integration]")
{
    fs::path src = "/tmp/acp_bound_exact_src";
    fs::path dst = "/tmp/acp_bound_exact_dst";
    ensure_clean_dir(src);
    ensure_clean_dir(dst);

    {
        std::ofstream ofs(src / "file.dat", std::ios::binary);
        std::string buf(4096, 'A');
        ofs.write(buf.data(), buf.size());
    }

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyDir(src, dst / src.filename(), MakeOptions(), logger);
    REQUIRE(rc == 0);

    REQUIRE(fs::exists(dst / src.filename() / "file.dat"));
    REQUIRE(fs::file_size(dst / src.filename() / "file.dat") == 4096);

    std::error_code ec;
    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
}

TEST_CASE("CopyDir file size IOSize plus one", "[integration]")
{
    fs::path src = "/tmp/acp_bound_plus1_src";
    fs::path dst = "/tmp/acp_bound_plus1_dst";
    ensure_clean_dir(src);
    ensure_clean_dir(dst);

    {
        std::ofstream ofs(src / "file.dat", std::ios::binary);
        std::string buf(4097, 'B');
        ofs.write(buf.data(), buf.size());
    }

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyDir(src, dst / src.filename(), MakeOptions(), logger);
    REQUIRE(rc == 0);

    REQUIRE(fs::exists(dst / src.filename() / "file.dat"));
    REQUIRE(fs::file_size(dst / src.filename() / "file.dat") == 4097);

    std::error_code ec;
    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
}

TEST_CASE("CopyDir many small files", "[integration]")
{
    fs::path src = "/tmp/acp_bound_many_src";
    fs::path dst = "/tmp/acp_bound_many_dst";
    ensure_clean_dir(src);
    ensure_clean_dir(dst);

    constexpr int kCount = 100;
    for (int i = 0; i < kCount; ++i)
    {
        std::ofstream(src / ("f" + std::to_string(i) + ".txt")) << i;
    }

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyDir(src, dst / src.filename(), MakeOptions(), logger);
    REQUIRE(rc == 0);

    for (int i = 0; i < kCount; ++i)
    {
        REQUIRE(fs::exists(dst / src.filename() / ("f" + std::to_string(i) + ".txt")));
    }

    std::error_code ec;
    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
}

TEST_CASE("CopyDir deep nesting", "[integration]")
{
    fs::path src = "/tmp/acp_bound_deep_src";
    fs::path dst = "/tmp/acp_bound_deep_dst";
    ensure_clean_dir(src);
    ensure_clean_dir(dst);

    fs::path deep = src;
    for (int i = 0; i < 10; ++i)
    {
        deep = deep / ("level" + std::to_string(i));
    }
    fs::create_directories(deep);
    std::ofstream(deep / "bottom.txt") << "bottom";

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyDir(src, dst / src.filename(), MakeOptions(), logger);
    REQUIRE(rc == 0);

    fs::path copied = dst / src.filename();
    for (int i = 0; i < 10; ++i)
    {
        copied = copied / ("level" + std::to_string(i));
    }
    REQUIRE(fs::exists(copied / "bottom.txt"));

    std::error_code ec;
    fs::remove_all(src, ec);
    fs::remove_all(dst, ec);
}

TEST_CASE("CopyFile overwrite existing", "[integration]")
{
    fs::path src = "/tmp/acp_bound_overwrite_src.dat";
    fs::path dst = "/tmp/acp_bound_overwrite_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        ofs.write("new-content", 11);
    }
    {
        std::ofstream ofs(dst, std::ios::binary);
        ofs.write("old-content-here", 16);
    }

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyFile(src, dst, MakeOptions(), logger);
    REQUIRE(rc == 0);

    REQUIRE(fs::file_size(dst) == 11);
    std::ifstream ifs(dst);
    std::string content;
    std::getline(ifs, content);
    REQUIRE(content == "new-content");

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CopyFile dst is directory returns error", "[integration]")
{
    fs::path src = "/tmp/acp_bound_dstdir_src.dat";
    fs::path dst = "/tmp/acp_bound_dstdir_dst";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove_all(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        ofs.write("content", 7);
    }
    fs::create_directories(dst);

    auto logger = std::make_shared<ConsoleLogger>();
    // CopyFile does not handle dst-as-directory internally; it expects the caller
    // (main.cpp) to construct the final file path. Passing a directory directly fails.
    int rc = CopyFile(src, dst, MakeOptions(), logger);
    REQUIRE(rc == 1);

    fs::remove(src, ec);
    fs::remove_all(dst, ec);
}
