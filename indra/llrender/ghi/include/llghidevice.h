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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

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
    virtual const RendererCapabilities& capabilities() const = 0;
    virtual CommandContext& commandContext() = 0;

    virtual BufferHandle createBuffer(const BufferDesc& desc, Status& status) = 0;
    virtual ImageHandle createImage(const ImageDesc& desc, Status& status) = 0;
    virtual ImageViewHandle createImageView(const ImageViewDesc& desc, Status& status) = 0;
    virtual SamplerHandle createSampler(const SamplerDesc& desc, Status& status) = 0;
    virtual QueryPoolHandle createQueryPool(const QueryPoolDesc& desc, Status& status) = 0;
    virtual ShaderPackageHandle createShaderPackage(
        const ShaderPackageDesc& desc,
        Status& status) = 0;
    virtual BindingSetHandle createBindingSet(
        const BindingSetDesc& desc,
        Status& status) = 0;
    virtual PipelineHandle createPipeline(const PipelineDesc& desc, Status& status) = 0;

    virtual Status destroy(BufferHandle handle) = 0;
    virtual Status destroy(ImageHandle handle) = 0;
    virtual Status destroy(ImageViewHandle handle) = 0;
    virtual Status destroy(SamplerHandle handle) = 0;
    virtual Status destroy(QueryPoolHandle handle) = 0;
    virtual Status destroy(ShaderPackageHandle handle) = 0;
    virtual Status destroy(BindingSetHandle handle) = 0;
    virtual Status destroy(PipelineHandle handle) = 0;

    // Host access is explicit and restricted by MemoryClass. Upload buffers
    // are writable; readback buffers are readable only after their producing
    // frame has completed. Neither operation hides a device synchronization.
    virtual Status writeBuffer(
        BufferHandle handle,
        std::uint64_t offset,
        std::span<const std::byte> data) = 0;
    virtual Status readBuffer(
        BufferHandle handle,
        std::uint64_t offset,
        std::span<std::byte> data) = 0;
    virtual Status getQueryResults(
        QueryPoolHandle pool,
        std::uint32_t firstQuery,
        std::span<std::uint64_t> results,
        QueryReadMode mode = QueryReadMode::AvailableOnly) = 0;

    // Destroy invalidates handles immediately. Backends retire native objects
    // after the configured in-flight window; waitIdle drains that queue.
    virtual Status waitIdle() = 0;
};

struct DeviceCreationResult
{
    std::unique_ptr<Device> device;
    Status status = Status::success();
};

// Dispatches to a compiled backend without exposing native API types here.
DeviceCreationResult createDevice(const DeviceCreateInfo& info);

} // namespace LL::GHI

#endif // LL_LLGHIDEVICE_H
