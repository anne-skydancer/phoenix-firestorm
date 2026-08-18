/**
 * @file llghiterraincapture.h
 * @brief Opt-in production terrain observation for Vulkan integration.
 */

#ifndef LL_LLGHITERRAINCAPTURE_H
#define LL_LLGHITERRAINCAPTURE_H

#include "llsingleton.h"

#include <cstdint>
#include <memory>
#include <vector>

class LLFace;
class LLTerrainMaterials;
class LLViewerRegion;
class LLViewerTexture;

class LLGHITerrainCapture final : public LLSingleton<LLGHITerrainCapture>
{
    LLSINGLETON(LLGHITerrainCapture);
    ~LLGHITerrainCapture() override;

public:
    static bool active() { return sActive; }
    bool beginFrame(std::uint32_t width, std::uint32_t height,
                    std::uint64_t frame_id);
    void record(const std::vector<LLFace*>& faces, LLViewerRegion& region,
                LLTerrainMaterials& materials, LLViewerTexture* composition,
                bool pbr, std::uint32_t paint_type,
                std::int32_t pbr_detail_mode, float detail_scale);
    void endFrame();

private:
    static bool sActive;
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

#endif // LL_LLGHITERRAINCAPTURE_H
