#include "lib/mainlib.hpp"
#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <future>

namespace fs = std::filesystem;

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

static RWCombinedCopyOptions make_inotify_options()
{
    RWCombinedCopyOptions options;
    options.LogLevel = "info";
    options.LogMode = "console";
    options.CopyEngine = "libaio";
    options.CopyMode = "CopyOnly";
    options.CopyParallelism = 1;
    options.EnableInotify = true;
    options.PreserveSparseFiles = false;
    options.DirectIO = false;
    options.SyncWrites = false;
    options.IoSize = 1 << 20;
    options.QueueDepth = 4;
    options.Batch = 2;
    options.IOReapWait = 1;
    return options;
}

// NOTE: This test blocks forever because CopyDir with EnableInotify=true
// enters an infinite monitoring loop. Marked as hidden [. ] so it is not
// executed in batch runs. Run it explicitly with: -t "inotify"
TEST_CASE("CopyDir with inotify initial copy", "[integration][inotify][.]")
{
    fs::path src_dir = fs::path("testdata") / "inotify_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_inotify_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Create initial file
    {
        std::ofstream ofs(src_dir / "initial.dat", std::ios::binary);
        ofs << "initial content";
    }

    auto options = make_inotify_options();
    auto logger = InitLogger(options);

    // CopyDir with inotify enabled blocks forever, so run it async
    std::future<int> copy_future = std::async(std::launch::async, [&]() {
        return CopyDir(src_dir, dst_dir, options, logger);
    });

    // Give it time to do the initial copy
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Verify initial copy succeeded
    REQUIRE(fs::exists(dst_dir / "initial.dat"));

    // Create a new file to trigger inotify
    {
        std::ofstream ofs(src_dir / "newfile.dat", std::ios::binary);
        ofs << "new content";
    }

    // Wait for inotify to pick up the change
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Verify the new file was copied
    REQUIRE(fs::exists(dst_dir / "newfile.dat"));

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
