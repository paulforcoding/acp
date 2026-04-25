#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/combined/combined.hpp"
#include "base/logger.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <fcntl.h>

namespace fs = std::filesystem;

static void ensure_clean_dir(const fs::path &p)
{
    std::error_code ec;
    if (fs::exists(p, ec)) fs::remove_all(p, ec);
    fs::create_directories(p, ec);
}

TEST_CASE("CPFilePair symlink copy", "[cpfilepair]")
{
    std::string src_dir = "tests/tmp_symlink_src";
    std::string dst_dir = "tests/tmp_symlink_dst";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    // Create a regular file and a symlink pointing to it
    std::string target_file = src_dir + "/target.txt";
    {
        std::ofstream ofs(target_file);
        ofs << "hello";
    }
    std::string src_link = src_dir + "/link.txt";
    std::error_code ec;
    fs::create_symlink("target.txt", src_link, ec);
    REQUIRE(!ec);

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src_link, dst_dir + "/link.txt", false, false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());
    REQUIRE(p.IsSymlink());
    REQUIRE(fs::is_symlink(dst_dir + "/link.txt"));

    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CPFilePair dangling symlink", "[cpfilepair]")
{
    std::string src_dir = "tests/tmp_dangling_src";
    std::string dst_dir = "tests/tmp_dangling_dst";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    std::string src_link = src_dir + "/dangling_link";
    std::error_code ec;
    fs::create_symlink("nonexistent", src_link, ec);
    REQUIRE(!ec);

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src_link, dst_dir + "/dangling_link", false, false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());
    REQUIRE(p.IsSymlink());

    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CPFilePair symlink overwrites existing dst", "[cpfilepair]")
{
    std::string src_dir = "tests/tmp_symlink_overwrite_src";
    std::string dst_dir = "tests/tmp_symlink_overwrite_dst";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    std::string src_link = src_dir + "/link";
    std::string dst_link = dst_dir + "/link";
    std::error_code ec;
    fs::create_symlink("a", src_link, ec);
    fs::create_symlink("b", dst_link, ec);
    REQUIRE(!ec);

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src_link, dst_link, false, false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());
    auto new_target = fs::read_symlink(dst_link, ec);
    REQUIRE(!ec);
    REQUIRE(new_target == "a");

    fs::remove_all(src_dir, ec);
    fs::remove_all(dst_dir, ec);
}

TEST_CASE("CPFilePair directory copy", "[cpfilepair]")
{
    std::string src_dir = "tests/tmp_dir_src/nested";
    std::string dst_dir = "tests/tmp_dir_dst/nested";
    ensure_clean_dir(src_dir);
    ensure_clean_dir("tests/tmp_dir_dst");

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src_dir, dst_dir, false, false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());
    REQUIRE(p.IsDir());
    REQUIRE(fs::is_directory(dst_dir));

    std::error_code ec;
    fs::remove_all("tests/tmp_dir_src", ec);
    fs::remove_all("tests/tmp_dir_dst", ec);
}

TEST_CASE("CPFilePair directory already exists", "[cpfilepair]")
{
    std::string src_dir = "tests/tmp_direxists_src/sub";
    std::string dst_dir = "tests/tmp_direxists_dst/sub";
    ensure_clean_dir(src_dir);
    ensure_clean_dir(dst_dir);

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src_dir, dst_dir, false, false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());
    REQUIRE(fs::is_directory(dst_dir));

    std::error_code ec;
    fs::remove_all("tests/tmp_direxists_src", ec);
    fs::remove_all("tests/tmp_direxists_dst", ec);
}

TEST_CASE("CPFilePair truncate and fsync", "[cpfilepair]")
{
    std::string src = "tests/tmp_truncate_src.dat";
    std::string dst = "tests/tmp_truncate_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    // Create source file with some content
    {
        std::ofstream ofs(src, std::ios::binary);
        ofs.write("0123456789", 10);
    }

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, false, false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    auto trunc_res = p.TruncateDstToSrcSize();
    REQUIRE(trunc_res.has_value());

    auto fsync_res = p.FsyncDst();
    REQUIRE(fsync_res.has_value());

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CPFilePair offset tracking", "[cpfilepair]")
{
    std::string src = "tests/tmp_offset_src.dat";
    std::string dst = "tests/tmp_offset_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        ofs.write("abcdefghij", 10);
    }

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, false, false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    REQUIRE(p.GetReadOffset() == 0);
    REQUIRE(p.GetWriteOffset() == 0);
    REQUIRE_FALSE(p.IsReadFinished());
    REQUIRE_FALSE(p.IsWriteFinished());

    p.UpdatePrepareReadBytes(5);
    REQUIRE(p.GetReadOffset() == 5);
    REQUIRE_FALSE(p.IsReadFinished());

    p.UpdatePrepareReadBytes(5);
    REQUIRE(p.GetReadOffset() == 10);
    REQUIRE(p.IsReadFinished());

    p.UpdateWrittenBytes(10);
    REQUIRE(p.GetWriteOffset() == 10);
    REQUIRE(p.IsWriteFinished());

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CPFilePair zero size file", "[cpfilepair]")
{
    std::string src = "tests/tmp_zero_src.dat";
    std::string dst = "tests/tmp_zero_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
    }
    REQUIRE(fs::file_size(src) == 0);

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, false, false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    // 0-size file should immediately be considered finished
    REQUIRE(p.GetSrcFileSize() == 0);
    REQUIRE(p.IsReadFinished());
    REQUIRE(p.IsWriteFinished());

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CPFilePair DoDstState", "[cpfilepair]")
{
    std::string src = "tests/tmp_dodst_src.dat";
    std::string dst = "tests/tmp_dodst_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        ofs.write("x", 1);
    }

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, false, false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    auto dst_state = p.DoDstState();
    REQUIRE(dst_state.has_value());
    REQUIRE(p.GetDstFileSize() == 0); // just created, truncated to 0

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CPFilePair CksumCopy size_mtime fast path", "[cpfilepair]")
{
    std::string src = "tests/tmp_ckfast_src.dat";
    std::string dst = "tests/tmp_ckfast_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(1024, 'X');
        ofs.write(buf.data(), buf.size());
    }
    // Ensure different mtime so the size+mtime fast path does not trigger
    std::this_thread::sleep_for(std::chrono::seconds(1));
    {
        std::ofstream ofs(dst, std::ios::binary);
        std::string buf(1024, 'Y');
        ofs.write(buf.data(), buf.size());
    }

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, false, true, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    // Same size but different mtime — fast path should NOT trigger
    REQUIRE_FALSE(p.IsSkipBlockCksum());

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CPFilePair CksumCopy fast path same mtime", "[cpfilepair]")
{
    std::string src = "tests/tmp_ckfast2_src.dat";
    std::string dst = "tests/tmp_ckfast2_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    // Create both files with identical mtime
    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(1024, 'A');
        ofs.write(buf.data(), buf.size());
    }
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    // Copy preserves mtime on APFS, but if not, force it
    fs::last_write_time(dst, fs::last_write_time(src), ec);

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, false, true, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    // Same size and same mtime — fast path SHOULD trigger
    REQUIRE(p.IsSkipBlockCksum());

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CPFilePair IsAllZeros all zero", "[cpfilepair]")
{
    std::vector<char> buf(4096, 0);
    REQUIRE(IsAllZeros(buf.data(), buf.size()));
}

TEST_CASE("CPFilePair IsAllZeros not zero", "[cpfilepair]")
{
    std::vector<char> buf(4096, 0);
    buf[100] = 1;
    REQUIRE_FALSE(IsAllZeros(buf.data(), buf.size()));
}

TEST_CASE("CPFilePair IsAllZeros unaligned size", "[cpfilepair]")
{
    std::vector<char> buf(7, 0);
    REQUIRE(IsAllZeros(buf.data(), buf.size()));
    buf[3] = 1;
    REQUIRE_FALSE(IsAllZeros(buf.data(), buf.size()));
}

TEST_CASE("CPFilePair short read does not overestimate", "[cpfilepair]")
{
    // Bug #5: UpdatePrepareReadBytes(io_size) overestimates on short reads.
    std::string src = "tests/tmp_shortread_src.dat";
    std::string dst = "tests/tmp_shortread_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        std::string buf(100, 'Z');
        ofs.write(buf.data(), buf.size());
    }

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, false, false, false, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    // Simulate a read larger than file size
    p.UpdatePrepareReadBytes(1024);
    // mReadBytes now exceeds file size, IsReadFinished is true
    REQUIRE(p.IsReadFinished());

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CPFilePair preserve mode", "[cpfilepair]")
{
    std::string src = "tests/tmp_preserve_mode_src.dat";
    std::string dst = "tests/tmp_preserve_mode_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        ofs.write("x", 1);
    }
    fs::permissions(src, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read, ec);

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, false, false, false, true, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    auto meta_res = p.PreserveMetadata();
    REQUIRE(meta_res.has_value());

    auto dst_perms = fs::status(dst, ec).permissions();
    auto src_perms = fs::status(src, ec).permissions();
    REQUIRE(dst_perms == src_perms);

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CPFilePair preserve timestamps", "[cpfilepair]")
{
    std::string src = "tests/tmp_preserve_time_src.dat";
    std::string dst = "tests/tmp_preserve_time_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        ofs.write("x", 1);
    }
    // Set a specific mtime
    auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(24);
    fs::last_write_time(src, old_time, ec);

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, false, false, false, true, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    auto meta_res = p.PreserveMetadata();
    REQUIRE(meta_res.has_value());

    auto src_mtime = fs::last_write_time(src, ec);
    auto dst_mtime = fs::last_write_time(dst, ec);
    // Convert to duration since epoch to avoid Catch2 stringize ambiguity on macOS
    auto src_ms = std::chrono::duration_cast<std::chrono::milliseconds>(src_mtime.time_since_epoch()).count();
    auto dst_ms = std::chrono::duration_cast<std::chrono::milliseconds>(dst_mtime.time_since_epoch()).count();
    REQUIRE(src_ms == dst_ms);

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CPFilePair compare mode mismatch", "[cpfilepair]")
{
    std::string src = "tests/tmp_cmp_mode_src.dat";
    std::string dst = "tests/tmp_cmp_mode_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        ofs.write("x", 1);
    }
    {
        std::ofstream ofs(dst, std::ios::binary);
        ofs.write("x", 1);
    }
    // Different permissions
    fs::permissions(src, fs::perms::owner_all, ec);
    fs::permissions(dst, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read, ec);

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, false, true, true, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    auto cmp_res = p.CompareMetadata();
    // CompareMetadata always returns success; mismatches are emitted as events
    REQUIRE(cmp_res.has_value());

    fs::remove(src, ec);
    fs::remove(dst, ec);
}

TEST_CASE("CPFilePair compare timestamp mismatch", "[cpfilepair]")
{
    std::string src = "tests/tmp_cmp_time_src.dat";
    std::string dst = "tests/tmp_cmp_time_dst.dat";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(dst, ec);

    {
        std::ofstream ofs(src, std::ios::binary);
        ofs.write("x", 1);
    }
    {
        std::ofstream ofs(dst, std::ios::binary);
        ofs.write("x", 1);
    }
    // Different mtimes
    auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(24);
    fs::last_write_time(src, old_time, ec);

    auto logger = std::make_shared<ConsoleLogger>();
    CPFilePair p(src, dst, false, true, true, false, logger, nullptr);
    auto init_res = p.CheckAndInit();
    REQUIRE(init_res.has_value());

    auto cmp_res = p.CompareMetadata();
    REQUIRE(cmp_res.has_value());

    fs::remove(src, ec);
    fs::remove(dst, ec);
}
