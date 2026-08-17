/**
 * @file llghiopaqueoffscreenprobe.cpp
 * @brief Asynchronous, non-presenting replay of a live opaque scene packet.
 */

#include "linden_common.h"

#include "ghi/include/llghiopaqueoffscreenprobe.h"

#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace LL::GHI
{
namespace
{
constexpr std::uint32_t PROBE_WIDTH = 256;
constexpr std::uint32_t PROBE_HEIGHT = 256;
constexpr std::array<Format, 4> COLOR_FORMATS{{
    Format::RGBA8UNorm, Format::RGBA8UNorm,
    Format::RGBA16UNorm, Format::RGBA16Float}};
constexpr std::array<std::uint32_t, 4> BYTES_PER_PIXEL{{4, 4, 8, 8}};
constexpr std::array<float, 16> MATERIAL{{
    1.f, 1.f, 1.f, 1.f, 0.25f, 0.5f, 0.75f, 1.f,
    0.5f, 0.5f, 1.f, 1.f, 0.125f, 0.25f, 0.5f, 1.f}};

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}
}

class OpaqueOffscreenProbe::Impl
{
public:
    Impl(Device& device, ShaderPackageDesc shader_package) :
        mDevice(device),
        mShaderPackage(std::move(shader_package))
    {
    }

    ~Impl()
    {
        shutdown();
    }

    Status submit(const OpaqueScenePacket& packet,
                  const OpaquePacketTransferLimits& limits)
    {
        if (mPending)
            return Status::failure(StatusCode::NotReady,
                                   "opaque offscreen probe is still pending");
        if (packet.vertices.empty() || packet.indices.empty() || packet.draws.empty())
            return invalid("live opaque packet contains no drawable geometry");
        if (!limits.maxVertices || !limits.maxIndices || !limits.maxDraws ||
            !limits.maxUploadBytes)
            return invalid("opaque offscreen probe limits must be nonzero");
        if (packet.vertices.size() > limits.maxVertices ||
            packet.indices.size() > limits.maxIndices ||
            packet.draws.size() > limits.maxDraws)
            return invalid("live opaque packet exceeds offscreen element limits");

        Status status = initialize();
        if (!status) return status;

        const std::uint64_t alignment = std::max<std::uint64_t>(
            16, mDevice.capabilities().uniformBufferOffsetAlignment);
        const auto align = [alignment](std::uint64_t value)
        {
            if (value > std::numeric_limits<std::uint64_t>::max() - alignment + 1)
                return std::numeric_limits<std::uint64_t>::max();
            return (value + alignment - 1) / alignment * alignment;
        };
        const std::uint64_t vertexBytes =
            packet.vertices.size() * sizeof(OpaqueSceneVertex);
        const std::uint64_t indexBytes =
            packet.indices.size() * sizeof(std::uint32_t);
        const std::uint64_t vertexOffset = 0;
        const std::uint64_t indexOffset = align(vertexBytes);
        const std::uint64_t transformOffset = align(indexOffset + indexBytes);
        const std::uint64_t transformStride =
            align(sizeof(packet.draws.front().transform));
        if (transformStride == std::numeric_limits<std::uint64_t>::max() ||
            packet.draws.size() >
                (std::numeric_limits<std::uint64_t>::max() - transformOffset) /
                    transformStride)
            return invalid("opaque offscreen transform size overflow");
        const std::uint64_t transformBytes =
            transformStride * packet.draws.size();
        const std::uint64_t materialOffset = align(transformOffset + transformBytes);
        if (materialOffset == std::numeric_limits<std::uint64_t>::max() ||
            materialOffset > std::numeric_limits<std::uint64_t>::max() -
                                 sizeof(MATERIAL))
            return invalid("opaque offscreen upload size overflow");
        const std::uint64_t uploadBytes = materialOffset + sizeof(MATERIAL);
        const RendererCapabilities& capabilities = mDevice.capabilities();
        if (uploadBytes > limits.maxUploadBytes)
            return invalid("live opaque packet exceeds offscreen byte limit");
        if (uploadBytes > capabilities.maxBufferSize ||
            vertexBytes > capabilities.maxBufferSize ||
            indexBytes > capabilities.maxBufferSize ||
            transformBytes > capabilities.maxBufferSize)
            return Status::failure(StatusCode::Unsupported,
                                   "opaque offscreen packet exceeds device buffer limits");
        if (sizeof(packet.draws.front().transform) >
                capabilities.maxUniformBufferSize ||
            sizeof(MATERIAL) > capabilities.maxUniformBufferSize)
            return Status::failure(StatusCode::Unsupported,
                                   "opaque offscreen binding exceeds uniform limit");

        std::vector<std::byte> encoded;
        if (!(status = encodeOpaqueScenePacket(packet, encoded))) return status;
        std::vector<std::byte> uploadData(static_cast<std::size_t>(uploadBytes));
        std::memcpy(uploadData.data() + vertexOffset, packet.vertices.data(),
                    static_cast<std::size_t>(vertexBytes));
        std::memcpy(uploadData.data() + indexOffset, packet.indices.data(),
                    static_cast<std::size_t>(indexBytes));
        for (std::size_t draw = 0; draw < packet.draws.size(); ++draw)
            std::memcpy(uploadData.data() + transformOffset + transformStride * draw,
                        packet.draws[draw].transform.data(),
                        sizeof(packet.draws[draw].transform));
        std::memcpy(uploadData.data() + materialOffset, MATERIAL.data(),
                    sizeof(MATERIAL));

        BufferHandle upload;
        BufferHandle vertices;
        BufferHandle indices;
        BufferHandle transforms;
        BufferHandle material;
        BindingSetHandle materialSet;
        std::vector<BindingSetHandle> frameSets;
        auto cleanup = [&]()
        {
            Status first = Status::success();
            for (BindingSetHandle& set : frameSets) destroy(set, first);
            destroy(materialSet, first);
            destroy(upload, first);
            destroy(vertices, first);
            destroy(indices, first);
            destroy(transforms, first);
            destroy(material, first);
            return first;
        };

        upload = mDevice.createBuffer(
            {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
        if (status)
            vertices = mDevice.createBuffer(
                {vertexBytes, ResourceUsage::Vertex |
                                  ResourceUsage::TransferDestination,
                 MemoryClass::DeviceLocal}, status);
        if (status)
            indices = mDevice.createBuffer(
                {indexBytes, ResourceUsage::Index |
                                 ResourceUsage::TransferDestination,
                 MemoryClass::DeviceLocal}, status);
        if (status)
            transforms = mDevice.createBuffer(
                {transformBytes, ResourceUsage::Uniform |
                                     ResourceUsage::TransferDestination,
                 MemoryClass::DeviceLocal}, status);
        if (status)
            material = mDevice.createBuffer(
                {sizeof(MATERIAL), ResourceUsage::Uniform |
                                       ResourceUsage::TransferDestination,
                 MemoryClass::DeviceLocal}, status);
        if (!status)
        {
            cleanup();
            return status;
        }
        if (!(status = mDevice.writeBuffer(upload, 0, uploadData)))
        {
            cleanup();
            return status;
        }

        frameSets.reserve(packet.draws.size());
        for (std::size_t draw = 0; draw < packet.draws.size(); ++draw)
        {
            BindingSetDesc desc;
            desc.shader = mShader;
            desc.group = 0;
            desc.resources.push_back(
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer, transforms,
                 transformStride * draw, sizeof(packet.draws[draw].transform), {}, {}});
            frameSets.push_back(mDevice.createBindingSet(desc, status));
            if (!status)
            {
                cleanup();
                return status;
            }
        }
        BindingSetDesc materialDesc;
        materialDesc.shader = mShader;
        materialDesc.group = 2;
        materialDesc.resources.push_back(
            {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
             material, 0, sizeof(MATERIAL), {}, {}});
        materialSet = mDevice.createBindingSet(materialDesc, status);
        if (!status)
        {
            cleanup();
            return status;
        }

        CommandContext& commands = mDevice.commandContext();
        bool frameBegun = false;
        bool renderingBegun = false;
        status = commands.beginFrame();
        frameBegun = status.ok();
        const std::array<BufferCopyRegion, 1> vertexCopy{{
            {vertexOffset, 0, vertexBytes}}};
        const std::array<BufferCopyRegion, 1> indexCopy{{
            {indexOffset, 0, indexBytes}}};
        const std::array<BufferCopyRegion, 1> transformCopy{{
            {transformOffset, 0, transformBytes}}};
        const std::array<BufferCopyRegion, 1> materialCopy{{
            {materialOffset, 0, sizeof(MATERIAL)}}};
        if (status) status = commands.copyBuffer(upload, vertices, vertexCopy);
        if (status) status = commands.copyBuffer(upload, indices, indexCopy);
        if (status) status = commands.copyBuffer(upload, transforms, transformCopy);
        if (status) status = commands.copyBuffer(upload, material, materialCopy);

        RenderingInfo rendering;
        rendering.semanticId = 0x49325f4c495645ull; // "I2_LIVE"
        rendering.width = PROBE_WIDTH;
        rendering.height = PROBE_HEIGHT;
        for (std::size_t target = 0; target < mColors.size(); ++target)
            rendering.colors.push_back(
                {mColorViews[target], COLOR_FORMATS[target], LoadOp::Clear,
                 StoreOp::Store, {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
        rendering.depthStencil = AttachmentDesc{
            mDepthView, mDepthFormat, LoadOp::Clear, StoreOp::Store,
            {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}};
        if (status)
        {
            status = commands.beginRendering(rendering);
            renderingBegun = status.ok();
        }
        if (status) status = commands.bindPipeline(mPipeline);
        if (status) status = commands.bindBindingSet(2, materialSet);
        if (status) status = commands.setViewport(
            {0.f, 0.f, static_cast<float>(PROBE_WIDTH),
             static_cast<float>(PROBE_HEIGHT), 0.f, 1.f});
        if (status) status = commands.setScissor(
            {0, 0, PROBE_WIDTH, PROBE_HEIGHT});
        if (status) status = commands.bindVertexBuffer(0, vertices, 0);
        if (status) status = commands.bindIndexBuffer(indices, 0, IndexType::UInt32);
        for (std::size_t draw = 0; status && draw < packet.draws.size(); ++draw)
        {
            status = commands.bindBindingSet(0, frameSets[draw]);
            if (status)
                status = commands.drawIndexed(
                    {packet.draws[draw].indexCount, 1,
                     packet.draws[draw].firstIndex, 0, 0});
        }
        if (renderingBegun)
        {
            const Status ended = commands.endRendering();
            renderingBegun = false;
            if (status && !ended) status = ended;
        }
        if (status)
        {
            for (std::size_t target = 0; status && target < mColors.size(); ++target)
            {
                BufferImageCopyRegion region;
                region.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                region.imageExtent = {PROBE_WIDTH, PROBE_HEIGHT, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{region}};
                status = commands.copyImageToBuffer(
                    mColors[target], mReadbacks[target], copies);
            }
        }
        if (frameBegun)
        {
            const Status ended = commands.endFrame();
            frameBegun = false;
            if (status && !ended) status = ended;
        }
        const Status cleanupStatus = cleanup();
        if (!status) return status;
        if (!cleanupStatus) return cleanupStatus;

        mPending = true;
        mPendingResult = {};
        mPendingResult.frameId = packet.frameId;
        mPendingResult.vertices = static_cast<std::uint32_t>(packet.vertices.size());
        mPendingResult.indices = static_cast<std::uint32_t>(packet.indices.size());
        mPendingResult.draws = static_cast<std::uint32_t>(packet.draws.size());
        mPendingResult.packetSha256 = sha256(encoded);
        return Status::success();
    }

    Status poll(OpaqueOffscreenProbeResult& result)
    {
        result = {};
        if (!mPending)
            return Status::failure(StatusCode::InvalidState,
                                   "opaque offscreen probe has no pending sample");
        for (std::size_t target = 0; target < mReadbacks.size(); ++target)
        {
            Status status = mDevice.readBuffer(mReadbacks[target], 0, mPixels[target]);
            if (!status) return status;
        }
        for (std::size_t target = 0; target < mPixels.size(); ++target)
        {
            if (mDevice.backend() == Backend::OpenGL)
            {
                const std::size_t rowBytes =
                    PROBE_WIDTH * BYTES_PER_PIXEL[target];
                for (std::uint32_t y = 0; y < PROBE_HEIGHT / 2; ++y)
                {
                    auto top = mPixels[target].begin() +
                        static_cast<std::ptrdiff_t>(y * rowBytes);
                    auto bottom = mPixels[target].begin() +
                        static_cast<std::ptrdiff_t>((PROBE_HEIGHT - 1 - y) * rowBytes);
                    std::swap_ranges(top, top + rowBytes, bottom);
                }
            }
            mPendingResult.colorSha256[target] = sha256(mPixels[target]);
            const std::size_t pixelCount =
                static_cast<std::size_t>(PROBE_WIDTH) * PROBE_HEIGHT;
            for (std::size_t pixel = 0; pixel < pixelCount; ++pixel)
            {
                const auto begin = mPixels[target].begin() +
                    static_cast<std::ptrdiff_t>(pixel * BYTES_PER_PIXEL[target]);
                if (std::any_of(begin, begin + BYTES_PER_PIXEL[target],
                                [](std::byte value)
                                { return value != std::byte{0}; }))
                    ++mPendingResult.nonClearPixels[target];
            }
        }
        result = std::move(mPendingResult);
        mPendingResult = {};
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
        destroy(mPipeline, first);
        destroy(mShader, first);
        destroy(mDepthView, first);
        destroy(mDepth, first);
        for (std::size_t target = 0; target < mColors.size(); ++target)
        {
            destroy(mColorViews[target], first);
            destroy(mColors[target], first);
            destroy(mReadbacks[target], first);
        }
        return first;
    }

private:
    template<typename HandleType>
    void destroy(HandleType& handle, Status& first)
    {
        if (!handle) return;
        const Status status = mDevice.destroy(handle);
        if (first && !status) first = status;
        handle = {};
    }

    void destroy(BindingSetHandle& handle, Status& first)
    {
        if (!handle) return;
        const Status status = mDevice.destroy(handle);
        if (first && !status) first = status;
        handle = {};
    }

    Status initialize()
    {
        if (mPipeline) return Status::success();
        if (mShutdown)
            return Status::failure(StatusCode::InvalidState,
                                   "opaque offscreen probe is shut down");
        const RendererCapabilities& capabilities = mDevice.capabilities();
        if (capabilities.maxColorAttachments < COLOR_FORMATS.size() ||
            capabilities.maxTexture2DSize < PROBE_WIDTH ||
            capabilities.preferredDepthStencilFormat == Format::Undefined)
            return Status::failure(StatusCode::Unsupported,
                                   "device lacks I2 offscreen target capabilities");

        Status status = Status::success();
        for (std::size_t target = 0; target < mColors.size(); ++target)
        {
            mColors[target] = mDevice.createImage(
                {{PROBE_WIDTH, PROBE_HEIGHT, 1}, COLOR_FORMATS[target],
                 ResourceUsage::ColorAttachment | ResourceUsage::TransferSource,
                 1, 1, 1}, status);
            if (status)
                mColorViews[target] = mDevice.createImageView(
                    {mColors[target], COLOR_FORMATS[target],
                     {ImageAspect::Color, 0, 1, 0, 1}}, status);
            if (status)
                mReadbacks[target] = mDevice.createBuffer(
                    {static_cast<std::uint64_t>(PROBE_WIDTH) * PROBE_HEIGHT *
                         BYTES_PER_PIXEL[target],
                     ResourceUsage::TransferDestination, MemoryClass::Readback},
                    status);
            mPixels[target].resize(
                static_cast<std::size_t>(PROBE_WIDTH) * PROBE_HEIGHT *
                BYTES_PER_PIXEL[target]);
            if (!status) break;
        }
        mDepthFormat = capabilities.preferredDepthStencilFormat;
        if (status)
            mDepth = mDevice.createImage(
                {{PROBE_WIDTH, PROBE_HEIGHT, 1}, mDepthFormat,
                 ResourceUsage::DepthStencilAttachment, 1, 1, 1}, status);
        if (status)
            mDepthView = mDevice.createImageView(
                {mDepth, mDepthFormat,
                 {ImageAspect::DepthStencil, 0, 1, 0, 1}}, status);
        if (status)
            mShader = mDevice.createShaderPackage(mShaderPackage, status);
        if (status)
        {
            PipelineDesc pipeline;
            pipeline.shader = mShader;
            pipeline.cullMode = CullMode::Back;
            pipeline.depthTest = true;
            pipeline.depthWrite = true;
            pipeline.depthCompare = CompareOp::GreaterEqual;
            pipeline.colorFormats.assign(COLOR_FORMATS.begin(), COLOR_FORMATS.end());
            pipeline.depthStencilFormat = mDepthFormat;
            pipeline.blendStates.assign(4, BlendState{});
            pipeline.blendStates[1].colorWriteMask = 0x07;
            pipeline.blendStates[2].colorWriteMask = 0x0b;
            pipeline.blendStates[3].colorWriteMask = 0x0d;
            pipeline.vertexBuffers = {
                {0, sizeof(OpaqueSceneVertex), VertexInputRate::PerVertex}};
            pipeline.vertexAttributes = {
                {0, 0, VertexFormat::Float32x3, 0},
                {1, 0, VertexFormat::UNorm8x4, 12}};
            mPipeline = mDevice.createPipeline(pipeline, status);
        }
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
    std::array<ImageHandle, 4> mColors{};
    std::array<ImageViewHandle, 4> mColorViews{};
    std::array<BufferHandle, 4> mReadbacks{};
    std::array<std::vector<std::byte>, 4> mPixels;
    ImageHandle mDepth;
    ImageViewHandle mDepthView;
    Format mDepthFormat = Format::Undefined;
    ShaderPackageHandle mShader;
    PipelineHandle mPipeline;
    OpaqueOffscreenProbeResult mPendingResult;
    bool mPending = false;
    bool mShutdown = false;
};

OpaqueOffscreenProbe::OpaqueOffscreenProbe(
    Device& device, ShaderPackageDesc shader_package) :
    mImpl(std::make_unique<Impl>(device, std::move(shader_package)))
{
}

OpaqueOffscreenProbe::~OpaqueOffscreenProbe() = default;

Status OpaqueOffscreenProbe::submit(
    const OpaqueScenePacket& packet,
    const OpaquePacketTransferLimits& limits)
{
    return mImpl->submit(packet, limits);
}

Status OpaqueOffscreenProbe::poll(OpaqueOffscreenProbeResult& result)
{
    return mImpl->poll(result);
}

bool OpaqueOffscreenProbe::pending() const
{
    return mImpl->pending();
}

Status OpaqueOffscreenProbe::shutdown()
{
    return mImpl->shutdown();
}

} // namespace LL::GHI
