/**
 * @file llghienvironmentcapture.h
 * @brief Opt-in live environment assembly for P0e2 qualification.
 */

#ifndef LL_LLGHIENVIRONMENTCAPTURE_H
#define LL_LLGHIENVIRONMENTCAPTURE_H

#include "llsingleton.h"

#include <cstdint>
#include <memory>
#include <vector>

class LLFace;
class LLRenderTarget;
class LLVector3;
class LLViewerTexture;

class LLGHIEnvironmentCapture final : public LLSingleton<LLGHIEnvironmentCapture>
{
    LLSINGLETON(LLGHIEnvironmentCapture);
    ~LLGHIEnvironmentCapture() override;

public:
    static bool active() { return sActive; }
    bool beginFrame(std::uint32_t width, std::uint32_t height,
                    std::uint64_t frame_id);
    void observeSky(const LLVector3& camera_position, float camera_height,
                    bool hdri);
    void observeWater(const std::vector<LLFace*>& faces,
                      LLViewerTexture* normal_map,
                      LLViewerTexture* next_normal_map,
                      bool normal_mip_filtering);
    void observeReflectionColor(LLRenderTarget& target);
    void observeWaterExclusionMask(LLRenderTarget& target);
    void endFrame();

private:
    static bool sActive;
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

#endif // LL_LLGHIENVIRONMENTCAPTURE_H
