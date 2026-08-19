/**
 * @file llghimaterialcapture.cpp
 * @brief Live R5 material/skin observer. Visible rendering remains OpenGL.
 */

#include "llviewerprecompiledheaders.h"

#include "llghimaterialcapture.h"
#include "llghiruntime.h"

#include "lldrawpool.h"
#include "llfetchedgltfmaterial.h"
#include "llmaterial.h"
#include "llmodel.h"
#include "llspatialpartition.h"
#include "llviewertexture.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"
#include "llvertexbuffer.h"
#include "llrender.h"
#include "ghi/core/llghihash.h"
#include "ghi/include/llghimaterialscenepacket.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>

#include <glm/gtc/type_ptr.hpp>

using namespace std::chrono_literals;

static_assert(LL::GHI::MATERIAL_MAX_JOINTS == LL_MAX_JOINTS_PER_MESH_OBJECT,
              "GHI skin capacity must track Firestorm's rigged-mesh ceiling");

namespace
{
LL::GHI::ResourceDigest digestFromHex(const std::string& hex)
{
    LL::GHI::ResourceDigest result{};
    if (hex.size() != result.size() * 2) return result;
    auto nibble = [](char value) -> std::uint8_t
    {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return 0;
    };
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::byte>((nibble(hex[i * 2]) << 4) |
                                           nibble(hex[i * 2 + 1]));
    return result;
}

LL::GHI::ResourceDigest digestString(const std::string& text)
{
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return digestFromHex(LL::GHI::sha256({first, text.size()}));
}

std::string digestKey(const LL::GHI::ResourceDigest& digest)
{
    static constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (std::byte value : digest)
    {
        const auto byte = std::to_integer<std::uint8_t>(value);
        result.push_back(HEX[byte >> 4]);
        result.push_back(HEX[byte & 0xf]);
    }
    return result;
}

LL::GHI::MaterialAlphaMode alphaMode(std::uint32_t mode)
{
    if (mode == LLMaterial::DIFFUSE_ALPHA_MODE_MASK ||
        mode == LLGLTFMaterial::ALPHA_MODE_MASK)
        return LL::GHI::MaterialAlphaMode::Mask;
    if (mode == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND ||
        mode == LLGLTFMaterial::ALPHA_MODE_BLEND)
        return LL::GHI::MaterialAlphaMode::Blend;
    return LL::GHI::MaterialAlphaMode::Opaque;
}

bool shadowMaskPass(std::uint32_t type)
{
    switch (type)
    {
    case LLRenderPass::PASS_ALPHA_MASK:
    case LLRenderPass::PASS_ALPHA_MASK_RIGGED:
    case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK:
    case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED:
    case LLRenderPass::PASS_MATERIAL_ALPHA_MASK:
    case LLRenderPass::PASS_MATERIAL_ALPHA_MASK_RIGGED:
    case LLRenderPass::PASS_SPECMAP_MASK:
    case LLRenderPass::PASS_SPECMAP_MASK_RIGGED:
    case LLRenderPass::PASS_NORMMAP_MASK:
    case LLRenderPass::PASS_NORMMAP_MASK_RIGGED:
    case LLRenderPass::PASS_NORMSPEC_MASK:
    case LLRenderPass::PASS_NORMSPEC_MASK_RIGGED:
    case LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK:
    case LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK_RIGGED:
        return true;
    default:
        return false;
    }
}

bool shadowOpaquePass(std::uint32_t type)
{
    switch (type)
    {
    case LLRenderPass::PASS_SIMPLE:
    case LLRenderPass::PASS_SIMPLE_RIGGED:
    case LLRenderPass::PASS_FULLBRIGHT:
    case LLRenderPass::PASS_FULLBRIGHT_RIGGED:
    case LLRenderPass::PASS_SHINY:
    case LLRenderPass::PASS_SHINY_RIGGED:
    case LLRenderPass::PASS_BUMP:
    case LLRenderPass::PASS_BUMP_RIGGED:
    case LLRenderPass::PASS_FULLBRIGHT_SHINY:
    case LLRenderPass::PASS_FULLBRIGHT_SHINY_RIGGED:
    case LLRenderPass::PASS_MATERIAL:
    case LLRenderPass::PASS_MATERIAL_RIGGED:
    case LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE:
    case LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE_RIGGED:
    case LLRenderPass::PASS_SPECMAP:
    case LLRenderPass::PASS_SPECMAP_RIGGED:
    case LLRenderPass::PASS_SPECMAP_EMISSIVE:
    case LLRenderPass::PASS_SPECMAP_EMISSIVE_RIGGED:
    case LLRenderPass::PASS_NORMMAP:
    case LLRenderPass::PASS_NORMMAP_RIGGED:
    case LLRenderPass::PASS_NORMMAP_EMISSIVE:
    case LLRenderPass::PASS_NORMMAP_EMISSIVE_RIGGED:
    case LLRenderPass::PASS_NORMSPEC:
    case LLRenderPass::PASS_NORMSPEC_RIGGED:
    case LLRenderPass::PASS_NORMSPEC_EMISSIVE:
    case LLRenderPass::PASS_NORMSPEC_EMISSIVE_RIGGED:
    case LLRenderPass::PASS_GLTF_PBR:
    case LLRenderPass::PASS_GLTF_PBR_RIGGED:
        return true;
    default:
        return false;
    }
}

} // namespace

class LLGHIMaterialCapture::Impl
{
public:
    enum class State { Disabled, Warming, Recording, Complete, Failed };
    enum class GeometryReject : std::size_t
    {
        None,
        InvalidPrimitive,
        MissingAttributes,
        MissingWeights,
        MissingCpuData,
        InvalidRange,
        IndexStride,
        Budget,
        NonFiniteWeights,
        ZeroWeights,
        IndexRange,
        ClipVolume,
        Count
    };
    struct ObservedTexture
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t components = 0;
        std::uint32_t discardLevel = 0;
        LL::GHI::ResourceDigest contentIdentity{};
        std::vector<std::byte> pixels;
        std::uint64_t serial = 0;
    };

    void configure()
    {
        if (mConfigured) return;
        mConfigured = true;
        const char* output = std::getenv("VULKANSTORM_GHI_R5_CAPTURE");
        if (!output || !*output) return;
        mOutput = std::filesystem::path(output);
        mWarmup = 120s;
        if (const char* value = std::getenv("VULKANSTORM_GHI_R5_WARMUP_SECONDS"))
        {
            char* end = nullptr;
            const double seconds = std::strtod(value, &end);
            if (end != value && seconds >= 0.0 && seconds <= 3600.0)
                mWarmup = std::chrono::milliseconds(
                    static_cast<std::int64_t>(seconds * 1000.0));
        }
        mState = State::Warming;
        mWarmupStart = std::chrono::steady_clock::now();
        LL_INFOS("GHI") << "R5 material/skin capture armed; warmup="
                         << mWarmup.count() << "ms output=" << mOutput.string()
                         << LL_ENDL;
    }

    bool begin(std::uint32_t width, std::uint32_t height,
               std::uint64_t frameId)
    {
        configure();
        if (mState == State::Warming &&
            std::chrono::steady_clock::now() - mWarmupStart >= mWarmup)
            mState = State::Recording;
        mCaptureFile = mState == State::Recording;
        mCaptureRuntime = LLGHIRuntime::shouldCaptureLiveMaterialPacket(frameId);
        mCaptureShadowRuntime = mCaptureRuntime &&
            LLGHIRuntime::shadowOffscreenRequested();
        if (!mCaptureFile && !mCaptureRuntime) return false;
        mPacket = {};
        mPacket.frameId = frameId;
        mPacket.sceneEpoch = ++mSceneEpoch;
        mPacket.sourceWidth = width;
        mPacket.sourceHeight = height;
        mTextureIndices.clear();
        mMaterialIndices.clear();
        mSkinIndices.clear();
        mRuntimeBudgetLimited = false;
        mRuntimeRigidDraws = 0;
        mRuntimeRiggedDraws = 0;
        mRuntimeLightingReceiverDraws = 0;
        mRuntimeShadowOpaqueRigidDraws = 0;
        mRuntimeRiggedCalls = 0;
        mRuntimeRiggedOpaqueCalls = 0;
        mRuntimeRiggedMaskCalls = 0;
        mRuntimeRiggedMissingSkin = 0;
        mRuntimeRiggedGeometryRejected = 0;
        mRuntimeRiggedRejectReasons.fill(0);
        mRuntimeTextureBytes = 0;
        mInFrame = true;
        return true;
    }

    void observe(const LLViewerFetchedTexture& texture, const LLImageRaw& image,
                 std::int32_t discardLevel)
    {
        configure();
        // Command-line/session settings are loaded before the viewer preloads
        // system textures, while the native Vulkan coexistence device is
        // intentionally created later. Key observation to the opt-in setting
        // rather than device lifetime so early resources such as the terrain
        // alpha ramp are not lost before I6 begins.
        const bool materialConfigured =
            gSavedSettings.getBOOL("RenderVulkanMaterialOffscreenProbe");
        const bool lightingConfigured =
            gSavedSettings.getBOOL("RenderVulkanLightingOffscreenProbe");
        const bool terrainConfigured =
            gSavedSettings.getBOOL("RenderVulkanTerrainOffscreenProbe");
        const bool terrainLightingConfigured =
            gSavedSettings.getBOOL("RenderVulkanTerrainLightingOffscreenProbe");
        const bool projectorLightingConfigured =
            gSavedSettings.getBOOL("RenderVulkanProjectorLightingOffscreenProbe");
        const bool shadowConfigured =
            gSavedSettings.getBOOL("RenderVulkanShadowOffscreenProbe");
        const bool frameAssemblyConfigured =
            gSavedSettings.getBOOL("RenderVulkanFrameAssemblyProbe") ||
            gSavedSettings.getBOOL("RenderVulkanTextureResidencyProbe") ||
            gSavedSettings.getBOOL("RenderVulkanFrameGraphProbe") ||
            gSavedSettings.getBOOL("RenderVulkanGBufferExecutionProbe") ||
            gSavedSettings.getBOOL("RenderVulkanLightingExecutionProbe") ||
            LLGHIRuntime::productionFrameCaptureRequested();
        const char* environmentOutput =
            std::getenv("VULKANSTORM_GHI_P0E2_CAPTURE");
        const bool environmentConfigured =
            environmentOutput && *environmentOutput;
        if ((mState == State::Disabled &&
             !materialConfigured && !lightingConfigured &&
             !terrainConfigured && !terrainLightingConfigured &&
             !projectorLightingConfigured && !shadowConfigured &&
             !frameAssemblyConfigured && !environmentConfigured) ||
            mState == State::Complete || mState == State::Failed ||
            !image.getData() || image.getDataSize() <= 0)
            return;
        const std::uint32_t sourceWidth = llmax(0, image.getWidth());
        const std::uint32_t sourceHeight = llmax(0, image.getHeight());
        const std::uint32_t components = llmax(0, image.getComponents());
        if (!sourceWidth || !sourceHeight || !components || components > 4)
            return;
        // Viewer texture assets are capped at 2048x2048. The verification
        // packet does not need their full 16 MiB RGBA representation: retain
        // at most a 64 KiB sample (normally 128x128 RGBA) from the normal
        // decoder path. The same reduced pixels are supplied to every peer
        // backend, while the bounded observation window remains large enough
        // for login-time avatar and region texture churn.
        constexpr std::size_t MAX_RUNTIME_IMAGE_BYTES = 64ull * 1024ull;
        const bool runtimeObservation =
            (materialConfigured || lightingConfigured || terrainConfigured ||
             terrainLightingConfigured || projectorLightingConfigured ||
             shadowConfigured || frameAssemblyConfigured ||
             environmentConfigured) &&
            mState == State::Disabled;
        std::uint32_t observedWidth = sourceWidth;
        std::uint32_t observedHeight = sourceHeight;
        std::uint32_t extraDiscard = 0;
        auto observedBytes = [&]() -> std::uint64_t
        {
            return static_cast<std::uint64_t>(observedWidth) *
                   observedHeight * components;
        };
        while (runtimeObservation && observedBytes() > MAX_RUNTIME_IMAGE_BYTES &&
               (observedWidth > 1 || observedHeight > 1))
        {
            observedWidth = std::max(1u, observedWidth / 2);
            observedHeight = std::max(1u, observedHeight / 2);
            ++extraDiscard;
        }
        const std::uint64_t targetBytes64 = observedBytes();
        if (!targetBytes64 || targetBytes64 > MAX_OBSERVED_TEXTURE_BYTES ||
            targetBytes64 > std::numeric_limits<std::size_t>::max())
            return;
        const std::size_t size = static_cast<std::size_t>(targetBytes64);
        const std::uint32_t effectiveDiscard =
            static_cast<std::uint32_t>(llmax(0, discardLevel)) + extraDiscard;
        const std::string key = texture.getID().asString();
        auto existing = mObservedTextures.find(key);
        if (existing != mObservedTextures.end() &&
            existing->second.discardLevel <= effectiveDiscard)
        {
            existing->second.serial = ++mObservationSerial;
            return;
        }
        const std::size_t previous = existing == mObservedTextures.end()
            ? 0 : existing->second.pixels.size();
        while (mObservedTextureBytes - previous + size >
               MAX_OBSERVED_TEXTURE_BYTES)
        {
            auto victim = mObservedTextures.end();
            for (auto candidate = mObservedTextures.begin();
                 candidate != mObservedTextures.end(); ++candidate)
            {
                if (candidate->first == key) continue;
                if (victim == mObservedTextures.end() ||
                    candidate->second.serial < victim->second.serial)
                    victim = candidate;
            }
            if (victim == mObservedTextures.end()) return;
            mObservedTextureBytes -= victim->second.pixels.size();
            mObservedTextures.erase(victim);
        }
        LLImageDataSharedLock lock(&image);
        ObservedTexture observed;
        observed.width = observedWidth;
        observed.height = observedHeight;
        observed.components = components;
        observed.discardLevel = effectiveDiscard;
        const auto* bytes = reinterpret_cast<const std::byte*>(image.getData());
        if (observedWidth == sourceWidth && observedHeight == sourceHeight)
        {
            if (size > static_cast<std::size_t>(image.getDataSize())) return;
            observed.pixels.assign(bytes, bytes + size);
        }
        else
        {
            observed.pixels.resize(size);
            for (std::uint32_t y = 0; y < observedHeight; ++y)
            {
                const std::uint32_t sourceY = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(y) * sourceHeight / observedHeight);
                for (std::uint32_t x = 0; x < observedWidth; ++x)
                {
                    const std::uint32_t sourceX = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(x) * sourceWidth / observedWidth);
                    const std::size_t sourceOffset =
                        (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) *
                        components;
                    const std::size_t targetOffset =
                        (static_cast<std::size_t>(y) * observedWidth + x) * components;
                    std::copy_n(bytes + sourceOffset, components,
                                observed.pixels.begin() +
                                    static_cast<std::ptrdiff_t>(targetOffset));
                }
            }
        }
        observed.contentIdentity =
            digestFromHex(LL::GHI::sha256(observed.pixels));
        observed.serial = ++mObservationSerial;
        mObservedTextureBytes = mObservedTextureBytes - previous + size;
        mObservedTextures[key] = std::move(observed);
    }

    bool copyObservation(const LLViewerFetchedTexture& texture,
                         LL::GHI::MaterialTextureResource& output) const
    {
        const auto found = mObservedTextures.find(texture.getID().asString());
        if (found == mObservedTextures.end()) return false;
        output.width = found->second.width;
        output.height = found->second.height;
        output.components = found->second.components;
        output.discardLevel = found->second.discardLevel;
        output.contentIdentity = found->second.contentIdentity;
        output.decodedPixels = found->second.pixels;
        return true;
    }

    void record(LLDrawInfo& draw, std::uint32_t renderType, bool rigged)
    {
        if (!mInFrame) return;
        const bool needsSkin = rigged || draw.mSkinInfo || draw.mAvatar;
        if (mCaptureRuntime && rigged)
        {
            ++mRuntimeRiggedCalls;
            if (renderType == LLRenderPass::PASS_GLTF_PBR_RIGGED)
                ++mRuntimeRiggedOpaqueCalls;
            else if (renderType == LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK_RIGGED)
                ++mRuntimeRiggedMaskCalls;
        }
        const bool opaquePbrPass = renderType == LLRenderPass::PASS_GLTF_PBR ||
            renderType == LLRenderPass::PASS_GLTF_PBR_RIGGED;
        const bool opaqueLegacyPass = !draw.mGLTFMaterial &&
            shadowOpaquePass(renderType) &&
            alphaMode(draw.mDiffuseAlphaMode) ==
                LL::GHI::MaterialAlphaMode::Opaque;
        const bool shadowPass = mCaptureShadowRuntime &&
            (shadowOpaquePass(renderType) || shadowMaskPass(renderType));
        const bool lightingReceiver = opaqueLegacyPass ||
            (opaquePbrPass && draw.mGLTFMaterial.notNull() &&
             alphaMode(draw.mGLTFMaterial->mAlphaMode) ==
                 LL::GHI::MaterialAlphaMode::Opaque);
        const bool runtimeGeometry = lightingReceiver || shadowPass;
        if (mCaptureRuntime && !mCaptureFile && !runtimeGeometry) return;

        // A live I5 sample is useful only when its production skin path is
        // executable. Render traversal normally reaches rigid PBR first; if
        // those resources consume the bounded texture packet, merely sorting
        // rigged draws in the consumer is too late. Let the first rigged draw
        // preempt the provisional rigid-only sample so its geometry, palette,
        // and texture set receive first claim on the same fixed budgets.
        if (mCaptureRuntime && !mCaptureFile && !mCaptureShadowRuntime &&
            rigged && lightingReceiver &&
            !mRuntimeRiggedDraws && mRuntimeRigidDraws)
        {
            mPacket.vertices.clear();
            mPacket.indices.clear();
            mPacket.draws.clear();
            mPacket.textures.clear();
            mPacket.materials.clear();
            mPacket.skins.clear();
            mTextureIndices.clear();
            mMaterialIndices.clear();
            mSkinIndices.clear();
            mRuntimeRigidDraws = 0;
            mRuntimeLightingReceiverDraws = 0;
            mRuntimeShadowOpaqueRigidDraws = 0;
            mRuntimeTextureBytes = 0;
            mRuntimeBudgetLimited = false;
        }
        LL::GHI::MaterialSceneDraw output;
        output.semanticId = 0x5235620000000000ull |
            static_cast<std::uint64_t>(mPacket.draws.size() & 0xffffffffull);
        if (needsSkin)
            output.skin = skin(draw);
        const LL::GHI::SkinResource* capturedSkin =
            output.skin == LL::GHI::NO_RESOURCE
                ? nullptr : &mPacket.skins[output.skin];
        const bool missingSkin = needsSkin && !capturedSkin;
        if (mCaptureRuntime && rigged && missingSkin)
            ++mRuntimeRiggedMissingSkin;
        GeometryReject geometryReject = GeometryReject::None;
        const bool shadowOnly = mCaptureShadowRuntime && !mCaptureFile &&
            shadowPass && !lightingReceiver;
        const bool geometryCaptured = !runtimeGeometry ||
            (!missingSkin && captureGeometry(
                draw, output, capturedSkin, shadowMaskPass(renderType),
                shadowOnly, geometryReject));
        if (mCaptureRuntime && rigged && runtimeGeometry && !geometryCaptured)
        {
            ++mRuntimeRiggedGeometryRejected;
            ++mRuntimeRiggedRejectReasons[static_cast<std::size_t>(geometryReject)];
        }
        if (runtimeGeometry && !geometryCaptured)
        {
            // Runtime packets contain executable geometry only. Archival R5
            // capture keeps the semantic-only records below.
            if (!mCaptureFile) return;
            output.comparability = output.comparability |
                LL::GHI::ResourceComparability::UnsupportedVertexLayout;
        }
        if (mCaptureRuntime && geometryCaptured && lightingReceiver)
            ++mRuntimeLightingReceiverDraws;
        output.material = material(draw, renderType, capturedSkin != nullptr,
                                   shadowOnly,
                                   shadowMaskPass(renderType));
        if (output.material != LL::GHI::NO_RESOURCE)
            output.comparability = output.comparability |
                mPacket.materials[output.material].comparability;
        if (output.skin != LL::GHI::NO_RESOURCE)
            output.comparability = output.comparability |
                mPacket.skins[output.skin].comparability;
        else if (needsSkin)
            output.comparability = output.comparability |
                LL::GHI::ResourceComparability::MissingSkinPalette;
        mPacket.draws.push_back(output);
    }

    void end()
    {
        if (!mInFrame) return;
        mInFrame = false;
        const bool captureFile = mCaptureFile;
        const bool captureRuntime = mCaptureRuntime;
        mCaptureFile = false;
        mCaptureRuntime = false;
        mCaptureShadowRuntime = false;
        const auto logRuntimeRouting = [this]()
        {
            std::ostringstream reasons;
            for (std::size_t reason = 1;
                 reason < static_cast<std::size_t>(GeometryReject::Count);
                 ++reason)
            {
                if (reason != 1) reasons << '/';
                reasons << mRuntimeRiggedRejectReasons[reason];
            }
            LL_INFOS("GHIIntegration")
                << "I5 capture routing diagnostic: rigged-calls="
                << mRuntimeRiggedCalls << " opaque-rigged-calls="
                << mRuntimeRiggedOpaqueCalls << " mask-rigged-calls="
                << mRuntimeRiggedMaskCalls << " missing-skin="
                << mRuntimeRiggedMissingSkin << " geometry-rejected="
                << mRuntimeRiggedGeometryRejected
                << " reject-reasons(primitive/attributes/weights/cpu/range/stride/budget/nonfinite/zero-weight/index/clip)="
                << reasons.str() << LL_ENDL;
        };
        if (mPacket.draws.empty())
        {
            if (captureRuntime)
            {
                logRuntimeRouting();
                LLGHIRuntime::consumeLiveMaterialPacket(
                    mPacket, mRuntimeBudgetLimited);
            }
            return;
        }

        canonicalizeResources();

        // An epoch changes only when the exact resource set changes, not merely
        // because a frame was observed.
        std::ostringstream signature;
        for (const auto& texture : mPacket.textures)
            signature << digestKey(texture.sourceIdentity)
                      << digestKey(texture.contentIdentity) << ':'
                      << texture.width << ':' << texture.height << ':'
                      << texture.components << ':' << texture.discardLevel << ':'
                      << static_cast<std::uint32_t>(texture.colorSpace) << ':'
                      << static_cast<std::uint32_t>(texture.comparability) << ';';
        for (const auto& material : mPacket.materials)
            signature << digestKey(material.identity) << ':'
                      << static_cast<std::uint32_t>(material.comparability) << ';';
        for (const auto& skin : mPacket.skins)
            signature << digestKey(skin.identity) << ':'
                      << static_cast<std::uint32_t>(skin.comparability) << ';';
        const std::string resourceSignature =
            digestKey(digestString(signature.str()));
        if (resourceSignature != mLastResourceSignature)
        {
            ++mResourceEpoch;
            mLastResourceSignature = resourceSignature;
        }
        mPacket.resourceEpoch = mResourceEpoch;

        if (captureRuntime)
        {
            if (!mRuntimeRiggedDraws)
                logRuntimeRouting();
            LLGHIRuntime::consumeLiveMaterialPacket(mPacket, mRuntimeBudgetLimited);
        }
        if (!captureFile) return;

        std::vector<std::byte> bytes;
        LL::GHI::Status status = LL::GHI::encodeMaterialScenePacket(mPacket, bytes);
        if (!status) { fail(status.message()); return; }
        std::error_code error;
        if (mOutput.has_parent_path())
            std::filesystem::create_directories(mOutput.parent_path(), error);
        std::ofstream stream(mOutput, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream) { fail("could not write capture file"); return; }
        mState = State::Complete;
        std::size_t comparable = 0;
        for (const auto& draw : mPacket.draws)
            comparable += draw.comparability ==
                LL::GHI::ResourceComparability::Comparable;
        LL_INFOS("GHI") << "R5 material/skin capture complete: frame="
                         << mPacket.frameId << " draws=" << mPacket.draws.size()
                         << " comparable=" << comparable << " textures="
                         << mPacket.textures.size() << " materials="
                         << mPacket.materials.size() << " skins="
                         << mPacket.skins.size() << " resource_epoch="
                         << mPacket.resourceEpoch << " bytes=" << bytes.size()
                         << " sha256=" << LL::GHI::sha256(bytes) << LL_ENDL;
    }

private:
    bool captureGeometry(const LLDrawInfo& source,
                         LL::GHI::MaterialSceneDraw& output,
                         const LL::GHI::SkinResource* skinResource,
                         bool alphaMasked,
                         bool shadowOnly,
                         GeometryReject& reject)
    {
        const LLVertexBuffer* buffer = source.mVertexBuffer.get();
        std::uint32_t required = LLVertexBuffer::MAP_VERTEX;
        if (mCaptureShadowRuntime)
        {
            if (alphaMasked) required |= LLVertexBuffer::MAP_TEXCOORD0;
        }
        else
        {
            const bool lit = !source.mFullbright;
            const bool hasTexture = source.mTexture.notNull() ||
                source.mNormalMap.notNull() || source.mSpecularMap.notNull() ||
                source.mGLTFMaterial.notNull();
            const bool hasNormalTexture = source.mNormalMap.notNull() ||
                (source.mGLTFMaterial.notNull() &&
                 source.mGLTFMaterial->mNormalTexture.notNull());
            if (lit) required |= LLVertexBuffer::MAP_NORMAL;
            if (hasTexture) required |= LLVertexBuffer::MAP_TEXCOORD0;
            if (lit && hasNormalTexture) required |= LLVertexBuffer::MAP_TANGENT;
        }
        const bool skinned = skinResource != nullptr;
        reject = GeometryReject::None;
        if (!buffer || !source.mCount || source.mCount % 3 != 0)
        { reject = GeometryReject::InvalidPrimitive; return false; }
        if ((buffer->getTypeMask() & required) != required)
        { reject = GeometryReject::MissingAttributes; return false; }
        if (skinned && !buffer->hasDataType(LLVertexBuffer::TYPE_WEIGHT4))
        { reject = GeometryReject::MissingWeights; return false; }
        if (!buffer->getMappedData() || !buffer->getMappedIndices())
        { reject = GeometryReject::MissingCpuData; return false; }
        if (source.mStart > source.mEnd || source.mEnd >= buffer->getNumVerts() ||
            source.mOffset > buffer->getNumIndices() ||
            source.mCount > buffer->getNumIndices() - source.mOffset)
        { reject = GeometryReject::InvalidRange; return false; }
        if (buffer->getIndexStride() != 2 && buffer->getIndexStride() != 4)
        { reject = GeometryReject::IndexStride; return false; }

        const std::uint8_t* rawIndices = buffer->getMappedIndices();
        const std::size_t vertexRange = source.mEnd - source.mStart + 1;
        std::vector<bool> referenced(vertexRange, false);
        for (std::uint32_t item = 0; item < source.mCount; ++item)
        {
            const std::size_t sourceIndex = source.mOffset + item;
            const std::uint32_t value = buffer->getIndexStride() == 2
                ? reinterpret_cast<const std::uint16_t*>(rawIndices)[sourceIndex]
                : reinterpret_cast<const std::uint32_t*>(rawIndices)[sourceIndex];
            if (value < source.mStart || value > source.mEnd)
            {
                reject = GeometryReject::IndexRange;
                return false;
            }
            referenced[value - source.mStart] = true;
        }

        const std::size_t vertexCount = static_cast<std::size_t>(
            std::count(referenced.begin(), referenced.end(), true));
        if (!vertexCount)
        {
            reject = GeometryReject::InvalidPrimitive;
            return false;
        }

        const std::size_t maxDraws = mCaptureShadowRuntime ? 64 : 32;
        const std::size_t maxRigidDraws = mCaptureShadowRuntime ? 48 : 24;
        constexpr std::size_t maxVertices = 65536;
        constexpr std::size_t maxIndices = 196608;
        constexpr std::size_t receiverDrawReserve = 4;
        constexpr std::size_t receiverVertexReserve = 8192;
        constexpr std::size_t receiverIndexReserve = 32768;
        constexpr std::size_t maxShadowOpaqueRigidDraws = 16;
        const std::size_t runtimeDraws =
            mRuntimeRigidDraws + mRuntimeRiggedDraws;
        const bool reserveReceiver = shadowOnly &&
            !mRuntimeLightingReceiverDraws;
        const std::size_t effectiveMaxDraws = maxDraws -
            (reserveReceiver ? receiverDrawReserve : 0);
        const std::size_t effectiveMaxRigidDraws = maxRigidDraws -
            (reserveReceiver ? receiverDrawReserve : 0);
        const std::size_t effectiveMaxVertices = maxVertices -
            (reserveReceiver ? receiverVertexReserve : 0);
        const std::size_t effectiveMaxIndices = maxIndices -
            (reserveReceiver ? receiverIndexReserve : 0);
        if (mCaptureRuntime &&
            (runtimeDraws >= effectiveMaxDraws ||
             (!skinned && mRuntimeRigidDraws >= effectiveMaxRigidDraws) ||
             (shadowOnly && !skinned && !alphaMasked &&
              mRuntimeShadowOpaqueRigidDraws >= maxShadowOpaqueRigidDraws) ||
             mPacket.vertices.size() > effectiveMaxVertices ||
             vertexCount > effectiveMaxVertices - mPacket.vertices.size() ||
             mPacket.indices.size() > effectiveMaxIndices ||
             source.mCount > effectiveMaxIndices - mPacket.indices.size()))
        {
            mRuntimeBudgetLimited = true;
            reject = GeometryReject::Budget;
            return false;
        }

        const std::uint32_t baseVertex =
            static_cast<std::uint32_t>(mPacket.vertices.size());
        const auto* data = buffer->getMappedData();
        const auto* positions = reinterpret_cast<const float*>(
            data + buffer->getOffset(LLVertexBuffer::TYPE_VERTEX));
        const auto* normals = buffer->hasDataType(LLVertexBuffer::TYPE_NORMAL)
            ? reinterpret_cast<const float*>(
                data + buffer->getOffset(LLVertexBuffer::TYPE_NORMAL)) : nullptr;
        const auto* tangents = buffer->hasDataType(LLVertexBuffer::TYPE_TANGENT)
            ? reinterpret_cast<const float*>(
                data + buffer->getOffset(LLVertexBuffer::TYPE_TANGENT)) : nullptr;
        const auto* texcoords = buffer->hasDataType(LLVertexBuffer::TYPE_TEXCOORD0)
            ? reinterpret_cast<const float*>(
                data + buffer->getOffset(LLVertexBuffer::TYPE_TEXCOORD0)) : nullptr;
        const LLColor4U* colors = buffer->hasDataType(LLVertexBuffer::TYPE_COLOR)
            ? reinterpret_cast<const LLColor4U*>(
                data + buffer->getOffset(LLVertexBuffer::TYPE_COLOR)) : nullptr;
        const float* packedWeights = skinned
            ? reinterpret_cast<const float*>(
                data + buffer->getOffset(LLVertexBuffer::TYPE_WEIGHT4)) : nullptr;
        const std::uint16_t* separateJoints = skinned &&
            buffer->hasDataType(LLVertexBuffer::TYPE_JOINT)
                ? reinterpret_cast<const std::uint16_t*>(
                    data + buffer->getOffset(LLVertexBuffer::TYPE_JOINT)) : nullptr;
        std::vector<std::uint32_t> vertexMap(
            vertexRange, std::numeric_limits<std::uint32_t>::max());
        mPacket.vertices.reserve(mPacket.vertices.size() + vertexCount);
        for (std::size_t localIndex = 0; localIndex < vertexRange; ++localIndex)
        {
            if (!referenced[localIndex]) continue;
            const std::uint32_t index = source.mStart +
                static_cast<std::uint32_t>(localIndex);
            vertexMap[localIndex] =
                static_cast<std::uint32_t>(mPacket.vertices.size());
            LL::GHI::MaterialSceneVertex vertex;
            std::copy_n(positions + static_cast<std::size_t>(index) * 4, 3,
                        vertex.position.begin());
            if (normals)
                std::copy_n(normals + static_cast<std::size_t>(index) * 4, 3,
                            vertex.normal.begin());
            if (tangents)
                std::copy_n(tangents + static_cast<std::size_t>(index) * 4, 4,
                            vertex.tangent.begin());
            if (texcoords)
                std::copy_n(texcoords + static_cast<std::size_t>(index) * 2, 2,
                            vertex.texCoord.begin());
            if (colors) std::copy_n(colors[index].mV, 4, vertex.color.begin());
            if (skinned)
            {
                float weightSum = 0.f;
                for (std::size_t influence = 0; influence < 4; ++influence)
                {
                    const float packed = packedWeights[
                        static_cast<std::size_t>(index) * 4 + influence];
                    if (!std::isfinite(packed))
                    {
                        mPacket.vertices.resize(baseVertex);
                        reject = GeometryReject::NonFiniteWeights;
                        return false;
                    }
                    if (separateJoints)
                    {
                        vertex.joints[influence] = separateJoints[
                            static_cast<std::size_t>(index) * 4 + influence];
                        vertex.weights[influence] = llmax(0.f, packed);
                    }
                    else
                    {
                        const float integral = std::floor(packed);
                        const auto joint = static_cast<std::uint32_t>(
                            llclamp(integral, 0.f,
                                static_cast<float>(skinResource->jointCount - 1)));
                        vertex.joints[influence] =
                            static_cast<std::uint16_t>(joint);
                        vertex.weights[influence] = packed - integral;
                    }
                    weightSum += vertex.weights[influence];
                }
                if (!std::isfinite(weightSum) || weightSum <= 1.e-6f)
                {
                    mPacket.vertices.resize(baseVertex);
                    reject = GeometryReject::ZeroWeights;
                    return false;
                }
            }
            mPacket.vertices.push_back(vertex);
        }

        const std::uint32_t firstIndex =
            static_cast<std::uint32_t>(mPacket.indices.size());
        mPacket.indices.reserve(mPacket.indices.size() + source.mCount);
        for (std::uint32_t item = 0; item < source.mCount; ++item)
        {
            const std::size_t sourceIndex = source.mOffset + item;
            const std::uint32_t value = buffer->getIndexStride() == 2
                ? reinterpret_cast<const std::uint16_t*>(rawIndices)[sourceIndex]
                : reinterpret_cast<const std::uint32_t*>(rawIndices)[sourceIndex];
            mPacket.indices.push_back(vertexMap[value - source.mStart]);
        }
        output.firstIndex = firstIndex;
        output.indexCount = source.mCount;
        const glm::mat4 transform = glm::make_mat4(gGLProjection) *
                                    glm::make_mat4(gGLModelView);
        glm::mat4 model{1.f};
        if (source.mModelMatrix)
            model = glm::make_mat4(&source.mModelMatrix->mMatrix[0][0]);

        // Runtime packets have a deliberately small geometry/texture budget.
        // Do not let main-view draw-map entries that are trivially outside the
        // clip volume consume that budget ahead of executable visible draws.
        // Archival captures retain the full post-cull observation set.
        if (mCaptureRuntime && !mCaptureFile)
        {
            const glm::mat4 clipTransform = transform * model;
            std::array<bool, 6> allOutside{{true, true, true, true, true, true}};
            for (std::uint32_t item = firstIndex;
                 item < firstIndex + source.mCount; ++item)
            {
                const auto& vertex = mPacket.vertices[mPacket.indices[item]];
                glm::vec4 local(vertex.position[0], vertex.position[1],
                                vertex.position[2], 1.f);
                if (skinResource)
                {
                    float weightSum = 0.f;
                    for (float weight : vertex.weights)
                        weightSum += llmax(0.f, weight);
                    glm::vec3 skinnedPosition(0.f);
                    for (std::size_t influence = 0; influence < 4; ++influence)
                    {
                        const float weight = llmax(0.f, vertex.weights[influence]) /
                                             llmax(weightSum, 1.e-6f);
                        const std::uint32_t joint = llmin<std::uint32_t>(
                            vertex.joints[influence], skinResource->jointCount - 1);
                        const float* matrix =
                            skinResource->matrixPalette.data() + joint * 12;
                        skinnedPosition.x += weight *
                            (matrix[0] * local.x + matrix[4] * local.y +
                             matrix[8] * local.z + matrix[3]);
                        skinnedPosition.y += weight *
                            (matrix[1] * local.x + matrix[5] * local.y +
                             matrix[9] * local.z + matrix[7]);
                        skinnedPosition.z += weight *
                            (matrix[2] * local.x + matrix[6] * local.y +
                             matrix[10] * local.z + matrix[11]);
                    }
                    local = glm::vec4(skinnedPosition, 1.f);
                }
                const glm::vec4 clip = clipTransform * local;
                if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
                    !std::isfinite(clip.z) || !std::isfinite(clip.w))
                    continue;
                const std::array<bool, 6> inside{{
                    clip.x >= -clip.w, clip.x <= clip.w,
                    clip.y >= -clip.w, clip.y <= clip.w,
                    clip.z >= -clip.w, clip.z <= clip.w}};
                for (std::size_t plane = 0; plane < allOutside.size(); ++plane)
                    allOutside[plane] = allOutside[plane] && !inside[plane];
            }
            if (std::any_of(allOutside.begin(), allOutside.end(),
                            [](bool outside) { return outside; }))
            {
                mPacket.vertices.resize(baseVertex);
                mPacket.indices.resize(firstIndex);
                reject = GeometryReject::ClipVolume;
                return false;
            }
        }
        std::copy_n(glm::value_ptr(transform), 16, output.transform.begin());
        std::copy_n(glm::value_ptr(model), 16, output.modelTransform.begin());
        if (mCaptureRuntime)
        {
            if (skinned) ++mRuntimeRiggedDraws;
            else ++mRuntimeRigidDraws;
            if (shadowOnly && !skinned && !alphaMasked)
                ++mRuntimeShadowOpaqueRigidDraws;
        }
        return true;
    }

    template<typename Resource>
    static std::vector<std::uint32_t> canonicalize(std::vector<Resource>& resources)
    {
        std::vector<std::uint32_t> order(resources.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](std::uint32_t lhs, std::uint32_t rhs)
        {
            return resources[lhs].identity < resources[rhs].identity;
        });
        std::vector<std::uint32_t> oldToNew(resources.size());
        std::vector<Resource> sorted;
        sorted.reserve(resources.size());
        for (std::uint32_t next = 0; next < order.size(); ++next)
        {
            oldToNew[order[next]] = next;
            sorted.push_back(std::move(resources[order[next]]));
        }
        resources = std::move(sorted);
        return oldToNew;
    }

    void canonicalizeResources()
    {
        // Textures have a distinct source identity; material and skin identity
        // already includes their exact dynamic content.
        std::vector<std::uint32_t> textureOrder(mPacket.textures.size());
        std::iota(textureOrder.begin(), textureOrder.end(), 0u);
        std::sort(textureOrder.begin(), textureOrder.end(), [&](std::uint32_t lhs,
                                                                std::uint32_t rhs)
        {
            return mPacket.textures[lhs].sourceIdentity <
                   mPacket.textures[rhs].sourceIdentity;
        });
        std::vector<std::uint32_t> textureRemap(mPacket.textures.size());
        std::vector<LL::GHI::MaterialTextureResource> sortedTextures;
        sortedTextures.reserve(mPacket.textures.size());
        for (std::uint32_t next = 0; next < textureOrder.size(); ++next)
        {
            textureRemap[textureOrder[next]] = next;
            sortedTextures.push_back(std::move(mPacket.textures[textureOrder[next]]));
        }
        mPacket.textures = std::move(sortedTextures);
        for (auto& material : mPacket.materials)
            for (auto& binding : material.textures)
                binding.texture = textureRemap[binding.texture];

        const auto materialRemap = canonicalize(mPacket.materials);
        const auto skinRemap = canonicalize(mPacket.skins);
        for (auto& draw : mPacket.draws)
        {
            if (draw.material != LL::GHI::NO_RESOURCE)
                draw.material = materialRemap[draw.material];
            if (draw.skin != LL::GHI::NO_RESOURCE)
                draw.skin = skinRemap[draw.skin];
        }
    }

    std::uint32_t texture(LLViewerTexture* source, LL::GHI::TextureSemantic semantic,
                          LL::GHI::TextureColorSpace colorSpace,
                          bool priority)
    {
        if (!source) return LL::GHI::NO_RESOURCE;
        const std::string sourceKey = source->getID().asString() + ":" +
            std::to_string(static_cast<std::uint32_t>(semantic)) + ":" +
            std::to_string(static_cast<std::uint32_t>(colorSpace));
        const auto sourceDigest = digestString(sourceKey);
        const std::string key = digestKey(sourceDigest);
        if (auto found = mTextureIndices.find(key); found != mTextureIndices.end())
            return found->second;

        LL::GHI::MaterialTextureResource resource;
        resource.sourceIdentity = sourceDigest;
        resource.colorSpace = colorSpace;
        resource.width = llmax(0, source->getFullWidth());
        resource.height = llmax(0, source->getFullHeight());
        resource.components = llmax<S32>(0, source->getComponents());
        resource.discardLevel = llmax<S32>(0, source->getDiscardLevel());
        auto* fetched = dynamic_cast<LLViewerFetchedTexture*>(source);
        const std::string assetKey = source->getID().asString();
        const auto runtimePixelsAllowed = [&](std::size_t bytes)
        {
            constexpr std::size_t maxImage = 4ull * 1024ull * 1024ull;
            constexpr std::size_t maxPacket = 16ull * 1024ull * 1024ull;
            return !mCaptureRuntime ||
                (bytes <= maxImage && mRuntimeTextureBytes <= maxPacket &&
                 bytes <= maxPacket - mRuntimeTextureBytes);
        };
        const auto useObserved = [&]()
        {
            auto observed = mObservedTextures.find(assetKey);
            if (observed == mObservedTextures.end() ||
                !runtimePixelsAllowed(observed->second.pixels.size()))
                return false;
            if (priority)
                observed->second.serial = ++mObservationSerial;
            resource.width = observed->second.width;
            resource.height = observed->second.height;
            resource.components = observed->second.components;
            resource.discardLevel = observed->second.discardLevel;
            resource.decodedPixels = observed->second.pixels;
            resource.contentIdentity = observed->second.contentIdentity;
            if (mCaptureRuntime)
                mRuntimeTextureBytes += resource.decodedPixels.size();
            return true;
        };
        useObserved();
        LLPointer<LLImageRaw> saved = fetched ? fetched->getSavedRawImage() : nullptr;
        if (resource.decodedPixels.empty() && saved.notNull())
        {
            // Saved/raw decoder buffers are transient. Retain the bounded
            // backend-neutral observation before the viewer releases them so
            // a teleport can replay the same material without a refetch or an
            // OpenGL texture readback.
            if (mCaptureRuntime)
            {
                observe(*fetched, *saved, fetched->getSavedRawImageLevel());
                useObserved();
            }
        }
        if (resource.decodedPixels.empty() && saved.notNull())
        {
            LLImageDataSharedLock lock(saved);
            const std::size_t size = static_cast<std::size_t>(
                llmax(0, saved->getDataSize()));
            if (saved->getData() && size && runtimePixelsAllowed(size))
            {
                resource.width = saved->getWidth();
                resource.height = saved->getHeight();
                resource.components = saved->getComponents();
                resource.discardLevel = llmax(0, fetched->getSavedRawImageLevel());
                const auto* bytes =
                    reinterpret_cast<const std::byte*>(saved->getData());
                resource.decodedPixels.assign(bytes, bytes + size);
                resource.contentIdentity =
                    digestFromHex(LL::GHI::sha256(resource.decodedPixels));
                if (mCaptureRuntime)
                    mRuntimeTextureBytes += resource.decodedPixels.size();
            }
        }
        LLPointer<LLImageRaw> raw = fetched ? fetched->getRawImage() : nullptr;
        if (resource.decodedPixels.empty() && fetched &&
            fetched->isRawImageValid() && raw.notNull())
        {
            if (mCaptureRuntime)
            {
                observe(*fetched, *raw, fetched->getRawImageLevel());
                useObserved();
            }
        }
        if (resource.decodedPixels.empty() && fetched &&
            fetched->isRawImageValid() && raw.notNull())
        {
            LLImageDataSharedLock lock(raw);
            const std::size_t size = static_cast<std::size_t>(llmax(0, raw->getDataSize()));
            if (raw->getData() && size && runtimePixelsAllowed(size))
            {
                resource.width = raw->getWidth(); resource.height = raw->getHeight();
                resource.components = raw->getComponents();
                resource.discardLevel = llmax(0, fetched->getRawImageLevel());
                const auto* bytes = reinterpret_cast<const std::byte*>(raw->getData());
                resource.decodedPixels.assign(bytes, bytes + size);
                resource.contentIdentity =
                    digestFromHex(LL::GHI::sha256(resource.decodedPixels));
                if (mCaptureRuntime)
                    mRuntimeTextureBytes += resource.decodedPixels.size();
            }
        }
        if (resource.decodedPixels.empty())
        {
            resource.comparability = LL::GHI::ResourceComparability::MissingCpuTexture;
            if (fetched && fetched->isFetching())
                resource.comparability = resource.comparability |
                    LL::GHI::ResourceComparability::TextureStillFetching;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(mPacket.textures.size());
        mPacket.textures.push_back(std::move(resource));
        mTextureIndices.emplace(key, index);
        return index;
    }

    void bindTexture(LL::GHI::MaterialResource& resource, LLViewerTexture* source,
                     LL::GHI::TextureSemantic semantic,
                     LL::GHI::TextureColorSpace colorSpace,
                     bool priority,
                     std::array<float, 5> transform = {{0.f, 0.f, 1.f, 1.f, 0.f}})
    {
        // The selected viewer renderer has already retired this declared
        // asset to its semantic fallback. Preserve that executable state in
        // the backend-neutral packet by omitting the binding; the consumer
        // supplies the glTF fallback for the semantic. A valid, merely
        // in-flight texture still remains non-comparable until decoded.
        if (source && source->isMissingAsset()) return;
        const std::uint32_t index = texture(source, semantic, colorSpace, priority);
        if (index == LL::GHI::NO_RESOURCE) return;
        resource.textures.push_back({semantic, index, 0, transform});
        resource.comparability = resource.comparability |
            mPacket.textures[index].comparability;
    }

    std::uint32_t material(LLDrawInfo& draw, std::uint32_t renderType,
                           bool priority, bool shadowOnly,
                           bool alphaMasked)
    {
        LL::GHI::MaterialResource resource;
        std::ostringstream identity;
        identity.precision(9);
        identity << draw.mMaterialID.asString() << ':' << renderType << ':'
                 << draw.mSpecColor.mV[0] << ':' << draw.mSpecColor.mV[1] << ':'
                 << draw.mSpecColor.mV[2] << ':' << draw.mSpecColor.mV[3] << ':'
                 << draw.mEnvIntensity << ':' << draw.mAlphaMaskCutoff << ':'
                 << draw.mFullbright;

        if (draw.mGLTFMaterial)
        {
            const LLFetchedGLTFMaterial& material = *draw.mGLTFMaterial;
            identity << ':' << material.getHash().asString();
            resource.model = LL::GHI::MaterialModel::MetallicRoughness;
            resource.alphaMode = alphaMode(material.mAlphaMode);
            std::copy_n(material.mBaseColor.mV, 4, resource.baseColor.begin());
            std::copy_n(material.mEmissiveColor.mV, 3, resource.emissive.begin());
            resource.metallic = material.mMetallicFactor;
            resource.roughness = material.mRoughnessFactor;
            resource.alphaCutoff = material.mAlphaCutoff;
            resource.doubleSided = material.mDoubleSided;
            LLViewerTexture* textures[] = {
                material.mBaseColorTexture.get(), material.mNormalTexture.get(),
                material.mMetallicRoughnessTexture.get(), material.mEmissiveTexture.get()};
            const LL::GHI::TextureSemantic semantics[] = {
                LL::GHI::TextureSemantic::BaseColor, LL::GHI::TextureSemantic::Normal,
                LL::GHI::TextureSemantic::MetallicRoughness,
                LL::GHI::TextureSemantic::Emissive};
            const std::size_t textureCount = shadowOnly
                ? (alphaMasked ? 1u : 0u) : 4u;
            for (std::size_t i = 0; i < textureCount; ++i)
            {
                const auto& t = material.mTextureTransform[i];
                bindTexture(resource, textures[i], semantics[i],
                    i == 0 || i == 3 ? LL::GHI::TextureColorSpace::SRGB
                                     : LL::GHI::TextureColorSpace::Linear,
                    priority,
                    {{t.mOffset.mV[0], t.mOffset.mV[1], t.mScale.mV[0],
                      t.mScale.mV[1], t.mRotation}});
            }
        }
        else
        {
            resource.model = LL::GHI::MaterialModel::Legacy;
            resource.alphaMode = alphaMode(draw.mDiffuseAlphaMode);
            std::copy_n(draw.mSpecColor.mV, 4, resource.legacySpecular.begin());
            resource.environmentIntensity = draw.mEnvIntensity;
            resource.alphaCutoff = draw.mAlphaMaskCutoff;
            resource.fullbright = draw.mFullbright;
            if (!shadowOnly || alphaMasked)
                bindTexture(resource, draw.mTexture.get(),
                            LL::GHI::TextureSemantic::BaseColor,
                            LL::GHI::TextureColorSpace::SRGB, priority);
            if (!shadowOnly)
            {
                bindTexture(resource, draw.mNormalMap.get(),
                            LL::GHI::TextureSemantic::Normal,
                            LL::GHI::TextureColorSpace::Linear, priority);
                bindTexture(resource, draw.mSpecularMap.get(),
                            LL::GHI::TextureSemantic::LegacySpecular,
                            LL::GHI::TextureColorSpace::SRGB, priority);
            }
        }
        if (resource.alphaMode == LL::GHI::MaterialAlphaMode::Blend)
            resource.comparability = resource.comparability |
                LL::GHI::ResourceComparability::AlphaDeferred;
        for (const auto& binding : resource.textures)
        {
            identity << ':' << static_cast<std::uint32_t>(binding.semantic) << ':'
                     << digestKey(mPacket.textures[binding.texture].sourceIdentity);
            for (float value : binding.transform) identity << ':' << value;
        }
        resource.identity = digestString(identity.str());
        const std::string key = digestKey(resource.identity);
        if (auto found = mMaterialIndices.find(key); found != mMaterialIndices.end())
            return found->second;
        const std::uint32_t index = static_cast<std::uint32_t>(mPacket.materials.size());
        mPacket.materials.push_back(std::move(resource));
        mMaterialIndices.emplace(key, index);
        return index;
    }

    std::uint32_t skin(LLDrawInfo& draw)
    {
        if (!draw.mSkinInfo || !draw.mAvatar) return LL::GHI::NO_RESOURCE;
        const auto& palette = draw.mAvatar->updateSkinInfoMatrixPalette(draw.mSkinInfo);
        if (palette.mGLMp.empty() || palette.mGLMp.size() % 12 != 0 ||
            palette.mGLMp.size() / 12 > LL::GHI::MATERIAL_MAX_JOINTS)
            return LL::GHI::NO_RESOURCE;
        std::ostringstream source;
        source << draw.mSkinInfo->mMeshID.asString() << ':' << draw.mSkinInfo->mHash;
        if (!palette.mGLMp.empty())
        {
            const auto* first = reinterpret_cast<const std::byte*>(palette.mGLMp.data());
            source << ':' << LL::GHI::sha256(
                {first, palette.mGLMp.size() * sizeof(float)});
        }
        LL::GHI::SkinResource resource;
        resource.identity = digestString(source.str());
        const std::string key = digestKey(resource.identity);
        if (auto found = mSkinIndices.find(key); found != mSkinIndices.end())
            return found->second;
        resource.jointCount = static_cast<std::uint32_t>(palette.mGLMp.size() / 12);
        resource.matrixPalette = palette.mGLMp;
        const std::uint32_t index = static_cast<std::uint32_t>(mPacket.skins.size());
        mPacket.skins.push_back(std::move(resource));
        mSkinIndices.emplace(key, index);
        return index;
    }

    void fail(const std::string& message)
    {
        mState = State::Failed;
        LL_WARNS("GHI") << "R5 material/skin capture failed: " << message << LL_ENDL;
    }

    bool mConfigured = false;
    bool mInFrame = false;
    bool mCaptureFile = false;
    bool mCaptureRuntime = false;
    bool mCaptureShadowRuntime = false;
    bool mRuntimeBudgetLimited = false;
    std::size_t mRuntimeRigidDraws = 0;
    std::size_t mRuntimeRiggedDraws = 0;
    std::size_t mRuntimeLightingReceiverDraws = 0;
    std::size_t mRuntimeShadowOpaqueRigidDraws = 0;
    std::size_t mRuntimeRiggedCalls = 0;
    std::size_t mRuntimeRiggedOpaqueCalls = 0;
    std::size_t mRuntimeRiggedMaskCalls = 0;
    std::size_t mRuntimeRiggedMissingSkin = 0;
    std::size_t mRuntimeRiggedGeometryRejected = 0;
    std::array<std::size_t, static_cast<std::size_t>(GeometryReject::Count)>
        mRuntimeRiggedRejectReasons{};
    std::size_t mRuntimeTextureBytes = 0;
    State mState = State::Disabled;
    std::filesystem::path mOutput;
    std::chrono::milliseconds mWarmup{0};
    std::chrono::steady_clock::time_point mWarmupStart{};
    std::uint64_t mSceneEpoch = 0;
    std::uint64_t mResourceEpoch = 0;
    std::string mLastResourceSignature;
    std::map<std::string, std::uint32_t> mTextureIndices;
    std::map<std::string, std::uint32_t> mMaterialIndices;
    std::map<std::string, std::uint32_t> mSkinIndices;
    // Development-probe retention only. Runtime observations are already
    // reduced from the viewer's 2048x2048 source ceiling to at most 64 KiB per
    // decoded image; this window retains roughly 4096 RGBA observations for
    // production avatar and region texture churn
    // without reading pixels back from the selected OpenGL provider.
    static constexpr std::size_t MAX_OBSERVED_TEXTURE_BYTES = 256ull * 1024ull * 1024ull;
    std::size_t mObservedTextureBytes = 0;
    std::uint64_t mObservationSerial = 0;
    std::map<std::string, ObservedTexture> mObservedTextures;
    LL::GHI::MaterialScenePacket mPacket;
};

LLGHIMaterialCapture::LLGHIMaterialCapture() : mImpl(std::make_unique<Impl>()) {}
LLGHIMaterialCapture::~LLGHIMaterialCapture() = default;
bool LLGHIMaterialCapture::sActive = false;
bool LLGHIMaterialCapture::beginFrame(std::uint32_t width,
                                      std::uint32_t height,
                                      std::uint64_t frame_id)
{
    sActive = mImpl->begin(width, height, frame_id);
    return sActive;
}
void LLGHIMaterialCapture::observeDecodedTexture(
    const LLViewerFetchedTexture& texture, const LLImageRaw& image,
    std::int32_t discard_level)
{
    mImpl->observe(texture, image, discard_level);
}
bool LLGHIMaterialCapture::copyDecodedTexture(
    const LLViewerFetchedTexture& texture,
    LL::GHI::MaterialTextureResource& output) const
{
    return mImpl->copyObservation(texture, output);
}
void LLGHIMaterialCapture::record(LLDrawInfo& draw,
                                  std::uint32_t render_type, bool rigged)
{
    mImpl->record(draw, render_type, rigged);
}
void LLGHIMaterialCapture::endFrame()
{
    mImpl->end();
    sActive = false;
}
