/**
 * @file llghiterrainoffscreenprobe.cpp
 * @brief I6 asynchronous native terrain execution probe.
 */

#include "linden_common.h"

#include "ghi/include/llghiterrainoffscreenprobe.h"
#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace LL::GHI
{
namespace
{
constexpr std::uint32_t WIDTH = 256, HEIGHT = 256;
constexpr std::size_t UNIFORM_FLOATS = 132;
constexpr std::size_t UNIFORM_BYTES = UNIFORM_FLOATS * sizeof(float);
constexpr std::array<Format, 4> COLOR_FORMATS{{
    Format::RGBA8UNorm, Format::RGBA8UNorm,
    Format::RGBA16UNorm, Format::RGBA16Float}};
constexpr std::array<std::uint32_t, 4> COLOR_BYTES{{4, 4, 8, 8}};
constexpr Format LIGHTING_FORMAT = Format::RGBA16Float;
constexpr std::uint32_t LIGHTING_BYTES = 8;
constexpr std::size_t SHADOW_MAP_COUNT =
    LIGHTING_DIRECTIONAL_SHADOW_CASCADES;
constexpr Format SHADOW_FORMAT = Format::Depth32Float;
constexpr std::size_t MAX_LIGHTING_POINT_LIGHTS = 64;
constexpr std::size_t LIGHTING_HEADER_FLOATS = 16 * 2 + 4 * 6;
constexpr std::size_t LIGHTING_POINT_DATA_FLOATS =
    LIGHTING_HEADER_FLOATS + MAX_LIGHTING_POINT_LIGHTS * 8;
constexpr std::size_t LIGHTING_DATA_FLOATS = LIGHTING_POINT_DATA_FLOATS +
    LIGHTING_DIRECTIONAL_SHADOW_CASCADES * 16 + 8;
using LightingData = std::array<float, LIGHTING_DATA_FLOATS>;

Status invalid(const char* text)
{
    return Status::failure(StatusCode::InvalidArgument, text);
}

LightingData makeLightingData(const LightingScenePacket& packet,
                              std::uint32_t maxPointLights,
                              std::uint32_t& directionalLights,
                              std::uint32_t& pointLights)
{
    LightingData data{};
    std::copy(packet.viewMatrix.begin(), packet.viewMatrix.end(), data.begin());
    std::copy(packet.projectionMatrix.begin(), packet.projectionMatrix.end(),
              data.begin() + 16);
    std::copy(packet.cameraOrigin.begin(), packet.cameraOrigin.end(),
              data.begin() + 32);
    std::copy(packet.ambientColor.begin(), packet.ambientColor.end(),
              data.begin() + 36);
    auto copyDirectional = [&data, &directionalLights](
        const DirectionalLightRecord& light, std::size_t offset)
    {
        std::copy(light.direction.begin(), light.direction.end(),
                  data.begin() + static_cast<std::ptrdiff_t>(offset));
        data[offset + 3] = light.active ? 1.f : 0.f;
        std::copy(light.color.begin(), light.color.end(),
                  data.begin() + static_cast<std::ptrdiff_t>(offset + 4));
        data[offset + 7] = light.intensity;
        if (light.active) ++directionalLights;
    };
    copyDirectional(packet.sun, 40);
    copyDirectional(packet.moon, 48);
    const std::uint32_t limit = std::min<std::uint32_t>(
        maxPointLights, static_cast<std::uint32_t>(MAX_LIGHTING_POINT_LIGHTS));
    for (const LocalLightRecord& light : packet.localLights)
    {
        if (light.kind != LocalLightKind::Point || pointLights == limit) continue;
        const std::size_t position = LIGHTING_HEADER_FLOATS + pointLights * 4;
        const std::size_t color = LIGHTING_HEADER_FLOATS +
                                  MAX_LIGHTING_POINT_LIGHTS * 4 +
                                  pointLights * 4;
        std::copy(light.position.begin(), light.position.end(),
                  data.begin() + static_cast<std::ptrdiff_t>(position));
        data[position + 3] = light.radius;
        std::copy(light.color.begin(), light.color.end(),
                  data.begin() + static_cast<std::ptrdiff_t>(color));
        data[color + 3] = light.falloff;
        ++pointLights;
    }
    data[35] = static_cast<float>(pointLights);
    const std::size_t shadowMatrices = LIGHTING_POINT_DATA_FLOATS;
    for (std::size_t cascade = 0;
         cascade < LIGHTING_DIRECTIONAL_SHADOW_CASCADES; ++cascade)
        std::copy(packet.shadows.matrices[cascade].begin(),
                  packet.shadows.matrices[cascade].end(),
                  data.begin() + static_cast<std::ptrdiff_t>(
                      shadowMatrices + cascade * 16));
    const std::size_t shadowClip = shadowMatrices +
        LIGHTING_DIRECTIONAL_SHADOW_CASCADES * 16;
    std::copy(packet.shadows.clipPlanes.begin(),
              packet.shadows.clipPlanes.end(),
              data.begin() + static_cast<std::ptrdiff_t>(shadowClip));
    data[shadowClip + 4] = packet.shadows.directionalBias;
    // Terrain I7b does not execute the I7d caster pass. Keep native shadow
    // sampling explicitly disabled while still satisfying the shared shader
    // package's complete descriptor contract.
    data[shadowClip + 5] = 0.f;
    data[shadowClip + 6] = 0.f;
    return data;
}

bool comparable(const MaterialTextureResource& texture)
{
    return texture.comparability == ResourceComparability::Comparable &&
        !texture.decodedPixels.empty() && texture.width && texture.height &&
        texture.components >= 1 && texture.components <= 4;
}

std::vector<std::byte> rgba(const TerrainScenePacket& packet,
                            std::uint32_t textureIndex,
                            const TerrainOffscreenProbeLimits& limits,
                            std::uint32_t& width, std::uint32_t& height,
                            Status& status)
{
    if (textureIndex >= packet.textures.size())
    {
        status = invalid("terrain draw references an absent texture");
        return {};
    }
    const auto& texture = packet.textures[textureIndex];
    if (!comparable(texture))
    {
        status = invalid("terrain texture has no comparable decoded pixels");
        return {};
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(texture.width) *
                                texture.height * 4;
    if (bytes > limits.maxTextureBytes ||
        bytes > std::numeric_limits<std::size_t>::max())
    {
        status = invalid("terrain texture exceeds per-image limit");
        return {};
    }
    width = texture.width;
    height = texture.height;
    std::vector<std::byte> output(static_cast<std::size_t>(bytes));
    for (std::uint64_t pixel = 0;
         pixel < static_cast<std::uint64_t>(width) * height; ++pixel)
    {
        const std::size_t source = static_cast<std::size_t>(pixel) * texture.components;
        const std::size_t target = static_cast<std::size_t>(pixel) * 4;
        const std::byte luma = texture.decodedPixels[source];
        output[target] = luma;
        output[target + 1] = texture.components < 3
            ? luma : texture.decodedPixels[source + 1];
        output[target + 2] = texture.components < 3
            ? luma : texture.decodedPixels[source + 2];
        output[target + 3] = texture.components == 2
            ? texture.decodedPixels[source + 1]
            : texture.components == 4 ? texture.decodedPixels[source + 3]
                                      : std::byte{255};
    }
    return output;
}

bool normalMatrix(const std::array<float, 16>& model,
                  std::array<float, 16>& output)
{
    const double a00=model[0],a01=model[4],a02=model[8];
    const double a10=model[1],a11=model[5],a12=model[9];
    const double a20=model[2],a21=model[6],a22=model[10];
    const double c00=a11*a22-a12*a21,c01=a12*a20-a10*a22,c02=a10*a21-a11*a20;
    const double c10=a02*a21-a01*a22,c11=a00*a22-a02*a20,c12=a01*a20-a00*a21;
    const double c20=a01*a12-a02*a11,c21=a02*a10-a00*a12,c22=a00*a11-a01*a10;
    const double det=a00*c00+a01*c01+a02*c02;
    if (!std::isfinite(det) || std::abs(det) < 1.e-12) return false;
    output = {{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}};
    const double inv=1.0/det;
    const std::array<double,9> n{{c00*inv,c10*inv,c20*inv,c01*inv,c11*inv,
                                  c21*inv,c02*inv,c12*inv,c22*inv}};
    constexpr std::array<std::size_t,9> at{{0,1,2,4,5,6,8,9,10}};
    for (std::size_t i=0;i<n.size();++i)
    {
        if (!std::isfinite(n[i])) return false;
        output[at[i]]=static_cast<float>(n[i]);
    }
    return true;
}
} // namespace

class TerrainOffscreenProbe::Impl
{
public:
    Impl(Device& device, ShaderPackageDesc package,
         std::optional<ShaderPackageDesc> lightingPackage = std::nullopt) :
        mDevice(device), mPackage(std::move(package)),
        mLightingPackage(std::move(lightingPackage)) {}
    ~Impl() { shutdown(); }

    Status submit(const TerrainScenePacket& packet,
                  const TerrainOffscreenProbeLimits& limits)
    {
        return submitImpl(packet, nullptr, limits);
    }

    Status submit(const TerrainScenePacket& terrainPacket,
                  const LightingScenePacket& lightingPacket,
                  const TerrainOffscreenProbeLimits& limits)
    {
        return submitImpl(terrainPacket, &lightingPacket, limits);
    }

private:
    Status submitImpl(const TerrainScenePacket& packet,
                      const LightingScenePacket* lightingPacket,
                      const TerrainOffscreenProbeLimits& limits)
    {
        if (mPending) return Status::failure(
            StatusCode::NotReady, "terrain offscreen probe is still pending");
        if (!limits.maxDraws || !limits.maxVertices || !limits.maxIndices ||
            !limits.maxTextures || !limits.maxUploadBytes || !limits.maxTextureBytes)
            return invalid("terrain offscreen limits must be nonzero");
        if (packet.vertices.empty() || packet.indices.empty() || packet.draws.empty())
            return invalid("terrain packet contains no drawable geometry");
        if (packet.vertices.size() > limits.maxVertices ||
            packet.indices.size() > limits.maxIndices)
            return invalid("terrain packet exceeds geometry limits");
        if (lightingPacket)
        {
            if (!mLightingPackage)
                return invalid("terrain probe was not configured for deferred lighting");
            if (lightingPacket->frameId != packet.frameId)
                return invalid("terrain and lighting packets are not from the same frame");
            if (lightingPacket->sourceWidth != packet.sourceWidth ||
                lightingPacket->sourceHeight != packet.sourceHeight)
                return invalid("terrain and lighting packet extents do not match");
        }

        std::vector<std::size_t> selected;
        for (std::size_t i=0;i<packet.draws.size();++i)
        {
            const auto& draw=packet.draws[i];
            if (!draw.indexCount || draw.region >= packet.regions.size()) continue;
            const auto& region=packet.regions[draw.region];
            if (draw.comparability != ResourceComparability::Comparable ||
                region.comparability != ResourceComparability::Comparable ||
                region.compositionTexture >= packet.textures.size() ||
                !comparable(packet.textures[region.compositionTexture])) continue;
            bool ready=true;
            for (const auto& layer:region.layers)
                ready = ready && layer.baseColorTexture < packet.textures.size() &&
                    comparable(packet.textures[layer.baseColorTexture]);
            if (ready) selected.push_back(i);
        }
        const std::size_t maxByTextures=limits.maxTextures/5;
        if (selected.size() > limits.maxDraws) selected.resize(limits.maxDraws);
        if (selected.size() > maxByTextures) selected.resize(maxByTextures);
        if (selected.empty()) return invalid(
            "terrain packet has no comparable executable base-composition draw");
        Status status=initialize();
        if (!status) return status;

        const std::uint64_t alignment=std::max<std::uint64_t>(
            16,mDevice.capabilities().uniformBufferOffsetAlignment);
        const auto align=[alignment](std::uint64_t value)
        { return (value+alignment-1)/alignment*alignment; };
        const std::uint64_t vertexBytes=packet.vertices.size()*sizeof(TerrainSceneVertex);
        const std::uint64_t indexBytes=packet.indices.size()*sizeof(std::uint32_t);
        const std::uint64_t vertexOffset=0,indexOffset=align(vertexBytes);
        const std::uint64_t uniformOffset=align(indexOffset+indexBytes);
        const std::uint64_t uniformStride=align(UNIFORM_BYTES);
        std::uint64_t next=align(uniformOffset+uniformStride*selected.size());

        struct DrawResources
        {
            std::size_t source=0;
            std::array<std::vector<std::byte>,5> pixels;
            std::array<std::uint32_t,5> widths{},heights{};
            std::array<std::uint64_t,5> offsets{};
            std::array<ImageHandle,5> images{};
            std::array<ImageViewHandle,5> views{};
            BindingSetHandle set;
        };
        std::vector<DrawResources> draws(selected.size());
        for (std::size_t item=0;item<selected.size();++item)
        {
            auto& resources=draws[item]; resources.source=selected[item];
            const auto& region=packet.regions[packet.draws[selected[item]].region];
            const std::array<std::uint32_t,5> textureIndices{{
                region.compositionTexture,region.layers[0].baseColorTexture,
                region.layers[1].baseColorTexture,region.layers[2].baseColorTexture,
                region.layers[3].baseColorTexture}};
            for (std::size_t texture=0;texture<5;++texture)
            {
                resources.pixels[texture]=rgba(packet,textureIndices[texture],limits,
                    resources.widths[texture],resources.heights[texture],status);
                if (!status) return status;
                resources.offsets[texture]=next;
                next=align(next+resources.pixels[texture].size());
            }
        }
        std::uint32_t directionalLights = 0;
        std::uint32_t pointLights = 0;
        LightingData lightingData{};
        const std::uint64_t lightingOffset = align(next);
        if (lightingPacket)
        {
            lightingData = makeLightingData(*lightingPacket,
                limits.maxPointLights, directionalLights, pointLights);
            next = align(lightingOffset + sizeof(lightingData));
        }
        if (next > limits.maxUploadBytes || next > mDevice.capabilities().maxBufferSize)
            return invalid("terrain packet exceeds upload byte limit");
        std::vector<std::byte> uploadData(static_cast<std::size_t>(next));
        std::memcpy(uploadData.data()+vertexOffset,packet.vertices.data(),vertexBytes);
        std::memcpy(uploadData.data()+indexOffset,packet.indices.data(),indexBytes);
        for (std::size_t item=0;item<draws.size();++item)
        {
            const auto& draw=packet.draws[draws[item].source];
            const auto& region=packet.regions[draw.region];
            std::array<float,UNIFORM_FLOATS> uniform{};
            std::copy(draw.viewProjection.begin(),draw.viewProjection.end(),uniform.begin());
            std::copy(draw.modelTransform.begin(),draw.modelTransform.end(),uniform.begin()+16);
            std::array<float,16> normal{};
            if (!normalMatrix(draw.modelTransform,normal))
                return invalid("terrain draw has singular model transform");
            std::copy(normal.begin(),normal.end(),uniform.begin()+32);
            for (std::size_t layer=0;layer<4;++layer)
            {
                const auto& source=region.layers[layer];
                const std::size_t uv=48+layer*4,rotation=64+layer*4;
                uniform[uv]=source.transform[0]; uniform[uv+1]=source.transform[1];
                uniform[uv+2]=source.transform[2]; uniform[uv+3]=source.transform[3];
                uniform[rotation]=std::cos(source.transform[4]);
                uniform[rotation+1]=std::sin(source.transform[4]);
                std::copy(source.baseColor.begin(),source.baseColor.end(),uniform.begin()+80+layer*4);
                uniform[96+layer*4]=source.emissive[0];
                uniform[97+layer*4]=source.emissive[1];
                uniform[98+layer*4]=source.emissive[2];
                uniform[99+layer*4]=source.metallic;
                uniform[112+layer*4]=source.roughness;
                uniform[113+layer*4]=source.alphaCutoff;
            }
            uniform[128]=region.regionScale;
            uniform[129]=region.model==MaterialModel::MetallicRoughness?1.f:0.f;
            uniform[130]=region.paintMode==TerrainPaintMode::PBRPaintMap?1.f:0.f;
            uniform[131]=static_cast<float>(region.projection==TerrainProjection::Triplanar?3:1);
            std::memcpy(uploadData.data()+uniformOffset+uniformStride*item,
                        uniform.data(),UNIFORM_BYTES);
            for (std::size_t texture=0;texture<5;++texture)
                std::memcpy(uploadData.data()+draws[item].offsets[texture],
                    draws[item].pixels[texture].data(),draws[item].pixels[texture].size());
        }
        if (lightingPacket)
            std::memcpy(uploadData.data() + lightingOffset,
                        lightingData.data(), sizeof(lightingData));

        BufferHandle upload,vertices,indices,uniforms,lightingBuffer;
        BindingSetHandle lightingSet;
        auto cleanup=[&]()
        {
            Status first=Status::success();
            for(auto& draw:draws){destroy(draw.set,first);for(std::size_t i=0;i<5;++i){destroy(draw.views[i],first);destroy(draw.images[i],first);}}
            destroy(upload,first);destroy(vertices,first);destroy(indices,first);destroy(uniforms,first);
            destroy(lightingSet,first);destroy(lightingBuffer,first);return first;
        };
        upload=mDevice.createBuffer({next,ResourceUsage::TransferSource,MemoryClass::Upload},status);
        if(status)vertices=mDevice.createBuffer({vertexBytes,ResourceUsage::Vertex|ResourceUsage::TransferDestination,MemoryClass::DeviceLocal},status);
        if(status)indices=mDevice.createBuffer({indexBytes,ResourceUsage::Index|ResourceUsage::TransferDestination,MemoryClass::DeviceLocal},status);
        if(status)uniforms=mDevice.createBuffer({uniformStride*draws.size(),ResourceUsage::Uniform|ResourceUsage::TransferDestination,MemoryClass::DeviceLocal},status);
        if(status&&lightingPacket)lightingBuffer=mDevice.createBuffer({sizeof(lightingData),ResourceUsage::Uniform|ResourceUsage::TransferDestination,MemoryClass::DeviceLocal},status);
        if(!status || !(status=mDevice.writeBuffer(upload,0,uploadData))){cleanup();return status;}
        for(std::size_t item=0;status&&item<draws.size();++item)
        {
            BindingSetDesc desc;desc.shader=mShader;desc.group=0;
            desc.resources.push_back({0,0,ShaderPackageDesc::BindingType::UniformBuffer,
                uniforms,uniformStride*item,UNIFORM_BYTES,{},{}});
            for(std::size_t texture=0;status&&texture<5;++texture)
            {
                const Format format=texture?Format::RGBA8SRGB:Format::RGBA8UNorm;
                draws[item].images[texture]=mDevice.createImage(
                    {{draws[item].widths[texture],draws[item].heights[texture],1},format,
                     ResourceUsage::Sampled|ResourceUsage::TransferDestination,1,1,1},status);
                if(status)draws[item].views[texture]=mDevice.createImageView(
                    {draws[item].images[texture],format,{ImageAspect::Color,0,1,0,1}},status);
                if(status)desc.resources.push_back({static_cast<std::uint16_t>(texture+1),0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,{},0,0,
                    draws[item].views[texture],texture?mRepeatSampler:mClampSampler});
            }
            if(status)draws[item].set=mDevice.createBindingSet(desc,status);
        }
        if(status&&lightingPacket)
        {
            BindingSetDesc desc;desc.shader=mLightingShader;desc.group=0;
            desc.resources.push_back({0,0,ShaderPackageDesc::BindingType::UniformBuffer,
                lightingBuffer,0,sizeof(lightingData),{},{}});
            for(std::size_t target=0;target<4;++target)
                desc.resources.push_back({static_cast<std::uint16_t>(target+1),0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,{},0,0,
                    mColorViews[target],mLightingSampler});
            desc.resources.push_back({5,0,
                ShaderPackageDesc::BindingType::CombinedImageSampler,{},0,0,
                mDepthSampleView,mLightingSampler});
            for(std::size_t shadow=0;shadow<SHADOW_MAP_COUNT;++shadow)
                desc.resources.push_back({static_cast<std::uint16_t>(shadow+6),0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,{},0,0,
                    mShadowViews[shadow],mLightingSampler});
            lightingSet=mDevice.createBindingSet(desc,status);
        }
        if(!status){cleanup();return status;}
        CommandContext& commands=mDevice.commandContext();bool begun=false,rendering=false;
        status=commands.beginFrame();begun=status.ok();
        const std::array<BufferCopyRegion,1> vc{{{vertexOffset,0,vertexBytes}}},ic{{{indexOffset,0,indexBytes}}},uc{{{uniformOffset,0,uniformStride*draws.size()}}},lc{{{lightingOffset,0,sizeof(lightingData)}}};
        if(status)status=commands.copyBuffer(upload,vertices,vc);if(status)status=commands.copyBuffer(upload,indices,ic);if(status)status=commands.copyBuffer(upload,uniforms,uc);
        if(status&&lightingPacket)status=commands.copyBuffer(upload,lightingBuffer,lc);
        for(auto& draw:draws)for(std::size_t texture=0;status&&texture<5;++texture){BufferImageCopyRegion copy;copy.bufferOffset=draw.offsets[texture];copy.imageSubresource={ImageAspect::Color,0,0,1};copy.imageExtent={draw.widths[texture],draw.heights[texture],1};const std::array<BufferImageCopyRegion,1> copies{{copy}};status=commands.copyBufferToImage(upload,draw.images[texture],copies);}
        RenderingInfo info;info.semanticId=0x49365f544552524eull;info.width=WIDTH;info.height=HEIGHT;
        for(std::size_t target=0;target<4;++target)info.colors.push_back({mColorViews[target],COLOR_FORMATS[target],LoadOp::Clear,StoreOp::Store,{{0,0,0,0},0,0}});
        info.depthStencil=AttachmentDesc{mDepthView,mDepthFormat,LoadOp::Clear,StoreOp::Store,{{0,0,0,0},0,0}};
        if(status){status=commands.beginRendering(info);rendering=status.ok();}
        if(status)status=commands.bindPipeline(mPipeline);if(status)status=commands.setViewport({0,0,float(WIDTH),float(HEIGHT),0,1});if(status)status=commands.setScissor({0,0,WIDTH,HEIGHT});if(status)status=commands.bindVertexBuffer(0,vertices,0);if(status)status=commands.bindIndexBuffer(indices,0,IndexType::UInt32);
        for(auto& resources:draws)if(status){const auto& draw=packet.draws[resources.source];status=commands.bindBindingSet(0,resources.set);if(status)status=commands.drawIndexed({draw.indexCount,1,draw.firstIndex,0,0});}
        if(rendering){const Status ended=commands.endRendering();if(status&&!ended)status=ended;}
        if(status&&lightingPacket)
        {
            status=commands.resourceBarrier(ResourceBarrier::ColorAttachmentWriteToSampledRead);
            if(status)status=commands.resourceBarrier(ResourceBarrier::DepthAttachmentWriteToSampledRead);
            for(std::size_t shadow=0;status&&shadow<SHADOW_MAP_COUNT;++shadow)
            {
                RenderingInfo clearShadow;clearShadow.semanticId=0x4937625f53484430ull+shadow;clearShadow.width=WIDTH;clearShadow.height=HEIGHT;
                clearShadow.depthStencil=AttachmentDesc{mShadowViews[shadow],SHADOW_FORMAT,LoadOp::Clear,StoreOp::Store,{{0.f,0.f,0.f,0.f},1.f,0}};
                bool shadowBegun=false;status=commands.beginRendering(clearShadow);shadowBegun=status.ok();
                if(shadowBegun){const Status ended=commands.endRendering();if(status&&!ended)status=ended;}
            }
            if(status)status=commands.resourceBarrier(ResourceBarrier::DepthAttachmentWriteToSampledRead);
            RenderingInfo lightingInfo;lightingInfo.semanticId=0x4937625f54455252ull;lightingInfo.width=WIDTH;lightingInfo.height=HEIGHT;
            lightingInfo.colors.push_back({mLightingView,LIGHTING_FORMAT,LoadOp::Clear,StoreOp::Store,{{0,0,0,0},0,0}});
            bool lightingBegun=false;
            if(status){status=commands.beginRendering(lightingInfo);lightingBegun=status.ok();}
            if(status)status=commands.setViewport({0,0,float(WIDTH),float(HEIGHT),0,1});
            if(status)status=commands.setScissor({0,0,WIDTH,HEIGHT});
            if(status)status=commands.bindPipeline(mLightingPipeline);
            if(status)status=commands.bindBindingSet(0,lightingSet);
            if(status)status=commands.draw({3,1,0,0});
            if(lightingBegun){const Status ended=commands.endRendering();if(status&&!ended)status=ended;}
            if(status){BufferImageCopyRegion copy;copy.imageSubresource={ImageAspect::Color,0,0,1};copy.imageExtent={WIDTH,HEIGHT,1};const std::array<BufferImageCopyRegion,1> copies{{copy}};status=commands.copyImageToBuffer(mLightingColor,mLightingReadback,copies);}
        }
        if(status)for(std::size_t target=0;status&&target<4;++target){BufferImageCopyRegion copy;copy.imageSubresource={ImageAspect::Color,0,0,1};copy.imageExtent={WIDTH,HEIGHT,1};const std::array<BufferImageCopyRegion,1> copies{{copy}};status=commands.copyImageToBuffer(mColors[target],mReadbacks[target],copies);}
        if(begun){const Status ended=commands.endFrame();if(status&&!ended)status=ended;}
        const Status cleaned=cleanup();if(!status)return status;if(!cleaned)return cleaned;
        mPending=true;mResult={};mResult.frameId=packet.frameId;mResult.sceneEpoch=packet.sceneEpoch;mResult.resourceEpoch=packet.resourceEpoch;mResult.vertices=static_cast<std::uint32_t>(packet.vertices.size());mResult.indices=static_cast<std::uint32_t>(packet.indices.size());mResult.draws=static_cast<std::uint32_t>(draws.size());
        std::set<std::uint32_t> regions;for(auto& resources:draws){const auto& draw=packet.draws[resources.source];regions.insert(draw.region);const auto& region=packet.regions[draw.region];mResult.pbrDraws+=region.model==MaterialModel::MetallicRoughness;mResult.triplanarDraws+=region.projection==TerrainProjection::Triplanar;}mResult.regions=static_cast<std::uint32_t>(regions.size());mResult.packetSha256=terrainScenePacketSha256(packet);
        if(lightingPacket){mResult.lightingExecuted=true;mResult.directionalLights=directionalLights;mResult.pointLights=pointLights;mResult.lightingPacketSha256=lightingScenePacketSha256(*lightingPacket);}return Status::success();
    }

public:
    Status poll(TerrainOffscreenProbeResult& result)
    {
        result={};if(!mPending)return Status::failure(StatusCode::InvalidState,"terrain probe has no pending sample");
        for(std::size_t target=0;target<4;++target){const Status status=mDevice.readBuffer(mReadbacks[target],0,mPixels[target]);if(!status)return status;}
        for(std::size_t target=0;target<4;++target){mResult.colorSha256[target]=sha256(mPixels[target]);for(std::size_t pixel=0;pixel<std::size_t(WIDTH)*HEIGHT;++pixel){const auto begin=mPixels[target].begin()+static_cast<std::ptrdiff_t>(pixel*COLOR_BYTES[target]);if(std::any_of(begin,begin+COLOR_BYTES[target],[](std::byte value){return value!=std::byte{0};}))++mResult.nonClearPixels[target];}}
        if(mResult.lightingExecuted){const Status status=mDevice.readBuffer(mLightingReadback,0,mLightingPixels);if(!status)return status;mResult.litColorSha256=sha256(mLightingPixels);for(std::size_t pixel=0;pixel<std::size_t(WIDTH)*HEIGHT;++pixel){const auto begin=mLightingPixels.begin()+static_cast<std::ptrdiff_t>(pixel*LIGHTING_BYTES);if(std::any_of(begin,begin+LIGHTING_BYTES,[](std::byte value){return value!=std::byte{0};}))++mResult.litNonClearPixels;}}
        result=std::move(mResult);mResult={};mPending=false;return Status::success();
    }
    bool pending()const{return mPending;}
    Status shutdown(){if(mShutdown)return Status::success();mShutdown=true;mPending=false;Status first=Status::success();destroy(mLightingPipeline,first);destroy(mLightingShader,first);destroy(mPipeline,first);destroy(mShader,first);destroy(mLightingSampler,first);destroy(mRepeatSampler,first);destroy(mClampSampler,first);destroy(mLightingView,first);destroy(mLightingColor,first);destroy(mLightingReadback,first);destroy(mDepthSampleView,first);destroy(mDepthView,first);destroy(mDepth,first);for(std::size_t shadow=0;shadow<SHADOW_MAP_COUNT;++shadow){destroy(mShadowViews[shadow],first);destroy(mShadowImages[shadow],first);}for(std::size_t i=0;i<4;++i){destroy(mColorViews[i],first);destroy(mColors[i],first);destroy(mReadbacks[i],first);}return first;}

private:
    template<typename T>void destroy(T& handle,Status& first){if(!handle)return;const Status status=mDevice.destroy(handle);if(first&&!status)first=status;handle={};}
    Status initialize()
    {
        if(mPipeline)return Status::success();
        if(mShutdown)return Status::failure(StatusCode::InvalidState,"terrain probe is shut down");
        const auto& caps=mDevice.capabilities();
        if(caps.maxColorAttachments<4||caps.maxTexture2DSize<WIDTH||
           caps.preferredDepthStencilFormat==Format::Undefined||
           caps.maxSampledImagesPerStage<(mLightingPackage ? 10u : 5u))
            return Status::failure(StatusCode::Unsupported,"device lacks I6 terrain target capabilities");
        Status status=Status::success();
        for(std::size_t i=0;i<4;++i)
        {
            mColors[i]=mDevice.createImage({{WIDTH,HEIGHT,1},COLOR_FORMATS[i],
                ResourceUsage::ColorAttachment|ResourceUsage::TransferSource|
                ResourceUsage::Sampled,1,1,1},status);
            if(status)mColorViews[i]=mDevice.createImageView({mColors[i],COLOR_FORMATS[i],{ImageAspect::Color,0,1,0,1}},status);
            if(status)mReadbacks[i]=mDevice.createBuffer({std::uint64_t(WIDTH)*HEIGHT*COLOR_BYTES[i],ResourceUsage::TransferDestination,MemoryClass::Readback},status);
            mPixels[i].resize(std::size_t(WIDTH)*HEIGHT*COLOR_BYTES[i]);
            if(!status)break;
        }
        mDepthFormat=caps.preferredDepthStencilFormat;
        ResourceUsage depthUsage=ResourceUsage::DepthStencilAttachment;
        if(mLightingPackage)depthUsage=depthUsage|ResourceUsage::Sampled;
        if(status)mDepth=mDevice.createImage({{WIDTH,HEIGHT,1},mDepthFormat,depthUsage,1,1,1},status);
        if(status)mDepthView=mDevice.createImageView({mDepth,mDepthFormat,{ImageAspect::DepthStencil,0,1,0,1}},status);
        if(status&&mLightingPackage)mDepthSampleView=mDevice.createImageView({mDepth,mDepthFormat,{ImageAspect::Depth,0,1,0,1}},status);
        if(status)mShader=mDevice.createShaderPackage(mPackage,status);
        SamplerDesc repeat;repeat.minFilter=repeat.magFilter=repeat.mipFilter=Filter::Linear;repeat.addressU=repeat.addressV=AddressMode::Repeat;
        if(status)mRepeatSampler=mDevice.createSampler(repeat,status);
        SamplerDesc clamp=repeat;clamp.addressU=clamp.addressV=AddressMode::ClampToEdge;
        if(status)mClampSampler=mDevice.createSampler(clamp,status);
        if(status&&mLightingPackage)
        {
            SamplerDesc lighting;lighting.minFilter=lighting.magFilter=lighting.mipFilter=Filter::Nearest;lighting.addressU=lighting.addressV=lighting.addressW=AddressMode::ClampToEdge;
            mLightingSampler=mDevice.createSampler(lighting,status);
            if(status)mLightingColor=mDevice.createImage({{WIDTH,HEIGHT,1},LIGHTING_FORMAT,ResourceUsage::ColorAttachment|ResourceUsage::TransferSource,1,1,1},status);
            if(status)mLightingView=mDevice.createImageView({mLightingColor,LIGHTING_FORMAT,{ImageAspect::Color,0,1,0,1}},status);
            if(status)mLightingReadback=mDevice.createBuffer({std::uint64_t(WIDTH)*HEIGHT*LIGHTING_BYTES,ResourceUsage::TransferDestination,MemoryClass::Readback},status);
            mLightingPixels.resize(std::size_t(WIDTH)*HEIGHT*LIGHTING_BYTES);
            if(status)mLightingShader=mDevice.createShaderPackage(*mLightingPackage,status);
            for(std::size_t shadow=0;status&&shadow<SHADOW_MAP_COUNT;++shadow)
            {
                mShadowImages[shadow]=mDevice.createImage({{WIDTH,HEIGHT,1},SHADOW_FORMAT,
                    ResourceUsage::DepthStencilAttachment|ResourceUsage::Sampled,1,1,1},status);
                if(status)mShadowViews[shadow]=mDevice.createImageView({mShadowImages[shadow],SHADOW_FORMAT,
                    {ImageAspect::Depth,0,1,0,1}},status);
            }
        }
        if(status)
        {
            PipelineDesc pipeline;pipeline.shader=mShader;pipeline.cullMode=CullMode::Back;pipeline.depthTest=true;pipeline.depthWrite=true;pipeline.depthCompare=CompareOp::GreaterEqual;pipeline.colorFormats.assign(COLOR_FORMATS.begin(),COLOR_FORMATS.end());pipeline.depthStencilFormat=mDepthFormat;pipeline.blendStates.assign(4,BlendState{});pipeline.vertexBuffers={{0,sizeof(TerrainSceneVertex),VertexInputRate::PerVertex}};pipeline.vertexAttributes={{0,0,VertexFormat::Float32x3,offsetof(TerrainSceneVertex,position)},{1,0,VertexFormat::Float32x3,offsetof(TerrainSceneVertex,normal)},{3,0,VertexFormat::Float32x2,offsetof(TerrainSceneVertex,compositionCoord)}};
            mPipeline=mDevice.createPipeline(pipeline,status);
            if(status&&mLightingPackage)
            {
                PipelineDesc lighting;lighting.shader=mLightingShader;lighting.cullMode=CullMode::None;lighting.depthTest=false;lighting.depthWrite=false;lighting.colorFormats={LIGHTING_FORMAT};lighting.blendStates={BlendState{}};
                mLightingPipeline=mDevice.createPipeline(lighting,status);
            }
        }
        if(!status){const Status failure=status;shutdown();return failure;}
        return Status::success();
    }
    Device& mDevice;ShaderPackageDesc mPackage;std::optional<ShaderPackageDesc> mLightingPackage;
    std::array<ImageHandle,4> mColors{};std::array<ImageViewHandle,4> mColorViews{};std::array<BufferHandle,4> mReadbacks{};std::array<std::vector<std::byte>,4> mPixels;
    ImageHandle mDepth,mLightingColor;ImageViewHandle mDepthView,mDepthSampleView,mLightingView;Format mDepthFormat=Format::Undefined;
    std::array<ImageHandle,SHADOW_MAP_COUNT> mShadowImages{};std::array<ImageViewHandle,SHADOW_MAP_COUNT> mShadowViews{};
    ShaderPackageHandle mShader,mLightingShader;SamplerHandle mRepeatSampler,mClampSampler,mLightingSampler;PipelineHandle mPipeline,mLightingPipeline;BufferHandle mLightingReadback;std::vector<std::byte> mLightingPixels;
    TerrainOffscreenProbeResult mResult;bool mPending=false,mShutdown=false;
};

TerrainOffscreenProbe::TerrainOffscreenProbe(Device& device,ShaderPackageDesc package):mImpl(std::make_unique<Impl>(device,std::move(package))){}
TerrainOffscreenProbe::TerrainOffscreenProbe(Device& device,ShaderPackageDesc terrainPackage,ShaderPackageDesc lightingPackage):mImpl(std::make_unique<Impl>(device,std::move(terrainPackage),std::move(lightingPackage))){}
TerrainOffscreenProbe::~TerrainOffscreenProbe()=default;
Status TerrainOffscreenProbe::submit(const TerrainScenePacket& packet,const TerrainOffscreenProbeLimits& limits){return mImpl->submit(packet,limits);} Status TerrainOffscreenProbe::submit(const TerrainScenePacket& terrainPacket,const LightingScenePacket& lightingPacket,const TerrainOffscreenProbeLimits& limits){return mImpl->submit(terrainPacket,lightingPacket,limits);} Status TerrainOffscreenProbe::poll(TerrainOffscreenProbeResult& result){return mImpl->poll(result);} bool TerrainOffscreenProbe::pending()const{return mImpl->pending();} Status TerrainOffscreenProbe::shutdown(){return mImpl->shutdown();}
} // namespace LL::GHI
