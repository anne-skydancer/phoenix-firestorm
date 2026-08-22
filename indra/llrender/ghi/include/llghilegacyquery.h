/**
 * @file llghilegacyquery.h
 * @brief Legacy query allocation behind the GHI API boundary.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHILEGACYQUERY_H
#define LL_LLGHILEGACYQUERY_H

#include "stdtypes.h"

#include <span>

namespace LL::GHI
{

U32 allocateLegacyQuery();
void destroyLegacyQueries(std::span<const U32> queries);

} // namespace LL::GHI

#endif // LL_LLGHILEGACYQUERY_H