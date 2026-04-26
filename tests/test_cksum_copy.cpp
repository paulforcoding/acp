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

TEST_CASE("CksumCopy truncates dst when src is smaller", "[integration][cksum]")
{
    fs::path src_dir = fs::path("testdata") / "cksum_trunc_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_cksum_trunc_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Create a small source file (12 bytes) and a larger destination file (34 bytes)
    write_file_exact(src_dir / "trunc.dat", 12, 'A');
    write_file_exact(dst_dir / "trunc.dat", 34, 'Z');

    auto options = make_cksum_options();
    auto logger = InitLogger(options);

    int rc = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc == 0);

    // After CksumCopy, dst must be truncated to src size
    REQUIRE(fs::file_size(dst_dir / "trunc.dat") == 12);
    REQUIRE(files_equal(src_dir / "trunc.dat", dst_dir / "trunc.dat"));

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
    REQUIRE(has_event_with(output, "cksum_result", "reason", "meta_match"));

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
    // CksumOnly filter suppresses per-file I/O events like file_complete
    REQUIRE_FALSE(has_event(output, "file_complete"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CksumOnly does block-level checksum even when size/mtime match", "[integration][cksum]")
{
    // Verify that CksumOnly does NOT skip block-level checksum based on
    // size/mtime alone.  We create identical files via CopyOnly, then
    // corrupt a single byte in the dst (preserving size), and finally
    // restore the original mtime on the dst so that size + mtime match.
    // If CksumOnly incorrectly used the size/mtime fast-path, it would
    // report "match" without ever reading file data.

    fs::path src_dir = fs::path("testdata") / "cksum_only_content_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_cksum_only_content_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    const size_t file_size = 65536;
    write_file_exact(src_dir / "content.dat", file_size, 'G');

    // Step 1: CopyOnly to create an identical destination
    {
        auto copy_opts = make_cksum_options();
        copy_opts.CopyMode = "CopyOnly";
        copy_opts.PreserveMeta = true;
        auto copy_logger = InitLogger(copy_opts);
        int rc = CopyDir(src_dir, dst_dir, copy_opts, copy_logger);
        REQUIRE(rc == 0);
    }

    // Step 2: Corrupt one byte in the destination (size unchanged)
    {
        std::fstream fdst(dst_dir / "content.dat", std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(fdst.good());
        char bad = 'Z';
        fdst.seekp(100);
        fdst.write(&bad, 1);
    }

    // Step 3: Restore the original mtime so size + mtime match src
    {
        struct stat src_stat;
        REQUIRE(lstat((src_dir / "content.dat").c_str(), &src_stat) == 0);
        struct timespec ts[2];
#ifdef __APPLE__
        ts[0] = src_stat.st_atimespec;
        ts[1] = src_stat.st_mtimespec;
#else
        ts[0] = src_stat.st_atim;
        ts[1] = src_stat.st_mtim;
#endif
        REQUIRE(utimensat(AT_FDCWD, (dst_dir / "content.dat").c_str(), ts, 0) == 0);
    }

    // Verify files are indeed different in content but same in size/mtime
    REQUIRE_FALSE(files_equal(src_dir / "content.dat", dst_dir / "content.dat"));
    {
        struct stat src_st, dst_st;
        REQUIRE(lstat((src_dir / "content.dat").c_str(), &src_st) == 0);
        REQUIRE(lstat((dst_dir / "content.dat").c_str(), &dst_st) == 0);
        REQUIRE(src_st.st_size == dst_st.st_size);
#ifdef __APPLE__
        REQUIRE(src_st.st_mtimespec.tv_sec == dst_st.st_mtimespec.tv_sec);
        REQUIRE(src_st.st_mtimespec.tv_nsec == dst_st.st_mtimespec.tv_nsec);
#else
        REQUIRE(src_st.st_mtim.tv_sec == dst_st.st_mtim.tv_sec);
        REQUIRE(src_st.st_mtim.tv_nsec == dst_st.st_mtim.tv_nsec);
#endif
    }

    // Step 4: Run CksumOnly — block-level checksum must be performed despite
    // size/mtime matching, and the result must appear in FileLog.
    auto options = make_cksum_options();
    options.CopyMode = "CksumOnly";
    options.PreserveMeta = true;
    auto logger = InitLogger(options);

    StdoutCapture capture;
    int rc = CopyDir(src_dir, dst_dir, options, logger);
    std::string output = capture.str();
    REQUIRE(rc == 0);

    // CksumOnly must NOT report "size_mtime_match" —
    // that fast-path is CksumCopy-only (guarded by !mCksumOnly).
    REQUIRE_FALSE(has_event_with(output, "cksum_result", "reason", "size_mtime_match"));

    // Block-level checksum must detect the content mismatch and emit it to FileLog.
    REQUIRE(has_event_with(output, "cksum_result", "result", "mismatch"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "content_mismatch"));

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
