/**
 * @file llghishaderpackage.h
 * @brief Runtime decoder for offline-built GHI shader packages.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHISHADERPACKAGE_H
#define LL_LLGHISHADERPACKAGE_H

#include "ghi/include/llghidescriptors.h"
#include "ghi/include/llghitypes.h"

#include <filesystem>
#include <string_view>

namespace LL::GHI
{

Status decodeShaderPackage(std::string_view encoded, ShaderPackageDesc& package);
Status loadShaderPackage(const std::filesystem::path& path, ShaderPackageDesc& package);

} // namespace LL::GHI

#endif // LL_LLGHISHADERPACKAGE_H
