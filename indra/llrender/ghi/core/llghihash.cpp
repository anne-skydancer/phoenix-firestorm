/**
 * @file llghihash.cpp
 * @brief Small backend-neutral hashing helpers for renderer identities.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "llghihash.h"

#include <openssl/evp.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace LL::GHI
{

std::string sha256(std::span<const std::byte> data)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize = 0;
    const void* bytes = data.empty() ? nullptr : data.data();
    if (EVP_Digest(bytes, data.size(), digest.data(), &digestSize,
                   EVP_sha256(), nullptr) != 1)
    {
        throw std::runtime_error("Unable to calculate SHA-256");
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestSize; ++i)
    {
        output << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    return output.str();
}

} // namespace LL::GHI
