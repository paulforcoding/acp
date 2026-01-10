#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "base/chan.hpp"

struct Foo
{
    int v;
};

TEST_CASE("Channel push/pop and close behavior")
{
    Channel<Foo> ch(2);

    auto p = std::make_unique<Foo>();
    p->v = 123;
    ch.Push(p);

    auto res = ch.Pop();
    REQUIRE(res.has_value());
    REQUIRE(res.value()->v == 123);

    // Close when empty -> Pop returns error
    ch.Close();
    auto res2 = ch.Pop();
    REQUIRE(!res2.has_value());
}

TEST_CASE("InotifyChannel dedupe and pop")
{
    InotifyChannel ic;
    ic.Push("a");
    ic.Push("a"); // duplicate should be ignored
    REQUIRE(ic.Size() == 1);

    auto r = ic.Pop();
    REQUIRE(r.has_value());
    REQUIRE(r.value() == "a");
    REQUIRE(ic.Size() == 0);

    ic.Close();
    auto r2 = ic.Pop();
    REQUIRE(!r2.has_value());
}
