#include "lib/mainlib.hpp"
#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

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

// On APFS/macOS, writing any data causes the entire file to be allocated,
// so traditional hole-in-the-middle sparse files are not supported.
// We still verify data integrity on all platforms, and check st_blocks on Linux.
static bool is_sparse(const fs::path &p)
{
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    return (st.st_blocks * 512) < st.st_size;
}

static bool platform_supports_sparse()
{
#ifdef __APPLE__
    return false; // APFS allocates full file once any data is written
#else
    return true;
#endif
}

TEST_CASE("SP-01: sparse file with hole in middle is preserved", "[sparse]")
{
    fs::path src_dir = fs::path("testdata") / "sp01_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_sp01_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Create sparse file: 4MB hole, 4MB data, 4MB hole
    fs::path src_file = src_dir / "sparse.bin";
    int fd = ::open(src_file.c_str(), O_CREAT | O_WRONLY, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, 12 * 1024 * 1024) == 0);
    std::vector<char> data(4 * 1024 * 1024, 'X');
    REQUIRE(::pwrite(fd, data.data(), data.size(), 4 * 1024 * 1024) == static_cast<ssize_t>(data.size()));
    ::close(fd);

    // Verify source is sparse (Linux only; APFS preallocates on write)
    if (platform_supports_sparse())
        REQUIRE(is_sparse(src_file));

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
    options.PreserveSparseFiles = true;
    options.PreserveMeta = false;
    options.DirectIO = false;
    options.SyncWrites = false;
    options.IoSize = 1 * 1024 * 1024;
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;

    auto logger = InitLogger(options);
    int rc = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc == 0);

    fs::path dst_file = dst_dir / "sparse.bin";
    REQUIRE(fs::exists(dst_file));
    REQUIRE(fs::file_size(dst_file) == fs::file_size(src_file));

    // Verify data integrity
    int src_fd = ::open(src_file.c_str(), O_RDONLY);
    int dst_fd = ::open(dst_file.c_str(), O_RDONLY);
    REQUIRE(src_fd >= 0);
    REQUIRE(dst_fd >= 0);

    std::vector<char> src_buf(4096), dst_buf(4096);
    off_t offsets[] = {0, 4 * 1024 * 1024LL, 8 * 1024 * 1024LL, 12 * 1024 * 1024LL - 4096};
    for (off_t off : offsets)
    {
        ssize_t src_n = ::pread(src_fd, src_buf.data(), src_buf.size(), off);
        ssize_t dst_n = ::pread(dst_fd, dst_buf.data(), dst_buf.size(), off);
        REQUIRE(src_n == dst_n);
        REQUIRE(std::memcmp(src_buf.data(), dst_buf.data(), static_cast<size_t>(src_n)) == 0);
    }
    ::close(src_fd);
    ::close(dst_fd);

    // Verify destination is also sparse (Linux only)
    if (platform_supports_sparse())
        REQUIRE(is_sparse(dst_file));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("SP-02: non-sparse file copy is not affected by heuristic", "[sparse]")
{
    fs::path src_dir = fs::path("testdata") / "sp02_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_sp02_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Create fully-populated non-sparse file
    fs::path src_file = src_dir / "dense.bin";
    write_file_exact(src_file, 2 * 1024 * 1024, 'D');

    REQUIRE_FALSE(is_sparse(src_file));

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
    options.PreserveSparseFiles = true;
    options.PreserveMeta = false;
    options.DirectIO = false;
    options.SyncWrites = false;
    options.IoSize = 1 * 1024 * 1024;
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;

    auto logger = InitLogger(options);
    int rc = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc == 0);

    fs::path dst_file = dst_dir / "dense.bin";
    REQUIRE(fs::exists(dst_file));
    REQUIRE(fs::file_size(dst_file) == fs::file_size(src_file));

    // Data integrity
    int src_fd = ::open(src_file.c_str(), O_RDONLY);
    int dst_fd = ::open(dst_file.c_str(), O_RDONLY);
    REQUIRE(src_fd >= 0);
    REQUIRE(dst_fd >= 0);
    std::vector<char> src_buf(4096), dst_buf(4096);
    ssize_t src_n = ::pread(src_fd, src_buf.data(), src_buf.size(), 0);
    ssize_t dst_n = ::pread(dst_fd, dst_buf.data(), dst_buf.size(), 0);
    REQUIRE(src_n == dst_n);
    REQUIRE(std::memcmp(src_buf.data(), dst_buf.data(), static_cast<size_t>(src_n)) == 0);
    ::close(src_fd);
    ::close(dst_fd);

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
