/**
 * @file llghiproductionenvironmentexecutor.cpp
 * @brief P0e2c production sky execution on shared private targets.
 */
#include "linden_common.h"

#include "ghi/include/llghiproductionenvironmentexecutor.h"
#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

namespace LL::GHI
{
namespace
{
constexpr std::array<Format, PRODUCTION_GBUFFER_TARGETS> COLOR_FORMATS{{
    Format::RGBA8UNorm, Format::RGBA8UNorm,
    Format::RGBA16UNorm, Format::RGBA16Float}};
constexpr std::array<std::uint32_t, PRODUCTION_GBUFFER_TARGETS> COLOR_BYTES{{4,4,8,8}};
constexpr std::size_t UNIFORM_FLOATS = 72;
constexpr std::size_t UNIFORM_BYTES = UNIFORM_FLOATS * sizeof(float);
using UniformData = std::array<float, UNIFORM_FLOATS>;

Status invalid(const char* text)
{
    return Status::failure(StatusCode::InvalidArgument, text);
}
Status unsupported(const char* text)
{
    return Status::failure(StatusCode::Unsupported, text);
}

std::array<float, 16> multiply(const std::array<float, 16>& a,
                               const std::array<float, 16>& b)
{
    std::array<float, 16> output{};
    for (std::size_t column=0;column<4;++column)
        for (std::size_t row=0;row<4;++row)
            for (std::size_t item=0;item<4;++item)
                output[column*4+row] += a[item*4+row] * b[column*4+item];
    return output;
}

const EnvironmentTextureBinding* binding(
    const EnvironmentScenePacket& packet, EnvironmentTextureSemantic semantic)
{
    const auto found = std::find_if(packet.sky.textures.begin(),
        packet.sky.textures.end(), [semantic](const auto& value)
        { return value.semantic == semantic; });
    return found == packet.sky.textures.end() ? nullptr : &*found;
}

struct TexturePixels
{
    EnvironmentTextureSemantic semantic = EnvironmentTextureSemantic::Hdri;
    std::uint32_t width = 1, height = 1;
    Format format = Format::RGBA8UNorm;
    std::vector<std::byte> bytes;
    ImageHandle image;
    ImageViewHandle view;
};

Status texturePixels(const EnvironmentScenePacket& packet,
                     EnvironmentTextureSemantic semantic, TexturePixels& output)
{
    const auto* route = binding(packet, semantic);
    if (!route) return unsupported("environment route texture is absent");
    if (route->texture >= packet.textures.size())
        return invalid("environment texture binding is outside the packet");
    const auto& source = packet.textures[route->texture];
    if (source.comparability != ResourceComparability::Comparable ||
        source.decodedPixels.empty() || !source.width || !source.height ||
        !source.components || source.components > 4)
        return unsupported("environment texture has no comparable decoded content");
    const std::uint64_t pixels = static_cast<std::uint64_t>(source.width)*source.height;
    if (pixels > std::numeric_limits<std::size_t>::max()/4)
        return unsupported("environment texture is too large");
    output = {};
    output.semantic = semantic;
    output.width = source.width;
    output.height = source.height;
    output.format = source.colorSpace == TextureColorSpace::SRGB
        ? Format::RGBA8SRGB : Format::RGBA8UNorm;
    output.bytes.resize(static_cast<std::size_t>(pixels)*4);
    for (std::size_t pixel=0;pixel<pixels;++pixel)
    {
        const std::size_t in=pixel*source.components,out=pixel*4;
        const std::byte luma=source.decodedPixels[in];
        output.bytes[out]=luma;
        output.bytes[out+1]=source.components<3?luma:source.decodedPixels[in+1];
        output.bytes[out+2]=source.components<3?luma:source.decodedPixels[in+2];
        output.bytes[out+3]=source.components==2?source.decodedPixels[in+1]:
            source.components==4?source.decodedPixels[in+3]:std::byte{255};
    }
    return Status::success();
}

void copy3(const std::array<float,3>& source, UniformData& output, std::size_t at)
{
    std::copy(source.begin(),source.end(),output.begin()+static_cast<std::ptrdiff_t>(at));
}

UniformData uniformData(const EnvironmentScenePacket& packet,
                        const SkySceneDraw& draw, std::uint32_t route)
{
    UniformData data{};
    const auto pv=multiply(packet.projectionMatrix,packet.viewMatrix);
    const auto mvp=multiply(pv,draw.modelTransform);
    std::copy(mvp.begin(),mvp.end(),data.begin());
    copy3(packet.cameraOrigin,data,16); data[19]=static_cast<float>(route);
    copy3(packet.sky.lightDirection,data,20); data[23]=packet.sky.sunUp?1.f:0.f;
    copy3(packet.atmosphere.sunlight,data,24); data[27]=packet.sky.cloudShadow;
    copy3(packet.atmosphere.moonlight,data,28); data[31]=packet.atmosphere.maxAltitude;
    copy3(packet.atmosphere.ambient,data,32); data[35]=packet.atmosphere.densityMultiplier;
    copy3(packet.atmosphere.blueHorizon,data,36); data[39]=packet.atmosphere.hazeHorizon;
    copy3(packet.atmosphere.blueDensity,data,40); data[43]=packet.atmosphere.hazeDensity;
    copy3(packet.atmosphere.glow,data,44); data[47]=packet.sky.sunMoonGlowFactor;
    copy3(packet.sky.cloudColor,data,48); data[51]=packet.sky.cloudScale;
    copy3(packet.sky.cloudPositionDensity1,data,52);
    copy3(packet.sky.cloudPositionDensity2,data,56);
    data[60]=packet.sky.cloudVariance; data[61]=packet.sky.blendFactor;
    data[62]=packet.sky.moonBrightness; data[63]=packet.sky.starBrightness;
    data[64]=packet.sky.starPhase; data[65]=packet.sky.emissiveBuffer?1.f:0.f;
    data[66]=packet.atmosphere.moisture; data[67]=packet.atmosphere.dropletRadius;
    data[68]=packet.atmosphere.iceLevel; data[69]=packet.sky.moonDirection[2];
    data[70]=packet.sky.hdriExposure; data[71]=packet.sky.hdriRotation;
    return data;
}

enum class PipelineKind : std::size_t { OpaqueStrip, AlphaTriangles,
                                        AdditiveTriangles, AlphaStrip, Count };
struct Execution
{
    std::size_t draw = 0;
    std::uint32_t route = 0;
    PipelineKind pipeline = PipelineKind::OpaqueStrip;
    EnvironmentTextureSemantic primary = EnvironmentTextureSemantic::Rainbow;
    EnvironmentTextureSemantic secondary = EnvironmentTextureSemantic::Rainbow;
};
} // namespace

class ProductionEnvironmentExecutor::Impl
{
public:
    Impl(Device& device, ShaderPackageDesc shader) :
        mDevice(device),mShaderPackage(std::move(shader)) {}
    ~Impl(){shutdown();}

    Status submit(const EnvironmentScenePacket& packet,
                  const ProductionFrameTargetSet& targets,
                  const ProductionEnvironmentLimits& limits)
    {
        if (mPending) return Status::failure(StatusCode::NotReady,
            "production environment execution is still pending");
        Status status=validateEnvironmentScenePacket(packet);
        if (!status) return status;
        if (!targets.generation || !targets.width || !targets.height ||
            !targets.depthView || std::any_of(targets.gbufferViews.begin(),
                targets.gbufferViews.end(),[](auto view){return !view;}))
            return invalid("production environment targets are incomplete");
        if (!limits.maxSkyDraws || !limits.maxSkyVertices ||
            !limits.maxSkyIndices || !limits.maxUploadBytes || !limits.maxTextureBytes)
            return invalid("production environment limits must be nonzero");
        if (packet.skyDraws.size()>limits.maxSkyDraws ||
            packet.skyVertices.size()>limits.maxSkyVertices ||
            packet.skyIndices.size()>limits.maxSkyIndices)
            return unsupported("production environment geometry exceeds limits");
        if (packet.passMask & environmentPassBit(EnvironmentPass::HdriSky))
            return unsupported("comparable decoded HDRI source is not available yet");
        if (!(status=initialize())) return status;

        std::vector<Execution> executions;
        auto add=[&](SkyGeometryKind kind,std::uint32_t route,PipelineKind pipeline,
                     EnvironmentTextureSemantic primary,
                     EnvironmentTextureSemantic secondary)
        {
            for (std::size_t draw=0;draw<packet.skyDraws.size();++draw)
                if (packet.skyDraws[draw].kind==kind)
                    executions.push_back({draw,route,pipeline,primary,secondary});
        };
        if (packet.passMask&environmentPassBit(EnvironmentPass::Atmosphere))
            add(SkyGeometryKind::Dome,0,PipelineKind::OpaqueStrip,
                EnvironmentTextureSemantic::Rainbow,EnvironmentTextureSemantic::Rainbow);
        if (packet.passMask&environmentPassBit(EnvironmentPass::Sun))
            add(SkyGeometryKind::Sun,1,PipelineKind::AlphaTriangles,
                EnvironmentTextureSemantic::Sun,EnvironmentTextureSemantic::SunNext);
        if (packet.passMask&environmentPassBit(EnvironmentPass::Moon))
            add(SkyGeometryKind::Moon,2,PipelineKind::AlphaTriangles,
                EnvironmentTextureSemantic::Moon,EnvironmentTextureSemantic::MoonNext);
        if (packet.passMask&environmentPassBit(EnvironmentPass::Stars))
            add(SkyGeometryKind::Stars,3,PipelineKind::AdditiveTriangles,
                EnvironmentTextureSemantic::StarBloom,EnvironmentTextureSemantic::StarBloomNext);
        if (packet.passMask&environmentPassBit(EnvironmentPass::Clouds))
            add(SkyGeometryKind::Dome,4,PipelineKind::AlphaStrip,
                EnvironmentTextureSemantic::CloudNoise,EnvironmentTextureSemantic::CloudNoiseNext);
        if (executions.empty()) return invalid("environment packet has no executable sky route");

        std::vector<EnvironmentTextureSemantic> semantics{
            EnvironmentTextureSemantic::Rainbow,EnvironmentTextureSemantic::Halo};
        for (const auto& execution:executions)
        {
            semantics.push_back(execution.primary);
            if (binding(packet,execution.secondary)) semantics.push_back(execution.secondary);
        }
        std::sort(semantics.begin(),semantics.end(),[](auto a,auto b)
            {return static_cast<std::uint32_t>(a)<static_cast<std::uint32_t>(b);});
        semantics.erase(std::unique(semantics.begin(),semantics.end()),semantics.end());
        std::vector<TexturePixels> textures;
        std::map<EnvironmentTextureSemantic,std::size_t> textureMap;
        std::uint64_t textureBytes=0;
        for (auto semantic:semantics)
        {
            if (!binding(packet,semantic)) continue;
            TexturePixels texture;
            if (!(status=texturePixels(packet,semantic,texture))) return status;
            textureBytes+=texture.bytes.size();
            if (textureBytes>limits.maxTextureBytes)
                return unsupported("production environment textures exceed limits");
            textureMap.emplace(semantic,textures.size());
            textures.push_back(std::move(texture));
        }
        if (!textureMap.contains(EnvironmentTextureSemantic::Rainbow) ||
            !textureMap.contains(EnvironmentTextureSemantic::Halo))
            return unsupported("atmosphere execution requires decoded rainbow and halo maps");
        TexturePixels fallback;
        fallback.bytes.assign(4,std::byte{0});
        textures.push_back(std::move(fallback));
        const std::size_t fallbackIndex=textures.size()-1;
        if (!(status=uploadTextures(textures))) return status;

        const std::uint64_t alignment=std::max<std::uint64_t>(
            16,mDevice.capabilities().uniformBufferOffsetAlignment);
        const auto align=[alignment](std::uint64_t value)
            {return (value+alignment-1)/alignment*alignment;};
        const std::uint64_t vertexBytes=packet.skyVertices.size()*sizeof(SkySceneVertex);
        const std::uint64_t indexBytes=packet.skyIndices.size()*sizeof(std::uint32_t);
        const std::uint64_t indexOffset=align(vertexBytes);
        const std::uint64_t uniformOffset=align(indexOffset+indexBytes);
        const std::uint64_t uniformStride=align(UNIFORM_BYTES);
        const std::uint64_t total=uniformOffset+uniformStride*executions.size();
        if (total>limits.maxUploadBytes || total>mDevice.capabilities().maxBufferSize)
        { destroyTextures(textures); return unsupported("environment upload exceeds limits"); }
        std::vector<std::byte> bytes(static_cast<std::size_t>(total));
        std::memcpy(bytes.data(),packet.skyVertices.data(),static_cast<std::size_t>(vertexBytes));
        std::memcpy(bytes.data()+indexOffset,packet.skyIndices.data(),static_cast<std::size_t>(indexBytes));
        for (std::size_t item=0;item<executions.size();++item)
        {
            const auto data=uniformData(packet,packet.skyDraws[executions[item].draw],
                                        executions[item].route);
            std::memcpy(bytes.data()+uniformOffset+uniformStride*item,
                        data.data(),UNIFORM_BYTES);
        }

        BufferHandle staging,vertices,indices,uniforms;
        std::vector<BindingSetHandle> sets;
        auto cleanup=[&](){Status first=Status::success();for(auto& set:sets)destroy(set,first);
            destroy(staging,first);destroy(vertices,first);destroy(indices,first);
            destroy(uniforms,first);destroyTextures(textures,first);return first;};
        staging=mDevice.createBuffer({total,ResourceUsage::TransferSource,MemoryClass::Upload},status);
        if(status) vertices=mDevice.createBuffer({vertexBytes,ResourceUsage::Vertex|
            ResourceUsage::TransferDestination,MemoryClass::DeviceLocal},status);
        if(status) indices=mDevice.createBuffer({indexBytes,ResourceUsage::Index|
            ResourceUsage::TransferDestination,MemoryClass::DeviceLocal},status);
        if(status) uniforms=mDevice.createBuffer({uniformStride*executions.size(),
            ResourceUsage::Uniform|ResourceUsage::TransferDestination,
            MemoryClass::DeviceLocal},status);
        if(status) status=mDevice.writeBuffer(staging,0,bytes);
        for(std::size_t item=0;status&&item<executions.size();++item)
        {
            const auto& execution=executions[item];
            auto locate=[&](EnvironmentTextureSemantic semantic,std::size_t alternate)
            {auto found=textureMap.find(semantic);return found==textureMap.end()?alternate:found->second;};
            const std::size_t primary=locate(execution.primary,fallbackIndex);
            const std::size_t secondary=locate(execution.secondary,primary);
            const std::size_t rainbow=locate(EnvironmentTextureSemantic::Rainbow,fallbackIndex);
            const std::size_t halo=locate(EnvironmentTextureSemantic::Halo,fallbackIndex);
            const bool repeat=execution.route==4;
            BindingSetDesc desc;desc.shader=mShader;desc.group=0;
            desc.resources={{0,0,ShaderPackageDesc::BindingType::UniformBuffer,
                uniforms,uniformStride*item,UNIFORM_BYTES,{},{}},
                {1,0,ShaderPackageDesc::BindingType::CombinedImageSampler,{},0,0,
                 textures[primary].view,repeat?mRepeatSampler:mClampSampler},
                {2,0,ShaderPackageDesc::BindingType::CombinedImageSampler,{},0,0,
                 textures[secondary].view,repeat?mRepeatSampler:mClampSampler},
                {3,0,ShaderPackageDesc::BindingType::CombinedImageSampler,{},0,0,
                 textures[rainbow].view,mRepeatSampler},
                {4,0,ShaderPackageDesc::BindingType::CombinedImageSampler,{},0,0,
                 textures[halo].view,mClampSampler}};
            sets.push_back(mDevice.createBindingSet(desc,status));
        }
        if(!status){cleanup();return status;}
        if(!(status=ensureReadbacks(targets.width,targets.height))){cleanup();return status;}

        CommandContext& commands=mDevice.commandContext();bool frame=false,rendering=false;
        status=commands.beginFrame();frame=status.ok();
        const std::array<BufferCopyRegion,1> vc{{{0,0,vertexBytes}}};
        const std::array<BufferCopyRegion,1> ic{{{indexOffset,0,indexBytes}}};
        const std::array<BufferCopyRegion,1> uc{{{uniformOffset,0,uniformStride*executions.size()}}};
        if(status)status=commands.copyBuffer(staging,vertices,vc);
        if(status)status=commands.copyBuffer(staging,indices,ic);
        if(status)status=commands.copyBuffer(staging,uniforms,uc);
        RenderingInfo info;info.semanticId=0x50304532534b59ull;info.width=targets.width;info.height=targets.height;
        for(std::size_t target=0;target<PRODUCTION_GBUFFER_TARGETS;++target)
            info.colors.push_back({targets.gbufferViews[target],COLOR_FORMATS[target],
                LoadOp::Load,StoreOp::Store,{}});
        info.depthStencil=AttachmentDesc{targets.depthView,Format::Depth32Float,
            LoadOp::Load,StoreOp::Store,{}};
        if(status){status=commands.beginRendering(info);rendering=status.ok();}
        if(status)status=commands.setViewport({0,0,static_cast<float>(targets.width),
            static_cast<float>(targets.height),0,1});
        if(status)status=commands.setScissor({0,0,targets.width,targets.height});
        if(status)status=commands.bindPipeline(
            mPipelines[static_cast<std::size_t>(executions.front().pipeline)]);
        if(status)status=commands.bindVertexBuffer(0,vertices,0);
        if(status)status=commands.bindIndexBuffer(indices,0,IndexType::UInt32);
        for(std::size_t item=0;status&&item<executions.size();++item)
        {
            if(item!=0)status=commands.bindPipeline(
                mPipelines[static_cast<std::size_t>(executions[item].pipeline)]);
            if(status)status=commands.bindBindingSet(0,sets[item]);
            const auto& draw=packet.skyDraws[executions[item].draw];
            if(status)status=commands.drawIndexed({draw.indexCount,1,draw.firstIndex,0,0});
        }
        if(rendering){const Status ended=commands.endRendering();rendering=false;if(status&&!ended)status=ended;}
        for(std::size_t target=0;status&&target<PRODUCTION_GBUFFER_TARGETS;++target)
        {
            BufferImageCopyRegion region;region.imageSubresource={ImageAspect::Color,0,0,1};
            region.imageExtent={targets.width,targets.height,1};
            const std::array<BufferImageCopyRegion,1> copies{{region}};
            status=commands.copyImageToBuffer(targets.gbufferImages[target],mReadbacks[target],copies);
        }
        if(frame){const Status ended=commands.endFrame();frame=false;if(status&&!ended)status=ended;}
        const Status cleaned=cleanup();if(!status)return status;if(!cleaned)return cleaned;
        mPending=true;mResult={};mResult.frameId=packet.frameId;mResult.sceneEpoch=packet.sceneEpoch;
        mResult.resourceEpoch=packet.resourceEpoch;mResult.targetGeneration=targets.generation;
        mResult.uploadBytes=total+textureBytes;mResult.packetSha256=environmentScenePacketSha256(packet);
        for(const auto& execution:executions)
            switch(execution.route){case 0:++mResult.atmosphereDraws;break;case 1:++mResult.sunDraws;break;
            case 2:++mResult.moonDraws;break;case 3:++mResult.starDraws;break;case 4:++mResult.cloudDraws;break;}
        return Status::success();
    }

    Status poll(ProductionEnvironmentResult& result)
    {
        result={};if(!mPending)return Status::failure(StatusCode::InvalidState,
            "production environment executor has no pending result");
        for(std::size_t target=0;target<PRODUCTION_GBUFFER_TARGETS;++target)
        {
            std::vector<std::byte> pixels(
                static_cast<std::size_t>(mReadbackWidth) * mReadbackHeight *
                COLOR_BYTES[target]);
            Status status=mDevice.readBuffer(mReadbacks[target],0,pixels);if(!status)return status;
            if(mDevice.backend()==Backend::OpenGL)
            {
                const std::size_t row=mReadbackWidth*COLOR_BYTES[target];
                for(std::uint32_t y=0;y<mReadbackHeight/2;++y)
                    std::swap_ranges(pixels.begin()+y*row,pixels.begin()+(y+1)*row,
                        pixels.begin()+(mReadbackHeight-1-y)*row);
            }
            mResult.colorSha256[target]=sha256(pixels);
            const std::size_t count=static_cast<std::size_t>(mReadbackWidth)*mReadbackHeight;
            for(std::size_t pixel=0;pixel<count;++pixel)
                if(std::any_of(pixels.begin()+pixel*COLOR_BYTES[target],
                    pixels.begin()+(pixel+1)*COLOR_BYTES[target],[](std::byte b){return b!=std::byte{};}))
                    ++mResult.nonClearPixels[target];
        }
        result=std::move(mResult);mResult={};mPending=false;return Status::success();
    }
    bool pending()const{return mPending;}
    Status shutdown()
    {
        if(mShutdown)return Status::success();mShutdown=true;mPending=false;Status first=Status::success();
        for(auto& buffer:mReadbacks)destroy(buffer,first);mReadbackWidth=mReadbackHeight=0;
        for(auto& pipeline:mPipelines)destroy(pipeline,first);destroy(mRepeatSampler,first);
        destroy(mClampSampler,first);destroy(mShader,first);return first;
    }

private:
    template<typename T>void destroy(T& handle,Status& first)
    {if(!handle)return;const Status status=mDevice.destroy(handle);if(first&&!status)first=status;handle={};}
    void destroy(BindingSetHandle& handle,Status& first)
    {if(!handle)return;const Status status=mDevice.destroy(handle);if(first&&!status)first=status;handle={};}
    void destroyTextures(std::vector<TexturePixels>& textures)
    {Status ignored=Status::success();destroyTextures(textures,ignored);}
    void destroyTextures(std::vector<TexturePixels>& textures,Status& first)
    {for(auto& texture:textures){destroy(texture.view,first);destroy(texture.image,first);}}

    Status uploadTextures(std::vector<TexturePixels>& textures)
    {
        std::uint64_t total=0;for(const auto& texture:textures)total+=texture.bytes.size();
        Status status=Status::success();BufferHandle staging=mDevice.createBuffer(
            {total,ResourceUsage::TransferSource,MemoryClass::Upload},status);
        std::vector<std::byte> bytes;bytes.reserve(static_cast<std::size_t>(total));
        std::vector<std::uint64_t> offsets;
        for(auto& texture:textures)
        {
            offsets.push_back(bytes.size());bytes.insert(bytes.end(),texture.bytes.begin(),texture.bytes.end());
            if(status)texture.image=mDevice.createImage({{texture.width,texture.height,1},texture.format,
                ResourceUsage::Sampled|ResourceUsage::TransferDestination,1,1,1},status);
            if(status)texture.view=mDevice.createImageView({texture.image,texture.format,
                {ImageAspect::Color,0,1,0,1}},status);
        }
        if(status)status=mDevice.writeBuffer(staging,0,bytes);
        if(status){auto& commands=mDevice.commandContext();status=commands.beginFrame();
            for(std::size_t i=0;status&&i<textures.size();++i){BufferImageCopyRegion copy;
                copy.bufferOffset=offsets[i];copy.imageSubresource={ImageAspect::Color,0,0,1};
                copy.imageExtent={textures[i].width,textures[i].height,1};
                const std::array<BufferImageCopyRegion,1> copies{{copy}};
                status=commands.copyBufferToImage(staging,textures[i].image,copies);}
            const Status ended=commands.endFrame();if(status&&!ended)status=ended;}
        if(staging){const Status ended=mDevice.destroy(staging);if(status&&!ended)status=ended;}
        if(!status)destroyTextures(textures);return status;
    }

    Status ensureReadbacks(std::uint32_t width,std::uint32_t height)
    {
        if(width==mReadbackWidth&&height==mReadbackHeight)return Status::success();
        Status status=Status::success();for(auto& buffer:mReadbacks)destroy(buffer,status);
        if(!status)return status;mReadbackWidth=width;mReadbackHeight=height;
        for(std::size_t target=0;status&&target<PRODUCTION_GBUFFER_TARGETS;++target)
            mReadbacks[target]=mDevice.createBuffer({static_cast<std::uint64_t>(width)*height*
                COLOR_BYTES[target],ResourceUsage::TransferDestination,MemoryClass::Readback},status);
        return status;
    }

    Status initialize()
    {
        if(mShader)return Status::success();if(mShutdown)return Status::failure(StatusCode::InvalidState,
            "production environment executor is shut down");
        Status status=Status::success();mShader=mDevice.createShaderPackage(mShaderPackage,status);
        SamplerDesc clamp;clamp.addressU=clamp.addressV=clamp.addressW=AddressMode::ClampToEdge;
        if(status)mClampSampler=mDevice.createSampler(clamp,status);
        SamplerDesc repeat;if(status)mRepeatSampler=mDevice.createSampler(repeat,status);
        auto make=[&](PipelineKind kind,PrimitiveTopology topology,bool depthWrite,
                      bool blend,bool additive)
        {
            PipelineDesc pipeline;pipeline.shader=mShader;pipeline.topology=topology;
            pipeline.cullMode=CullMode::None;pipeline.depthTest=true;pipeline.depthWrite=depthWrite;
            pipeline.depthCompare=CompareOp::GreaterEqual;pipeline.colorFormats.assign(
                COLOR_FORMATS.begin(),COLOR_FORMATS.end());pipeline.depthStencilFormat=Format::Depth32Float;
            pipeline.blendStates.assign(PRODUCTION_GBUFFER_TARGETS,BlendState{});
            if(blend)for(auto& state:pipeline.blendStates){state.enabled=true;
                state.sourceColor=BlendFactor::SourceAlpha;
                state.destinationColor=additive?BlendFactor::One:BlendFactor::OneMinusSourceAlpha;
                state.sourceAlpha=BlendFactor::SourceAlpha;
                state.destinationAlpha=additive?BlendFactor::One:BlendFactor::OneMinusSourceAlpha;}
            pipeline.vertexBuffers={{0,sizeof(SkySceneVertex),VertexInputRate::PerVertex}};
            pipeline.vertexAttributes={{0,0,VertexFormat::Float32x3,offsetof(SkySceneVertex,position)},
                {1,0,VertexFormat::Float32x2,offsetof(SkySceneVertex,texCoord)},
                {2,0,VertexFormat::Float32x4,offsetof(SkySceneVertex,color)}};
            mPipelines[static_cast<std::size_t>(kind)]=mDevice.createPipeline(pipeline,status);
        };
        if(status)make(PipelineKind::OpaqueStrip,PrimitiveTopology::TriangleStrip,false,false,false);
        if(status)make(PipelineKind::AlphaTriangles,PrimitiveTopology::Triangles,true,true,false);
        if(status)make(PipelineKind::AdditiveTriangles,PrimitiveTopology::Triangles,false,true,true);
        if(status)make(PipelineKind::AlphaStrip,PrimitiveTopology::TriangleStrip,false,true,false);
        if(!status){const Status failure=status;shutdown();return failure;}return status;
    }

    Device& mDevice;ShaderPackageDesc mShaderPackage;ShaderPackageHandle mShader;
    SamplerHandle mClampSampler,mRepeatSampler;
    std::array<PipelineHandle,static_cast<std::size_t>(PipelineKind::Count)> mPipelines{};
    std::array<BufferHandle,PRODUCTION_GBUFFER_TARGETS> mReadbacks{};
    std::uint32_t mReadbackWidth=0,mReadbackHeight=0;ProductionEnvironmentResult mResult;
    bool mPending=false,mShutdown=false;
};

ProductionEnvironmentExecutor::ProductionEnvironmentExecutor(Device& device,ShaderPackageDesc shader):
    mImpl(std::make_unique<Impl>(device,std::move(shader))){}
ProductionEnvironmentExecutor::~ProductionEnvironmentExecutor()=default;
Status ProductionEnvironmentExecutor::submit(const EnvironmentScenePacket& packet,
    const ProductionFrameTargetSet& targets,const ProductionEnvironmentLimits& limits)
{return mImpl->submit(packet,targets,limits);}
Status ProductionEnvironmentExecutor::poll(ProductionEnvironmentResult& result)
{return mImpl->poll(result);}
bool ProductionEnvironmentExecutor::pending()const{return mImpl->pending();}
Status ProductionEnvironmentExecutor::shutdown(){return mImpl->shutdown();}
} // namespace LL::GHI
