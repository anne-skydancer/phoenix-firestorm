/**
 * @file llghiopenglinfo.h
 * @brief OpenGL renderer identity and semantic capability adapter.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIOPENGLINFO_H
#define LL_LLGHIOPENGLINFO_H

#include "ghi/include/llghirendererinfo.h"

namespace LL::GHI
{

// Requires an initialized legacy OpenGL context. Native GL details remain in
// this backend directory and are translated into the shared snapshot.
RendererSnapshot queryOpenGLRendererSnapshot();

} // namespace LL::GHI

#endif // LL_LLGHIOPENGLINFO_H
