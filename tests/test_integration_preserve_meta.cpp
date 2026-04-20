#include "lib/mainlib.hpp"
#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <utime.h>
#include <sys/xattr.h>

namespace fs = std::filesystem;

// ==================== 辅助工具 ====================

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

static int count_events_with(const std::string &output,
                              const std::string &event_type,
                              const std::string &key,
                              const std::string &value)
{
    int count = 0;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.find("\"event\":\"" + event_type + "\"") != std::string::npos &&
            line.find("\"" + key + "\":\"" + value + "\"") != std::string::npos)
            ++count;
    }
    return count;
}

static RWCombinedCopyOptions make_test_options()
{
    RWCombinedCopyOptions options;
    options.ProgramLogLevel = "error";
    options.ProgramLogMode = "console";
#ifdef __APPLE__
    options.CopyEngine = "gcd";
#else
    options.CopyEngine = "libaio";
#endif
    options.CopyMode = "CopyOnly";
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
    options.FileLogIntervalSec = 5;
    options.FileLogPath = "./.acp_state.json";
    return options;
}

static std::string run_cksum_only(const fs::path &src_dir, const fs::path &dst_dir)
{
    auto options = make_test_options();
    options.CopyMode = "CksumOnly";
    options.PreserveMeta = true;
    auto logger = InitLogger(options);

    StdoutCapture capture;
    int rc = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc == 0);
    return capture.str();
}

static int run_copy_only(const fs::path &src_dir, const fs::path &dst_dir, bool preserve_meta)
{
    auto options = make_test_options();
    options.CopyMode = "CopyOnly";
    options.PreserveMeta = preserve_meta;
    auto logger = InitLogger(options);
    return CopyDir(src_dir, dst_dir, options, logger);
}

// 创建带特定 mode 的文件
static void create_file_with_mode(const fs::path &p, mode_t mode)
{
    int fd = ::open(p.c_str(), O_CREAT | O_WRONLY, mode);
    REQUIRE(fd >= 0);
    ::close(fd);
    // 确保 mode 被正确设置（umask 可能影响创建时的 mode）
    REQUIRE(::chmod(p.c_str(), mode) == 0);
}

// 设置文件时间戳（纳秒级）
static void set_file_times(const fs::path &p, time_t sec, long nsec)
{
    struct timespec times[2];
    times[0].tv_sec = sec;
    times[0].tv_nsec = nsec;
    times[1].tv_sec = sec;
    times[1].tv_nsec = nsec;
#ifdef __APPLE__
    REQUIRE(::utimensat(AT_FDCWD, p.c_str(), times, 0) == 0);
#else
    REQUIRE(::utimensat(AT_FDCWD, p.c_str(), times, 0) == 0);
#endif
}

// 设置 xattr（平台适配）
static void set_xattr(const fs::path &p, const char *name, const char *value)
{
#ifdef __APPLE__
    REQUIRE(::setxattr(p.c_str(), name, value, strlen(value), 0, 0) == 0);
#else
    REQUIRE(::setxattr(p.c_str(), name, value, strlen(value), 0) == 0);
#endif
}

// ==================== 测试用例 ====================

TEST_CASE("PM-01: PreserveMeta=true then CksumOnly reports match", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm01_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm01_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    create_file_with_mode(src_dir / "file.dat", 0750);
    write_file_exact(src_dir / "file.dat", 1024, 'A');

    REQUIRE(run_copy_only(src_dir, dst_dir, true) == 0);
    REQUIRE(files_equal(src_dir / "file.dat", dst_dir / "file.dat"));

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "match"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "block_match"));
    // atime may differ because reading src updates it; only check critical metadata
    REQUIRE_FALSE(has_event_with(output, "cksum_result", "reason", "mode_mismatch"));
    REQUIRE_FALSE(has_event_with(output, "cksum_result", "reason", "xattr_mismatch"));
    REQUIRE_FALSE(has_event_with(output, "cksum_result", "reason", "owner_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-02: PreserveMeta=false then CksumOnly reports mismatch", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm02_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm02_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    create_file_with_mode(src_dir / "file.dat", 0750);
    write_file_exact(src_dir / "file.dat", 1024, 'B');

    REQUIRE(run_copy_only(src_dir, dst_dir, false) == 0);
    REQUIRE(files_equal(src_dir / "file.dat", dst_dir / "file.dat"));

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "mismatch"));
    // mode mismatch expected because default umask creates different mode
    REQUIRE(has_event_with(output, "cksum_result", "reason", "mode_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-04: timestamp precision preserved then CksumOnly match", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm04_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm04_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file_exact(src_dir / "file.dat", 1024, 'C');
    set_file_times(src_dir / "file.dat", 1609459200, 123456789); // 2021-01-01 with nanoseconds

    REQUIRE(run_copy_only(src_dir, dst_dir, true) == 0);

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "match"));
    REQUIRE_FALSE(has_event_with(output, "cksum_result", "reason", "timestamp_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-05: timestamp not preserved then CksumOnly reports timestamp_mismatch", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm05_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm05_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file_exact(src_dir / "file.dat", 1024, 'D');
    set_file_times(src_dir / "file.dat", 1609459200, 123456789);

    REQUIRE(run_copy_only(src_dir, dst_dir, false) == 0);

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "mismatch"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "timestamp_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-09: xattr preserved then CksumOnly match", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm09_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm09_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file_exact(src_dir / "file.dat", 1024, 'E');
    set_xattr(src_dir / "file.dat", "user.test_key", "hello_value");

    REQUIRE(run_copy_only(src_dir, dst_dir, true) == 0);

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "match"));
    REQUIRE_FALSE(has_event_with(output, "cksum_result", "reason", "xattr_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-10: xattr not preserved then CksumOnly reports xattr_mismatch", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm10_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm10_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file_exact(src_dir / "file.dat", 1024, 'F');
    set_xattr(src_dir / "file.dat", "user.test_key", "hello_value");

    REQUIRE(run_copy_only(src_dir, dst_dir, false) == 0);

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "mismatch"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "xattr_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-20: manual mode change then CksumOnly reports mode_mismatch", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm20_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm20_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    create_file_with_mode(src_dir / "file.dat", 0750);
    write_file_exact(src_dir / "file.dat", 1024, 'G');

    REQUIRE(run_copy_only(src_dir, dst_dir, true) == 0);

    // Manually change dst mode
    ::chmod((dst_dir / "file.dat").c_str(), 0644);

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "mismatch"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "mode_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-21: manual timestamp change then CksumOnly reports timestamp_mismatch", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm21_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm21_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file_exact(src_dir / "file.dat", 1024, 'H');
    set_file_times(src_dir / "file.dat", 1609459200, 123456789);

    REQUIRE(run_copy_only(src_dir, dst_dir, true) == 0);

    // Manually change dst timestamp
    set_file_times(dst_dir / "file.dat", 1700000000, 0);

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "mismatch"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "timestamp_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-22: manual xattr deletion then CksumOnly reports xattr_mismatch", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm22_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm22_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file_exact(src_dir / "file.dat", 1024, 'I');
    set_xattr(src_dir / "file.dat", "user.test_key", "hello_value");

    REQUIRE(run_copy_only(src_dir, dst_dir, true) == 0);

    // Manually remove dst xattr
#ifdef __APPLE__
    ::removexattr((dst_dir / "file.dat").c_str(), "user.test_key", 0);
#else
    ::removexattr((dst_dir / "file.dat").c_str(), "user.test_key");
#endif

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "mismatch"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "xattr_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-30: directory tree with all metadata preserved then CksumOnly match", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm30_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm30_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Build a small tree with varied metadata
    fs::create_directories(src_dir / "subdir1" / "nested");
    write_file_exact(src_dir / "file1.dat", 4096, 'J');
    write_file_exact(src_dir / "subdir1" / "file2.dat", 8192, 'K');
    write_file_exact(src_dir / "subdir1" / "nested" / "file3.dat", 2048, 'L');

    create_file_with_mode(src_dir / "file1.dat", 0750);
    create_file_with_mode(src_dir / "subdir1" / "file2.dat", 0640);
    create_file_with_mode(src_dir / "subdir1" / "nested" / "file3.dat", 0600);

    set_file_times(src_dir / "file1.dat", 1609459200, 123456789);
    set_file_times(src_dir / "subdir1" / "file2.dat", 1609459201, 987654321);

    set_xattr(src_dir / "file1.dat", "user.tag", "important");
    set_xattr(src_dir / "subdir1" / "file2.dat", "user.level", "debug");

    REQUIRE(run_copy_only(src_dir, dst_dir, true) == 0);

    std::string output = run_cksum_only(src_dir, dst_dir);

    // All regular files should match (atime may differ due to read access)
    int match_count = count_events_with(output, "cksum_result", "result", "match");
    REQUIRE(match_count >= 3); // at least the 3 regular files
    REQUIRE_FALSE(has_event_with(output, "cksum_result", "reason", "mode_mismatch"));
    REQUIRE_FALSE(has_event_with(output, "cksum_result", "reason", "xattr_mismatch"));
    REQUIRE_FALSE(has_event_with(output, "cksum_result", "reason", "owner_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-31: directory tree without PreserveMeta then CksumOnly reports mismatches", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm31_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm31_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    fs::create_directories(src_dir / "subdir1");
    write_file_exact(src_dir / "file1.dat", 4096, 'M');
    write_file_exact(src_dir / "subdir1" / "file2.dat", 4096, 'N');

    create_file_with_mode(src_dir / "file1.dat", 0750);
    create_file_with_mode(src_dir / "subdir1" / "file2.dat", 0640);
    set_file_times(src_dir / "file1.dat", 1609459200, 123456789);
    set_xattr(src_dir / "file1.dat", "user.tag", "test");

    REQUIRE(run_copy_only(src_dir, dst_dir, false) == 0);

    std::string output = run_cksum_only(src_dir, dst_dir);

    int mismatch_count = count_events_with(output, "cksum_result", "result", "mismatch");
    REQUIRE(mismatch_count >= 2); // at least the 2 regular files

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

#ifndef __APPLE__
// Linux-only: ACL tests require libacl
#ifdef HAS_LIBACL
#include <sys/acl.h>

static void set_file_acl(const fs::path &p, const char *acl_text)
{
    acl_t acl = acl_from_text(acl_text);
    REQUIRE(acl != nullptr);
    REQUIRE(acl_set_file(p.c_str(), ACL_TYPE_ACCESS, acl) == 0);
    acl_free(acl);
}

TEST_CASE("PM-13: ACL preserved then CksumOnly match", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm13_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm13_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file_exact(src_dir / "file.dat", 1024, 'O');
    set_file_acl(src_dir / "file.dat", "u::rw,g::r,o::r\nu:nobody:rwx");

    REQUIRE(run_copy_only(src_dir, dst_dir, true) == 0);

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "match"));
    REQUIRE_FALSE(has_event_with(output, "cksum_result", "reason", "acl_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-14: ACL not preserved then CksumOnly reports acl_mismatch", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm14_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm14_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file_exact(src_dir / "file.dat", 1024, 'P');
    set_file_acl(src_dir / "file.dat", "u::rw,g::r,o::r\nu:nobody:rwx");

    REQUIRE(run_copy_only(src_dir, dst_dir, false) == 0);

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "mismatch"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "acl_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("PM-23: manual ACL change then CksumOnly reports acl_mismatch", "[preserve_meta]")
{
    fs::path src_dir = fs::path("testdata") / "pm23_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_pm23_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    write_file_exact(src_dir / "file.dat", 1024, 'Q');
    set_file_acl(src_dir / "file.dat", "u::rw,g::r,o::r\nu:nobody:rwx");

    REQUIRE(run_copy_only(src_dir, dst_dir, true) == 0);

    // Remove the ACL entry from dst
    acl_t acl = acl_from_text("u::rw,g::r,o::r");
    REQUIRE(acl != nullptr);
    REQUIRE(acl_set_file((dst_dir / "file.dat").c_str(), ACL_TYPE_ACCESS, acl) == 0);
    acl_free(acl);

    std::string output = run_cksum_only(src_dir, dst_dir);
    REQUIRE(has_event_with(output, "cksum_result", "result", "mismatch"));
    REQUIRE(has_event_with(output, "cksum_result", "reason", "acl_mismatch"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
#endif // HAS_LIBACL
#endif // !__APPLE__
