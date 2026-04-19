#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "base/logger.hpp"
#include <sstream>
#include <fstream>
#include <filesystem>

TEST_CASE("ILogger enabled boundary")
{
    ConsoleLogger logger;
    logger.set_level(Level::Info);

    REQUIRE(logger.enabled(Level::Info));
    REQUIRE(logger.enabled(Level::Warn));
    REQUIRE(logger.enabled(Level::Error));
    REQUIRE(logger.enabled(Level::Fatal));
    REQUIRE_FALSE(logger.enabled(Level::Debug));
    REQUIRE_FALSE(logger.enabled(Level::Trace));
}

TEST_CASE("ILogger set_level case insensitive")
{
    ConsoleLogger logger;

    logger.set_level("INFO");
    REQUIRE(logger.level() == Level::Info);

    logger.set_level("info");
    REQUIRE(logger.level() == Level::Info);

    logger.set_level("Warn");
    REQUIRE(logger.level() == Level::Warn);

    logger.set_level("warning");
    REQUIRE(logger.level() == Level::Warn);

    logger.set_level("TRACE");
    REQUIRE(logger.level() == Level::Trace);

    logger.set_level("Debug");
    REQUIRE(logger.level() == Level::Debug);

    logger.set_level("ERROR");
    REQUIRE(logger.level() == Level::Error);

    logger.set_level("fatal");
    REQUIRE(logger.level() == Level::Fatal);
}

TEST_CASE("ILogger unknown level leaves unchanged")
{
    ConsoleLogger logger;
    logger.set_level(Level::Error);

    logger.set_level("UNKNOWN");
    REQUIRE(logger.level() == Level::Error);

    logger.set_level("");
    REQUIRE(logger.level() == Level::Error);
}

TEST_CASE("ConsoleLogger log_impl outputs all levels")
{
    ConsoleLogger logger;
    logger.set_level(Level::Trace);

    std::stringstream ss;
    auto oldBuf = std::cerr.rdbuf(ss.rdbuf());

    logger.trace("trace-msg");
    logger.debug("debug-msg");
    logger.info("info-msg");
    logger.warn("warn-msg");
    logger.error("error-msg");
    logger.fatal("fatal-msg");

    std::cerr.rdbuf(oldBuf);
    std::string output = ss.str();

    REQUIRE(output.find("\"type\":\"program_log\"") != std::string::npos);
    REQUIRE(output.find("\"level\":\"trace\"") != std::string::npos);
    REQUIRE(output.find("\"msg\":\"trace-msg\"") != std::string::npos);
    REQUIRE(output.find("\"level\":\"debug\"") != std::string::npos);
    REQUIRE(output.find("\"msg\":\"debug-msg\"") != std::string::npos);
    REQUIRE(output.find("\"level\":\"info\"") != std::string::npos);
    REQUIRE(output.find("\"msg\":\"info-msg\"") != std::string::npos);
    REQUIRE(output.find("\"level\":\"warn\"") != std::string::npos);
    REQUIRE(output.find("\"msg\":\"warn-msg\"") != std::string::npos);
    REQUIRE(output.find("\"level\":\"error\"") != std::string::npos);
    REQUIRE(output.find("\"msg\":\"error-msg\"") != std::string::npos);
    REQUIRE(output.find("\"level\":\"fatal\"") != std::string::npos);
    REQUIRE(output.find("\"msg\":\"fatal-msg\"") != std::string::npos);
    REQUIRE(output.find("\"timestamp\"") != std::string::npos);
}

TEST_CASE("ConsoleLogger filters below level")
{
    ConsoleLogger logger;
    logger.set_level(Level::Warn);

    std::stringstream ss;
    auto oldBuf = std::cerr.rdbuf(ss.rdbuf());

    logger.info("should-not-appear");
    logger.warn("should-appear");

    std::cerr.rdbuf(oldBuf);
    std::string output = ss.str();

    REQUIRE(output.find("should-not-appear") == std::string::npos);
    REQUIRE(output.find("should-appear") != std::string::npos);
}

TEST_CASE("SpdLogger null logger")
{
    SpdLogger logger(nullptr);
    logger.set_level(Level::Info);
    // Should not crash
    logger.info("test");
    logger.error("test");
}
