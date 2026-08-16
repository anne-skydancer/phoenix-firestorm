/**
 * @file llghipipelinecache.h
 * @brief Backend-neutral native pipeline-cache identity contract.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIPIPELINECACHE_H
#define LL_LLGHIPIPELINECACHE_H

#include "ghi/include/llghidevice.h"

#include <string>

namespace LL::GHI
{

std::string pipelineCacheIdentity(
    const ShaderPackageDesc& shaderPackage,
    const PipelineDesc& pipeline,
    ShaderPackageDesc::TargetProfile target,
    Backend backend,
    const PipelineCacheDomain& domain);

} // namespace LL::GHI

#endif // LL_LLGHIPIPELINECACHE_H
