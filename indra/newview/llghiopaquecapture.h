/**
 * @file llghiopaquecapture.h
 * @brief Opt-in production opaque draw packet capture for GHI verification.
 */

#ifndef LL_LLGHIOPAQUECAPTURE_H
#define LL_LLGHIOPAQUECAPTURE_H

#include "llsingleton.h"

#include <cstdint>
#include <memory>

class LLDrawInfo;

class LLGHIOpaqueCapture final : public LLSingleton<LLGHIOpaqueCapture>
{
    LLSINGLETON(LLGHIOpaqueCapture);
    ~LLGHIOpaqueCapture() override;

public:
    // No work is performed unless VULKANSTORM_GHI_R4_CAPTURE names an output file.
    bool beginFrame(std::uint32_t width, std::uint32_t height,
                    std::uint64_t frame_id, bool production_occlusion_enabled);
    void record(const LLDrawInfo& draw, std::uint32_t render_type, bool rigged,
                bool textured);
    void endFrame();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

#endif // LL_LLGHIOPAQUECAPTURE_H
