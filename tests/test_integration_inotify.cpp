#include "lib/mainlib.hpp"
#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <sys/wait.h>
#include <signal.h>

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

    // Run CopyDir with inotify in a child process (it runs indefinitely)
    pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
    {
        // child: reset signal handlers inherited from parent (Catch2 installs its own)
        // so that SIGTERM kills us cleanly instead of being caught as a test failure
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        // run CopyDir; when inotify is enabled it never returns normally
        int rc = CopyDir(src_dir, dst_dir, options, logger, nullptr);
        _exit(rc);
    }

    // parent: wait for initial copy to complete
    bool initial_copied = false;
    for (int i = 0; i < 50; ++i)
    {
        if (fs::exists(dst_dir / "initial.dat"))
        {
            initial_copied = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(initial_copied);

    // Create new files to trigger inotify
    write_file(src_dir / "newfile1.dat", "new content one");
    write_file(src_dir / "newfile2.dat", "new content two");

    // Wait for inotify to pick up the changes
    bool new1_copied = false;
    bool new2_copied = false;
    for (int i = 0; i < 50; ++i)
    {
        if (!new1_copied && fs::exists(dst_dir / "newfile1.dat"))
            new1_copied = true;
        if (!new2_copied && fs::exists(dst_dir / "newfile2.dat"))
            new2_copied = true;
        if (new1_copied && new2_copied)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    REQUIRE(new1_copied);
    REQUIRE(new2_copied);
    REQUIRE(files_equal(src_dir / "newfile1.dat", dst_dir / "newfile1.dat"));
    REQUIRE(files_equal(src_dir / "newfile2.dat", dst_dir / "newfile2.dat"));

    // Kill the child process (inotify mode runs forever)
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i)
    {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
