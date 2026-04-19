#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "base/digest.hpp"
#include <vector>
#include <cstring>
#include <xxhash.h>

TEST_CASE("Digest empty data")
{
    MD5Digest md5;
    SHA256Digest sha256;
    XXHash64Digest xxh;

    std::string md5_res = md5.Do("", 0);
    std::string sha256_res = sha256.Do("", 0);
    std::string xxh_res = xxh.Do("", 0);

    REQUIRE(md5_res == "d41d8cd98f00b204e9800998ecf8427e");
    REQUIRE(sha256_res == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // Use XXH64 directly to avoid version mismatch in hardcoded expected value
    unsigned long long expected_xxh = XXH64("", 0, 0);
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", expected_xxh);
    REQUIRE(xxh_res == std::string(buf));
}

TEST_CASE("Digest known input")
{
    MD5Digest md5;
    SHA256Digest sha256;
    XXHash64Digest xxh;

    const char* data = "hello";
    size_t len = std::strlen(data);

    std::string md5_res = md5.Do(data, len);
    std::string sha256_res = sha256.Do(data, len);
    std::string xxh_res = xxh.Do(data, len);

    REQUIRE(md5_res == "5d41402abc4b2a76b9719d911017c592");
    REQUIRE(sha256_res == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    unsigned long long expected_xxh = XXH64(data, len, 0);
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", expected_xxh);
    REQUIRE(xxh_res == std::string(buf));
}

TEST_CASE("Digest large buffer")
{
    MD5Digest md5;
    SHA256Digest sha256;
    XXHash64Digest xxh;

    std::vector<char> buf(1024 * 1024, 'A');

    std::string md5_res = md5.Do(buf.data(), buf.size());
    std::string sha256_res = sha256.Do(buf.data(), buf.size());
    std::string xxh_res = xxh.Do(buf.data(), buf.size());

    REQUIRE(md5_res.size() == 32);
    REQUIRE(sha256_res.size() == 64);
    REQUIRE(xxh_res.size() == 16);

    // Same input should produce same output
    REQUIRE(md5.Do(buf.data(), buf.size()) == md5_res);
    REQUIRE(sha256.Do(buf.data(), buf.size()) == sha256_res);
    REQUIRE(xxh.Do(buf.data(), buf.size()) == xxh_res);
}

TEST_CASE("Digest cross-verify same input")
{
    MD5Digest md5;
    SHA256Digest sha256;
    XXHash64Digest xxh;

    const char* data = "The quick brown fox jumps over the lazy dog";
    size_t len = std::strlen(data);

    std::string md5_1 = md5.Do(data, len);
    std::string md5_2 = md5.Do(data, len);
    std::string sha_1 = sha256.Do(data, len);
    std::string sha_2 = sha256.Do(data, len);
    std::string xxh_1 = xxh.Do(data, len);
    std::string xxh_2 = xxh.Do(data, len);

    REQUIRE(md5_1 == md5_2);
    REQUIRE(sha_1 == sha_2);
    REQUIRE(xxh_1 == xxh_2);
}

TEST_CASE("Digest different inputs produce different outputs")
{
    XXHash64Digest xxh;

    const char* a = "foo";
    const char* b = "bar";

    REQUIRE(xxh.Do(a, 3) != xxh.Do(b, 3));
}
