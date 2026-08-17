/**
 * @file llghihash.h
 * @brief Small backend-neutral hashing helpers for renderer identities.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIHASH_H
#define LL_LLGHIHASH_H

#include <cstddef>
#include <span>
#include <string>

namespace LL::GHI
{

std::string sha256(std::span<const std::byte> data);

} // namespace LL::GHI

#endif // LL_LLGHIHASH_H
