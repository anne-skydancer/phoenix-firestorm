/**
 * @file llghidevicebackend.h
 * @brief Private dispatch seam for GHI device implementations.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIDEVICEBACKEND_H
#define LL_LLGHIDEVICEBACKEND_H

#include "ghi/include/llghidevice.h"

namespace LL::GHI
{

DeviceCreationResult createValidationDevice(const DeviceCreateInfo& info);
DeviceCreationResult createOpenGLDevice(const DeviceCreateInfo& info);
DeviceCreationResult createVulkanDevice(const DeviceCreateInfo& info);

} // namespace LL::GHI

#endif // LL_LLGHIDEVICEBACKEND_H
