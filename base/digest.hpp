#pragma once
#include <string>
#include <cstddef> // size_t
#include <cstdio>
#include <array>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <xxhash.h>

// OpenSSL compatibility: use EVP_MD_CTX_new/free on 1.1.0+; fall back to
// EVP_MD_CTX_create/destroy on 1.0.2.
#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define ACP_EVP_MD_CTX_NEW() EVP_MD_CTX_create()
#define ACP_EVP_MD_CTX_FREE(ctx) EVP_MD_CTX_destroy(ctx)
#else
#define ACP_EVP_MD_CTX_NEW() EVP_MD_CTX_new()
#define ACP_EVP_MD_CTX_FREE(ctx) EVP_MD_CTX_free(ctx)
#endif

class Digest
{
public:
    virtual ~Digest() = default;
    virtual std::string Do(const void *data, size_t len) = 0;
};
class MD5Digest : public Digest
{
public:
    std::string Do(const void *data, size_t len) override
    {
        EVP_MD_CTX *ctx = ACP_EVP_MD_CTX_NEW();
        if (!ctx)
        {
            return {};
        }

        std::array<unsigned char, MD5_DIGEST_LENGTH> md5_result{};
        unsigned int out_len = 0;

        const unsigned char *udata = static_cast<const unsigned char *>(data);
        if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1 ||
            EVP_DigestUpdate(ctx, udata, len) != 1 ||
            EVP_DigestFinal_ex(ctx, md5_result.data(), &out_len) != 1)
        {
            ACP_EVP_MD_CTX_FREE(ctx);
            return {};
        }

        ACP_EVP_MD_CTX_FREE(ctx);

        char buf[MD5_DIGEST_LENGTH * 2 + 1];
        for (unsigned int i = 0; i < out_len; ++i)
        {
            std::snprintf(&buf[i * 2], 3, "%02x", md5_result[i]);
        }
        buf[out_len * 2] = '\0';
        return std::string(buf);
    }
};

class SHA256Digest : public Digest
{
public:
    std::string Do(const void *data, size_t len) override
    {
        unsigned char sha256_result[SHA256_DIGEST_LENGTH];
        SHA256(static_cast<const unsigned char *>(data), len, sha256_result);
        char buf[SHA256_DIGEST_LENGTH * 2 + 1];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        {
            sprintf(&buf[i * 2], "%02x", sha256_result[i]);
        }
        return std::string(buf);
    }
};
class XXHash64Digest : public Digest
{
public:
    std::string Do(const void *data, size_t len) override
    {
        unsigned long long hash = XXH64(data, len, 0);
        char buf[17];
        sprintf(buf, "%016llx", hash);
        return std::string(buf);
    }
};