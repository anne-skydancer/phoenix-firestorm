/**
 * @file llghinestedviewcapture.h
 * @brief Dormant production nested-view observation for P0e4 qualification.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHINESTEDVIEWCAPTURE_H
#define LL_LLGHINESTEDVIEWCAPTURE_H

#include "llsingleton.h"
#include "ghi/include/llghioffscreencontract.h"

#include <cstdint>
#include <memory>

class LLGHINestedViewCapture final : public LLSingleton<LLGHINestedViewCapture>
{
    LLSINGLETON(LLGHINestedViewCapture);
    ~LLGHINestedViewCapture() override;

public:
    void observeCubeView(LL::GHI::RenderViewClass view,
                         std::uint32_t cube_index,
                                LL::GHI::CubeFace face,
                                LL::GHI::ProbePhase phase,
                                                 std::uint64_t frame_id,
                                                 std::uint64_t resource_generation);
    void observeSingleView(LL::GHI::RenderViewClass view,
                                                     std::uint64_t frame_id,
                                                     std::uint64_t resource_generation);

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

#endif // LL_LLGHINESTEDVIEWCAPTURE_H