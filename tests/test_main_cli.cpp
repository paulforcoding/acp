#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

#include <sys/wait.h>

static int run_acp(const std::string &args)
{
    std::string cmd = "./build/acp " + args + " >/dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc == -1)
        return -1;
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return -1;
}

TEST_CASE("CLI help returns 0", "[cli]")
{
    REQUIRE(run_acp("--help") == 0);
}

TEST_CASE("CLI version returns 0", "[cli]")
{
    REQUIRE(run_acp("--version") == 0);
}

TEST_CASE("CLI missing source returns 1", "[cli]")
{
    REQUIRE(run_acp("/nonexistent/path /tmp/dummy") == 1);
}

TEST_CASE("CLI multi-source non-dir dst returns 1", "[cli]")
{
    fs::path f1 = "/tmp/acp_cli_f1.txt";
    fs::path f2 = "/tmp/acp_cli_f2.txt";
    fs::path dst = "/tmp/acp_cli_dst.txt";

    std::error_code ec;
    {
        std::ofstream(f1) << "a";
        std::ofstream(f2) << "b";
        std::ofstream(dst) << "c";
    }

    int rc = run_acp(f1.string() + " " + f2.string() + " " + dst.string());
    REQUIRE(rc == 1);

    fs::remove(f1, ec);
    fs::remove(f2, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CLI same src dst returns 1", "[cli]")
{
    fs::path p = "/tmp/acp_cli_same.txt";
    std::error_code ec;
    {
        std::ofstream ofs(p);
        ofs << "x";
    }

    int rc = run_acp(p.string() + " " + p.string());
    REQUIRE(rc == 1);

    fs::remove(p, ec);
}

TEST_CASE("CLI subdirectory copy returns 1", "[cli]")
{
    fs::path src = "/tmp/acp_cli_sub_src";
    fs::path dst = "/tmp/acp_cli_sub_src/nested";
    std::error_code ec;
    fs::remove_all(src, ec);
    fs::create_directories(dst, ec);

    int rc = run_acp(src.string() + " " + dst.string());
    REQUIRE(rc == 1);

    fs::remove_all(src, ec);
}

TEST_CASE("CLI invalid engine returns 1", "[cli]")
{
    fs::path f = "/tmp/acp_cli_engine.txt";
    std::error_code ec;
    {
        std::ofstream ofs(f);
        ofs << "x";
    }

#ifdef __APPLE__
    int rc = run_acp("--engine=libaio " + f.string() + " /tmp/acp_cli_engine_dst.txt");
    // macOS explicit engine check returns 1
    REQUIRE(rc == 1);
#else
    int rc = run_acp("--engine=invalid " + f.string() + " /tmp/acp_cli_engine_dst.txt");
    // CLI11 ValidationError returns 105
    REQUIRE(rc != 0);
#endif

    fs::remove(f, ec);
    fs::remove("/tmp/acp_cli_engine_dst.txt", ec);
}

TEST_CASE("CLI IOSize zero returns 1", "[cli]")
{
    fs::path f = "/tmp/acp_cli_iosize.txt";
    std::error_code ec;
    {
        std::ofstream ofs(f);
        ofs << "x";
    }

    int rc = run_acp("--io-size=0 " + f.string() + " /tmp/acp_cli_iosize_dst.txt");
    // CLI11 ValidationError returns 105, not 1
    REQUIRE(rc != 0);

    fs::remove(f, ec);
    fs::remove("/tmp/acp_cli_iosize_dst.txt", ec);
}

struct CfgGuard
{
    fs::path cfg = "./acp_config.json";
    fs::path backup = "./acp_config.json.bak";
    CfgGuard()
    {
        std::error_code ec;
        if (fs::exists(cfg, ec))
            fs::rename(cfg, backup, ec);
    }
    ~CfgGuard()
    {
        std::error_code ec;
        fs::remove(cfg, ec);
        if (fs::exists(backup, ec))
            fs::rename(backup, cfg, ec);
    }
};

TEST_CASE("CLI config file loading from cwd", "[cli]")
{
    CfgGuard guard;
    fs::path f = "/tmp/acp_cli_cfg.txt";
    fs::path dst = "/tmp/acp_cli_cfg_dst.txt";
    std::error_code ec;

    {
        std::ofstream(f) << "x";
        std::ofstream(guard.cfg) << R"({"CopyOptions":{"IOSize":0}})";
    }

    int rc = run_acp(f.string() + " " + dst.string());
    REQUIRE(rc != 0);

    fs::remove(f, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CLI multi-source inotify disabled", "[cli]")
{
    fs::path d1 = "/tmp/acp_cli_ms1";
    fs::path d2 = "/tmp/acp_cli_ms2";
    fs::path dst = "/tmp/acp_cli_ms_dst";
    std::error_code ec;
    fs::remove_all(d1, ec);
    fs::remove_all(d2, ec);
    fs::remove_all(dst, ec);
    fs::create_directories(d1, ec);
    fs::create_directories(d2, ec);
    fs::create_directories(dst, ec);
    {
        std::ofstream(d1 / "a.txt") << "a";
        std::ofstream(d2 / "b.txt") << "b";
    }

    int rc = run_acp("--enable-inotify --io-stuck-timeout=0 " + d1.string() + " " + d2.string() + " " + dst.string());
    REQUIRE(rc == 0);
    // Inotify should be auto-disabled for multi-source; destination should still be copied
    // Multi-source preserves source directory structure
    REQUIRE(fs::exists(dst / "acp_cli_ms1" / "a.txt"));
    REQUIRE(fs::exists(dst / "acp_cli_ms2" / "b.txt"));

    fs::remove_all(d1, ec);
    fs::remove_all(d2, ec);
    fs::remove_all(dst, ec);
}
