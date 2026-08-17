/**
 * @file llghipresentation.h
 * @brief Backend-neutral window presentation lifecycle.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIPRESENTATION_H
#define LL_LLGHIPRESENTATION_H

#include "llghirendererinfo.h"

#include <cstdint>
#include <memory>

namespace LL::GHI
{

struct ClearColor
{
    float red = 0.f;
    float green = 0.f;
    float blue = 0.f;
    float alpha = 1.f;
};

// Native handles remain opaque above the backend boundary. On Windows these
// are HWND and HINSTANCE respectively; only the Vulkan backend performs the
// casts and includes platform/Vulkan headers.
struct PresentationCreateInfo
{
    Backend backend = Backend::Validation;
    void* nativeWindow = nullptr;
    void* nativeInstance = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t adapterIndex = 0;
    std::uint32_t framesInFlight = 2;
    bool enableValidation = false;
    bool enableVsync = true;
};

class PresentationSurface
{
public:
    virtual ~PresentationSurface() = default;

    virtual Backend backend() const = 0;
    virtual const RendererSnapshot& rendererSnapshot() const = 0;

    virtual Status presentClear(const ClearColor& color) = 0;
    virtual Status resize(std::uint32_t width, std::uint32_t height) = 0;
    // Notify the backend that the desktop display topology or mode changed.
    // Platform event types remain outside this contract.
    virtual Status displayChanged() = 0;
    virtual Status setSuspended(bool suspended) = 0;
    virtual Status shutdown() = 0;
};

struct PresentationCreationResult
{
    std::unique_ptr<PresentationSurface> surface;
    Status status = Status::success();
};

// R1 initially exposes native Vulkan presentation through this developer-only
// factory. Production selection remains disabled until later parity gates.
PresentationCreationResult createPresentationSurface(const PresentationCreateInfo& info);
bool presentationBackendBuilt(Backend backend);

} // namespace LL::GHI

#endif // LL_LLGHIPRESENTATION_H
