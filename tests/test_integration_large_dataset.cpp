#include "lib/mainlib.hpp"
#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <filesystem>
#include <fstream>
#include <random>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>

namespace fs = std::filesystem;

static void write_file(const fs::path &p, size_t bytes)
{
    std::ofstream ofs(p, std::ios::binary);
    REQUIRE(ofs.good());
    const size_t chunk = 1 << 20; // 1MB
    std::vector<char> buf(std::min(chunk, bytes), '\0');
    // Fill deterministic pattern
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<char>(i % 251);
    size_t remaining = bytes;
    while (remaining > 0)
    {
        size_t n = std::min(buf.size(), remaining);
        ofs.write(buf.data(), static_cast<std::streamsize>(n));
        REQUIRE(ofs.good());
        remaining -= n;
    }
}

static void create_sparse_file(const fs::path &p, size_t size)
{
    int fd = ::open(p.c_str(), O_CREAT | O_WRONLY, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, static_cast<off_t>(size)) == 0);
    ::close(fd);
}

static void ensure_clean_dir(const fs::path &p)
{
    std::error_code ec;
    if (fs::exists(p, ec)) fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    REQUIRE(!ec);
}

static void generate_dataset(const fs::path &root)
{
    ensure_clean_dir(root);
    // Directories
    std::vector<fs::path> dirs;
    for (int i = 0; i < 19; ++i)
    {
        fs::path d = root / ("dir_" + std::to_string(i));
        fs::create_directories(d);
        dirs.push_back(d);
    }

    // Big file (>1GB)
    const size_t big_size = (static_cast<size_t>(1) << 30) + (8 << 20); // 1GB + 8MB
    create_sparse_file(root / "bigfile.bin", big_size);

    // Small files (varying sizes 0..12MB)
    std::mt19937_64 rng(123456);
    std::uniform_int_distribution<size_t> dist_bytes(0, 12 << 20);
    for (int i = 0; i < 60; ++i)
    {
        fs::path d = dirs[static_cast<size_t>(i) % dirs.size()];
        fs::path f = d / ("small_" + std::to_string(i) + ".dat");
        size_t sz = dist_bytes(rng);
        if (sz == 0)
        {
            std::ofstream ofs(f, std::ios::binary);
            REQUIRE(ofs.good());
        }
        else
        {
            write_file(f, sz);
        }
    }

    // Symlinks (point to a mix of files/dirs)
    for (int i = 0; i < 20; ++i)
    {
        fs::path target = (i % 2 == 0) ? (dirs[static_cast<size_t>(i) % dirs.size()])
                                       : (root / "bigfile.bin");
        fs::path link = root / ("link_" + std::to_string(i));
        std::error_code ec;
        fs::create_symlink(target, link, ec);
        REQUIRE(!ec);
    }
}

static void compare_trees(const fs::path &src, const fs::path &dst)
{
    for (auto &entry : fs::recursive_directory_iterator(src))
    {
        fs::path rel = fs::relative(entry.path(), src);
        fs::path d = dst / rel;
        if (fs::is_symlink(entry.path()))
        {
            REQUIRE(fs::is_symlink(d));
            std::error_code ec1, ec2;
            auto t1 = fs::read_symlink(entry.path(), ec1);
            auto t2 = fs::read_symlink(d, ec2);
            REQUIRE(!ec1);
            REQUIRE(!ec2);
            // Compare targets
            REQUIRE(t1 == t2);
        }
        else if (fs::is_directory(entry.path()))
        {
            REQUIRE(fs::is_directory(d));
        }
        else if (fs::is_regular_file(entry.path()))
        {
            REQUIRE(fs::is_regular_file(d));
            REQUIRE(fs::file_size(entry.path()) == fs::file_size(d));
        }
        else
        {
            // Unsupported types shouldn't appear
            REQUIRE(false);
        }
    }
}

TEST_CASE("integration_large_dataset_copy_verify", "[integration][large]")
{
    fs::path src_root = fs::path("testdata") / "int_large_dataset";
    generate_dataset(src_root);

    RWCombinedCopyOptions options;
    options.LogLevel = "info";
    options.LogMode = "console";
    options.CopyEngine = "libaio"; // or "liburing"
    options.CopyMode = "CopyOnly";
    options.CopyParallelism = 2;
    options.EnableInotify = false;
    options.PreserveSparseFiles = true;
    options.DirectIO = false;
    options.SyncWrites = false;
    options.IoSize = 1 << 20; // 1MB
    options.QueueDepth = 8;
    options.Batch = 4;
    options.IOReapWait = 1;

    auto logger = InitLogger(options);

    // First copy to /tmp
    fs::path dst_root = fs::path("/tmp") / ("acp_int_dst_" + std::to_string(::getpid()));
    int rc1 = CopyDir(src_root, dst_root, options, logger);
    REQUIRE(rc1 == 0);

    // Second copy (verification pass): copy dst to a verification folder
    fs::path verify_root = fs::path("/tmp") / ("acp_int_verify_" + std::to_string(::getpid()));
    int rc2 = CopyDir(dst_root, verify_root, options, logger);
    REQUIRE(rc2 == 0);

    // Compare src and verify trees
    compare_trees(src_root, verify_root);
}