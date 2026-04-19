#include "lib/mainlib.hpp"
#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <fcntl.h>

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

static RWCombinedCopyOptions make_directio_options()
{
    RWCombinedCopyOptions options;
    options.LogLevel = "info";
    options.LogMode = "console";
    options.CopyEngine = "libaio";
    options.CopyMode = "CopyOnly";
    options.CopyParallelism = 1;
    options.EnableInotify = false;
    options.PreserveSparseFiles = false;
    options.DirectIO = true;
    options.SyncWrites = false;
    options.IoSize = 1 << 20; // 1MB, aligned to sector size
    options.QueueDepth = 8;
    options.Batch = 4;
    options.IOReapWait = 1;
    return options;
}

TEST_CASE("DirectIO copy aligned file", "[integration][directio]")
{
    fs::path src_dir = fs::path("testdata") / "dio_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_dio_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Use a size that is a multiple of sector size for DirectIO
    write_file_exact(src_dir / "aligned.bin", 2 << 20, 'D');

    auto options = make_directio_options();
    auto logger = InitLogger(options);

    int rc = CopyDir(src_dir, dst_dir, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(files_equal(src_dir / "aligned.bin", dst_dir / "aligned.bin"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
