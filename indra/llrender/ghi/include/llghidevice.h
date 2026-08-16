/**
 * @file llghidevice.h
 * @brief Backend-neutral GHI device and lifecycle contract.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIDEVICE_H
#define LL_LLGHIDEVICE_H

#include "llghicommand.h"

#include <cstdint>
#include <memory>

namespace LL::GHI
{

struct DeviceCreateInfo
{
    Backend backend = Backend::Validation;
    std::uint32_t adapterIndex = 0;
    std::uint32_t framesInFlight = 2;
    bool enableValidation = false;
};

class Device
{
public:
    virtual ~Device() = default;

    virtual Backend backend() const = 0;
    virtual const DeviceCapabilities& capabilities() const = 0;
    virtual CommandContext& commandContext() = 0;

    virtual BufferHandle createBuffer(const BufferDesc& desc, Status& status) = 0;
    virtual ImageHandle createImage(const ImageDesc& desc, Status& status) = 0;
    virtual SamplerHandle createSampler(const SamplerDesc& desc, Status& status) = 0;
    virtual ShaderPackageHandle createShaderPackage(
        const ShaderPackageDesc& desc,
        Status& status) = 0;
    virtual PipelineHandle createPipeline(const PipelineDesc& desc, Status& status) = 0;

    virtual Status destroy(BufferHandle handle) = 0;
    virtual Status destroy(ImageHandle handle) = 0;
    virtual Status destroy(SamplerHandle handle) = 0;
    virtual Status destroy(ShaderPackageHandle handle) = 0;
    virtual Status destroy(PipelineHandle handle) = 0;
};

struct DeviceCreationResult
{
    std::unique_ptr<Device> device;
    Status status = Status::success();
};

// R0 implements only Backend::Validation. OpenGL and Vulkan factories are
// added by later increments without changing this renderer-facing contract.
DeviceCreationResult createDevice(const DeviceCreateInfo& info);

} // namespace LL::GHI

#endif // LL_LLGHIDEVICE_H
