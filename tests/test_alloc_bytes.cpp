#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "base/base.hpp"
#include <cstdint>

TEST_CASE("AllocBytes sector aligned")
{
    char* p = AllocBytes(SECTORSIZE, 4096);
    REQUIRE(p != nullptr);
    // Check alignment: address should be multiple of SECTORSIZE
    REQUIRE(reinterpret_cast<uintptr_t>(p) % SECTORSIZE == 0);
    FreeBytes(p);
    REQUIRE(p == nullptr);
}

TEST_CASE("AllocBytes larger alignment")
{
    constexpr size_t kAlign = 4096;
    constexpr size_t kSize = 8192;

    char* p = AllocBytes(kAlign, kSize);
    REQUIRE(p != nullptr);
    REQUIRE(reinterpret_cast<uintptr_t>(p) % kAlign == 0);

    // Write to the full buffer to ensure it's actually allocated
    std::memset(p, 0xAB, kSize);

    FreeBytes(p);
    REQUIRE(p == nullptr);
}

TEST_CASE("AllocBytes small allocation")
{
    char* p = AllocBytes(SECTORSIZE, 1);
    REQUIRE(p != nullptr);
    *p = 'x';
    REQUIRE(*p == 'x');
    FreeBytes(p);
    REQUIRE(p == nullptr);
}

TEST_CASE("FreeBytes with nullptr")
{
    char* p = nullptr;
    FreeBytes(p);
    REQUIRE(p == nullptr);
}
