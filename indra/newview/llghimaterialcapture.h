/**
 * @file llghimaterialcapture.h
 * @brief Opt-in production material/skin observation for GHI verification.
 */

#ifndef LL_LLGHIMATERIALCAPTURE_H
#define LL_LLGHIMATERIALCAPTURE_H

#include "llsingleton.h"

#include <cstdint>
#include <memory>

class LLDrawInfo;
class LLImageRaw;
class LLViewerFetchedTexture;

namespace LL::GHI
{
struct MaterialScenePacket;
struct MaterialTextureResource;
}

class LLGHIMaterialCapture final : public LLSingleton<LLGHIMaterialCapture>
{
    LLSINGLETON(LLGHIMaterialCapture);
    ~LLGHIMaterialCapture() override;

public:
    static bool active() { return sActive; }

    // Dormant unless VULKANSTORM_GHI_R5_CAPTURE names an output file.
    bool beginFrame(std::uint32_t width, std::uint32_t height,
                    std::uint64_t frame_id);
    void observeDecodedTexture(const LLViewerFetchedTexture& texture,
                               const LLImageRaw& image, std::int32_t discard_level);
    // Shared decoder-path observation used by later integration slices. The
    // caller supplies its own semantic source identity and color space.
    bool copyDecodedTexture(const LLViewerFetchedTexture& texture,
                            LL::GHI::MaterialTextureResource& output) const;
    // Reuse the production material/resource/geometry builder for a separate,
    // sequential observer. The caller owns the surrounding frame lifecycle.
    bool beginPacketAssembly(std::uint32_t width, std::uint32_t height,
                             std::uint64_t frame_id);
    bool recordPacketDraw(LLDrawInfo& draw, std::uint32_t render_type,
                          bool rigged);
    bool endPacketAssembly(LL::GHI::MaterialScenePacket& output,
                           bool& budget_limited);
    void record(LLDrawInfo& draw, std::uint32_t render_type, bool rigged);
    void endFrame();

private:
    static bool sActive;
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

#endif // LL_LLGHIMATERIALCAPTURE_H
