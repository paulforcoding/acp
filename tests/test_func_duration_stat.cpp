#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "base/base.hpp"
#include <sstream>

TEST_CASE("FuncDurationStat single entry")
{
    auto logger = std::make_shared<ConsoleLogger>();
    FuncDurationStat stats(logger);

    stats.AddDuration("TestFunc", 100);

    std::stringstream ss;
    auto oldCoutBuf = std::cerr.rdbuf(ss.rdbuf());
    stats.PrintStats();
    std::cerr.rdbuf(oldCoutBuf);

    std::string output = ss.str();
    REQUIRE(output.find("TestFunc") != std::string::npos);
    REQUIRE(output.find("Count: 1") != std::string::npos);
}

TEST_CASE("FuncDurationStat multiple entries")
{
    auto logger = std::make_shared<ConsoleLogger>();
    FuncDurationStat stats(logger);

    stats.AddDuration("FuncA", 50);
    stats.AddDuration("FuncA", 150);
    stats.AddDuration("FuncA", 100);
    stats.AddDuration("FuncB", 200);

    std::stringstream ss;
    auto oldCoutBuf = std::cerr.rdbuf(ss.rdbuf());
    stats.PrintStats();
    std::cerr.rdbuf(oldCoutBuf);

    std::string output = ss.str();
    REQUIRE(output.find("FuncA") != std::string::npos);
    REQUIRE(output.find("FuncB") != std::string::npos);
    REQUIRE(output.find("Count: 3") != std::string::npos);
    REQUIRE(output.find("Count: 1") != std::string::npos);
}

TEST_CASE("FuncDurationStat time_point overload")
{
    auto logger = std::make_shared<ConsoleLogger>();
    FuncDurationStat stats(logger);

    auto start = std::chrono::system_clock::now();
    auto end = start + std::chrono::microseconds(500);
    stats.AddDuration("TimedFunc", start, end);

    std::stringstream ss;
    auto oldCoutBuf = std::cerr.rdbuf(ss.rdbuf());
    stats.PrintStats();
    std::cerr.rdbuf(oldCoutBuf);

    std::string output = ss.str();
    REQUIRE(output.find("TimedFunc") != std::string::npos);
}

TEST_CASE("FuncDurationStat empty stats")
{
    auto logger = std::make_shared<ConsoleLogger>();
    FuncDurationStat stats(logger);

    std::stringstream ss;
    auto oldCoutBuf = std::cerr.rdbuf(ss.rdbuf());
    stats.PrintStats();
    std::cerr.rdbuf(oldCoutBuf);

    std::string output = ss.str();
    REQUIRE(output.find("Function Duration Statistics") != std::string::npos);
}
