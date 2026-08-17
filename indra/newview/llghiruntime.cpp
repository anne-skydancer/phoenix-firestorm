/**
 * @file llghiruntime.cpp
 * @brief Developer-gated native Vulkan coexistence lifetime.
 */

#include "llviewerprecompiledheaders.h"

#include "llghiruntime.h"

#include "lldir.h"
#include "llviewercontrol.h"
#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghidevice.h"
#include "ghi/include/llghiopaqueoffscreenprobe.h"
#include "ghi/include/llghiopaquepacketconsumer.h"
#include "ghi/include/llghimaterialoffscreenprobe.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace
{
std::unique_ptr<LL::GHI::Device> sVulkanDevice;
std::unique_ptr<LL::GHI::OpaqueOffscreenProbe> sOffscreenProbe;
std::unique_ptr<LL::GHI::MaterialOffscreenProbe> sMaterialProbe;
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

bool offscreenProbeRequested()
{
    return gSavedSettings.getBOOL("RenderVulkanOffscreenPacketProbe");
}

bool livePacketRequested()
{
    return gSavedSettings.getBOOL("RenderVulkanLivePacketProbe") ||
           offscreenProbeRequested();
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
                << "I3 live material offscreen poll failed: "
                << status.message() << LL_ENDL;
            sMaterialDisabled = true;
        }
        return;
    }
    ++sMaterialSamples;
    LL_INFOS("GHIIntegration")
        << "I3 live rigid opaque PBR offscreen PASS: sample="
        << sMaterialSamples << '/' << livePacketMaximum()
        << " frame=" << result.frameId << " draws=" << result.draws
        << " vertices=" << result.vertices << " indices=" << result.indices
        << " textures=" << result.textures << " packet-sha256="
        << result.packetSha256 << " color-sha256="
        << result.colorSha256[0] << ',' << result.colorSha256[1] << ','
        << result.colorSha256[2] << ',' << result.colorSha256[3]
        << " non-clear-pixels=" << result.nonClearPixels[0] << ','
        << result.nonClearPixels[1] << ',' << result.nonClearPixels[2] << ','
        << result.nonClearPixels[3] << " capture-budget-limited="
        << (sPendingMaterialBudgetLimited ? "yes" : "no")
        << ". Alpha, rigged, HUD, mirror, cube-snapshot, and presentation paths remain excluded; visible rendering remains OpenGL."
        << LL_ENDL;
    sPendingMaterialBudgetLimited = false;
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

    if (gSavedSettings.getBOOL("RenderVulkanMaterialOffscreenProbe"))
    {
        const std::string packagePath = gDirUtilp->getExpandedFilename(
            LL_PATH_APP_SETTINGS, "ghi_shaders", "r5_material_skin.llghisp");
        LL::GHI::ShaderPackageDesc shaderPackage;
        const LL::GHI::Status status = LL::GHI::loadShaderPackage(
            packagePath, shaderPackage);
        if (!status)
        {
            LL_WARNS("GHIIntegration")
                << "I3 runtime material shader package was not loaded from "
                << packagePath << ": " << status.message()
                << ". Visible rendering remains OpenGL." << LL_ENDL;
        }
        else
        {
            sMaterialProbe =
                std::make_unique<LL::GHI::MaterialOffscreenProbe>(
                    *sVulkanDevice, std::move(shaderPackage));
            const LL::GHI::MaterialOffscreenProbeLimits limits;
            LL_INFOS("GHIIntegration")
                << "I3 asynchronous rigid opaque PBR material probe armed from "
                << packagePath << "; extent=256x256 limits(draws/vertices/indices/textures/bytes)="
                << limits.maxDraws << '/' << limits.maxVertices << '/'
                << limits.maxIndices << '/' << limits.maxTextures << '/'
                << limits.maxUploadBytes
                << ". The probe owns no surface, swapchain, or presentation path."
                << LL_ENDL;
        }
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
                << "I3 material offscreen resources did not retire cleanly: "
                << probeStatus.message() << LL_ENDL;
        sMaterialProbe.reset();
    }
    const LL::GHI::Status status = sVulkanDevice->waitIdle();
    if (!status)
    {
        LL_WARNS("GHIIntegration")
            << "Native Vulkan coexistence device did not become idle during shutdown: "
            << status.message() << LL_ENDL;
    }
    sVulkanDevice.reset();
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
    return sVulkanDevice && !sMaterialDisabled && sMaterialProbe &&
           gSavedSettings.getBOOL("RenderVulkanMaterialOffscreenProbe");
}

bool shouldCaptureLiveMaterialPacket(std::uint64_t frame_id)
{
    pollMaterialProbe();
    if (!materialCaptureRequested() || sMaterialCaptureClaimed ||
        sMaterialProbe->pending())
        return false;
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
    if (!sMaterialCaptureClaimed || !sMaterialProbe) return;
    sMaterialCaptureClaimed = false;
    ++sMaterialAttempts;
    sNextMaterialFrame = packet.frameId + livePacketInterval();
    const LL::GHI::MaterialOffscreenProbeLimits limits;
    const LL::GHI::Status status = sMaterialProbe->submit(packet, limits);
    if (!status)
    {
        LL_WARNS("GHIIntegration")
            << "I3 live material offscreen submission rejected at frame "
            << packet.frameId << ": " << status.message()
            << " attempt=" << sMaterialAttempts << LL_ENDL;
        if (status.code() == LL::GHI::StatusCode::DeviceLost)
        {
            sMaterialDisabled = true;
            LL_WARNS("GHIIntegration")
                << "I3 material probe disabled after device loss. The production OpenGL renderer remains active."
                << LL_ENDL;
        }
        return;
    }
    sPendingMaterialBudgetLimited = budget_limited;
    LL_INFOS("GHIIntegration")
        << "I3 live rigid opaque PBR offscreen submitted asynchronously: frame="
        << packet.frameId << " draws=" << packet.draws.size()
        << " vertices=" << packet.vertices.size() << " indices="
        << packet.indices.size()
        << ". Completion will be polled on later OpenGL frames." << LL_ENDL;
}

} // namespace LLGHIRuntime
