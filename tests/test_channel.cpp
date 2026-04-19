#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "base/chan.hpp"
#include <thread>
#include <chrono>

using namespace std::chrono_literals;

struct Dummy
{
    int v;
    Dummy(int x = 0) : v(x) {}
};

TEST_CASE("Channel push/pop basic", "[channel]")
{
    Channel<Dummy> ch(2);

    auto item = std::make_unique<Dummy>(42);
    ch.Push(item);

    auto res = ch.Pop();
    REQUIRE(res.has_value());
    auto up = std::move(res.value());
    REQUIRE(up->v == 42);
}

TEST_CASE("Channel blocks until push", "[channel][blocking]")
{
    Channel<Dummy> ch(1);

    std::thread producer([&]()
                         {
        std::this_thread::sleep_for(50ms);
        auto it = std::make_unique<Dummy>(7);
        ch.Push(it); });

    // Pop should block until the producer pushes
    auto res = ch.Pop();
    REQUIRE(res.has_value());
    REQUIRE(res.value()->v == 7);

    producer.join();
}

TEST_CASE("Channel Close returns error on empty pop", "[channel][close]")
{
    Channel<Dummy> ch(2);
    ch.Close();

    auto res = ch.Pop();
    REQUIRE_FALSE(res.has_value());
}

TEST_CASE("InotifyChannel deduplication and pop", "[inotify]")
{
    InotifyChannel ic;

    ic.Push("fileA");
    ic.Push("fileA"); // duplicate should be ignored

    REQUIRE(ic.Size() == 1);

    auto r = ic.Pop();
    REQUIRE(r.has_value());
    REQUIRE(r.value() == "fileA");

    // after popping, pushing same key should succeed again
    ic.Push("fileA");
    REQUIRE(ic.Size() == 1);
    auto r2 = ic.Pop();
    REQUIRE(r2.has_value());
    REQUIRE(r2.value() == "fileA");
}

TEST_CASE("Channel Size concurrent", "[channel][thread]")
{
    constexpr int kCapacity = 64;
    constexpr int kTotalItems = 2000;
    Channel<Dummy> ch(kCapacity);
    std::atomic<int> pushed{0};
    std::atomic<int> popped{0};
    std::atomic<bool> done{false};

    std::thread producer([&]()
                         {
        for (int i = 0; i < kTotalItems; ++i)
        {
            auto item = std::make_unique<Dummy>(i);
            ch.Push(item);
            pushed.fetch_add(1);
        }
        done.store(true);
    });

    std::thread consumer([&]()
                         {
        while (popped.load() < kTotalItems || !done.load())
        {
            auto res = ch.Pop();
            if (res.has_value())
            {
                popped.fetch_add(1);
            }
        }
    });

    // Third thread continuously calls Size() while producer/consumer are active
    std::thread sizer([&]()
                      {
        int size_call_count = 0;
        while (popped.load() < kTotalItems || !done.load())
        {
            int sz = ch.Size();
            REQUIRE(sz >= 0);
            REQUIRE(sz <= kCapacity);
            ++size_call_count;
        }
    });

    producer.join();
    consumer.join();
    sizer.join();

    REQUIRE(pushed == kTotalItems);
    REQUIRE(popped == kTotalItems);
    REQUIRE(ch.Size() == 0);
}
