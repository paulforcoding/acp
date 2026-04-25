#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "base/base.hpp"
#include "base/inotify.hpp"
#include <thread>
#include <vector>
#include <atomic>

TEST_CASE("StackError basic behavior")
{
    StackError e("first", 42);
    REQUIRE(e.Code() == 42);
    std::string what = e.ToString();
    REQUIRE(what.find("first") != std::string::npos);

    StackError e2("second", e, 42);
    REQUIRE(e2.Code() == 42);
    std::string what2 = e2.ToString();
    REQUIRE(what2.find("second") != std::string::npos);

    auto from_errno = StackError::FromErrno(EINVAL);
    REQUIRE(from_errno.Code() == EINVAL);
    REQUIRE(std::string(from_errno.ToString()).size() > 0);
}

TEST_CASE("ILogger set_level string", "[logger]")
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

    // unknown string should leave level unchanged
    logger.set_level("Fatal");
    logger.set_level("UNKNOWN");
    REQUIRE(logger.level() == Level::Fatal);
}

TEST_CASE("StackError thread-safe ToString", "[thread]")
{
    StackError err("concurrent-test", 99);
    constexpr int kThreadCount = 8;
    constexpr int kIterations = 1000;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreadCount; ++t)
    {
        threads.emplace_back([&err, &success_count]()
                             {
            for (int i = 0; i < kIterations; ++i)
            {
                std::string s = err.ToString();
                if (s.find("concurrent-test") != std::string::npos)
                {
                    success_count.fetch_add(1);
                }
            } });
    }

    for (auto &t : threads)
        t.join();

    REQUIRE(success_count == kThreadCount * kIterations);
}

TEST_CASE("Inotify invalid path returns expected", "[inotify]")
{
    auto logger = std::make_shared<ConsoleLogger>();
    Inotify watcher("/nonexistent/path/for/inotify/test", logger);
    auto res = watcher.Init();
    REQUIRE_FALSE(res.has_value());
}

TEST_CASE("StackError is not std exception", "[base]")
{
    // Bug #1: StackError does not inherit std::exception.
    // This means catch (const std::exception&) will miss it.
    StackError err("test error", 42);
    bool caught_as_exception = false;
    try
    {
        throw err;
    }
    catch (const std::exception &)
    {
        caught_as_exception = true;
    }
    catch (const StackError &)
    {
        caught_as_exception = false;
    }
    REQUIRE_FALSE(caught_as_exception);
}

TEST_CASE("StackError FromFormat basic", "[base]")
{
    auto err = StackError::FromFormat("code={}, msg={}", 42, "hello");
    REQUIRE(err.Code() == 0);
    REQUIRE(std::string(err.ToString()).find("code=42, msg=hello") != std::string::npos);
}

TEST_CASE("StackError Append chain", "[base]")
{
    StackError err("first");
    err.Append("second: {}", 2);
    err.Append("third: {}", 3);
    auto msg = std::string(err.ToString());
    REQUIRE(msg.find("first") != std::string::npos);
    REQUIRE(msg.find("second: 2") != std::string::npos);
    REQUIRE(msg.find("third: 3") != std::string::npos);
}

TEST_CASE("AllocBytes alignment 512", "[base]")
{
    char *p = AllocBytes(512, 1024);
    REQUIRE(p != nullptr);
    REQUIRE(reinterpret_cast<uintptr_t>(p) % 512 == 0);
    FreeBytes(p);
}

TEST_CASE("FreeBytes null safety", "[base]")
{
    char *p = nullptr;
    FreeBytes(p); // should not crash
    REQUIRE(p == nullptr);
}
