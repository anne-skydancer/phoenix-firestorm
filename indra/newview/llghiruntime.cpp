/**
 * @file llghiruntime.cpp
 * @brief Developer-gated native Vulkan coexistence lifetime.
 */

#include "llviewerprecompiledheaders.h"

#include "llghiruntime.h"

#include "lldir.h"
#include "llviewercontrol.h"
#include "ghi/core/llghishaderpackage.h"
#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"
#include "ghi/include/llghiopaqueoffscreenprobe.h"
#include "ghi/include/llghiopaquepacketconsumer.h"
#include "ghi/include/llghimaterialoffscreenprobe.h"
#include "ghi/include/llghiproductionframeconsumer.h"
#include "ghi/include/llghiproductiongbufferexecutor.h"
#include "ghi/include/llghiproductionlightingexecutor.h"
#include "ghi/include/llghiproductionframetargets.h"
#include "ghi/include/llghiproductiontextureresidency.h"
#include "ghi/include/llghiterrainscenepacket.h"
#include "ghi/include/llghiterrainoffscreenprobe.h"
#include "ghi/include/llghilightingpacketconsumer.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace
{
std::unique_ptr<LL::GHI::Device> sVulkanDevice;
std::unique_ptr<LL::GHI::OpaqueOffscreenProbe> sOffscreenProbe;
std::unique_ptr<LL::GHI::MaterialOffscreenProbe> sMaterialProbe;
std::unique_ptr<LL::GHI::TerrainOffscreenProbe> sTerrainProbe;
std::unique_ptr<LL::GHI::ProductionTextureResidency> sTextureResidency;
std::unique_ptr<LL::GHI::ProductionFrameTargets> sFrameTargets;
std::unique_ptr<LL::GHI::ProductionGBufferExecutor> sGBufferExecutor;
std::unique_ptr<LL::GHI::ProductionLightingExecutor> sProductionLightingExecutor;
std::optional<LL::GHI::ProductionTextureResidencyResult>
    sPendingProductionResidency;
std::optional<std::string> sPendingProductionGBufferHash;
bool sPendingProductionBudgetLimited = false;
std::uint64_t sNextLivePacketFrame = 0;
std::uint32_t sLivePacketAttempts = 0;
std::uint32_t sLivePacketSamples = 0;
bool sLivePacketCaptureClaimed = false;
bool sLivePacketDisabled = false;
bool sPendingBudgetLimited = false;
std::uint64_t sNextMaterialFrame = 0;
std::uint32_t sMaterialAttempts = 0;
std::uint32_t sMaterialSamples = 0;
bool sMaterialCaptureClaimed = false;
bool sMaterialDisabled = false;
bool sPendingMaterialBudgetLimited = false;
std::optional<LL::GHI::OpaqueScenePacket> sPendingFrameOpaque;
std::optional<LL::GHI::MaterialScenePacket> sPendingLightingMaterial;
std::optional<LL::GHI::TerrainScenePacket> sPendingLightingTerrain;
std::uint64_t sNextFrameAssemblyFrame = 0;
std::uint64_t sFrameAssemblyEpoch = 0;
std::uint32_t sFrameAssemblyAttempts = 0;
std::uint32_t sFrameAssemblySamples = 0;
bool sFrameAssemblyDisabled = false;
bool sPendingFrameOpaqueBudgetLimited = false;
bool sPendingFrameMaterialBudgetLimited = false;
bool sPendingFrameTerrainBudgetLimited = false;
std::uint64_t sNextTerrainFrame = 0;
std::uint32_t sTerrainAttempts = 0;
std::uint32_t sTerrainSamples = 0;
bool sTerrainCaptureClaimed = false;
bool sTerrainDisabled = false;
bool sPendingTerrainBudgetLimited = false;
std::uint64_t sNextLightingFrame = 0;
std::uint32_t sLightingAttempts = 0;
std::uint32_t sLightingSamples = 0;
bool sLightingCaptureClaimed = false;
bool sLightingDisabled = false;

bool shadowOffscreenRequestedInternal();

bool productionLightingExecutionRequested()
{
    return gSavedSettings.getBOOL("RenderVulkanLightingExecutionProbe");
}

bool gBufferExecutionRequested()
{
    return gSavedSettings.getBOOL("RenderVulkanGBufferExecutionProbe") ||
           productionLightingExecutionRequested();
}

bool frameGraphRequested()
{
    return gSavedSettings.getBOOL("RenderVulkanFrameGraphProbe") ||
           gBufferExecutionRequested();
}

bool textureResidencyRequested()
{
    return gSavedSettings.getBOOL("RenderVulkanTextureResidencyProbe") ||
           frameGraphRequested();
}

bool frameAssemblyRequested()
{
    return gSavedSettings.getBOOL("RenderVulkanFrameAssemblyProbe") ||
           textureResidencyRequested();
}

bool projectorLightingOffscreenRequested()
{
    return gSavedSettings.getBOOL(
        "RenderVulkanProjectorLightingOffscreenProbe") ||
        shadowOffscreenRequestedInternal();
}

bool shadowOffscreenRequestedInternal()
{
    return gSavedSettings.getBOOL("RenderVulkanShadowOffscreenProbe");
}

bool lightingOffscreenRequested()
{
    return gSavedSettings.getBOOL("RenderVulkanLightingOffscreenProbe") ||
           projectorLightingOffscreenRequested() ||
           shadowOffscreenRequestedInternal();
}

bool terrainLightingOffscreenRequested()
{
    return gSavedSettings.getBOOL("RenderVulkanTerrainLightingOffscreenProbe") &&
           !lightingOffscreenRequested();
}

bool offscreenProbeRequested()
{
    return gSavedSettings.getBOOL("RenderVulkanOffscreenPacketProbe");
}

bool livePacketRequested()
{
    return gSavedSettings.getBOOL("RenderVulkanLivePacketProbe") ||
           offscreenProbeRequested() || frameAssemblyRequested();
}

std::uint32_t livePacketInterval()
{
    return std::max(1u,
        gSavedSettings.getU32("RenderVulkanLivePacketIntervalFrames"));
}

std::uint32_t livePacketMaximum()
{
    return gSavedSettings.getU32("RenderVulkanLivePacketMaxSamples");
}

void disableLivePacketProbe(const LL::GHI::Status& status,
                            const char* operation)
{
    LL_WARNS("GHIIntegration")
        << "I2 live opaque offscreen " << operation << " failed: "
        << status.message() << LL_ENDL;
    if (status.code() == LL::GHI::StatusCode::DeviceLost)
    {
        sLivePacketDisabled = true;
        LL_WARNS("GHIIntegration")
            << "Live Vulkan packet probes disabled after device loss. "
            << "The production OpenGL renderer remains active."
            << LL_ENDL;
    }
}

void pollOffscreenProbe()
{
    if (!sOffscreenProbe || !sOffscreenProbe->pending()) return;
    LL::GHI::OpaqueOffscreenProbeResult result;
    const LL::GHI::Status status = sOffscreenProbe->poll(result);
    if (!status)
    {
        if (status.code() != LL::GHI::StatusCode::NotReady)
        {
            disableLivePacketProbe(status, "poll");
            sLivePacketDisabled = true;
        }
        return;
    }
    ++sLivePacketSamples;
    LL_INFOS("GHIIntegration")
        << "I2 live opaque offscreen PASS: sample=" << sLivePacketSamples << '/'
        << livePacketMaximum() << " frame=" << result.frameId
        << " draws=" << result.draws << " vertices=" << result.vertices
        << " indices=" << result.indices << " packet-sha256="
        << result.packetSha256 << " color-sha256="
        << result.colorSha256[0] << ',' << result.colorSha256[1] << ','
        << result.colorSha256[2] << ',' << result.colorSha256[3]
        << " non-clear-pixels=" << result.nonClearPixels[0] << ','
        << result.nonClearPixels[1] << ',' << result.nonClearPixels[2] << ','
        << result.nonClearPixels[3] << " capture-budget-limited="
        << (sPendingBudgetLimited ? "yes" : "no")
        << ". No Vulkan surface, swapchain, or presentation path was used; "
        << "visible rendering remains OpenGL." << LL_ENDL;
    sPendingBudgetLimited = false;
}

void pollMaterialProbe()
{
    if (!sMaterialProbe || !sMaterialProbe->pending()) return;
    LL::GHI::MaterialOffscreenProbeResult result;
    const LL::GHI::Status status = sMaterialProbe->poll(result);
    if (!status)
    {
        if (status.code() != LL::GHI::StatusCode::NotReady)
        {
            LL_WARNS("GHIIntegration")
                << "I5 live material offscreen poll failed: "
                << status.message() << LL_ENDL;
            sMaterialDisabled = true;
        }
        return;
    }
    if (!std::all_of(result.nonClearPixels.begin(), result.nonClearPixels.end(),
                     [](std::uint64_t pixels) { return pixels != 0; }))
    {
        LL_INFOS("GHIIntegration")
            << "I5 live material sample completed without comparable coverage; "
            << "retrying without counting a pass. frame=" << result.frameId
            << " draws=" << result.draws << " uv-transformed-draws="
            << result.textureTransformedDraws << " rigged-draws="
            << result.riggedDraws << " max-joints=" << result.maxJointCount
            << " non-clear-pixels="
            << result.nonClearPixels[0] << ',' << result.nonClearPixels[1] << ','
            << result.nonClearPixels[2] << ',' << result.nonClearPixels[3]
            << LL_ENDL;
        sPendingMaterialBudgetLimited = false;
        return;
    }
    if (result.lightingExecuted)
    {
        const std::uint32_t nonClearShadowMaps =
            static_cast<std::uint32_t>(std::count_if(
                result.shadowNonClearPixels.begin(),
                result.shadowNonClearPixels.end(),
                [](std::uint64_t pixels) { return pixels != 0; }));
        const bool directionalShadowCoverage = std::any_of(
            result.shadowNonClearPixels.begin(),
            result.shadowNonClearPixels.begin() + 4,
            [](std::uint64_t pixels) { return pixels != 0; });
        const bool projectorShadowCoverage = std::any_of(
            result.shadowNonClearPixels.begin() + 4,
            result.shadowNonClearPixels.end(),
            [](std::uint64_t pixels) { return pixels != 0; });
        const bool completeShadows = !shadowOffscreenRequestedInternal() ||
            (result.shadowsExecuted && result.shadowMaps &&
             result.directionalShadowMaps && result.projectorShadowMaps &&
             result.shadowCasterDraws && result.shadowRiggedDraws &&
             result.shadowMaskedDraws && directionalShadowCoverage &&
             projectorShadowCoverage);
        if (!result.litNonClearPixels || !result.directionalLights ||
            !result.pointLights ||
            (projectorLightingOffscreenRequested() &&
             (!result.projectorLights || !result.projectorTextures)) ||
            !completeShadows)
        {
            LL_INFOS("GHIIntegration")
                << (shadowOffscreenRequestedInternal()
                    ? "I7d shadow sample lacked complete directional/projector maps or opaque/masked/rigged caster coverage; retrying. frame="
                    : projectorLightingOffscreenRequested()
                    ? "I7c projector-lighting sample lacked complete directional/point/projector coverage; retrying. frame="
                    : "I7b deferred-lighting sample lacked complete directional/point coverage; retrying. frame=")
                << result.frameId << " directional="
                << result.directionalLights << " points=" << result.pointLights
                << " projectors/textures=" << result.projectorLights << '/'
                << result.projectorTextures
                << " shadow-maps(directional/projector/non-clear)="
                << result.shadowMaps << '(' << result.directionalShadowMaps
                << '/' << result.projectorShadowMaps << '/'
                << nonClearShadowMaps << ") casters(rigged/masked)="
                << result.shadowCasterDraws << '(' << result.shadowRiggedDraws
                << '/' << result.shadowMaskedDraws << ") lit-pixels="
                << result.litNonClearPixels << LL_ENDL;
            sPendingMaterialBudgetLimited = false;
            return;
        }
        ++sMaterialSamples;
        ++sLightingSamples;
        LL_INFOS("GHIIntegration")
            << (shadowOffscreenRequestedInternal()
                ? "I7d live same-frame native shadow production/sampling PASS: sample="
                : projectorLightingOffscreenRequested()
                ? "I7c live same-frame native projector lighting PASS: sample="
                : "I7b live same-frame native deferred lighting PASS: sample=")
            << sLightingSamples << '/' << livePacketMaximum() << " frame="
            << result.frameId << " draws=" << result.draws
            << " vertices=" << result.vertices << " indices="
            << result.indices << " directional=" << result.directionalLights
            << " points=" << result.pointLights << " projectors/textures/volume/fullscreen="
            << result.projectorLights << '/' << result.projectorTextures << '/'
            << result.projectorVolumeLights << '/'
            << result.projectorFullscreenLights
            << " shadow-maps(directional/projector)=" << result.shadowMaps
            << '(' << result.directionalShadowMaps << '/'
            << result.projectorShadowMaps << ") shadow-casters(rigged/masked)="
            << result.shadowCasterDraws << '(' << result.shadowRiggedDraws
            << '/' << result.shadowMaskedDraws << ") shadow-non-clear-pixels=";
        for (std::size_t shadow = 0;
             shadow < result.shadowNonClearPixels.size(); ++shadow)
        {
            if (shadow) LL_CONT << ',';
            LL_CONT << result.shadowNonClearPixels[shadow];
        }
        LL_CONT << " material-sha256="
            << result.packetSha256 << " lighting-sha256="
            << result.lightingPacketSha256 << " gbuffer-sha256="
            << result.colorSha256[0] << ',' << result.colorSha256[1] << ','
            << result.colorSha256[2] << ',' << result.colorSha256[3]
            << " lit-sha256=" << result.litColorSha256
            << " lit-non-clear-pixels=" << result.litNonClearPixels
            << " capture-budget-limited="
            << (sPendingMaterialBudgetLimited ? "yes" : "no")
            << (shadowOffscreenRequestedInternal()
                ? ". Native directional/projector depth maps are private; surfaces, swapchains, and presentation remain excluded; visible rendering remains OpenGL."
                : projectorLightingOffscreenRequested()
                ? ". Projector shadows, surfaces, swapchains, and presentation remain excluded; visible rendering remains OpenGL."
                : ". Projector images, shadows, surfaces, swapchains, and presentation remain excluded; visible rendering remains OpenGL.")
            << LL_ENDL;
        sPendingMaterialBudgetLimited = false;
        return;
    }
    if (!result.riggedDraws || !result.maxJointCount)
    {
        LL_INFOS("GHIIntegration")
            << "I5 live material sample contained executable rigid PBR only; "
            << "retrying without counting a pass. frame=" << result.frameId
            << " draws=" << result.draws << " uv-transformed-draws="
            << result.textureTransformedDraws << " non-clear-pixels="
            << result.nonClearPixels[0] << ',' << result.nonClearPixels[1] << ','
            << result.nonClearPixels[2] << ',' << result.nonClearPixels[3]
            << LL_ENDL;
        sPendingMaterialBudgetLimited = false;
        return;
    }
    ++sMaterialSamples;
    LL_INFOS("GHIIntegration")
        << "I5 live rigid/rigged opaque PBR offscreen PASS: sample="
        << sMaterialSamples << '/' << livePacketMaximum()
        << " frame=" << result.frameId << " draws=" << result.draws
        << " vertices=" << result.vertices << " indices=" << result.indices
        << " textures=" << result.textures << " uv-transformed-draws="
        << result.textureTransformedDraws << " rigged-draws="
        << result.riggedDraws << " max-joints=" << result.maxJointCount
        << " packet-sha256="
        << result.packetSha256 << " color-sha256="
        << result.colorSha256[0] << ',' << result.colorSha256[1] << ','
        << result.colorSha256[2] << ',' << result.colorSha256[3]
        << " non-clear-pixels=" << result.nonClearPixels[0] << ','
        << result.nonClearPixels[1] << ',' << result.nonClearPixels[2] << ','
        << result.nonClearPixels[3] << " capture-budget-limited="
        << (sPendingMaterialBudgetLimited ? "yes" : "no")
        << ". Alpha, HUD, mirror, cube-snapshot, and presentation paths remain excluded; visible rendering remains OpenGL."
        << LL_ENDL;
    sPendingMaterialBudgetLimited = false;
}

void pollTerrainProbe()
{
    if (!sTerrainProbe || !sTerrainProbe->pending()) return;
    LL::GHI::TerrainOffscreenProbeResult result;
    const LL::GHI::Status status = sTerrainProbe->poll(result);
    if (!status)
    {
        if (status.code() != LL::GHI::StatusCode::NotReady)
        {
            LL_WARNS("GHIIntegration")
                << "I6 live terrain offscreen poll failed: "
                << status.message() << LL_ENDL;
            sTerrainDisabled = true;
        }
        return;
    }
    if (!std::all_of(result.nonClearPixels.begin(), result.nonClearPixels.end(),
                     [](std::uint64_t pixels) { return pixels != 0; }))
    {
        LL_INFOS("GHIIntegration")
            << "I6 terrain sample completed without four-target coverage; retrying. frame="
            << result.frameId << " draws=" << result.draws
            << " non-clear-pixels=" << result.nonClearPixels[0] << ','
            << result.nonClearPixels[1] << ',' << result.nonClearPixels[2]
            << ',' << result.nonClearPixels[3] << LL_ENDL;
        sPendingTerrainBudgetLimited = false;
        return;
    }
    if (result.lightingExecuted)
    {
        if (!result.pbrDraws || !result.litNonClearPixels ||
            !result.directionalLights || !result.pointLights)
        {
            LL_INFOS("GHIIntegration")
                << "I7b terrain-lighting sample lacked complete PBR/directional/point coverage; retrying. frame="
                << result.frameId << " pbr-draws=" << result.pbrDraws
                << " directional=" << result.directionalLights
                << " points=" << result.pointLights << " lit-pixels="
                << result.litNonClearPixels << LL_ENDL;
            sPendingTerrainBudgetLimited = false;
            return;
        }
        ++sTerrainSamples;
        ++sLightingSamples;
        LL_INFOS("GHIIntegration")
            << "I7b live same-frame native terrain deferred lighting PASS: sample="
            << sLightingSamples << '/' << livePacketMaximum() << " frame="
            << result.frameId << " draws=" << result.draws
            << " regions=" << result.regions << " pbr-draws="
            << result.pbrDraws << " triplanar-draws="
            << result.triplanarDraws << " directional="
            << result.directionalLights << " points=" << result.pointLights
            << " terrain-sha256=" << result.packetSha256
            << " lighting-sha256=" << result.lightingPacketSha256
            << " gbuffer-sha256=" << result.colorSha256[0] << ','
            << result.colorSha256[1] << ',' << result.colorSha256[2] << ','
            << result.colorSha256[3] << " lit-sha256="
            << result.litColorSha256 << " lit-non-clear-pixels="
            << result.litNonClearPixels << " capture-budget-limited="
            << (sPendingTerrainBudgetLimited ? "yes" : "no")
            << ". Projector images, shadows, surfaces, swapchains, and presentation remain excluded; visible rendering remains OpenGL."
            << LL_ENDL;
        sPendingTerrainBudgetLimited = false;
        return;
    }
    ++sTerrainSamples;
    LL_INFOS("GHIIntegration")
        << "I6 live production terrain Vulkan offscreen PASS: sample="
        << sTerrainSamples << '/' << livePacketMaximum() << " frame="
        << result.frameId << " scene-epoch=" << result.sceneEpoch
        << " resource-epoch=" << result.resourceEpoch
        << " draws=" << result.draws << " regions="
        << result.regions << " pbr-draws=" << result.pbrDraws
        << " triplanar-draws=" << result.triplanarDraws << " vertices="
        << result.vertices << " indices=" << result.indices
        << " packet-sha256=" << result.packetSha256 << " color-sha256="
        << result.colorSha256[0] << ',' << result.colorSha256[1] << ','
        << result.colorSha256[2] << ',' << result.colorSha256[3]
        << " capture-budget-limited="
        << (sPendingTerrainBudgetLimited ? "yes" : "no")
        << ". No Vulkan surface, swapchain, or presentation path was used; visible rendering remains OpenGL."
        << LL_ENDL;
    sPendingTerrainBudgetLimited = false;
}

void pollProductionGBuffer()
{
    if (!sGBufferExecutor || !sGBufferExecutor->pending()) return;
    LL::GHI::ProductionGBufferResult result;
    const LL::GHI::Status status = sGBufferExecutor->poll(result);
    if (!status)
    {
        if (status.code() != LL::GHI::StatusCode::NotReady)
        {
            LL_WARNS("GHIIntegration")
                << "I8c2 production G-buffer verification failed: "
                << status.message() << LL_ENDL;
            sFrameAssemblyDisabled = true;
        }
        return;
    }
    const bool completeCoverage = std::all_of(
        result.nonClearPixels.begin(), result.nonClearPixels.end(),
        [](std::uint64_t pixels) { return pixels != 0; });
    if (!result.opaqueDraws || !result.materialDraws || !result.terrainDraws ||
        !result.pbrTerrainDraws || !completeCoverage)
    {
        LL_INFOS("GHIIntegration")
            << "P0e1 completed without full opaque/material/PBR-terrain/four-target coverage; retrying. frame="
            << result.frameId << " draws(opaque/material/legacy/rigged/terrain/pbr)="
            << result.opaqueDraws << '/' << result.materialDraws << '/'
            << result.legacyMaterialDraws << '/' << result.riggedMaterialDraws
            << '/' << result.terrainDraws << '/' << result.pbrTerrainDraws
            << " non-clear-pixels=" << result.nonClearPixels[0] << ','
            << result.nonClearPixels[1] << ',' << result.nonClearPixels[2]
            << ',' << result.nonClearPixels[3] << LL_ENDL;
        sPendingProductionGBufferHash.reset();
        if (!productionLightingExecutionRequested())
        {
            sPendingProductionResidency.reset();
            sPendingProductionBudgetLimited = false;
        }
        return;
    }
    if (productionLightingExecutionRequested())
    {
        sPendingProductionGBufferHash = result.frameSha256;
        LL_INFOS("GHIIntegration")
            << "I8c3 shared production G-buffer stage ready: frame="
            << result.frameId << " assembly-epoch=" << result.assemblyEpoch
            << " target-generation/extent=" << result.targetGeneration << '/'
            << result.width << 'x' << result.height
            << " draws(opaque/material/legacy/rigged/terrain/pbr/deferred-material/deferred-terrain)="
            << result.opaqueDraws << '/' << result.materialDraws << '/'
            << result.legacyMaterialDraws << '/'
            << result.riggedMaterialDraws << '/'
            << result.terrainDraws << '/' << result.pbrTerrainDraws << '/'
            << result.deferredMaterialDraws << '/'
            << result.deferredTerrainDraws
            << " frame-sha256=" << result.frameSha256
            << ". Shadow and lighting completion remains pending; visible rendering remains OpenGL."
            << LL_ENDL;
        return;
    }
    ++sFrameAssemblySamples;
    LL_INFOS("GHIIntegration")
        << "P0e1 shared-target opaque, material, and terrain G-buffer PASS: sample="
        << sFrameAssemblySamples << '/' << livePacketMaximum()
        << " frame=" << result.frameId << " assembly-epoch="
        << result.assemblyEpoch << " target-generation/extent="
        << result.targetGeneration << '/' << result.width << 'x'
        << result.height << " draws(opaque/material/legacy/rigged/terrain/pbr/deferred-material/deferred-terrain)="
        << result.opaqueDraws << '/' << result.materialDraws << '/'
        << result.legacyMaterialDraws << '/' << result.riggedMaterialDraws << '/'
        << result.terrainDraws << '/' << result.pbrTerrainDraws << '/'
        << result.deferredMaterialDraws << '/' << result.deferredTerrainDraws
        << " upload-bytes=" << result.uploadBytes;
    if (sPendingProductionResidency)
        LL_CONT << " residency(hits/uploads/entries/bytes)="
                << sPendingProductionResidency->cacheHits << '/'
                << sPendingProductionResidency->uploads << '/'
                << sPendingProductionResidency->residentEntries << '/'
                << sPendingProductionResidency->residentBytes;
    LL_CONT << " frame-sha256=" << result.frameSha256
            << " gbuffer-sha256=" << result.colorSha256[0] << ','
            << result.colorSha256[1] << ',' << result.colorSha256[2] << ','
            << result.colorSha256[3] << " non-clear-pixels="
            << result.nonClearPixels[0] << ',' << result.nonClearPixels[1]
            << ',' << result.nonClearPixels[2] << ','
            << result.nonClearPixels[3] << " capture-budget-limited="
            << (sPendingProductionBudgetLimited ? "yes" : "no")
            << ". Shared native attachments remain private; lighting, surfaces, swapchains, presentation, and visible output remain excluded."
            << LL_ENDL;
    sPendingProductionResidency.reset();
    sPendingProductionBudgetLimited = false;
}

void pollProductionLighting()
{
    if (!sProductionLightingExecutor ||
        !sProductionLightingExecutor->pending()) return;
    LL::GHI::ProductionLightingResult result;
    const LL::GHI::Status status =
        sProductionLightingExecutor->poll(result);
    if (!status)
    {
        if (status.code() != LL::GHI::StatusCode::NotReady)
        {
            LL_WARNS("GHIIntegration")
                << "I8c3 production shadow/lighting verification failed: "
                << status.message() << LL_ENDL;
            sFrameAssemblyDisabled = true;
        }
        return;
    }
    const bool frameIdentityMatches = sPendingProductionGBufferHash &&
        *sPendingProductionGBufferHash == result.frameSha256;
    const bool directionalCoverage = result.directionalShadowMaps &&
        std::any_of(result.shadowNonClearPixels.begin(),
                    result.shadowNonClearPixels.begin() +
                        LL::GHI::LIGHTING_DIRECTIONAL_SHADOW_CASCADES,
                    [](std::uint64_t pixels) { return pixels != 0; });
    const bool projectorCoverage = result.projectorLights &&
        result.projectorShadowMaps &&
        std::any_of(result.shadowNonClearPixels.begin() +
                        LL::GHI::LIGHTING_DIRECTIONAL_SHADOW_CASCADES,
                    result.shadowNonClearPixels.end(),
                    [](std::uint64_t pixels) { return pixels != 0; });
    if (!frameIdentityMatches || !result.litNonClearPixels ||
        !result.shadowCasterDraws || !directionalCoverage ||
        !projectorCoverage)
    {
        LL_INFOS("GHIIntegration")
            << "I8c3 completed without full coherent directional/projector shadow and lighting coverage; retrying. frame="
            << result.frameId << " identity-match="
            << (frameIdentityMatches ? "yes" : "no")
            << " lights(directional/point/projector)="
            << result.directionalLights << '/' << result.pointLights << '/'
            << result.projectorLights << " shadows(total/directional/projector/casters)="
            << result.shadowMaps << '/' << result.directionalShadowMaps << '/'
            << result.projectorShadowMaps << '/' << result.shadowCasterDraws
            << " lit-pixels=" << result.litNonClearPixels
            << " shadow-pixels=";
        for (std::size_t shadow = 0;
             shadow < result.shadowNonClearPixels.size(); ++shadow)
            LL_CONT << (shadow ? "," : "")
                    << result.shadowNonClearPixels[shadow];
        LL_CONT << LL_ENDL;
        sPendingProductionGBufferHash.reset();
        sPendingProductionResidency.reset();
        sPendingProductionBudgetLimited = false;
        return;
    }
    ++sFrameAssemblySamples;
    LL_INFOS("GHIIntegration")
        << "I8c3 shared-target native shadow and lighting PASS: sample="
        << sFrameAssemblySamples << '/' << livePacketMaximum()
        << " frame=" << result.frameId << " assembly-epoch="
        << result.assemblyEpoch << " target-generation/extent="
        << result.targetGeneration << '/' << result.width << 'x'
        << result.height << " lights(directional/point/projector/textures/volume/fullscreen)="
        << result.directionalLights << '/' << result.pointLights << '/'
        << result.projectorLights << '/' << result.projectorTextures << '/'
        << result.projectorVolumeLights << '/'
        << result.projectorFullscreenLights
        << " shadows(total/directional/projector/casters/rigged/masked/deferred)="
        << result.shadowMaps << '/' << result.directionalShadowMaps << '/'
        << result.projectorShadowMaps << '/' << result.shadowCasterDraws << '/'
        << result.shadowRiggedDraws << '/' << result.shadowMaskedDraws << '/'
        << result.deferredShadowDraws << " upload-bytes="
        << result.uploadBytes << " frame-sha256=" << result.frameSha256
        << " lighting-sha256=" << result.lightingSha256
        << " lit-non-clear-pixels=" << result.litNonClearPixels
        << " shadow-non-clear-pixels=";
    for (std::size_t shadow = 0;
         shadow < result.shadowNonClearPixels.size(); ++shadow)
        LL_CONT << (shadow ? "," : "")
                << result.shadowNonClearPixels[shadow];
    LL_CONT << " capture-budget-limited="
            << (sPendingProductionBudgetLimited ? "yes" : "no")
            << ". G-buffer, depth, shadow, and lighting targets share one private production-frame identity; surfaces, swapchains, presentation, and visible output remain excluded."
            << LL_ENDL;
    sPendingProductionGBufferHash.reset();
    sPendingProductionResidency.reset();
    sPendingProductionBudgetLimited = false;
}
}

namespace LLGHIRuntime
{

void initialize()
{
    if (sVulkanDevice || !gSavedSettings.getBOOL("RenderVulkanDeveloperProbe"))
    {
        return;
    }

    LL::GHI::DeviceCreateInfo info;
    info.backend = LL::GHI::Backend::Vulkan;
    info.adapterIndex = gSavedSettings.getU32("RenderVulkanAdapterIndex");
    info.framesInFlight = 2;
    info.enableValidation = gSavedSettings.getBOOL("RenderVulkanValidation");

    LL_INFOS("GHIIntegration")
        << "Creating developer-gated native Vulkan coexistence device; adapter="
        << info.adapterIndex << " validation="
        << (info.enableValidation ? "on" : "off") << LL_ENDL;

    LL::GHI::DeviceCreationResult creation = LL::GHI::createDevice(info);
    if (!creation.status || !creation.device)
    {
        LL_WARNS("GHIIntegration")
            << "Native Vulkan coexistence device was not created: "
            << creation.status.message()
            << ". The production OpenGL renderer remains active."
            << LL_ENDL;
        return;
    }

    sVulkanDevice = std::move(creation.device);
    const LL::GHI::PipelineCacheDomain domain =
        sVulkanDevice->pipelineCacheDomain();
    const LL::GHI::RendererCapabilities& capabilities =
        sVulkanDevice->capabilities();

    LL_INFOS("GHIIntegration")
        << "Native Vulkan coexistence device active; device-domain="
        << domain.deviceIdentity << " driver-domain=" << domain.driverIdentity
        << " color-attachments=" << capabilities.maxColorAttachments
        << " sampled-images=" << capabilities.maxSampledImagesPerStage
        << " storage-buffers=" << capabilities.maxStorageBuffersPerStage
        << " max-texture=" << capabilities.maxTexture2DSize
        << " timestamps=" << (capabilities.timestampQueries ? "yes" : "no")
        << " storage-atomics=" << (capabilities.storageImageAtomics ? "yes" : "no")
        << " cube-arrays=" << (capabilities.cubeMapArrays ? "yes" : "no")
        << ". Visible rendering and the active renderer snapshot remain OpenGL."
        << LL_ENDL;

    if (frameAssemblyRequested())
    {
        if (textureResidencyRequested())
        {
            sTextureResidency =
                std::make_unique<LL::GHI::ProductionTextureResidency>(
                    *sVulkanDevice);
            if (frameGraphRequested())
                sFrameTargets =
                    std::make_unique<LL::GHI::ProductionFrameTargets>(
                        *sVulkanDevice);
            if (gBufferExecutionRequested())
            {
                const std::string opaquePath =
                    gDirUtilp->getExpandedFilename(
                        LL_PATH_APP_SETTINGS, "ghi_shaders",
                        "r4_opaque.llghisp");
                const std::string materialPath =
                    gDirUtilp->getExpandedFilename(
                        LL_PATH_APP_SETTINGS, "ghi_shaders",
                        "r5_material_skin.llghisp");
                const std::string terrainPath =
                    gDirUtilp->getExpandedFilename(
                        LL_PATH_APP_SETTINGS, "ghi_shaders",
                        "i6_terrain.llghisp");
                LL::GHI::ShaderPackageDesc opaquePackage;
                LL::GHI::ShaderPackageDesc materialPackage;
                LL::GHI::ShaderPackageDesc terrainPackage;
                const LL::GHI::Status opaqueStatus =
                    LL::GHI::loadShaderPackage(opaquePath, opaquePackage);
                const LL::GHI::Status materialStatus =
                    LL::GHI::loadShaderPackage(materialPath, materialPackage);
                const LL::GHI::Status terrainStatus =
                    LL::GHI::loadShaderPackage(terrainPath, terrainPackage);
                if (!opaqueStatus || !materialStatus || !terrainStatus)
                {
                    LL_WARNS("GHIIntegration")
                        << "P0e1 production G-buffer shader packages were not loaded: opaque="
                        << opaqueStatus.message() << " material="
                        << materialStatus.message() << " terrain="
                        << terrainStatus.message()
                        << ". Visible rendering remains OpenGL." << LL_ENDL;
                }
                else
                {
                    sGBufferExecutor = std::make_unique<
                        LL::GHI::ProductionGBufferExecutor>(
                            *sVulkanDevice, std::move(opaquePackage),
                            std::move(materialPackage),
                            std::move(terrainPackage));
                }
                if (productionLightingExecutionRequested())
                {
                    const std::string lightingPath =
                        gDirUtilp->getExpandedFilename(
                            LL_PATH_APP_SETTINGS, "ghi_shaders",
                            "i7_deferred_lighting.llghisp");
                    const std::string projectorPath =
                        gDirUtilp->getExpandedFilename(
                            LL_PATH_APP_SETTINGS, "ghi_shaders",
                            "i7_projector_lighting.llghisp");
                    const std::string shadowPath =
                        gDirUtilp->getExpandedFilename(
                            LL_PATH_APP_SETTINGS, "ghi_shaders",
                            "i7_shadow.llghisp");
                    LL::GHI::ShaderPackageDesc lightingPackage;
                    LL::GHI::ShaderPackageDesc projectorPackage;
                    LL::GHI::ShaderPackageDesc shadowPackage;
                    const LL::GHI::Status lightingStatus =
                        LL::GHI::loadShaderPackage(
                            lightingPath, lightingPackage);
                    const LL::GHI::Status projectorStatus =
                        LL::GHI::loadShaderPackage(
                            projectorPath, projectorPackage);
                    const LL::GHI::Status shadowStatus =
                        LL::GHI::loadShaderPackage(
                            shadowPath, shadowPackage);
                    if (!lightingStatus || !projectorStatus || !shadowStatus)
                    {
                        LL_WARNS("GHIIntegration")
                            << "I8c3 production lighting shader packages were not loaded: lighting="
                            << lightingStatus.message() << " projector="
                            << projectorStatus.message() << " shadow="
                            << shadowStatus.message()
                            << ". Visible rendering remains OpenGL."
                            << LL_ENDL;
                    }
                    else
                    {
                        sProductionLightingExecutor = std::make_unique<
                            LL::GHI::ProductionLightingExecutor>(
                                *sVulkanDevice, std::move(lightingPackage),
                                std::move(projectorPackage),
                                std::move(shadowPackage));
                    }
                }
            }
            const LL::GHI::ProductionTextureResidencyLimits limits;
            LL_INFOS("GHIIntegration")
                << (productionLightingExecutionRequested()
                    ? "I8c3 shared-target native shadow and lighting execution armed; interval="
                    : gBufferExecutionRequested()
                    ? "I8c2 shared-target material and terrain G-buffer execution armed; interval="
                    : frameGraphRequested()
                    ? "I8c1 shared production frame targets and retained texture residency armed; interval="
                    : "I8b retained production texture residency armed; interval=")
                << livePacketInterval() << " frames max-samples="
                << livePacketMaximum()
                << " limits(entries/sources/resident-bytes/upload-bytes/stale-epochs)="
                << limits.maxEntries << '/' << limits.maxSourceRecords << '/'
                << limits.maxResidentBytes << '/'
                << limits.maxUploadBytesPerFrame << '/'
                << limits.staleAfterAssemblyEpochs
                << (productionLightingExecutionRequested()
                    ? ". Immutable decoded images are retained by content; material, terrain, directional/projector shadows, deferred lighting, and projector lighting execute against one private target graph with asynchronous verification and no presentation."
                    : gBufferExecutionRequested()
                    ? ". Immutable decoded images are retained by content; material and terrain draws execute only into private shared attachments with asynchronous verification and no presentation."
                    : ". Immutable decoded images are deduplicated by content while logical source generations remain explicit; no draw or presentation is recorded.")
                << LL_ENDL;
        }
        else
        {
            const LL::GHI::ProductionFrameTransferLimits limits;
            LL_INFOS("GHIIntegration")
                << "I8a same-frame production assembly transfer armed; interval="
                << livePacketInterval() << " frames max-samples="
                << livePacketMaximum()
                << " limits(opaque-draws/material-draws/terrain-draws/vertices/indices/resources/decoded-bytes/encoded-bytes)="
                << limits.maxOpaqueDraws << '/' << limits.maxMaterialDraws
                << '/' << limits.maxTerrainDraws << '/'
                << limits.maxVertices << '/' << limits.maxIndices << '/'
                << limits.maxUniqueResources << '/'
                << limits.maxDecodedTextureBytes << '/' << limits.maxEncodedBytes
                << ". Material, terrain, and lighting components must share one production frame; no draw or presentation is recorded."
                << LL_ENDL;
        }
        if (gSavedSettings.getBOOL("RenderVulkanMaterialOffscreenProbe") ||
            gSavedSettings.getBOOL("RenderVulkanTerrainOffscreenProbe") ||
            gSavedSettings.getBOOL("RenderVulkanLightingPacketProbe") ||
            lightingOffscreenRequested() || terrainLightingOffscreenRequested())
            LL_WARNS("GHIIntegration")
                << (textureResidencyRequested()
                    ? "I8b texture residency was combined with an earlier packet/offscreen probe. I8b capture precedence applies; use one integration mode per run."
                    : "I8a frame assembly was combined with an earlier packet/offscreen probe. I8a capture precedence applies; use one integration mode per run.")
                << LL_ENDL;
    }

    if (offscreenProbeRequested())
    {
        const std::string packagePath = gDirUtilp->getExpandedFilename(
            LL_PATH_APP_SETTINGS, "ghi_shaders", "r4_opaque.llghisp");
        LL::GHI::ShaderPackageDesc shaderPackage;
        const LL::GHI::Status status = LL::GHI::loadShaderPackage(
            packagePath, shaderPackage);
        if (!status)
        {
            LL_WARNS("GHIIntegration")
                << "I2 runtime opaque shader package was not loaded from "
                << packagePath << ": " << status.message()
                << ". Visible rendering remains OpenGL." << LL_ENDL;
        }
        else
        {
            sOffscreenProbe =
                std::make_unique<LL::GHI::OpaqueOffscreenProbe>(
                    *sVulkanDevice, std::move(shaderPackage));
            LL_INFOS("GHIIntegration")
                << "I2 asynchronous live opaque offscreen probe armed from "
                << packagePath << "; extent=256x256 attachments=4. "
                << "The probe owns no surface, swapchain, or presentation path."
                << LL_ENDL;
        }
    }

    if (gSavedSettings.getBOOL("RenderVulkanMaterialOffscreenProbe") ||
        lightingOffscreenRequested())
    {
        const std::string packagePath = gDirUtilp->getExpandedFilename(
            LL_PATH_APP_SETTINGS, "ghi_shaders", "r5_material_skin.llghisp");
        LL::GHI::ShaderPackageDesc shaderPackage;
        const LL::GHI::Status status = LL::GHI::loadShaderPackage(
            packagePath, shaderPackage);
        if (!status)
        {
            LL_WARNS("GHIIntegration")
                << "I5 runtime material shader package was not loaded from "
                << packagePath << ": " << status.message()
                << ". Visible rendering remains OpenGL." << LL_ENDL;
        }
        else
        {
            if (lightingOffscreenRequested())
            {
                const std::string lightingPath = gDirUtilp->getExpandedFilename(
                    LL_PATH_APP_SETTINGS, "ghi_shaders",
                    "i7_deferred_lighting.llghisp");
                LL::GHI::ShaderPackageDesc lightingPackage;
                const LL::GHI::Status lightingStatus =
                    LL::GHI::loadShaderPackage(lightingPath, lightingPackage);
                if (!lightingStatus)
                {
                    LL_WARNS("GHIIntegration")
                        << "I7b deferred-lighting shader package was not loaded from "
                        << lightingPath << ": " << lightingStatus.message()
                        << LL_ENDL;
                }
                else
                {
                    if (projectorLightingOffscreenRequested())
                    {
                        const std::string projectorPath =
                            gDirUtilp->getExpandedFilename(
                                LL_PATH_APP_SETTINGS, "ghi_shaders",
                                "i7_projector_lighting.llghisp");
                        LL::GHI::ShaderPackageDesc projectorPackage;
                        const LL::GHI::Status projectorStatus =
                            LL::GHI::loadShaderPackage(
                                projectorPath, projectorPackage);
                        if (!projectorStatus)
                        {
                            LL_WARNS("GHIIntegration")
                                << "I7c projector-lighting shader package was not loaded from "
                                << projectorPath << ": "
                                << projectorStatus.message() << LL_ENDL;
                        }
                        else
                        {
                            if (shadowOffscreenRequestedInternal())
                            {
                                const std::string shadowPath =
                                    gDirUtilp->getExpandedFilename(
                                        LL_PATH_APP_SETTINGS, "ghi_shaders",
                                        "i7_shadow.llghisp");
                                LL::GHI::ShaderPackageDesc shadowPackage;
                                const LL::GHI::Status shadowStatus =
                                    LL::GHI::loadShaderPackage(
                                        shadowPath, shadowPackage);
                                if (!shadowStatus)
                                {
                                    LL_WARNS("GHIIntegration")
                                        << "I7d shadow shader package was not loaded from "
                                        << shadowPath << ": "
                                        << shadowStatus.message() << LL_ENDL;
                                }
                                else
                                {
                                    sMaterialProbe = std::make_unique<
                                        LL::GHI::MaterialOffscreenProbe>(
                                            *sVulkanDevice,
                                            std::move(shaderPackage),
                                            std::move(lightingPackage),
                                            std::move(projectorPackage),
                                            std::move(shadowPackage));
                                    LL_INFOS("GHIIntegration")
                                        << "I7d same-frame material G-buffer plus native directional/projector shadow production and sampling probe armed from "
                                        << packagePath << ", " << lightingPath
                                        << ", " << projectorPath << ", and "
                                        << shadowPath
                                        << "; extent=256x256 shadow-maps=4+2. Visible rendering remains OpenGL."
                                        << LL_ENDL;
                                }
                            }
                            else
                            {
                                sMaterialProbe = std::make_unique<
                                    LL::GHI::MaterialOffscreenProbe>(
                                        *sVulkanDevice,
                                        std::move(shaderPackage),
                                        std::move(lightingPackage),
                                        std::move(projectorPackage));
                                LL_INFOS("GHIIntegration")
                                    << "I7c same-frame material G-buffer plus directional, point, and decoded-image projector lighting probe armed from "
                                    << packagePath << ", " << lightingPath
                                    << ", and " << projectorPath
                                    << "; extent=256x256. Projector shadows remain deferred."
                                    << LL_ENDL;
                            }
                        }
                    }
                    else
                    {
                        sMaterialProbe =
                            std::make_unique<LL::GHI::MaterialOffscreenProbe>(
                                *sVulkanDevice, std::move(shaderPackage),
                                std::move(lightingPackage));
                        LL_INFOS("GHIIntegration")
                            << "I7b same-frame material G-buffer plus directional/point lighting probe armed from "
                            << packagePath << " and " << lightingPath
                            << "; extent=256x256. Projector images and shadows remain deferred."
                            << LL_ENDL;
                    }
                }
            }
            else
            {
                sMaterialProbe =
                    std::make_unique<LL::GHI::MaterialOffscreenProbe>(
                        *sVulkanDevice, std::move(shaderPackage));
            }
            const LL::GHI::MaterialOffscreenProbeLimits limits;
            if (sMaterialProbe && !lightingOffscreenRequested())
                LL_INFOS("GHIIntegration")
                << "I5 asynchronous rigid/rigged opaque PBR material probe armed from "
                << packagePath << "; extent=256x256 limits(draws/vertices/indices/textures/bytes)="
                << limits.maxDraws << '/' << limits.maxVertices << '/'
                << limits.maxIndices << '/' << limits.maxTextures << '/'
                << limits.maxUploadBytes
                << ". The probe owns no surface, swapchain, or presentation path."
                << LL_ENDL;
        }
    }

    const std::uint32_t lightingModes =
        (gSavedSettings.getBOOL("RenderVulkanLightingOffscreenProbe") ? 1u : 0u) +
        (gSavedSettings.getBOOL("RenderVulkanTerrainLightingOffscreenProbe") ? 1u : 0u) +
        (projectorLightingOffscreenRequested() ? 1u : 0u);
    if (lightingModes > 1)
        LL_WARNS("GHIIntegration")
            << "Multiple I7 lighting probes were requested; material/projector precedence applies. Enable only one mode per run."
            << LL_ENDL;

    if (gSavedSettings.getBOOL("RenderVulkanTerrainOffscreenProbe") ||
        terrainLightingOffscreenRequested())
    {
        const std::string packagePath = gDirUtilp->getExpandedFilename(
            LL_PATH_APP_SETTINGS, "ghi_shaders", "i6_terrain.llghisp");
        LL::GHI::ShaderPackageDesc package;
        const LL::GHI::Status status =
            LL::GHI::loadShaderPackage(packagePath, package);
        if (!status)
            LL_WARNS("GHIIntegration")
                << "I6 terrain shader package was not loaded from "
                << packagePath << ": " << status.message() << LL_ENDL;
        else if (terrainLightingOffscreenRequested())
        {
            const std::string lightingPath = gDirUtilp->getExpandedFilename(
                LL_PATH_APP_SETTINGS, "ghi_shaders",
                "i7_deferred_lighting.llghisp");
            LL::GHI::ShaderPackageDesc lightingPackage;
            const LL::GHI::Status lightingStatus =
                LL::GHI::loadShaderPackage(lightingPath, lightingPackage);
            if (!lightingStatus)
                LL_WARNS("GHIIntegration")
                    << "I7b terrain deferred-lighting shader package was not loaded from "
                    << lightingPath << ": " << lightingStatus.message()
                    << LL_ENDL;
            else
            {
                sTerrainProbe = std::make_unique<LL::GHI::TerrainOffscreenProbe>(
                    *sVulkanDevice, std::move(package),
                    std::move(lightingPackage));
                LL_INFOS("GHIIntegration")
                    << "I7b same-frame terrain G-buffer plus directional/point lighting probe armed; interval="
                    << livePacketInterval() << " frames max-samples="
                    << livePacketMaximum()
                    << ". Projector images and shadows remain deferred; the probe owns no surface, swapchain, or presentation path."
                    << LL_ENDL;
            }
        }
        else
        {
            sTerrainProbe = std::make_unique<LL::GHI::TerrainOffscreenProbe>(
                *sVulkanDevice, std::move(package));
            LL_INFOS("GHIIntegration")
                << "I6 production terrain Vulkan offscreen probe armed; interval="
                << livePacketInterval() << " frames max-samples="
                << livePacketMaximum()
                << ". The probe owns no surface, swapchain, or presentation path."
                << LL_ENDL;
        }
    }

    if (gSavedSettings.getBOOL("RenderVulkanLightingPacketProbe"))
    {
        const LL::GHI::LightingPacketTransferLimits limits;
        LL_INFOS("GHIIntegration")
            << "I7a live deferred-lighting packet transfer armed; interval="
            << livePacketInterval() << " frames max-samples="
            << livePacketMaximum() << " limits(lights/bytes)="
            << limits.maxLocalLights << '/' << limits.maxUploadBytes
            << ". Shadow matrices and policy are captured, while OpenGL shadow images are explicitly deferred."
            << LL_ENDL;
    }

    if (sOffscreenProbe ||
        gSavedSettings.getBOOL("RenderVulkanLivePacketProbe"))
    {
        const LL::GHI::OpaquePacketTransferLimits limits;
        LL_INFOS("GHIIntegration")
            << (sOffscreenProbe
                    ? "I2 live post-cull offscreen execution armed; interval="
                    : "I1 live post-cull packet transfer armed; interval=")
            << livePacketInterval() << " frames max-samples="
            << livePacketMaximum() << " limits(draws/vertices/indices/bytes)="
            << limits.maxDraws << '/' << limits.maxVertices << '/'
            << limits.maxIndices << '/' << limits.maxUploadBytes
            << (sOffscreenProbe
                    ? ". Submissions draw only into isolated Vulkan attachments and are polled asynchronously."
                    : ". Transfers are non-presenting and record no draw calls.")
            << LL_ENDL;
    }
}

void shutdown()
{
    if (!sVulkanDevice)
    {
        return;
    }

    if (sOffscreenProbe)
    {
        const LL::GHI::Status probeStatus = sOffscreenProbe->shutdown();
        if (!probeStatus)
            LL_WARNS("GHIIntegration")
                << "I2 offscreen resources did not retire cleanly: "
                << probeStatus.message() << LL_ENDL;
        sOffscreenProbe.reset();
    }
    if (sMaterialProbe)
    {
        const LL::GHI::Status probeStatus = sMaterialProbe->shutdown();
        if (!probeStatus)
            LL_WARNS("GHIIntegration")
                << "I5 material offscreen resources did not retire cleanly: "
                << probeStatus.message() << LL_ENDL;
        sMaterialProbe.reset();
    }
    if (sTerrainProbe)
    {
        const LL::GHI::Status probeStatus = sTerrainProbe->shutdown();
        if (!probeStatus)
            LL_WARNS("GHIIntegration")
                << "I6 terrain offscreen resources did not retire cleanly: "
                << probeStatus.message() << LL_ENDL;
        sTerrainProbe.reset();
    }
    if (sProductionLightingExecutor)
    {
        const LL::GHI::Status executionStatus =
            sProductionLightingExecutor->shutdown();
        if (!executionStatus)
            LL_WARNS("GHIIntegration")
                << "I8c3 production shadow/lighting executor did not retire cleanly: "
                << executionStatus.message() << LL_ENDL;
        sProductionLightingExecutor.reset();
    }
    if (sGBufferExecutor)
    {
        const LL::GHI::Status executionStatus = sGBufferExecutor->shutdown();
        if (!executionStatus)
            LL_WARNS("GHIIntegration")
                << "I8c2 production G-buffer executor did not retire cleanly: "
                << executionStatus.message() << LL_ENDL;
        sGBufferExecutor.reset();
    }
    if (sFrameTargets)
    {
        const LL::GHI::Status targetStatus = sFrameTargets->shutdown();
        if (!targetStatus)
            LL_WARNS("GHIIntegration")
                << "I8c1 shared production frame targets did not retire cleanly: "
                << targetStatus.message() << LL_ENDL;
        sFrameTargets.reset();
    }
    if (sTextureResidency)
    {
        const LL::GHI::Status residencyStatus = sTextureResidency->shutdown();
        if (!residencyStatus)
            LL_WARNS("GHIIntegration")
                << "I8b retained texture resources did not retire cleanly: "
                << residencyStatus.message() << LL_ENDL;
        sTextureResidency.reset();
    }
    const LL::GHI::Status status = sVulkanDevice->waitIdle();
    if (!status)
    {
        LL_WARNS("GHIIntegration")
            << "Native Vulkan coexistence device did not become idle during shutdown: "
            << status.message() << LL_ENDL;
    }
    sVulkanDevice.reset();
    sPendingProductionResidency.reset();
    sPendingProductionGBufferHash.reset();
    sPendingProductionBudgetLimited = false;
    sNextLivePacketFrame = 0;
    sLivePacketAttempts = 0;
    sLivePacketSamples = 0;
    sLivePacketCaptureClaimed = false;
    sLivePacketDisabled = false;
    sPendingBudgetLimited = false;
    sNextMaterialFrame = 0;
    sMaterialAttempts = 0;
    sMaterialSamples = 0;
    sMaterialCaptureClaimed = false;
    sMaterialDisabled = false;
    sPendingMaterialBudgetLimited = false;
    sPendingFrameOpaque.reset();
    sPendingLightingMaterial.reset();
    sPendingLightingTerrain.reset();
    sNextFrameAssemblyFrame = 0;
    sFrameAssemblyEpoch = 0;
    sFrameAssemblyAttempts = 0;
    sFrameAssemblySamples = 0;
    sFrameAssemblyDisabled = false;
    sPendingFrameOpaqueBudgetLimited = false;
    sPendingFrameMaterialBudgetLimited = false;
    sPendingFrameTerrainBudgetLimited = false;
    sNextTerrainFrame = 0;
    sTerrainAttempts = 0;
    sTerrainSamples = 0;
    sTerrainCaptureClaimed = false;
    sTerrainDisabled = false;
    sPendingTerrainBudgetLimited = false;
    sNextLightingFrame = 0;
    sLightingAttempts = 0;
    sLightingSamples = 0;
    sLightingCaptureClaimed = false;
    sLightingDisabled = false;
    LL_INFOS("GHIIntegration")
        << "Native Vulkan coexistence device shut down."
        << LL_ENDL;
}

bool active()
{
    return static_cast<bool>(sVulkanDevice);
}

bool shouldCaptureLiveOpaquePacket(std::uint64_t frame_id)
{
    pollOffscreenProbe();
    if (!sVulkanDevice || sLivePacketDisabled || sLivePacketCaptureClaimed ||
        !livePacketRequested())
        return false;
    if (frameAssemblyRequested())
    {
        if (sFrameAssemblyDisabled || sPendingFrameOpaque ||
            (sGBufferExecutor && sGBufferExecutor->pending()) ||
            (sProductionLightingExecutor &&
             sProductionLightingExecutor->pending()))
            return false;
        const std::uint32_t maximum = livePacketMaximum();
        if (!maximum || sFrameAssemblySamples >= maximum ||
            static_cast<std::uint64_t>(sFrameAssemblyAttempts) >=
                static_cast<std::uint64_t>(maximum) * 8ull)
            return false;
        if (!sNextFrameAssemblyFrame)
        {
            sNextFrameAssemblyFrame = frame_id + livePacketInterval();
            return false;
        }
        if (frame_id < sNextFrameAssemblyFrame) return false;
        sLivePacketCaptureClaimed = true;
        return true;
    }
    if (offscreenProbeRequested() && !sOffscreenProbe) return false;
    if (sOffscreenProbe && sOffscreenProbe->pending()) return false;
    const std::uint32_t maximum = livePacketMaximum();
    if (!maximum || sLivePacketSamples >= maximum ||
        static_cast<std::uint64_t>(sLivePacketAttempts) >=
            static_cast<std::uint64_t>(maximum) * 4ull)
        return false;
    if (!sNextLivePacketFrame)
    {
        sNextLivePacketFrame = frame_id + livePacketInterval();
        return false;
    }
    if (frame_id < sNextLivePacketFrame) return false;
    sLivePacketCaptureClaimed = true;
    return true;
}

void consumeLiveOpaquePacket(const LL::GHI::OpaqueScenePacket& packet,
                             bool budget_limited)
{
    if (!sLivePacketCaptureClaimed || !sVulkanDevice) return;
    sLivePacketCaptureClaimed = false;
    if (frameAssemblyRequested())
    {
        sPendingFrameOpaque = packet;
        sPendingFrameOpaqueBudgetLimited = budget_limited;
        LL_INFOS("GHIIntegration")
            << "P0e1 captured rigid opaque component for production-frame assembly: frame="
            << packet.frameId << " draws=" << packet.draws.size()
            << " vertices=" << packet.vertices.size() << " indices="
            << packet.indices.size() << LL_ENDL;
        return;
    }
    ++sLivePacketAttempts;
    sNextLivePacketFrame = packet.frameId + livePacketInterval();

    const LL::GHI::OpaquePacketTransferLimits limits;
    if (sOffscreenProbe)
    {
        const LL::GHI::Status status = sOffscreenProbe->submit(packet, limits);
        if (!status)
        {
            LL_WARNS("GHIIntegration")
                << "I2 live opaque offscreen submission rejected at frame "
                << packet.frameId << ": " << status.message()
                << " attempt=" << sLivePacketAttempts << LL_ENDL;
            disableLivePacketProbe(status, "submission");
            return;
        }
        sPendingBudgetLimited = budget_limited;
        LL_INFOS("GHIIntegration")
            << "I2 live opaque offscreen submitted asynchronously: frame="
            << packet.frameId << " draws=" << packet.draws.size()
            << " vertices=" << packet.vertices.size() << " indices="
            << packet.indices.size()
            << ". Completion will be polled on later OpenGL frames."
            << LL_ENDL;
        return;
    }

    LL::GHI::OpaquePacketTransferResult result;
    const LL::GHI::Status status = LL::GHI::consumeOpaquePacketTransfer(
        *sVulkanDevice, packet, limits, result);
    if (!status)
    {
        LL_WARNS("GHIIntegration")
            << "I1 live packet transfer rejected at frame " << packet.frameId
            << ": " << status.message() << " attempt=" << sLivePacketAttempts
            << LL_ENDL;
        if (status.code() == LL::GHI::StatusCode::DeviceLost)
        {
            sLivePacketDisabled = true;
            LL_WARNS("GHIIntegration")
                << "I1 live packet transfer disabled after device loss."
                << LL_ENDL;
        }
        return;
    }

    ++sLivePacketSamples;
    LL_INFOS("GHIIntegration")
        << "I1 live packet transfer PASS: sample=" << sLivePacketSamples << '/'
        << livePacketMaximum() << " frame=" << result.frameId
        << " draws=" << result.draws << " vertices=" << result.vertices
        << " indices=" << result.indices << " upload-bytes="
        << result.uploadBytes << " encoded-bytes=" << result.encodedBytes
        << " sha256=" << result.packetSha256 << " capture-budget-limited="
        << (budget_limited ? "yes" : "no")
        << ". Visible rendering remains OpenGL."
        << LL_ENDL;
}

bool materialCaptureRequested()
{
    return sVulkanDevice && !sMaterialDisabled &&
           ((frameAssemblyRequested() && !sFrameAssemblyDisabled) ||
            (sMaterialProbe &&
             (gSavedSettings.getBOOL("RenderVulkanMaterialOffscreenProbe") ||
              lightingOffscreenRequested())));
}

bool shadowOffscreenRequested()
{
    return shadowOffscreenRequestedInternal();
}

bool shouldCaptureLiveMaterialPacket(std::uint64_t frame_id)
{
    pollProductionGBuffer();
    pollProductionLighting();
    pollMaterialProbe();
    if (!materialCaptureRequested() || sMaterialCaptureClaimed ||
        (sMaterialProbe && sMaterialProbe->pending()) ||
        (sGBufferExecutor && sGBufferExecutor->pending()) ||
        (sProductionLightingExecutor &&
         sProductionLightingExecutor->pending()) ||
        sPendingLightingMaterial)
        return false;
    if (frameAssemblyRequested())
    {
        const std::uint32_t maximum = livePacketMaximum();
        if (!maximum || sFrameAssemblySamples >= maximum ||
            static_cast<std::uint64_t>(sFrameAssemblyAttempts) >=
                static_cast<std::uint64_t>(maximum) * 8ull)
            return false;
        if (!sNextFrameAssemblyFrame)
        {
            sNextFrameAssemblyFrame = frame_id + livePacketInterval();
            return false;
        }
        if (frame_id < sNextFrameAssemblyFrame) return false;
        sMaterialCaptureClaimed = true;
        return true;
    }
    const std::uint32_t maximum = livePacketMaximum();
    if (!maximum || sMaterialSamples >= maximum ||
        static_cast<std::uint64_t>(sMaterialAttempts) >=
            static_cast<std::uint64_t>(maximum) * 8ull)
        return false;
    if (!sNextMaterialFrame)
    {
        sNextMaterialFrame = frame_id + livePacketInterval();
        return false;
    }
    if (frame_id < sNextMaterialFrame) return false;
    sMaterialCaptureClaimed = true;
    return true;
}

void consumeLiveMaterialPacket(const LL::GHI::MaterialScenePacket& packet,
                               bool budget_limited)
{
    if (!sMaterialCaptureClaimed ||
        (!frameAssemblyRequested() && !sMaterialProbe)) return;
    sMaterialCaptureClaimed = false;
    if (frameAssemblyRequested())
    {
        sPendingLightingMaterial = packet;
        sPendingFrameMaterialBudgetLimited = budget_limited;
        LL_INFOS("GHIIntegration")
            << "I8a captured material component for production-frame assembly: frame="
            << packet.frameId << " draws=" << packet.draws.size()
            << " vertices=" << packet.vertices.size() << " indices="
            << packet.indices.size() << " resources="
            << packet.textures.size() + packet.materials.size() +
                   packet.skins.size()
            << LL_ENDL;
        return;
    }
    ++sMaterialAttempts;
    sNextMaterialFrame = packet.frameId + livePacketInterval();
    if (packet.draws.empty())
    {
            LL_INFOS("GHIIntegration")
                << (lightingOffscreenRequested()
                    ? (shadowOffscreenRequestedInternal()
                        ? "I7d material capture contained no executable shadow/receiver geometry; retrying. frame="
                        : projectorLightingOffscreenRequested()
                        ? "I7c material capture contained no executable opaque PBR geometry; retrying. frame="
                        : "I7b material capture contained no executable opaque PBR geometry; retrying. frame=")
                    : "I5 material capture contained no executable opaque PBR geometry; retrying. frame=")
            << packet.frameId << LL_ENDL;
        sPendingMaterialBudgetLimited = false;
        return;
    }
    if (lightingOffscreenRequested())
    {
        sPendingLightingMaterial = packet;
        sPendingMaterialBudgetLimited = budget_limited;
        LL_INFOS("GHIIntegration")
            << (shadowOffscreenRequestedInternal()
                ? "I7d captured material packet for same-frame shadow/lighting pair: frame="
                : projectorLightingOffscreenRequested()
                ? "I7c captured material packet for same-frame projector-lighting pair: frame="
                : "I7b captured material packet for same-frame lighting pair: frame=")
            << packet.frameId << " draws=" << packet.draws.size()
            << " vertices=" << packet.vertices.size() << " indices="
            << packet.indices.size() << LL_ENDL;
        return;
    }
    const LL::GHI::MaterialOffscreenProbeLimits limits;
    const LL::GHI::Status status = sMaterialProbe->submit(packet, limits);
    if (!status)
    {
        LL_WARNS("GHIIntegration")
            << "I5 live material offscreen submission rejected at frame "
            << packet.frameId << ": " << status.message()
            << " attempt=" << sMaterialAttempts << LL_ENDL;
        if (status.code() == LL::GHI::StatusCode::DeviceLost)
        {
            sMaterialDisabled = true;
            LL_WARNS("GHIIntegration")
                << "I5 material probe disabled after device loss. The production OpenGL renderer remains active."
                << LL_ENDL;
        }
        return;
    }
    sPendingMaterialBudgetLimited = budget_limited;
    LL_INFOS("GHIIntegration")
        << "I5 live rigid/rigged opaque PBR offscreen submitted asynchronously: frame="
        << packet.frameId << " draws=" << packet.draws.size()
        << " vertices=" << packet.vertices.size() << " indices="
        << packet.indices.size()
        << ". Completion will be polled on later OpenGL frames." << LL_ENDL;
}

bool terrainCaptureRequested()
{
    return sVulkanDevice && !sTerrainDisabled &&
           ((frameAssemblyRequested() && !sFrameAssemblyDisabled) ||
            (sTerrainProbe &&
             (gSavedSettings.getBOOL("RenderVulkanTerrainOffscreenProbe") ||
              terrainLightingOffscreenRequested())));
}

bool shouldCaptureLiveTerrainPacket(std::uint64_t frame_id)
{
    pollProductionGBuffer();
    pollProductionLighting();
    pollTerrainProbe();
    if (!terrainCaptureRequested() || sTerrainCaptureClaimed ||
        (sTerrainProbe && sTerrainProbe->pending()) ||
        (sGBufferExecutor && sGBufferExecutor->pending()) ||
        (sProductionLightingExecutor &&
         sProductionLightingExecutor->pending()) ||
        sPendingLightingTerrain) return false;
    if (frameAssemblyRequested())
    {
        const std::uint32_t maximum = livePacketMaximum();
        if (!maximum || sFrameAssemblySamples >= maximum ||
            static_cast<std::uint64_t>(sFrameAssemblyAttempts) >=
                static_cast<std::uint64_t>(maximum) * 8ull)
            return false;
        if (!sNextFrameAssemblyFrame)
        {
            sNextFrameAssemblyFrame = frame_id + livePacketInterval();
            return false;
        }
        if (frame_id < sNextFrameAssemblyFrame) return false;
        sTerrainCaptureClaimed = true;
        return true;
    }
    const std::uint32_t maximum = livePacketMaximum();
    if (!maximum || sTerrainSamples >= maximum ||
        static_cast<std::uint64_t>(sTerrainAttempts) >=
            static_cast<std::uint64_t>(maximum) * 8ull)
        return false;
    if (!sNextTerrainFrame)
    {
        sNextTerrainFrame = frame_id + livePacketInterval();
        return false;
    }
    if (frame_id < sNextTerrainFrame) return false;
    sTerrainCaptureClaimed = true;
    return true;
}

void consumeLiveTerrainPacket(const LL::GHI::TerrainScenePacket& packet,
                              bool budget_limited)
{
    if (!sTerrainCaptureClaimed || !sVulkanDevice) return;
    sTerrainCaptureClaimed = false;
    if (frameAssemblyRequested())
    {
        sPendingLightingTerrain = packet;
        sPendingFrameTerrainBudgetLimited = budget_limited;
        LL_INFOS("GHIIntegration")
            << "I8a captured terrain component for production-frame assembly: frame="
            << packet.frameId << " draws=" << packet.draws.size()
            << " regions=" << packet.regions.size() << " vertices="
            << packet.vertices.size() << " indices=" << packet.indices.size()
            << " textures=" << packet.textures.size() << LL_ENDL;
        return;
    }
    ++sTerrainAttempts;
    sNextTerrainFrame = packet.frameId + livePacketInterval();
    if (packet.draws.empty())
    {
        LL_INFOS("GHIIntegration")
            << "I6 terrain capture contained no executable production faces; retrying. frame="
            << packet.frameId << LL_ENDL;
        return;
    }
    if (terrainLightingOffscreenRequested())
    {
        sPendingLightingTerrain = packet;
        sPendingTerrainBudgetLimited = budget_limited;
        LL_INFOS("GHIIntegration")
            << "I7b captured terrain packet for same-frame lighting pair: frame="
            << packet.frameId << " draws=" << packet.draws.size()
            << " regions=" << packet.regions.size() << " vertices="
            << packet.vertices.size() << " indices=" << packet.indices.size()
            << LL_ENDL;
        return;
    }
    const LL::GHI::TerrainOffscreenProbeLimits limits;
    const LL::GHI::Status status = sTerrainProbe->submit(packet, limits);
    if (!status)
    {
        LL_WARNS("GHIIntegration")
            << "I6 live terrain Vulkan offscreen submission rejected at frame " << packet.frameId
            << ": " << status.message() << LL_ENDL;
        if (status.code() == LL::GHI::StatusCode::DeviceLost)
        {
            sTerrainDisabled = true;
            LL_WARNS("GHIIntegration")
                << "I6 terrain probe disabled after device loss. The production OpenGL renderer remains active."
                << LL_ENDL;
        }
        return;
    }
    sPendingTerrainBudgetLimited = budget_limited;
    LL_INFOS("GHIIntegration")
        << "I6 live production terrain submitted asynchronously: frame="
        << packet.frameId << " draws=" << packet.draws.size()
        << " regions=" << packet.regions.size() << " vertices="
        << packet.vertices.size() << " indices=" << packet.indices.size()
        << " textures=" << packet.textures.size()
        << ". Completion will be polled on later OpenGL frames."
        << LL_ENDL;
}

bool lightingCaptureRequested()
{
    return sVulkanDevice && !sLightingDisabled &&
           ((frameAssemblyRequested() && !sFrameAssemblyDisabled) ||
            gSavedSettings.getBOOL("RenderVulkanLightingPacketProbe") ||
            lightingOffscreenRequested() ||
            terrainLightingOffscreenRequested());
}

bool shouldCaptureLiveLightingPacket(std::uint64_t frame_id)
{
    pollProductionGBuffer();
    pollProductionLighting();
    if (!lightingCaptureRequested() || sLightingCaptureClaimed) return false;
    if ((sGBufferExecutor && sGBufferExecutor->pending()) ||
        (sProductionLightingExecutor &&
         sProductionLightingExecutor->pending())) return false;
    if (frameAssemblyRequested())
    {
        const std::uint32_t maximum = livePacketMaximum();
        if (!maximum || sFrameAssemblySamples >= maximum ||
            static_cast<std::uint64_t>(sFrameAssemblyAttempts) >=
                static_cast<std::uint64_t>(maximum) * 8ull)
            return false;
        if (!sPendingFrameOpaque || !sPendingLightingMaterial ||
            !sPendingLightingTerrain)
            return false;
        const std::uint64_t opaqueFrame = sPendingFrameOpaque->frameId;
        const std::uint64_t materialFrame =
            sPendingLightingMaterial->frameId;
        const std::uint64_t terrainFrame = sPendingLightingTerrain->frameId;
        if (opaqueFrame != frame_id || materialFrame != frame_id ||
            terrainFrame != frame_id)
        {
            LL_WARNS("GHIIntegration")
                << "I8a discarded unpaired production-frame components: current="
                << frame_id << " opaque=" << opaqueFrame << " material="
                << materialFrame << " terrain=" << terrainFrame << LL_ENDL;
            sPendingFrameOpaque.reset();
            sPendingLightingMaterial.reset();
            sPendingLightingTerrain.reset();
            sPendingFrameOpaqueBudgetLimited = false;
            sPendingFrameMaterialBudgetLimited = false;
            sPendingFrameTerrainBudgetLimited = false;
            sNextFrameAssemblyFrame = frame_id + livePacketInterval();
            return false;
        }
        sLightingCaptureClaimed = true;
        return true;
    }
    const std::uint32_t maximum = livePacketMaximum();
    if (!maximum || sLightingSamples >= maximum ||
        static_cast<std::uint64_t>(sLightingAttempts) >=
        static_cast<std::uint64_t>(maximum) * 4ull)
        return false;
    if (lightingOffscreenRequested() || terrainLightingOffscreenRequested())
    {
        const std::uint64_t pendingFrame = lightingOffscreenRequested()
            ? (sPendingLightingMaterial ? sPendingLightingMaterial->frameId : 0)
            : (sPendingLightingTerrain ? sPendingLightingTerrain->frameId : 0);
        if (!pendingFrame) return false;
        if (pendingFrame != frame_id)
        {
            LL_WARNS("GHIIntegration")
                << (shadowOffscreenRequestedInternal() ? "I7d" :
                    projectorLightingOffscreenRequested() ? "I7c" : "I7b")
                << " discarded an unpaired "
                << (lightingOffscreenRequested() ? "material" : "terrain")
                << " packet from frame " << pendingFrame
                << " at frame " << frame_id << LL_ENDL;
            sPendingLightingMaterial.reset();
            sPendingLightingTerrain.reset();
            sPendingMaterialBudgetLimited = false;
            sPendingTerrainBudgetLimited = false;
            return false;
        }
        sLightingCaptureClaimed = true;
        return true;
    }
    if (!sNextLightingFrame)
    {
        sNextLightingFrame = frame_id + livePacketInterval();
        return false;
    }
    if (frame_id < sNextLightingFrame) return false;
    sLightingCaptureClaimed = true;
    return true;
}

void consumeLiveLightingPacket(const LL::GHI::LightingScenePacket& packet,
                               bool budget_limited)
{
    if (!sLightingCaptureClaimed || !sVulkanDevice) return;
    sLightingCaptureClaimed = false;
    if (frameAssemblyRequested())
    {
        ++sFrameAssemblyAttempts;
        sNextFrameAssemblyFrame = packet.frameId + livePacketInterval();
        if (!sPendingFrameOpaque || !sPendingLightingMaterial ||
            !sPendingLightingTerrain ||
            sPendingFrameOpaque->frameId != packet.frameId ||
            sPendingLightingMaterial->frameId != packet.frameId ||
            sPendingLightingTerrain->frameId != packet.frameId)
        {
            LL_WARNS("GHIIntegration")
                << "P0e1 rejected lighting without same-frame opaque, material, and terrain components: frame="
                << packet.frameId << LL_ENDL;
            sPendingFrameOpaque.reset();
            sPendingLightingMaterial.reset();
            sPendingLightingTerrain.reset();
            sPendingFrameOpaqueBudgetLimited = false;
            sPendingFrameMaterialBudgetLimited = false;
            sPendingFrameTerrainBudgetLimited = false;
            return;
        }

        LL::GHI::ProductionFramePacket frame;
        frame.frameId = packet.frameId;
        frame.assemblyEpoch = ++sFrameAssemblyEpoch;
        frame.sourceWidth = packet.sourceWidth;
        frame.sourceHeight = packet.sourceHeight;
        frame.passes =
            LL::GHI::productionFramePassBit(
                LL::GHI::ProductionFramePass::OpaqueGBuffer) |
            LL::GHI::productionFramePassBit(
                LL::GHI::ProductionFramePass::MaterialGBuffer) |
            LL::GHI::productionFramePassBit(
                LL::GHI::ProductionFramePass::TerrainGBuffer) |
            LL::GHI::productionFramePassBit(
                LL::GHI::ProductionFramePass::DeferredLighting);
        const bool projectorLighting = std::any_of(
            packet.localLights.begin(), packet.localLights.end(),
            [](const LL::GHI::LocalLightRecord& light)
            { return light.kind == LL::GHI::LocalLightKind::Projector; }) &&
            !packet.projectorTextures.empty();
        if (packet.shadows.enabled &&
            packet.shadows.directionalCascadeCount)
            frame.passes |= LL::GHI::productionFramePassBit(
                LL::GHI::ProductionFramePass::DirectionalShadow);
        if (projectorLighting)
            frame.passes |= LL::GHI::productionFramePassBit(
                LL::GHI::ProductionFramePass::ProjectorLighting);
        if (projectorLighting && packet.shadows.enabled &&
            packet.shadows.projectorShadowCount)
            frame.passes |= LL::GHI::productionFramePassBit(
                LL::GHI::ProductionFramePass::ProjectorShadow);
        frame.opaque = std::move(*sPendingFrameOpaque);
        frame.materials = std::move(*sPendingLightingMaterial);
        frame.terrain = std::move(*sPendingLightingTerrain);
        frame.lighting = packet;
        sPendingFrameOpaque.reset();
        sPendingLightingMaterial.reset();
        sPendingLightingTerrain.reset();

        const bool captureBudgetLimited = budget_limited ||
            sPendingFrameOpaqueBudgetLimited ||
            sPendingFrameMaterialBudgetLimited ||
            sPendingFrameTerrainBudgetLimited;
        sPendingFrameOpaqueBudgetLimited = false;
        sPendingFrameMaterialBudgetLimited = false;
        sPendingFrameTerrainBudgetLimited = false;
        if (textureResidencyRequested())
        {
            if (!sTextureResidency)
            {
                LL_WARNS("GHIIntegration")
                    << "I8b texture residency was requested without an active cache."
                    << LL_ENDL;
                sFrameAssemblyDisabled = true;
                return;
            }
            const LL::GHI::ProductionTextureResidencyLimits limits;
            LL::GHI::ProductionTextureResidencyResult result;
            const LL::GHI::Status status = sTextureResidency->update(
                frame, limits, result);
            if (!status)
            {
                LL_WARNS("GHIIntegration")
                    << "I8b production texture residency rejected frame "
                    << packet.frameId << ": " << status.message()
                    << " attempt=" << sFrameAssemblyAttempts << LL_ENDL;
                if (status.code() == LL::GHI::StatusCode::DeviceLost)
                    sFrameAssemblyDisabled = true;
                return;
            }
            LL::GHI::ProductionFrameTargetResult targetResult;
            if (frameGraphRequested())
            {
                if (!sFrameTargets)
                {
                    LL_WARNS("GHIIntegration")
                        << "I8c1 frame targets were requested without an active owner."
                        << LL_ENDL;
                    sFrameAssemblyDisabled = true;
                    return;
                }
                const LL::GHI::ProductionFrameTargetLimits targetLimits;
                const LL::GHI::Status targetStatus = sFrameTargets->ensure(
                    frame, targetLimits, targetResult);
                if (!targetStatus)
                {
                    LL_WARNS("GHIIntegration")
                        << "I8c1 production target topology rejected frame "
                        << packet.frameId << ": " << targetStatus.message()
                        << " attempt=" << sFrameAssemblyAttempts << LL_ENDL;
                    if (targetStatus.code() == LL::GHI::StatusCode::DeviceLost)
                        sFrameAssemblyDisabled = true;
                    return;
                }
            }
            if (gBufferExecutionRequested())
            {
                if (!sGBufferExecutor || !sFrameTargets)
                {
                    LL_WARNS("GHIIntegration")
                        << "I8c2 G-buffer execution was requested without an active executor and target owner."
                        << LL_ENDL;
                    sFrameAssemblyDisabled = true;
                    return;
                }
                const LL::GHI::ProductionGBufferLimits executionLimits;
                const LL::GHI::Status executionStatus =
                    sGBufferExecutor->submit(
                        frame, sFrameTargets->targets(),
                        *sTextureResidency, executionLimits);
                if (!executionStatus)
                {
                    LL_WARNS("GHIIntegration")
                        << "I8c2 production G-buffer submission rejected frame "
                        << packet.frameId << ": "
                        << executionStatus.message() << " attempt="
                        << sFrameAssemblyAttempts << LL_ENDL;
                    if (executionStatus.code() ==
                        LL::GHI::StatusCode::DeviceLost)
                        sFrameAssemblyDisabled = true;
                    return;
                }
                if (productionLightingExecutionRequested())
                {
                    if (!sProductionLightingExecutor)
                    {
                        LL_WARNS("GHIIntegration")
                            << "I8c3 shadow/lighting execution was requested without an active executor."
                            << LL_ENDL;
                        sFrameAssemblyDisabled = true;
                        return;
                    }
                    const LL::GHI::ProductionLightingLimits lightingLimits;
                    const LL::GHI::Status lightingStatus =
                        sProductionLightingExecutor->submit(
                            frame, sFrameTargets->targets(),
                            *sTextureResidency, lightingLimits);
                    if (!lightingStatus)
                    {
                        LL_WARNS("GHIIntegration")
                            << "I8c3 production shadow/lighting submission rejected frame "
                            << packet.frameId << ": "
                            << lightingStatus.message() << " attempt="
                            << sFrameAssemblyAttempts << LL_ENDL;
                        if (lightingStatus.code() ==
                            LL::GHI::StatusCode::DeviceLost)
                            sFrameAssemblyDisabled = true;
                        return;
                    }
                }
                sPendingProductionResidency = result;
                sPendingProductionBudgetLimited = captureBudgetLimited;
                LL_INFOS("GHIIntegration")
                    << (productionLightingExecutionRequested()
                        ? "I8c3 shared-target G-buffer, shadow, and lighting graph submitted asynchronously: frame="
                        : "I8c2 shared-target material and terrain G-buffer submitted asynchronously: frame=")
                    << packet.frameId << " assembly-epoch="
                    << frame.assemblyEpoch << " target-generation/extent="
                    << targetResult.targetGeneration << '/'
                    << targetResult.width << 'x' << targetResult.height
                    << " resident-entries/bytes=" << result.residentEntries
                    << '/' << result.residentBytes
                    << (productionLightingExecutionRequested()
                        ? ". Both ordered native submissions share the same frame identity and private targets; completion will be polled on later OpenGL frames and visible rendering remains OpenGL."
                        : ". Completion will be polled on later OpenGL frames; visible rendering remains OpenGL.")
                    << LL_ENDL;
                return;
            }
            ++sFrameAssemblySamples;
            const bool frameGraph = frameGraphRequested();
            LL_INFOS("GHIIntegration")
                << (frameGraph
                    ? "I8c1 shared production frame targets PASS: sample="
                    : "I8b retained production texture residency PASS: sample=")
                << sFrameAssemblySamples << '/' << livePacketMaximum()
                << " frame=" << result.frameId << " assembly-epoch="
                << result.assemblyEpoch
                << " sources/unique/deferred=" << result.requestedSources
                << '/' << result.uniqueContents << '/'
                << result.deferredSources << " hits/uploads/generation-changes/evictions="
                << result.cacheHits << '/' << result.uploads << '/'
                << result.generationChanges << '/' << result.evictions
                << " upload-bytes=" << result.uploadBytes
                << " resident-entries/bytes=" << result.residentEntries
                << '/' << result.residentBytes
                << " capture-budget-limited="
                << (captureBudgetLimited ? "yes" : "no");
            if (frameGraph)
                LL_CONT << " target-generation/extent/images/bytes/reused="
                        << targetResult.targetGeneration << '/'
                        << targetResult.width << 'x' << targetResult.height
                        << '/' << targetResult.imageCount << '/'
                        << targetResult.allocatedBytes << '/'
                        << (targetResult.reused ? "yes" : "no");
            LL_CONT
                << (frameGraph
                    ? ". The shared private attachment topology records no draw, surface, swapchain, or presentation; visible rendering remains OpenGL."
                    : ". Residency contains immutable decoded images only and records no draw, surface, swapchain, or presentation; visible rendering remains OpenGL.")
                << LL_ENDL;
            return;
        }
        else
        {
            const LL::GHI::ProductionFrameTransferLimits limits;
            LL::GHI::ProductionFrameTransferResult result;
            const LL::GHI::Status status =
                LL::GHI::consumeProductionFrameTransfer(
                    *sVulkanDevice, frame, limits, result);
            if (!status)
            {
                LL_WARNS("GHIIntegration")
                    << "I8a production-frame assembly transfer rejected at frame "
                    << packet.frameId << ": " << status.message()
                    << " attempt=" << sFrameAssemblyAttempts << LL_ENDL;
                if (status.code() == LL::GHI::StatusCode::DeviceLost)
                    sFrameAssemblyDisabled = true;
                return;
            }
            ++sFrameAssemblySamples;
            LL_INFOS("GHIIntegration")
                << "I8a same-frame production assembly/transfer PASS: sample="
                << sFrameAssemblySamples << '/' << livePacketMaximum()
                << " frame=" << result.frameId << " assembly-epoch="
                << result.assemblyEpoch << " pass-mask=0x" << std::hex
                << result.passes << std::dec
                << " draws(opaque/material/terrain)="
                << result.resources.opaqueDraws << '/'
                << result.resources.materialDraws << '/'
                << result.resources.terrainDraws << " vertices/indices="
                << result.resources.vertices << '/' << result.resources.indices
                << " resources(unique/material-textures/terrain-textures/projector-textures)="
                << result.resources.uniqueResources << '/'
                << result.resources.materialTextures << '/'
                << result.resources.terrainTextures << '/'
                << result.resources.projectorTextures << " decoded-bytes="
                << result.resources.decodedTextureBytes << " upload-bytes="
                << result.uploadBytes << " sha256=" << result.packetSha256
                << " capture-budget-limited="
                << (captureBudgetLimited ? "yes" : "no")
                << ". The assembled frame contains no native handles and records no draw, surface, swapchain, or presentation; visible rendering remains OpenGL."
                << LL_ENDL;
            return;
        }
    }
    ++sLightingAttempts;
    sNextLightingFrame = packet.frameId + livePacketInterval();
    if (lightingOffscreenRequested())
    {
        if (!sPendingLightingMaterial || !sMaterialProbe ||
            sPendingLightingMaterial->frameId != packet.frameId)
        {
            LL_WARNS("GHIIntegration")
                << (shadowOffscreenRequestedInternal()
                    ? "I7d rejected a lighting packet without its same-frame material/shadow pair: frame="
                    : projectorLightingOffscreenRequested()
                    ? "I7c rejected a lighting packet without its same-frame material pair: frame="
                    : "I7b rejected a lighting packet without its same-frame material pair: frame=")
                << packet.frameId << LL_ENDL;
            sPendingLightingMaterial.reset();
            sPendingMaterialBudgetLimited = false;
            return;
        }
        const LL::GHI::MaterialOffscreenProbeLimits limits;
        const LL::GHI::Status status = sMaterialProbe->submit(
            *sPendingLightingMaterial, packet, limits);
        const std::size_t draws = sPendingLightingMaterial->draws.size();
        sPendingLightingMaterial.reset();
        if (!status)
        {
            LL_WARNS("GHIIntegration")
                << (shadowOffscreenRequestedInternal()
                    ? "I7d same-frame shadow/lighting submission rejected at frame "
                    : projectorLightingOffscreenRequested()
                    ? "I7c same-frame projector-lighting submission rejected at frame "
                    : "I7b same-frame deferred-lighting submission rejected at frame ")
                << packet.frameId << ": " << status.message() << LL_ENDL;
            sPendingMaterialBudgetLimited = false;
            if (status.code() == LL::GHI::StatusCode::DeviceLost)
            {
                sMaterialDisabled = true;
                sLightingDisabled = true;
            }
            return;
        }
        sPendingMaterialBudgetLimited =
            sPendingMaterialBudgetLimited || budget_limited;
        LL_INFOS("GHIIntegration")
            << (shadowOffscreenRequestedInternal()
                ? "I7d same-frame native shadow/lighting submitted asynchronously: frame="
                : projectorLightingOffscreenRequested()
                ? "I7c same-frame native projector-lighting submitted asynchronously: frame="
                : "I7b same-frame native deferred-lighting submitted asynchronously: frame=")
            << packet.frameId << " draws=" << draws << " local-lights="
            << packet.localLights.size() << " projector-images="
            << packet.projectorTextures.size()
            << ". Completion will be polled on later OpenGL frames."
            << LL_ENDL;
        return;
    }
    if (terrainLightingOffscreenRequested())
    {
        if (!sPendingLightingTerrain || !sTerrainProbe ||
            sPendingLightingTerrain->frameId != packet.frameId)
        {
            LL_WARNS("GHIIntegration")
                << "I7b rejected a lighting packet without its same-frame terrain pair: frame="
                << packet.frameId << LL_ENDL;
            sPendingLightingTerrain.reset();
            sPendingTerrainBudgetLimited = false;
            return;
        }
        const LL::GHI::TerrainOffscreenProbeLimits limits;
        const LL::GHI::Status status = sTerrainProbe->submit(
            *sPendingLightingTerrain, packet, limits);
        const std::size_t draws = sPendingLightingTerrain->draws.size();
        const std::size_t regions = sPendingLightingTerrain->regions.size();
        sPendingLightingTerrain.reset();
        if (!status)
        {
            LL_WARNS("GHIIntegration")
                << "I7b same-frame terrain deferred-lighting submission rejected at frame "
                << packet.frameId << ": " << status.message() << LL_ENDL;
            sPendingTerrainBudgetLimited = false;
            if (status.code() == LL::GHI::StatusCode::DeviceLost)
            {
                sTerrainDisabled = true;
                sLightingDisabled = true;
            }
            return;
        }
        sPendingTerrainBudgetLimited =
            sPendingTerrainBudgetLimited || budget_limited;
        LL_INFOS("GHIIntegration")
            << "I7b same-frame native terrain deferred-lighting submitted asynchronously: frame="
            << packet.frameId << " draws=" << draws << " regions="
            << regions << " local-lights=" << packet.localLights.size()
            << ". Completion will be polled on later OpenGL frames."
            << LL_ENDL;
        return;
    }
    const LL::GHI::LightingPacketTransferLimits limits;
    LL::GHI::LightingPacketTransferResult result;
    const LL::GHI::Status status = LL::GHI::consumeLightingPacketTransfer(
        *sVulkanDevice, packet, limits, result);
    if (!status)
    {
        LL_WARNS("GHIIntegration")
            << "I7a live lighting packet transfer rejected at frame "
            << packet.frameId << ": " << status.message() << LL_ENDL;
        if (status.code() == LL::GHI::StatusCode::DeviceLost)
        {
            sLightingDisabled = true;
            LL_WARNS("GHIIntegration")
                << "I7a lighting transfer disabled after device loss. The production OpenGL renderer remains active."
                << LL_ENDL;
        }
        return;
    }
    ++sLightingSamples;
    LL_INFOS("GHIIntegration")
        << "I7a live deferred-lighting packet transfer PASS: sample="
        << sLightingSamples << '/' << livePacketMaximum() << " frame="
        << result.frameId << " scene-epoch=" << result.sceneEpoch
        << " resource-epoch=" << result.resourceEpoch << " local-lights="
        << result.localLights << " projectors=" << result.projectorLights
        << " shadow-cascades=" << result.shadowCascades
        << " upload-bytes=" << result.uploadBytes << " sha256="
        << result.packetSha256 << " capture-budget-limited="
        << (budget_limited ? "yes" : "no")
        << ". No shadow image, draw, surface, swapchain, or presentation operation was used; visible rendering remains OpenGL."
        << LL_ENDL;
}

} // namespace LLGHIRuntime
