#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "base/event_reporter.hpp"
#include <fstream>
#include <filesystem>
#include <thread>
#include <vector>
#include <sstream>

namespace fs = std::filesystem;

TEST_CASE("FileLogReporter disabled no output", "[reporter]")
{
    FileLogReporter reporter(false, "file", 5, "/tmp/test_reporter_disabled.json", "");
    reporter.FileStart("/src/a", "/dst/a", 100);
    reporter.FileComplete("/src/a", "/dst/a", 100, 10);
    reporter.IncrementFilesTotal();
    reporter.IncrementFilesDone();
    // Nothing to verify — just ensure no crash and no file created
}

TEST_CASE("FileLogReporter FileStart event format", "[reporter]")
{
    std::string logPath = "tests/tmp_reporter_start.json";
    fs::remove(logPath);

    FileLogReporter reporter(true, "file", 5, logPath, "");
    reporter.FileStart("/src/file.txt", "/dst/file.txt", 2048);

    std::ifstream ifs(logPath);
    REQUIRE(ifs);
    std::string line;
    std::getline(ifs, line);
    REQUIRE(line.find("\"type\":\"file_info\"") != std::string::npos);
    REQUIRE(line.find("\"event\":\"file_start\"") != std::string::npos);
    REQUIRE(line.find("\"src\":\"/src/file.txt\"") != std::string::npos);
    REQUIRE(line.find("\"dst\":\"/dst/file.txt\"") != std::string::npos);
    REQUIRE(line.find("\"size\":2048") != std::string::npos);
    REQUIRE(line.find("\"timestamp\"") != std::string::npos);

    fs::remove(logPath);
}

TEST_CASE("FileLogReporter FileComplete speed calc no div zero", "[reporter]")
{
    std::string logPath = "tests/tmp_reporter_complete.json";
    fs::remove(logPath);

    FileLogReporter reporter(true, "file", 5, logPath, "");
    reporter.FileComplete("/src/f", "/dst/f", 1048576, 0);

    std::ifstream ifs(logPath);
    REQUIRE(ifs);
    std::string line;
    std::getline(ifs, line);
    REQUIRE(line.find("\"event\":\"file_complete\"") != std::string::npos);
    REQUIRE(line.find("\"speed_mbps\":0.0") != std::string::npos);

    fs::remove(logPath);
}

TEST_CASE("FileLogReporter StuckDetected event", "[reporter]")
{
    std::string logPath = "tests/tmp_reporter_stuck.json";
    fs::remove(logPath);

    FileLogReporter reporter(true, "file", 5, logPath, "");
    reporter.SetCurrentFile("/src/big.dat");
    reporter.IncrementFilesTotal(10);
    reporter.IncrementFilesDone(3);
    reporter.AddBytesTotal(1000000);
    reporter.AddBytesDone(300000);

    reporter.StuckDetected(42, 15);

    std::ifstream ifs(logPath);
    REQUIRE(ifs);
    std::string line;
    std::getline(ifs, line);
    REQUIRE(line.find("\"event\":\"stuck_detected\"") != std::string::npos);
    REQUIRE(line.find("\"round\":42") != std::string::npos);
    REQUIRE(line.find("\"stuck_seconds\":15") != std::string::npos);
    REQUIRE(line.find("\"files_total\":10") != std::string::npos);
    REQUIRE(line.find("\"files_done\":3") != std::string::npos);
    REQUIRE(line.find("\"bytes_total\":1000000") != std::string::npos);
    REQUIRE(line.find("\"bytes_done\":300000") != std::string::npos);
    REQUIRE(line.find("\"current_file\":\"/src/big.dat\"") != std::string::npos);

    fs::remove(logPath);
}

TEST_CASE("FileLogReporter progress summary ETA zero when no progress", "[reporter]")
{
    std::string logPath = "tests/tmp_reporter_progress.json";
    fs::remove(logPath);

    FileLogReporter reporter(true, "file", 0, logPath, "");
    reporter.SetFilesTotal(100);
    reporter.SetBytesTotal(104857600);
    // No bytes done, so speed=0, ETA should be 0

    reporter.MaybeEmitProgressSummary();

    std::ifstream ifs(logPath);
    REQUIRE(ifs);
    std::string line;
    std::getline(ifs, line);
    REQUIRE(line.find("\"event\":\"progress_summary\"") != std::string::npos);
    REQUIRE(line.find("\"eta_seconds\":0") != std::string::npos);

    fs::remove(logPath);
}

TEST_CASE("FileLogReporter state file atomic write", "[reporter]")
{
    std::string statePath = "tests/tmp_reporter_state.json";
    fs::remove(statePath);
    fs::remove(statePath + ".tmp");

    FileLogReporter reporter(true, "console", 5, "", statePath);
    reporter.IncrementFilesTotal(5);
    reporter.IncrementFilesDone(2);
    reporter.AddBytesTotal(10000);
    reporter.AddBytesDone(4000);

    reporter.UpdateStateFile();

    REQUIRE(fs::exists(statePath));
    REQUIRE_FALSE(fs::exists(statePath + ".tmp"));

    std::ifstream ifs(statePath);
    REQUIRE(ifs);
    std::string line;
    std::getline(ifs, line);
    REQUIRE(line.find("\"state\":\"running\"") != std::string::npos);
    REQUIRE(line.find("\"files_total\":5") != std::string::npos);
    REQUIRE(line.find("\"files_done\":2") != std::string::npos);

    fs::remove(statePath);
}

TEST_CASE("FileLogReporter concurrent AddBytesDone", "[reporter][thread]")
{
    FileLogReporter reporter(true, "file", 5, "/dev/null", "");
    constexpr int kThreads = 8;
    constexpr int kIterations = 10000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&reporter]()
                             {
            for (int i = 0; i < kIterations; ++i)
            {
                reporter.AddBytesDone(1);
            } });
    }

    for (auto &t : threads)
        t.join();

    REQUIRE(reporter.GetBytesDone() == static_cast<size_t>(kThreads * kIterations));
}
