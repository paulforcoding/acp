#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "base/chan.hpp"

TEST_CASE("DedupList push/remove semantics")
{
    DedupList<int> dl;
    auto p = std::make_shared<int>(7);
    dl.Push(p, "k");
    dl.Push(p, "k"); // duplicate ignored
    REQUIRE(dl.Size() == 1);

    dl.Remove(p, "k");
    REQUIRE(dl.Empty());
}
