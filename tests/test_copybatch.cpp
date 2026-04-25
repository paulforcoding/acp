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

static void write_file(const fs::path &p, size_t bytes, char seed)
{
    std::ofstream ofs(p, std::ios::binary);
    std::vector<char> buf(4096);
    size_t written = 0;
    while (written < bytes)
    {
        size_t n = std::min(buf.size(), bytes - written);
        for (size_t i = 0; i < n; ++i)
            buf[i] = static_cast<char>(seed + static_cast<char>((written + i) % 251));
        ofs.write(buf.data(), static_cast<std::streamsize>(n));
        written += n;
    }
}

[[maybe_unused]] static bool files_equal(const fs::path &a, const fs::path &b)
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

#ifndef __APPLE__
TEST_CASE("CopyBatch CksumCopy multiple files", "[integration][batch][cksum]")
{
    fs::path src_dir = "/tmp/acp_batch_ck_src";
    fs::path dst_dir = "/tmp/acp_batch_ck_dst";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file(src_dir / "a.dat", 8192, 'A');
    write_file(src_dir / "b.dat", 16384, 'B');
    write_file(src_dir / "c.dat", 32768, 'C');

    RWCombinedCopyOptions copyOpts;
    copyOpts.ProgramLogLevel = "error";
    copyOpts.ProgramLogMode = "console";
    copyOpts.FileLogEnabled = false;
    copyOpts.CopyEngine = "libaio";
    copyOpts.CopyMode = "CopyOnly";
    copyOpts.CopyParallelism = 1;
    copyOpts.CopyChanSize = 10;
    copyOpts.IoSize = 4096;
    copyOpts.QueueDepth = 4;
    copyOpts.Batch = 2;
    copyOpts.IOReapWait = 1;
    copyOpts.IOStuckTimeout = 10;

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyDir(src_dir, dst_dir, copyOpts, logger);
    REQUIRE(rc == 0);
    REQUIRE(files_equal(src_dir / "a.dat", dst_dir / "a.dat"));
    REQUIRE(files_equal(src_dir / "b.dat", dst_dir / "b.dat"));
    REQUIRE(files_equal(src_dir / "c.dat", dst_dir / "c.dat"));

    // Corrupt two destination files
    {
        std::fstream f(dst_dir / "b.dat", std::ios::in | std::ios::out | std::ios::binary);
        char bad = 'X';
        f.seekp(100);
        f.write(&bad, 1);
    }
    {
        std::fstream f(dst_dir / "c.dat", std::ios::in | std::ios::out | std::ios::binary);
        char bad = 'Y';
        f.seekp(200);
        f.write(&bad, 1);
    }
    REQUIRE_FALSE(files_equal(src_dir / "b.dat", dst_dir / "b.dat"));
    REQUIRE_FALSE(files_equal(src_dir / "c.dat", dst_dir / "c.dat"));

    // CksumCopy should fix the corrupted files, leave a.dat untouched
    RWCombinedCopyOptions cksumOpts = copyOpts;
    cksumOpts.CopyMode = "CksumCopy";
    rc = CopyDir(src_dir, dst_dir, cksumOpts, logger);
    REQUIRE(rc == 0);
    REQUIRE(files_equal(src_dir / "a.dat", dst_dir / "a.dat"));
    REQUIRE(files_equal(src_dir / "b.dat", dst_dir / "b.dat"));
    REQUIRE(files_equal(src_dir / "c.dat", dst_dir / "c.dat"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CopyBatch CksumCopy partial dst missing", "[integration][batch][cksum]")
{
    fs::path src_dir = "/tmp/acp_batch_ckmiss_src";
    fs::path dst_dir = "/tmp/acp_batch_ckmiss_dst";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file(src_dir / "existing.dat", 8192, 'E');
    write_file(src_dir / "missing.dat", 8192, 'M');

    RWCombinedCopyOptions copyOpts;
    copyOpts.ProgramLogLevel = "error";
    copyOpts.ProgramLogMode = "console";
    copyOpts.FileLogEnabled = false;
    copyOpts.CopyEngine = "libaio";
    copyOpts.CopyMode = "CopyOnly";
    copyOpts.CopyParallelism = 1;
    copyOpts.CopyChanSize = 10;
    copyOpts.IoSize = 4096;
    copyOpts.QueueDepth = 4;
    copyOpts.Batch = 2;
    copyOpts.IOReapWait = 1;
    copyOpts.IOStuckTimeout = 10;

    auto logger = std::make_shared<ConsoleLogger>();
    // Only copy one file to destination
    int rc = CopyFile(src_dir / "existing.dat", dst_dir / "existing.dat", copyOpts, logger);
    REQUIRE(rc == 0);

    // CksumCopy the whole directory: existing should match, missing should be created
    RWCombinedCopyOptions cksumOpts = copyOpts;
    cksumOpts.CopyMode = "CksumCopy";
    rc = CopyDir(src_dir, dst_dir, cksumOpts, logger);
    REQUIRE(rc == 0);
    REQUIRE(files_equal(src_dir / "existing.dat", dst_dir / "existing.dat"));
    REQUIRE(files_equal(src_dir / "missing.dat", dst_dir / "missing.dat"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
#endif // __APPLE__

TEST_CASE("CopyBatch CksumOnly multiple files", "[integration][batch][cksum]")
{
    fs::path src_dir = "/tmp/acp_batch_ckonly_src";
    fs::path dst_dir = "/tmp/acp_batch_ckonly_dst";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file(src_dir / "match.dat", 8192, 'M');
    write_file(src_dir / "mismatch.dat", 8192, 'N');

    RWCombinedCopyOptions copyOpts;
    copyOpts.ProgramLogLevel = "error";
    copyOpts.ProgramLogMode = "console";
    copyOpts.FileLogEnabled = false;
#ifdef __APPLE__
    copyOpts.CopyEngine = "gcd";
#else
    copyOpts.CopyEngine = "libaio";
#endif
    copyOpts.CopyMode = "CopyOnly";
    copyOpts.CopyParallelism = 1;
    copyOpts.CopyChanSize = 10;
    copyOpts.IoSize = 4096;
    copyOpts.QueueDepth = 4;
    copyOpts.Batch = 2;
    copyOpts.IOReapWait = 1;
    copyOpts.IOStuckTimeout = 10;

    auto logger = std::make_shared<ConsoleLogger>();
    int rc = CopyDir(src_dir, dst_dir, copyOpts, logger);
    REQUIRE(rc == 0);

    // Corrupt one destination file
    {
        std::fstream f(dst_dir / "mismatch.dat", std::ios::in | std::ios::out | std::ios::binary);
        char bad = 'Z';
        f.seekp(50);
        f.write(&bad, 1);
    }

    // CksumOnly should report match for match.dat, mismatch for mismatch.dat
    RWCombinedCopyOptions cksumOpts = copyOpts;
    cksumOpts.CopyMode = "CksumOnly";
    cksumOpts.FileLogEnabled = true;
    cksumOpts.FileLogMode = "console";
    cksumOpts.FileLogPath = "./.acp_state.json";

    rc = CopyDir(src_dir, dst_dir, cksumOpts, logger);
    REQUIRE(rc == 0);

    // Verify dst was NOT overwritten
    std::ifstream ifs(dst_dir / "mismatch.dat", std::ios::binary);
    char byte_at_50 = 0;
    ifs.seekg(50);
    ifs.read(&byte_at_50, 1);
    REQUIRE(byte_at_50 == 'Z');

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
