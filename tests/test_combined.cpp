#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/combined/combined.hpp"

TEST_CASE("CopyEntry equality")
{
    CopyEntry a{"/tmp/src", "/tmp/dst"};
    CopyEntry b{"/tmp/src", "/tmp/dst"};
    CopyEntry c{"/tmp/src2", "/tmp/dst"};

    REQUIRE(a == b);
    REQUIRE(!(a == c));
}
