#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/combined/combined.hpp"
#include "base/logger.hpp"
#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <sys/un.h>

TEST_CASE("CPFilePair CheckAndInit skips socket file", "[integration][cpfilepair]")
{
    namespace fs = std::filesystem;
    std::string src_dir = "tests/tmp_sock";
    std::string src_sock = src_dir + "/test.sock";
    std::string dst_dir = "tests/tmp_out_sock";
    std::string dst_sock = dst_dir + "/test.sock";

    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
    fs::create_directories(src_dir, ec);
    fs::create_directories(dst_dir, ec);

    // Create a Unix domain socket file
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, src_sock.c_str(), sizeof(addr.sun_path) - 1);
    int bind_rc = bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    REQUIRE(bind_rc == 0);
    ::close(fd);
    REQUIRE(fs::exists(src_sock));

    auto logger = std::make_shared<ConsoleLogger>();
    RWCombinedCopyOptions opts;
    opts.IoSize = 4096;
    opts.DirectIO = false;
    opts.SyncWrites = false;
    opts.ProgramLogMode = "file";
    opts.ProgramLogFilePath = "/tmp/acp_program.log";

    CPFilePair p(src_sock, dst_sock, opts.IoSize, opts.DirectIO, opts.SyncWrites, /*cksum*/ false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE_FALSE(init_res.has_value());
    REQUIRE(init_res.error().Code() == ENOTSUP);

    // cleanup
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CPFilePair CheckAndInit with real file", "[integration][cpfilepair]")
{
    namespace fs = std::filesystem;
    std::string src_dir = "tests/tmp_src_hello";
    std::string src = src_dir + "/hello.txt";
    std::string dst_dir = "tests/tmp_out";
    std::string dst = dst_dir + "/hello_copy.txt";

    // cleanup and create src file
    std::error_code ec;
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
    fs::create_directories(src_dir, ec);
    fs::create_directories(dst_dir, ec);
    {
        std::ofstream ofs(src);
        REQUIRE(ofs.good());
        ofs << "hello world";
    }
    REQUIRE(fs::exists(src));

    auto logger = std::make_shared<ConsoleLogger>();
    RWCombinedCopyOptions opts;
    opts.IoSize = 4096;
    opts.DirectIO = false;
    opts.SyncWrites = false;
    opts.ProgramLogMode = "file";
    opts.ProgramLogFilePath = "/tmp/acp_program.log";

    CPFilePair p(src, dst, opts.IoSize, opts.DirectIO, opts.SyncWrites, /*cksum*/ false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    // check src fd valid
    REQUIRE(p.GetSrcFd() >= 0);
    // dst file should be created (CheckAndInit creates it)
    REQUIRE(fs::exists(dst));

    // cleanup
    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}
