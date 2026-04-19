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

static RWCombinedCopyOptions make_copyfile_options()
{
    RWCombinedCopyOptions options;
    options.ProgramLogLevel = "info";
    options.ProgramLogMode = "console";
    options.CopyEngine = "libaio";
    options.CopyMode = "CopyOnly";
    options.CopyParallelism = 1;
    options.EnableInotify = false;
    options.PreserveSparseFiles = false;
    options.DirectIO = false;
    options.SyncWrites = false;
    options.IoSize = 1 << 20;
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;
    return options;
}

TEST_CASE("CopyFile single file", "[integration][copyfile]")
{
    fs::path src_file = fs::path("testdata") / "copyfile_src.dat";
    fs::path dst_file = fs::path("/tmp") / ("acp_copyfile_dst_" + std::to_string(::getpid()) + ".dat");

    write_file_exact(src_file, 1 << 20, 'F');

    auto options = make_copyfile_options();
    auto logger = InitLogger(options);

    int rc = CopyFile(src_file, dst_file, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(files_equal(src_file, dst_file));

    std::error_code ec;
    fs::remove(src_file, ec);
    fs::remove(dst_file, ec);
}

TEST_CASE("CopyFile small file", "[integration][copyfile]")
{
    fs::path src_file = fs::path("testdata") / "copyfile_small.dat";
    fs::path dst_file = fs::path("/tmp") / ("acp_copyfile_small_dst_" + std::to_string(::getpid()) + ".dat");

    write_file_exact(src_file, 12345, 'G');

    auto options = make_copyfile_options();
    auto logger = InitLogger(options);

    int rc = CopyFile(src_file, dst_file, options, logger);
    REQUIRE(rc == 0);
    REQUIRE(files_equal(src_file, dst_file));

    std::error_code ec;
    fs::remove(src_file, ec);
    fs::remove(dst_file, ec);
}
