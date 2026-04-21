#include "lib/mainlib.hpp"
#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>

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
    options.ProgramLogLevel = "error";
    options.ProgramLogMode = "file";
    options.ProgramLogFilePath = "/tmp/acp_program.log";
    options.CopyEngine = "libaio";
    options.CopyMode = "CksumCopy";
    options.CksumAlgorithm = "xxhash64";
    options.CopyParallelism = 1;
    options.EnableInotify = false;
    options.PreserveSparseFiles = false;
    options.PreserveMeta = true;
    options.DirectIO = false;
    options.SyncWrites = false;
    options.IoSize = 1 << 20; // 1MB
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;
    options.FileLogEnabled = true;
    options.FileLogMode = "console";
    options.FileLogIntervalSec = 5;
    options.FileLogPath = "./.acp_state.json";
    return options;
}

class StdoutCapture
{
    std::ostringstream mBuffer;
    std::streambuf *mOldBuf;

public:
    StdoutCapture() : mOldBuf(std::cout.rdbuf(mBuffer.rdbuf())) {}
    ~StdoutCapture() { std::cout.rdbuf(mOldBuf); }
    std::string str() const { return mBuffer.str(); }
};

static bool has_event(const std::string &output, const std::string &event_type)
{
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.find("\"event\":\"" + event_type + "\"") != std::string::npos)
            return true;
    }
    return false;
}

static bool has_event_with(const std::string &output,
                           const std::string &event_type,
                           const std::string &key,
                           const std::string &value)
{
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.find("\"event\":\"" + event_type + "\"") != std::string::npos &&
            line.find("\"" + key + "\":\"" + value + "\"") != std::string::npos)
            return true;
    }
    return false;
}

#ifndef __APPLE__
// CksumCopy integration tests require libaio backend, not available on macOS
TEST_CASE("CksumCopy partial last block", "[integration][cksum]")
{
    fs::path src_dir = fs::path("testdata") / "cksum_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_cksum_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    const size_t file_size = (1 << 20) / 2; // 512KB
    write_file_exact(src_dir / "partial.dat", file_size, 'A');

    auto options = make_cksum_options();
    auto logger = InitLogger(options);

    int rc = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc == 0);

    REQUIRE(files_equal(src_dir / "partial.dat", dst_dir / "partial.dat"));

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

    const size_t file_size = (1 << 20) + 12345;
    write_file_exact(src_dir / "identical.dat", file_size, 'B');

    auto options = make_cksum_options();
    auto logger = InitLogger(options);

    int rc1 = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc1 == 0);
    REQUIRE(files_equal(src_dir / "identical.dat", dst_dir / "identical.dat"));

    int rc2 = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc2 == 0);
    REQUIRE(files_equal(src_dir / "identical.dat", dst_dir / "identical.dat"));

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

    int rc1 = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc1 == 0);
    REQUIRE(files_equal(src_dir / "diff.dat", dst_dir / "diff.dat"));

    {
        std::fstream fdst(dst_dir / "diff.dat", std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(fdst.good());
        char bad = 'X';
        fdst.seekp(100);
        fdst.write(&bad, 1);
    }
    REQUIRE_FALSE(files_equal(src_dir / "diff.dat", dst_dir / "diff.dat"));

    int rc2 = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc2 == 0);
    REQUIRE(files_equal(src_dir / "diff.dat", dst_dir / "diff.dat"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
#endif

TEST_CASE("CksumOnly emits FileLog cksum_result match events", "[integration][cksum]")
{
    fs::path src_dir = fs::path("testdata") / "cksum_only_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_cksum_only_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    const size_t file_size = 65536;
    write_file_exact(src_dir / "check.dat", file_size, 'D');

    // First copy to create destination
    {
        auto copy_opts = make_cksum_options();
        copy_opts.CopyMode = "CopyOnly";
        auto copy_logger = InitLogger(copy_opts);
        int rc = CopyDir(src_dir, dst_dir, copy_opts, copy_logger);
        REQUIRE(rc == 0);
    }

    auto options = make_cksum_options();
    options.CopyMode = "CksumOnly";
    auto logger = InitLogger(options);

    StdoutCapture capture;
    int rc = CopyDir(src_dir, dst_dir, options, logger);
    std::string output = capture.str();
    REQUIRE(rc == 0);

    // Verify cksum_result events exist
    REQUIRE(has_event_with(output, "cksum_result", "result", "match"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "block_match"));

    // Verify no cksum_result.log is created
    std::error_code ec;
    REQUIRE(!fs::exists("./cksum_result.log"));

    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CksumOnly emits skipped when dst missing", "[integration][cksum]")
{
    fs::path src_dir = fs::path("testdata") / "cksum_only_missing_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_cksum_only_missing_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file_exact(src_dir / "missing.dat", 1024, 'E');

    auto options = make_cksum_options();
    options.CopyMode = "CksumOnly";
    auto logger = InitLogger(options);

    // Do NOT copy first — dst file does not exist
    StdoutCapture capture;
    int rc = CopyDir(src_dir, dst_dir, options, logger);
    std::string output = capture.str();
    REQUIRE(rc == 0);

    REQUIRE(has_event_with(output, "cksum_result", "result", "skipped"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "dst_missing"));
    // FileComplete should still be emitted
    REQUIRE(has_event(output, "file_complete"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CksumOnly emits mismatch when mode differs", "[integration][cksum]")
{
    fs::path src_dir = fs::path("testdata") / "cksum_only_mode_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_cksum_only_mode_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file_exact(src_dir / "mode.dat", 1024, 'F');

    // First copy to create destination
    {
        auto copy_opts = make_cksum_options();
        copy_opts.CopyMode = "CopyOnly";
        auto copy_logger = InitLogger(copy_opts);
        int rc = CopyDir(src_dir, dst_dir, copy_opts, copy_logger);
        REQUIRE(rc == 0);
    }

    // Change dst file mode
    chmod((dst_dir / "mode.dat").c_str(), 0644);

    // Change src file mode to something different
    chmod((src_dir / "mode.dat").c_str(), 0750);

    auto options = make_cksum_options();
    options.CopyMode = "CksumOnly";
    auto logger = InitLogger(options);

    StdoutCapture capture;
    int rc = CopyDir(src_dir, dst_dir, options, logger);
    std::string output = capture.str();
    REQUIRE(rc == 0);

    REQUIRE(has_event_with(output, "cksum_result", "result", "mismatch"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "mode_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
