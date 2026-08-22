/**
 * @file llghiproductionwaterexecutor.cpp
 * @brief P0e2d production water composition from explicit GHI dependencies.
 */
#include "linden_common.h"

#include "ghi/include/llghiproductionwaterexecutor.h"

#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace LL::GHI
{
namespace
{
constexpr Format OUTPUT_FORMAT = Format::RGBA16Float;
constexpr std::uint32_t OUTPUT_BYTES = 8;
constexpr std::size_t UNIFORM_FLOATS = 64;
constexpr std::size_t UNIFORM_BYTES = UNIFORM_FLOATS * sizeof(float);
using UniformData = std::array<float, UNIFORM_FLOATS>;

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

Status unsupported(const char* message)
{
    return Status::failure(StatusCode::Unsupported, message);
}

std::array<float, 16> multiply(const std::array<float, 16>& left,
                               const std::array<float, 16>& right)
{
    std::array<float, 16> output{};
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t item = 0; item < 4; ++item)
                output[column * 4 + row] +=
                    left[item * 4 + row] * right[column * 4 + item];
    return output;
}

const EnvironmentTextureBinding* binding(
    const EnvironmentScenePacket& packet,
    EnvironmentTextureSemantic semantic)
{
    const auto found = std::find_if(
        packet.water.textures.begin(), packet.water.textures.end(),
        [semantic](const auto& value) { return value.semantic == semantic; });
    return found == packet.water.textures.end() ? nullptr : &*found;
}

struct Texture
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Format format = Format::RGBA8UNorm;
    std::vector<std::byte> bytes;
    ImageHandle image;
    ImageViewHandle view;
};

Status decodeTexture(const EnvironmentScenePacket& packet,
                     EnvironmentTextureSemantic semantic, Texture& output)
{
    const auto* route = binding(packet, semantic);
    if (!route || route->texture >= packet.textures.size())
        return unsupported("water normal texture is unavailable");
    const auto& source = packet.textures[route->texture];
    if (source.comparability != ResourceComparability::Comparable ||
        source.decodedPixels.empty() || !source.width || !source.height ||
        !source.components || source.components > 4)
        return unsupported("water normal texture has no decoded content");
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(source.width) * source.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 4)
        return unsupported("water normal texture is too large");

    output = {};
    output.width = source.width;
    output.height = source.height;
    output.bytes.resize(static_cast<std::size_t>(pixels) * 4);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel)
    {
        const std::size_t input = pixel * source.components;
        const std::size_t result = pixel * 4;
        const std::byte luma = source.decodedPixels[input];
        output.bytes[result] = luma;
        output.bytes[result + 1] = source.components < 3
            ? luma : source.decodedPixels[input + 1];
        output.bytes[result + 2] = source.components < 3
            ? luma : source.decodedPixels[input + 2];
        output.bytes[result + 3] = source.components == 2
            ? source.decodedPixels[input + 1]
            : source.components == 4 ? source.decodedPixels[input + 3]
                                     : std::byte{255};
    }
    return Status::success();
}

UniformData waterUniforms(const EnvironmentScenePacket& packet,
                          const WaterSceneDraw* draw, float route)
{
    UniformData data{};
    std::array<float, 16> model{};
    model[0] = model[5] = model[10] = model[15] = 1.f;
    if (draw) model = draw->modelTransform;
    const auto modelView = multiply(packet.viewMatrix, model);
    const auto mvp = multiply(packet.projectionMatrix, modelView);
    std::copy(mvp.begin(), mvp.end(), data.begin());
    std::copy(modelView.begin(), modelView.end(), data.begin() + 16);
    std::copy(packet.water.clampedLightNormal.begin(),
              packet.water.clampedLightNormal.end(),
              data.begin() + 32);
    data[35] = route;
    data[36] = packet.water.waveDirection1[0];
    data[37] = packet.water.waveDirection1[1];
    data[38] = packet.water.waveDirection2[0];
    data[39] = packet.water.waveDirection2[1];
    data[40] = packet.water.cameraToWaterHeight;
    data[41] = packet.water.phase;
    data[42] = packet.water.normalBlendFactor;
    data[43] = route > 1.5f ? packet.water.scaleBelow
                           : packet.water.scaleAbove;
    data[44] = packet.water.fresnelOffset;
    data[45] = packet.water.blurMultiplier;
    data[46] = packet.water.fogDensity;
    data[47] = packet.water.exposure;
    std::copy(packet.water.normalScale.begin(), packet.water.normalScale.end(),
              data.begin() + 48);
    data[51] = packet.water.sunUp ? 1.f : 0.f;
    std::copy(packet.water.lightDirection.begin(),
              packet.water.lightDirection.end(), data.begin() + 52);
    data[55] = packet.water.fresnelScale;
    std::copy(packet.water.lightColor.begin(), packet.water.lightColor.end(),
              data.begin() + 56);
    data[59] = packet.water.tonemapMix;
    std::copy(packet.water.fogColor.begin(), packet.water.fogColor.end(),
              data.begin() + 60);
    data[63] = static_cast<float>(packet.water.tonemapType);
    return data;
}
} // namespace

class ProductionWaterExecutor::Impl
{
public:
    Impl(Device& device, ShaderPackageDesc shader) :
        mDevice(device), mShaderPackage(std::move(shader))
    {
    }

    ~Impl() { shutdown(); }

    Status submit(const EnvironmentScenePacket& packet,
                  const ProductionFrameTargetSet& targets,
                  const ProductionWaterDependencies& dependencies,
                  const ProductionWaterLimits& limits)
    {
        if (mPending)
            return Status::failure(StatusCode::NotReady,
                                   "production water execution is pending");
        Status status = validateEnvironmentScenePacket(packet);
        if (!status) return status;
        const bool surface = (packet.passMask & environmentPassBit(
            EnvironmentPass::WaterSurface)) != 0;
        const bool underwater = (packet.passMask & environmentPassBit(
            EnvironmentPass::Underwater)) != 0;
        if (!surface && !underwater)
            return invalid("environment packet has no active water route");
        if (!targets.generation || !targets.width || !targets.height ||
            !targets.lightingView || !targets.depthView)
            return invalid("production water shared targets are incomplete");
        if (dependencies.generation != targets.generation ||
            !dependencies.reflectionColorView ||
            !dependencies.refractionColorView ||
            !dependencies.exclusionMaskView || !dependencies.waterDepthView)
            return invalid("production water dependencies are incomplete or stale");
        if (!limits.maxDraws || !limits.maxVertices || !limits.maxIndices ||
            !limits.maxUploadBytes || !limits.maxTextureBytes)
            return invalid("production water limits must be nonzero");
        if (packet.waterDraws.size() > limits.maxDraws ||
            packet.waterVertices.size() > limits.maxVertices ||
            packet.waterIndices.size() > limits.maxIndices)
            return unsupported("production water geometry exceeds limits");
        if (!(status = initialize())) return status;
        if (!(status = ensureOutput(targets))) return status;

        Texture normal;
        if (!(status = decodeTexture(packet,
                EnvironmentTextureSemantic::WaterNormal, normal)))
            return status;
        Texture nextNormal;
        if (binding(packet, EnvironmentTextureSemantic::WaterNormalNext))
            status = decodeTexture(packet,
                EnvironmentTextureSemantic::WaterNormalNext, nextNormal);
        else
            nextNormal = normal;
        if (!status) return status;
        const std::uint64_t textureBytes = normal.bytes.size() +
            (binding(packet, EnvironmentTextureSemantic::WaterNormalNext)
                ? nextNormal.bytes.size() : 0);
        if (textureBytes > limits.maxTextureBytes)
            return unsupported("production water textures exceed limits");

        std::vector<Texture> textures;
        textures.push_back(std::move(normal));
        const bool distinctNormals =
            binding(packet, EnvironmentTextureSemantic::WaterNormalNext) != nullptr;
        if (distinctNormals) textures.push_back(std::move(nextNormal));
        if (!(status = uploadTextures(textures))) return status;
        const std::size_t nextTexture = distinctNormals ? 1 : 0;
        const ImageViewHandle refractionView = underwater
            ? dependencies.refractionColorView : targets.lightingView;

        std::vector<WaterSceneVertex> vertices(3);
        vertices.insert(vertices.end(), packet.waterVertices.begin(),
                        packet.waterVertices.end());
        std::vector<std::uint32_t> indices{0, 1, 2};
        indices.reserve(3 + packet.waterIndices.size());
        for (std::uint32_t index : packet.waterIndices)
            indices.push_back(index + 3);

        const std::uint64_t alignment = std::max<std::uint64_t>(
            16, mDevice.capabilities().uniformBufferOffsetAlignment);
        const auto align = [alignment](std::uint64_t value)
        {
            return (value + alignment - 1) / alignment * alignment;
        };
        const std::uint64_t vertexBytes =
            vertices.size() * sizeof(WaterSceneVertex);
        const std::uint64_t indexBytes = indices.size() * sizeof(std::uint32_t);
        const std::uint64_t indexOffset = align(vertexBytes);
        const std::uint64_t uniformOffset = align(indexOffset + indexBytes);
        const std::uint64_t uniformStride = align(UNIFORM_BYTES);
        const std::size_t executionCount = 1 + packet.waterDraws.size();
        const std::uint64_t total =
            uniformOffset + uniformStride * executionCount;
        if (total > limits.maxUploadBytes ||
            total > mDevice.capabilities().maxBufferSize)
        {
            destroyTextures(textures);
            return unsupported("production water upload exceeds limits");
        }

        std::vector<std::byte> bytes(static_cast<std::size_t>(total));
        std::memcpy(bytes.data(), vertices.data(),
                    static_cast<std::size_t>(vertexBytes));
        std::memcpy(bytes.data() + indexOffset, indices.data(),
                    static_cast<std::size_t>(indexBytes));
        const UniformData copy = waterUniforms(packet, nullptr, 0.f);
        std::memcpy(bytes.data() + uniformOffset, copy.data(), UNIFORM_BYTES);
        const float waterRoute = underwater ? 2.f : 1.f;
        for (std::size_t draw = 0; draw < packet.waterDraws.size(); ++draw)
        {
            const UniformData uniforms = waterUniforms(
                packet, &packet.waterDraws[draw], waterRoute);
            std::memcpy(bytes.data() + uniformOffset +
                            uniformStride * (draw + 1),
                        uniforms.data(), UNIFORM_BYTES);
        }

        BufferHandle staging;
        BufferHandle vertexBuffer;
        BufferHandle indexBuffer;
        BufferHandle uniformBuffer;
        std::vector<BindingSetHandle> sets;
        auto cleanup = [&]()
        {
            Status first = Status::success();
            for (auto& set : sets) destroy(set, first);
            destroy(staging, first);
            destroy(vertexBuffer, first);
            destroy(indexBuffer, first);
            destroy(uniformBuffer, first);
            destroyTextures(textures, first);
            return first;
        };

        staging = mDevice.createBuffer(
            {total, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
        if (status) vertexBuffer = mDevice.createBuffer(
            {vertexBytes, ResourceUsage::Vertex |
                ResourceUsage::TransferDestination, MemoryClass::DeviceLocal},
            status);
        if (status) indexBuffer = mDevice.createBuffer(
            {indexBytes, ResourceUsage::Index |
                ResourceUsage::TransferDestination, MemoryClass::DeviceLocal},
            status);
        if (status) uniformBuffer = mDevice.createBuffer(
            {uniformStride * executionCount, ResourceUsage::Uniform |
                ResourceUsage::TransferDestination, MemoryClass::DeviceLocal},
            status);
        if (status) status = mDevice.writeBuffer(staging, 0, bytes);
        for (std::size_t execution = 0;
             status && execution < executionCount; ++execution)
        {
            BindingSetDesc desc;
            desc.shader = mShader;
            desc.group = 0;
            desc.resources = {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 uniformBuffer, uniformStride * execution, UNIFORM_BYTES,
                 {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                 {}, 0, 0, textures[0].view, mNormalSampler},
                {2, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                 {}, 0, 0, textures[nextTexture].view, mNormalSampler},
                {3, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                 {}, 0, 0, refractionView, mClampSampler},
                {4, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                 {}, 0, 0, dependencies.reflectionColorView, mClampSampler},
                {5, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                 {}, 0, 0, dependencies.exclusionMaskView, mClampSampler},
                {6, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                 {}, 0, 0, dependencies.waterDepthView, mDepthSampler}};
            sets.push_back(mDevice.createBindingSet(desc, status));
        }
        if (!status) { cleanup(); return status; }

        CommandContext& commands = mDevice.commandContext();
        bool frame = false;
        bool rendering = false;
        status = commands.beginFrame();
        frame = status.ok();
        const std::array<BufferCopyRegion, 1> vertexCopy{{
            {0, 0, vertexBytes}}};
        const std::array<BufferCopyRegion, 1> indexCopy{{
            {indexOffset, 0, indexBytes}}};
        const std::array<BufferCopyRegion, 1> uniformCopy{{
            {uniformOffset, 0, uniformStride * executionCount}}};
        if (status) status = commands.copyBuffer(
            staging, vertexBuffer, vertexCopy);
        if (status) status = commands.copyBuffer(
            staging, indexBuffer, indexCopy);
        if (status) status = commands.copyBuffer(
            staging, uniformBuffer, uniformCopy);
        RenderingInfo renderingInfo;
        renderingInfo.semanticId = 0x5030453257415445ull;
        renderingInfo.width = targets.width;
        renderingInfo.height = targets.height;
        renderingInfo.colors.push_back(
            {mOutputView, OUTPUT_FORMAT, LoadOp::Clear, StoreOp::Store, {}});
        if (status)
        {
            status = commands.beginRendering(renderingInfo);
            rendering = status.ok();
        }
        if (status) status = commands.setViewport(
            {0, 0, static_cast<float>(targets.width),
             static_cast<float>(targets.height), 0, 1});
        if (status) status = commands.setScissor(
            {0, 0, targets.width, targets.height});
        if (status) status = commands.bindPipeline(mCopyPipeline);
        if (status) status = commands.bindVertexBuffer(0, vertexBuffer, 0);
        if (status) status = commands.bindIndexBuffer(
            indexBuffer, 0, IndexType::UInt32);
        if (status) status = commands.bindBindingSet(0, sets[0]);
        if (status) status = commands.drawIndexed({3, 1, 0, 0, 0});
        if (rendering)
        {
            const Status ended = commands.endRendering();
            rendering = false;
            if (status && !ended) status = ended;
        }
        if (status)
        {
            BufferImageCopyRegion region;
            region.imageSubresource = {ImageAspect::Color, 0, 0, 1};
            region.imageExtent = {targets.width, targets.height, 1};
            const std::array<BufferImageCopyRegion, 1> copies{{region}};
            status = commands.copyImageToBuffer(
                mOutputImage, mInputReadback, copies);
        }
        renderingInfo.colors[0].load = LoadOp::Load;
        if (status)
        {
            status = commands.beginRendering(renderingInfo);
            rendering = status.ok();
        }
        if (status) status = commands.setViewport(
            {0, 0, static_cast<float>(targets.width),
             static_cast<float>(targets.height), 0, 1});
        if (status) status = commands.setScissor(
            {0, 0, targets.width, targets.height});
        if (status) status = commands.bindPipeline(mWaterPipeline);
        if (status) status = commands.bindVertexBuffer(0, vertexBuffer, 0);
        if (status) status = commands.bindIndexBuffer(
            indexBuffer, 0, IndexType::UInt32);
        for (std::size_t draw = 0;
             status && draw < packet.waterDraws.size(); ++draw)
        {
            status = commands.bindBindingSet(0, sets[draw + 1]);
            if (status)
            {
                const auto& waterDraw = packet.waterDraws[draw];
                status = commands.drawIndexed(
                    {waterDraw.indexCount, 1,
                     waterDraw.firstIndex + 3, 0, 0});
            }
        }
        if (rendering)
        {
            const Status ended = commands.endRendering();
            rendering = false;
            if (status && !ended) status = ended;
        }
        if (status)
        {
            BufferImageCopyRegion region;
            region.imageSubresource = {ImageAspect::Color, 0, 0, 1};
            region.imageExtent = {targets.width, targets.height, 1};
            const std::array<BufferImageCopyRegion, 1> copies{{region}};
            status = commands.copyImageToBuffer(
                mOutputImage, mReadback, copies);
        }
        if (frame)
        {
            const Status ended = commands.endFrame();
            frame = false;
            if (status && !ended) status = ended;
        }
        const Status cleaned = cleanup();
        if (!status) return status;
        if (!cleaned) return cleaned;

        mResult = {};
        mResult.frameId = packet.frameId;
        mResult.sceneEpoch = packet.sceneEpoch;
        mResult.resourceEpoch = packet.resourceEpoch;
        mResult.targetGeneration = targets.generation;
        mResult.draws = static_cast<std::uint32_t>(packet.waterDraws.size());
        mResult.uploadBytes = total + textureBytes;
        mResult.underwater = underwater;
        mResult.packetSha256 = environmentScenePacketSha256(packet);
        mPending = true;
        return Status::success();
    }

    Status poll(ProductionWaterResult& result)
    {
        result = {};
        if (!mPending)
            return Status::failure(StatusCode::InvalidState,
                                   "production water executor has no pending result");
        std::vector<std::byte> inputPixels(
            static_cast<std::size_t>(mWidth) * mHeight * OUTPUT_BYTES);
        std::vector<std::byte> pixels(inputPixels.size());
        Status status = mDevice.readBuffer(mInputReadback, 0, inputPixels);
        if (!status) return status;
        status = mDevice.readBuffer(mReadback, 0, pixels);
        if (!status) return status;
        if (mDevice.backend() == Backend::OpenGL)
        {
            const std::size_t row =
                static_cast<std::size_t>(mWidth) * OUTPUT_BYTES;
            for (std::uint32_t y = 0; y < mHeight / 2; ++y)
            {
                std::swap_ranges(
                    pixels.begin() + y * row,
                    pixels.begin() + (y + 1) * row,
                    pixels.begin() + (mHeight - 1 - y) * row);
                std::swap_ranges(
                    inputPixels.begin() + y * row,
                    inputPixels.begin() + (y + 1) * row,
                    inputPixels.begin() + (mHeight - 1 - y) * row);
            }
        }
        mResult.colorSha256 = sha256(pixels);
        mResult.inputPixels = inputPixels;
        mResult.colorPixels = pixels;
        for (std::size_t pixel = 0;
             pixel < static_cast<std::size_t>(mWidth) * mHeight; ++pixel)
        {
            const auto begin = pixels.begin() + pixel * OUTPUT_BYTES;
            if (std::any_of(begin, begin + OUTPUT_BYTES,
                [](std::byte value) { return value != std::byte{}; }))
                ++mResult.nonClearPixels;
            const auto input = inputPixels.begin() + pixel * OUTPUT_BYTES;
            if (!std::equal(begin, begin + OUTPUT_BYTES, input))
                ++mResult.modifiedPixels;
        }
        result = std::move(mResult);
        mResult = {};
        mPending = false;
        return Status::success();
    }

    bool pending() const { return mPending; }

    Status shutdown()
    {
        if (mShutdown) return Status::success();
        mShutdown = true;
        mPending = false;
        Status first = Status::success();
        destroyOutput(first);
        destroy(mWaterPipeline, first);
        destroy(mCopyPipeline, first);
        destroy(mNormalSampler, first);
        destroy(mDepthSampler, first);
        destroy(mClampSampler, first);
        destroy(mShader, first);
        return first;
    }

private:
    template<typename Handle>
    void destroy(Handle& handle, Status& first)
    {
        if (!handle) return;
        const Status status = mDevice.destroy(handle);
        if (first && !status) first = status;
        handle = {};
    }

    void destroyTextures(std::vector<Texture>& textures)
    {
        Status ignored = Status::success();
        destroyTextures(textures, ignored);
    }

    void destroyTextures(std::vector<Texture>& textures, Status& first)
    {
        for (auto& texture : textures)
        {
            destroy(texture.view, first);
            destroy(texture.image, first);
        }
    }

    Status uploadTextures(std::vector<Texture>& textures)
    {
        std::uint64_t total = 0;
        for (const auto& texture : textures) total += texture.bytes.size();
        Status status = Status::success();
        BufferHandle staging = mDevice.createBuffer(
            {total, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
        std::vector<std::byte> bytes;
        bytes.reserve(static_cast<std::size_t>(total));
        std::vector<std::uint64_t> offsets;
        for (auto& texture : textures)
        {
            offsets.push_back(bytes.size());
            bytes.insert(bytes.end(), texture.bytes.begin(), texture.bytes.end());
            if (status) texture.image = mDevice.createImage(
                {{texture.width, texture.height, 1}, texture.format,
                 ResourceUsage::Sampled | ResourceUsage::TransferDestination,
                 1, 1, 1}, status);
            if (status) texture.view = mDevice.createImageView(
                {texture.image, texture.format,
                 {ImageAspect::Color, 0, 1, 0, 1}}, status);
        }
        if (status) status = mDevice.writeBuffer(staging, 0, bytes);
        if (status)
        {
            CommandContext& commands = mDevice.commandContext();
            status = commands.beginFrame();
            for (std::size_t texture = 0;
                 status && texture < textures.size(); ++texture)
            {
                BufferImageCopyRegion copy;
                copy.bufferOffset = offsets[texture];
                copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                copy.imageExtent = {
                    textures[texture].width, textures[texture].height, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{copy}};
                status = commands.copyBufferToImage(
                    staging, textures[texture].image, copies);
            }
            const Status ended = commands.endFrame();
            if (status && !ended) status = ended;
        }
        if (staging)
        {
            const Status destroyed = mDevice.destroy(staging);
            if (status && !destroyed) status = destroyed;
        }
        if (!status) destroyTextures(textures);
        return status;
    }

    void destroyOutput(Status& first)
    {
        destroy(mReadback, first);
        destroy(mInputReadback, first);
        destroy(mOutputView, first);
        destroy(mOutputImage, first);
        mWidth = mHeight = 0;
        mGeneration = 0;
    }

    Status ensureOutput(const ProductionFrameTargetSet& targets)
    {
        if (mGeneration == targets.generation &&
            mWidth == targets.width && mHeight == targets.height)
            return Status::success();
        Status first = Status::success();
        destroyOutput(first);
        if (!first) return first;
        Status status = Status::success();
        mOutputImage = mDevice.createImage(
            {{targets.width, targets.height, 1}, OUTPUT_FORMAT,
             ResourceUsage::ColorAttachment | ResourceUsage::Sampled |
                 ResourceUsage::TransferSource,
             1, 1, 1}, status);
        if (status) mOutputView = mDevice.createImageView(
            {mOutputImage, OUTPUT_FORMAT,
             {ImageAspect::Color, 0, 1, 0, 1}}, status);
        if (status) mReadback = mDevice.createBuffer(
            {static_cast<std::uint64_t>(targets.width) * targets.height *
                 OUTPUT_BYTES,
             ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
        if (status) mInputReadback = mDevice.createBuffer(
            {static_cast<std::uint64_t>(targets.width) * targets.height *
                 OUTPUT_BYTES,
             ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
        if (!status)
        {
            Status ignored = Status::success();
            destroyOutput(ignored);
            return status;
        }
        mWidth = targets.width;
        mHeight = targets.height;
        mGeneration = targets.generation;
        return Status::success();
    }

    Status initialize()
    {
        if (mShader) return Status::success();
        if (mShutdown)
            return Status::failure(StatusCode::InvalidState,
                                   "production water executor is shut down");
        Status status = Status::success();
        mShader = mDevice.createShaderPackage(mShaderPackage, status);
        SamplerDesc clamp;
        clamp.addressU = clamp.addressV = clamp.addressW =
            AddressMode::ClampToEdge;
        if (status) mClampSampler = mDevice.createSampler(clamp, status);
        SamplerDesc normal;
        normal.addressU = normal.addressV = normal.addressW = AddressMode::Repeat;
        if (status) mNormalSampler = mDevice.createSampler(normal, status);
        SamplerDesc depth = clamp;
        depth.minFilter = depth.magFilter = depth.mipFilter = Filter::Nearest;
        if (status) mDepthSampler = mDevice.createSampler(depth, status);

        auto makePipeline = [&]()
        {
            PipelineDesc pipeline;
            pipeline.shader = mShader;
            pipeline.topology = PrimitiveTopology::Triangles;
            pipeline.cullMode = CullMode::None;
            // Water samples the shared depth image, so it cannot also bind it
            // as an attachment. The fragment shader performs the equivalent
            // reverse-Z rejection explicitly.
            pipeline.depthTest = false;
            pipeline.depthWrite = false;
            pipeline.depthCompare = CompareOp::GreaterEqual;
            pipeline.colorFormats = {OUTPUT_FORMAT};
            pipeline.blendStates = {BlendState{}};
            pipeline.vertexBuffers = {{
                0, sizeof(WaterSceneVertex), VertexInputRate::PerVertex}};
            pipeline.vertexAttributes = {
                {0, 0, VertexFormat::Float32x3,
                 offsetof(WaterSceneVertex, position)},
                {1, 0, VertexFormat::Float32x3,
                 offsetof(WaterSceneVertex, normal)}};
            return mDevice.createPipeline(pipeline, status);
        };
        if (status) mCopyPipeline = makePipeline();
        if (status) mWaterPipeline = makePipeline();
        if (!status)
        {
            const Status failure = status;
            shutdown();
            return failure;
        }
        return Status::success();
    }

    Device& mDevice;
    ShaderPackageDesc mShaderPackage;
    ShaderPackageHandle mShader;
    PipelineHandle mCopyPipeline;
    PipelineHandle mWaterPipeline;
    SamplerHandle mClampSampler;
    SamplerHandle mNormalSampler;
    SamplerHandle mDepthSampler;
    ImageHandle mOutputImage;
    ImageViewHandle mOutputView;
    BufferHandle mReadback;
    BufferHandle mInputReadback;
    std::uint32_t mWidth = 0;
    std::uint32_t mHeight = 0;
    std::uint64_t mGeneration = 0;
    ProductionWaterResult mResult;
    bool mPending = false;
    bool mShutdown = false;
};

ProductionWaterExecutor::ProductionWaterExecutor(
    Device& device, ShaderPackageDesc shader) :
    mImpl(std::make_unique<Impl>(device, std::move(shader)))
{
}

ProductionWaterExecutor::~ProductionWaterExecutor() = default;

Status ProductionWaterExecutor::submit(
    const EnvironmentScenePacket& packet,
    const ProductionFrameTargetSet& targets,
    const ProductionWaterDependencies& dependencies,
    const ProductionWaterLimits& limits)
{
    return mImpl->submit(packet, targets, dependencies, limits);
}

Status ProductionWaterExecutor::poll(ProductionWaterResult& result)
{
    return mImpl->poll(result);
}

bool ProductionWaterExecutor::pending() const { return mImpl->pending(); }

Status ProductionWaterExecutor::shutdown() { return mImpl->shutdown(); }
} // namespace LL::GHI
