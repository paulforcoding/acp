#include "lib/mainlib.hpp"
#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

static void write_file_exact(const fs::path &p, size_t bytes, char seed)
{
    std::ofstream ofs(p, std::ios::binary);
    REQUIRE(ofs.good());
    std::vector<char> buf(4096);
    size_t written = 0;
    while (written < bytes)
    {
        size_t n = std::min(buf.size(), bytes - written);
        for (size_t i = 0; i < n; ++i)
            buf[i] = static_cast<char>(seed + static_cast<char>((written + i) % 251));
        ofs.write(buf.data(), static_cast<std::streamsize>(n));
        REQUIRE(ofs.good());
        written += n;
    }
}

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

static RWCombinedCopyOptions make_cksum_options()
{
    RWCombinedCopyOptions options;
    options.ProgramLogLevel = "info";
    options.ProgramLogMode = "console";
    options.CopyEngine = "libaio";
    options.CopyMode = "CksumCopy";
    options.CksumAlgorithm = "xxhash64";
    options.CopyParallelism = 1;
    options.EnableInotify = false;
    options.PreserveSparseFiles = false;
    options.DirectIO = false;
    options.SyncWrites = false;
    options.IoSize = 1 << 20; // 1MB
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;
    return options;
}

TEST_CASE("CksumCopy partial last block", "[integration][cksum]")
{
    fs::path src_dir = fs::path("testdata") / "cksum_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_cksum_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Create a file smaller than IoSize to test the last partial block handling
    const size_t file_size = (1 << 20) / 2; // 512KB
    write_file_exact(src_dir / "partial.dat", file_size, 'A');

    auto options = make_cksum_options();
    auto logger = InitLogger(options);

    int rc = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc == 0);

    REQUIRE(files_equal(src_dir / "partial.dat", dst_dir / "partial.dat"));

    // cleanup
    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CksumCopy identical files skip write", "[integration][cksum]")
{
    fs::path src_dir = fs::path("testdata") / "cksum_ident_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_cksum_ident_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    const size_t file_size = (1 << 20) + 12345; // slightly larger than 1MB
    write_file_exact(src_dir / "identical.dat", file_size, 'B');

    auto options = make_cksum_options();
    auto logger = InitLogger(options);

    // First copy: dst does not exist
    int rc1 = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc1 == 0);
    REQUIRE(files_equal(src_dir / "identical.dat", dst_dir / "identical.dat"));

    // Second copy: dst already exists and is identical, should succeed without errors
    int rc2 = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc2 == 0);
    REQUIRE(files_equal(src_dir / "identical.dat", dst_dir / "identical.dat"));

    // cleanup
    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CksumCopy mismatch file writes diff", "[integration][cksum]")
{
    fs::path src_dir = fs::path("testdata") / "cksum_diff_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_cksum_diff_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    const size_t file_size = (1 << 20) + 12345;
    write_file_exact(src_dir / "diff.dat", file_size, 'C');

    auto options = make_cksum_options();
    auto logger = InitLogger(options);

    // First copy
    int rc1 = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc1 == 0);
    REQUIRE(files_equal(src_dir / "diff.dat", dst_dir / "diff.dat"));

    // Corrupt the destination file
    {
        std::fstream fdst(dst_dir / "diff.dat", std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(fdst.good());
        char bad = 'X';
        fdst.seekp(100);
        fdst.write(&bad, 1);
    }
    REQUIRE_FALSE(files_equal(src_dir / "diff.dat", dst_dir / "diff.dat"));

    // Second copy: cksum should detect mismatch and rewrite the differing block
    int rc2 = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc2 == 0);
    REQUIRE(files_equal(src_dir / "diff.dat", dst_dir / "diff.dat"));

    // cleanup
    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CksumOnly generates result log", "[integration][cksum]")
{
    fs::path src_dir = fs::path("testdata") / "cksum_only_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_cksum_only_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    const size_t file_size = 65536;
    write_file_exact(src_dir / "check.dat", file_size, 'D');

    auto options = make_cksum_options();
    options.CopyMode = "CksumOnly";
    auto logger = InitLogger(options);

    // First copy to create destination
    {
        auto copy_opts = make_cksum_options();
        copy_opts.CopyMode = "CopyOnly";
        auto copy_logger = InitLogger(copy_opts);
        int rc = CopyDir(src_dir, dst_dir, copy_opts, copy_logger);
        REQUIRE(rc == 0);
    }

    // Remove old result log if any
    std::error_code ec;
    fs::remove("./cksum_result.log", ec);

    // CksumOnly run
    int rc = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc == 0);

    // Verify result log exists and contains expected content
    REQUIRE(fs::exists("./cksum_result.log"));
    std::ifstream log("./cksum_result.log");
    REQUIRE(log.good());
    std::string line;
    bool found_match = false;
    while (std::getline(log, line))
    {
        if (line.find("MATCHED") != std::string::npos)
        {
            found_match = true;
        }
    }
    REQUIRE(found_match);

    // cleanup
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
    fs::remove("./cksum_result.log", ec);
}
