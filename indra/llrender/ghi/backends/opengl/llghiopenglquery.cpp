/**
 * @file llghiopenglquery.cpp
 * @brief Legacy OpenGL query allocation behind the GHI API boundary.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "ghi/include/llghilegacyquery.h"

#include "llglheaders.h"

#include <limits>
#include <vector>

namespace LL::GHI
{

U32 allocateLegacyQuery()
{
    GLuint query = 0;
    glGenQueries(1, &query);
    return static_cast<U32>(query);
}

void destroyLegacyQueries(std::span<const U32> queries)
{
    if (queries.empty())
    {
        return;
    }

    llassert(queries.size() <= static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()));
    std::vector<GLuint> native_queries(queries.begin(), queries.end());
    glDeleteQueries(static_cast<GLsizei>(native_queries.size()), native_queries.data());
}

} // namespace LL::GHI