#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "base/chan.hpp"
#include "lib/combined/combined.hpp"
#include "base/logger.hpp"
#include <thread>
#include <chrono>

static std::shared_ptr<CPFilePair> MakeFilePair(const std::string &src, const std::string &dst)
{
    auto logger = std::make_shared<ConsoleLogger>();
    return std::make_shared<CPFilePair>(src, dst, false, false, false, false, logger, nullptr);
}

TEST_CASE("FPChannel Push Peek Pop sequence", "[fpchannel]")
{
    FPChannel ch;
    auto fp = MakeFilePair("/tmp/fp_src1", "/tmp/fp_dst1");
    ch.Push(fp);

    REQUIRE(ch.PendingCount() == 1);
    REQUIRE(ch.InflightCount() == 0);

    auto front = ch.PeekFront();
    REQUIRE(front == fp);
    REQUIRE(ch.PendingCount() == 1);

    ch.MoveToInflight(fp);
    REQUIRE(ch.PendingCount() == 0);
    REQUIRE(ch.InflightCount() == 1);

    ch.RemoveFromInflight(fp);
    REQUIRE(ch.PendingCount() == 0);
    REQUIRE(ch.InflightCount() == 0);
}

TEST_CASE("FPChannel WaitForWorkOrClose inflight", "[fpchannel]")
{
    FPChannel ch;
    auto fp = MakeFilePair("/tmp/fp_src2", "/tmp/fp_dst2");
    ch.Push(fp);
    ch.MoveToInflight(fp);

    // Even though pending is empty and not closed, inflight is non-empty
    bool should_continue = ch.WaitForWorkOrClose(std::chrono::milliseconds(50));
    REQUIRE(should_continue);

    ch.RemoveFromInflight(fp);
    // When not closed and empty, WaitForWorkOrClose returns true (spurious wakeup)
    should_continue = ch.WaitForWorkOrClose(std::chrono::milliseconds(50));
    REQUIRE(should_continue);

    // After close, it should return false
    ch.Close();
    should_continue = ch.WaitForWorkOrClose(std::chrono::milliseconds(50));
    REQUIRE_FALSE(should_continue);
}

TEST_CASE("FPChannel Close wakes waiters", "[fpchannel]")
{
    FPChannel ch;

    std::thread waiter([&ch]()
                       {
        bool result = ch.WaitForWorkOrClose(std::chrono::milliseconds(5000));
        // Should return false because channel is closed and empty
        REQUIRE_FALSE(result); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ch.Close();
    waiter.join();
}

TEST_CASE("FPChannel concurrent push and pop", "[fpchannel][thread]")
{
    FPChannel ch;
    constexpr int kCount = 100;

    std::thread producer([&ch]()
                         {
        for (int i = 0; i < kCount; ++i)
        {
            ch.Push(MakeFilePair("/tmp/fp_src_" + std::to_string(i),
                                   "/tmp/fp_dst_" + std::to_string(i)));
        }
        ch.Close(); });

    std::thread consumer([&ch]()
                         {
        int processed = 0;
        while (true)
        {
            auto fp = ch.PeekFront();
            if (!fp)
            {
                if (!ch.WaitForWorkOrClose(std::chrono::milliseconds(10)))
                    break;
                continue;
            }
            ch.MoveToInflight(fp);
            ch.RemoveFromInflight(fp);
            ++processed;
        }
        REQUIRE(processed == kCount); });

    producer.join();
    consumer.join();
}
