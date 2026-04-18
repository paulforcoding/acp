#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "base/base.hpp"

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
