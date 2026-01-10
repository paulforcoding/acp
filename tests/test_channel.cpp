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
