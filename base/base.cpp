#include "base/base.hpp"
#include <new> // std::align_val_t, std::nothrow

char *AllocBytes(size_t align, size_t bytes)
{
    auto *buf = new (std::align_val_t(align), std::nothrow) char[bytes]{0};
    if (buf == nullptr)
    {
        throw AcpException("AllocBytes() failed");
    }
    return buf;
}

