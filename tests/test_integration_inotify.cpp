#include "lib/mainlib.hpp"
#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <future>
#include <atomic>

namespace fs = std::filesystem;

static void ensure_clean_dir(const fs::path &p)
{
    std::error_code ec;
    if (fs::exists(p, ec)) fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    REQUIRE(!ec);
}

static void write_file(const fs::path &p, const std::string &content)
{
    std::ofstream ofs(p, std::ios::binary);
    REQUIRE(ofs.good());
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
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
    options.ProgramLogLevel = "warn";
    options.ProgramLogMode = "console";
    options.CopyEngine = "libaio";
    options.CopyMode = "CopyOnly";
    options.CopyParallelism = 1;
    options.CopyChanSize = 10;
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

TEST_CASE("CopyDir with inotify detects new files", "[integration][inotify]")
{
    fs::path src_dir = fs::path("testdata") / "inotify_src";
    fs::path dst_dir = fs::path("/tmp") / ("acp_inotify_dst_" + std::to_string(::getpid()));
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Create initial file
    write_file(src_dir / "initial.dat", "initial content");

    auto options = make_inotify_options();
    auto logger = InitLogger(options);

    std::atomic<bool> stopFlag{false};

    // Run CopyDir with inotify in background
    std::future<int> copy_future = std::async(std::launch::async, [&]() {
        return CopyDir(src_dir, dst_dir, options, logger, &stopFlag);
    });

    // Wait for initial copy to complete (SSD: should finish within 1s)
    bool initial_copied = false;
    for (int i = 0; i < 20; ++i)
    {
        if (fs::exists(dst_dir / "initial.dat"))
        {
            initial_copied = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(initial_copied);

    // Create new files to trigger inotify
    write_file(src_dir / "newfile1.dat", "new content one");
    write_file(src_dir / "newfile2.dat", "new content two");

    // Wait for inotify to pick up the changes (SSD: should finish within 1s)
    bool new1_copied = false;
    bool new2_copied = false;
    for (int i = 0; i < 20; ++i)
    {
        if (!new1_copied && fs::exists(dst_dir / "newfile1.dat"))
            new1_copied = true;
        if (!new2_copied && fs::exists(dst_dir / "newfile2.dat"))
            new2_copied = true;
        if (new1_copied && new2_copied)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    REQUIRE(new1_copied);
    REQUIRE(new2_copied);
    REQUIRE(files_equal(src_dir / "newfile1.dat", dst_dir / "newfile1.dat"));
    REQUIRE(files_equal(src_dir / "newfile2.dat", dst_dir / "newfile2.dat"));

    // Signal CopyDir to stop
    stopFlag.store(true);

    // Wait for CopyDir to finish (should exit within 2s)
    auto status = copy_future.wait_for(std::chrono::seconds(2));
    REQUIRE(status == std::future_status::ready);

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
