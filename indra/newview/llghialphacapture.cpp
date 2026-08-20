/**
 * @file llghialphacapture.cpp
 * @brief Live P0e3 alpha observer; visible rendering remains OpenGL.
 */

#include "llviewerprecompiledheaders.h"

#include "llghialphacapture.h"
#include "llghimaterialcapture.h"

#include "lldrawpool.h"
#include "llviewercontrol.h"
#include "pipeline.h"
#include "ghi/core/llghihash.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace std::chrono_literals;

class LLGHIAlphaCapture::Impl
{
public:
    enum class State { Disabled, Warming, Recording, Complete, Failed };

    void configure()
    {
        if (mConfigured) return;
        mConfigured = true;
        const char* output = std::getenv("VULKANSTORM_GHI_P0E3_CAPTURE");
        if (!output || !*output) return;
        mOutput = std::filesystem::path(output);
        mWarmup = 120s;
        if (const char* value =
                std::getenv("VULKANSTORM_GHI_P0E3_WARMUP_SECONDS"))
        {
            char* end = nullptr;
            const double seconds = std::strtod(value, &end);
            if (end != value && seconds >= 0.0 && seconds <= 3600.0)
                mWarmup = std::chrono::milliseconds(
                    static_cast<std::int64_t>(seconds * 1000.0));
        }
        mState = State::Warming;
        mWarmupStart = std::chrono::steady_clock::now();
        LL_INFOS("GHI") << "P0e3 alpha capture armed; warmup="
                         << mWarmup.count() << "ms output=" << mOutput.string()
                         << LL_ENDL;
    }

    bool begin(std::uint32_t width, std::uint32_t height,
               std::uint64_t frameId, LL::GHI::AlphaViewPhase phase)
    {
        configure();
        if (mState == State::Warming &&
            std::chrono::steady_clock::now() - mWarmupStart >= mWarmup)
            mState = State::Recording;
        if (mState != State::Recording ||
            phase != LL::GHI::AlphaViewPhase::MainPostWater)
            return false;
        if (!LLGHIMaterialCapture::instance().beginPacketAssembly(
                width, height, frameId))
            return false;

        mPacket = {};
        mPacket.frameId = frameId;
        mPacket.sourceWidth = width;
        mPacket.sourceHeight = height;
        mPacket.phase = phase;
        const std::uint32_t method = gSavedSettings.getU32("RenderAlphaSortMethod");
        if (method <= static_cast<std::uint32_t>(LL::GHI::AlphaMethod::DepthPeeling))
            mPacket.requestedMethod = static_cast<LL::GHI::AlphaMethod>(method);
        mPacket.ppllPolicy = {
            std::clamp(gSavedSettings.getU32("RenderAlphaOITNodesPerPixel"), 1u, 32u),
            std::clamp(gSavedSettings.getU32("RenderAlphaOITMemoryMB"), 32u, 2048u),
            std::clamp(gSavedSettings.getU32("RenderAlphaOITMaxPixelLayers"), 4u, 32u)};
        mPacket.depthPeelPolicy = LL::GHI::clampAlphaDepthPeelPolicy({
            gSavedSettings.getU32("RenderAlphaDepthPeelLayers"),
            gSavedSettings.getU32("RenderAlphaDepthPeelTimeBudgetMS")});
        mInFrame = true;
        return true;
    }

    void setTransientLoad(bool transientLoad)
    {
        if (mInFrame) mPacket.transientLoad = transientLoad;
    }

    void recordMasks()
    {
        if (!mInFrame) return;
        static constexpr std::array<std::pair<std::uint32_t, bool>, 15>
            MASK_PASSES{{
                {LLRenderPass::PASS_ALPHA_MASK, false},
                {LLRenderPass::PASS_ALPHA_MASK_RIGGED, true},
                {LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK, false},
                {LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED, true},
                {LLRenderPass::PASS_MATERIAL_ALPHA_MASK, false},
                {LLRenderPass::PASS_MATERIAL_ALPHA_MASK_RIGGED, true},
                {LLRenderPass::PASS_SPECMAP_MASK, false},
                {LLRenderPass::PASS_SPECMAP_MASK_RIGGED, true},
                {LLRenderPass::PASS_NORMMAP_MASK, false},
                {LLRenderPass::PASS_NORMMAP_MASK_RIGGED, true},
                {LLRenderPass::PASS_NORMSPEC_MASK, false},
                {LLRenderPass::PASS_NORMSPEC_MASK_RIGGED, true},
                {LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK, false},
                {LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK_RIGGED, true},
                {LLRenderPass::PASS_GRASS, false}}};
        for (const auto& [renderType, rigged] : MASK_PASSES)
        {
            auto* current = gPipeline.beginRenderMap(renderType);
            auto* end = gPipeline.endRenderMap(renderType);
            while (current != end)
            {
                LLDrawInfo& draw = **current;
                LLCullResult::increment_iterator(current, end);
                LL::GHI::AlphaSceneDraw policy;
                policy.classification = LL::GHI::AlphaSubmissionClass::Mask;
                policy.rigged = rigged;
                policy.fullbright = draw.mFullbright;
                policy.emissive = draw.mVertexBuffer->hasDataType(
                    LLVertexBuffer::TYPE_EMISSIVE);
                policy.minimumAlpha = draw.mAlphaMaskCutoff;
                record(draw, renderType, rigged, policy);
            }
        }
    }

    void record(LLDrawInfo& draw, std::uint32_t renderType, bool rigged,
                const LL::GHI::AlphaSceneDraw& policy)
    {
        if (!mInFrame) return;
        if (LLGHIMaterialCapture::instance().recordPacketDraw(
                draw, renderType, rigged))
            mPacket.draws.push_back(policy);
    }

    void end()
    {
        if (!mInFrame) return;
        mInFrame = false;
        bool budgetLimited = false;
        if (!LLGHIMaterialCapture::instance().endPacketAssembly(
                mPacket.materials, budgetLimited))
            return;
        mPacket.sceneEpoch = mPacket.materials.sceneEpoch;
        mPacket.resourceEpoch = mPacket.materials.resourceEpoch;

        std::vector<std::byte> encoded;
        const LL::GHI::Status status =
            LL::GHI::encodeAlphaScenePacket(mPacket, encoded);
        if (!status)
        {
            fail(status.message());
            return;
        }
        std::error_code error;
        if (mOutput.has_parent_path())
            std::filesystem::create_directories(mOutput.parent_path(), error);
        std::ofstream stream(mOutput, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(encoded.data()),
                     static_cast<std::streamsize>(encoded.size()));
        stream.close();
        if (!stream)
        {
            fail("could not write alpha capture file");
            return;
        }
        mState = State::Complete;

        std::array<std::size_t, 4> classes{};
        std::size_t rigged = 0;
        std::size_t fullbright = 0;
        std::size_t emissive = 0;
        for (const auto& draw : mPacket.draws)
        {
            ++classes[static_cast<std::size_t>(draw.classification)];
            rigged += draw.rigged;
            fullbright += draw.fullbright;
            emissive += draw.emissive;
        }
        LL_INFOS("GHI")
            << "P0e3 alpha capture complete: frame=" << mPacket.frameId
            << " draws=" << mPacket.draws.size()
            << " classes(mask/standard/custom/particle)="
            << classes[0] << '/' << classes[1] << '/' << classes[2] << '/'
            << classes[3] << " rigged=" << rigged
            << " fullbright=" << fullbright << " emissive=" << emissive
            << " vertices=" << mPacket.materials.vertices.size()
            << " indices=" << mPacket.materials.indices.size()
            << " budget-limited=" << budgetLimited
            << " bytes=" << encoded.size()
            << " sha256=" << LL::GHI::sha256(encoded) << LL_ENDL;
    }

private:
    void fail(const std::string& message)
    {
        mState = State::Failed;
        LL_WARNS("GHI") << "P0e3 alpha capture failed: " << message << LL_ENDL;
    }

    bool mConfigured = false;
    bool mInFrame = false;
    State mState = State::Disabled;
    std::filesystem::path mOutput;
    std::chrono::milliseconds mWarmup{0};
    std::chrono::steady_clock::time_point mWarmupStart{};
    LL::GHI::AlphaScenePacket mPacket;
};

LLGHIAlphaCapture::LLGHIAlphaCapture() : mImpl(std::make_unique<Impl>()) {}
LLGHIAlphaCapture::~LLGHIAlphaCapture() = default;
bool LLGHIAlphaCapture::sActive = false;

bool LLGHIAlphaCapture::beginFrame(std::uint32_t width, std::uint32_t height,
                                   std::uint64_t frame_id,
                                   LL::GHI::AlphaViewPhase phase)
{
    sActive = mImpl->begin(width, height, frame_id, phase);
    return sActive;
}

void LLGHIAlphaCapture::recordMasks()
{
    mImpl->recordMasks();
}

void LLGHIAlphaCapture::setTransientLoad(bool transient_load)
{
    mImpl->setTransientLoad(transient_load);
}

void LLGHIAlphaCapture::record(LLDrawInfo& draw, std::uint32_t render_type,
                               bool rigged,
                               const LL::GHI::AlphaSceneDraw& policy)
{
    mImpl->record(draw, render_type, rigged, policy);
}

void LLGHIAlphaCapture::endFrame()
{
    mImpl->end();
    sActive = false;
}