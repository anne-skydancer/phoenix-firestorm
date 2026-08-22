/**
 * @file llghiproductionwaterresources.cpp
 * @brief GHI-owned upload and lifetime for captured water dependencies.
 */
#include "linden_common.h"

#include "ghi/include/llghiproductionwaterresources.h"

#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace LL::GHI
{
namespace
{
Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

Status unsupported(const char* message)
{
    return Status::failure(StatusCode::Unsupported, message);
}

const MaterialTextureResource* resource(
    const EnvironmentScenePacket& packet, EnvironmentTextureSemantic semantic)
{
    const auto binding = std::find_if(
        packet.water.textures.begin(), packet.water.textures.end(),
        [semantic](const auto& value) { return value.semantic == semantic; });
    if (binding == packet.water.textures.end() ||
        binding->texture >= packet.textures.size())
        return nullptr;
    return &packet.textures[binding->texture];
}

struct Upload
{
    const MaterialTextureResource* source = nullptr;
    Format format = Format::Undefined;
    ImageHandle image;
    ImageViewHandle view;
};
} // namespace

class ProductionWaterResources::Impl
{
public:
    explicit Impl(Device& device) : mDevice(device) {}
    ~Impl() { shutdown(); }

    Status update(const EnvironmentScenePacket& packet,
                  std::uint64_t generation,
                  const ProductionWaterResourceLimits& limits,
                  ProductionWaterResourceResult& result)
    {
        result = {};
        Status status = validateEnvironmentScenePacket(packet);
        if (!status) return status;
        if (packet.version < 3 || !generation)
            return invalid("production water resources require packet v3 and a generation");
        if (!limits.maxPixels || !limits.maxUploadBytes)
            return invalid("production water resource limits must be nonzero");
        if (packet.resourceEpoch <= mResourceEpoch)
            return invalid("production water resources require an increasing resource epoch");

        std::array<Upload, 2> uploads{{
            {resource(packet, EnvironmentTextureSemantic::ReflectionColor),
             Format::RGBA8UNorm},
            {resource(packet, EnvironmentTextureSemantic::WaterExclusionMask),
             Format::R8UNorm}}};
        if (!uploads[0].source || !uploads[1].source ||
            uploads[0].source->components != 4 ||
            uploads[1].source->components != 1)
            return invalid("production water dependency formats are invalid");

        std::uint64_t uploadBytes = 0;
        for (const Upload& upload : uploads)
        {
            const auto& source = *upload.source;
            const std::uint64_t pixels =
                static_cast<std::uint64_t>(source.width) * source.height;
            if (!source.width || !source.height || pixels > limits.maxPixels ||
                source.comparability != ResourceComparability::Comparable ||
                source.decodedPixels.empty() ||
                pixels > std::numeric_limits<std::uint64_t>::max() /
                    source.components ||
                source.decodedPixels.size() != pixels * source.components ||
                source.decodedPixels.size() >
                    std::numeric_limits<std::uint64_t>::max() - uploadBytes)
                return invalid("production water dependency content is invalid");
            uploadBytes += source.decodedPixels.size();
        }
        if (uploadBytes > limits.maxUploadBytes)
            return unsupported("production water dependency upload exceeds limit");

        BufferHandle staging = mDevice.createBuffer(
            {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload},
            status);
        std::vector<std::byte> bytes;
        bytes.reserve(static_cast<std::size_t>(uploadBytes));
        std::array<std::uint64_t, 2> offsets{};
        for (std::size_t index = 0; index < uploads.size(); ++index)
        {
            offsets[index] = bytes.size();
            bytes.insert(bytes.end(), uploads[index].source->decodedPixels.begin(),
                         uploads[index].source->decodedPixels.end());
            if (!status) break;
            const auto& source = *uploads[index].source;
            uploads[index].image = mDevice.createImage(
                {{source.width, source.height, 1}, uploads[index].format,
                 ResourceUsage::Sampled | ResourceUsage::TransferDestination |
                     ResourceUsage::TransferSource,
                 1, 1, 1}, status);
            if (status) uploads[index].view = mDevice.createImageView(
                {uploads[index].image, uploads[index].format,
                 {ImageAspect::Color, 0, 1, 0, 1}}, status);
        }
        if (status) status = mDevice.writeBuffer(staging, 0, bytes);
        if (status)
        {
            CommandContext& commands = mDevice.commandContext();
            status = commands.beginFrame();
            for (std::size_t index = 0; status && index < uploads.size(); ++index)
            {
                BufferImageCopyRegion copy;
                copy.bufferOffset = offsets[index];
                copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                copy.imageExtent = {uploads[index].source->width,
                                    uploads[index].source->height, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{copy}};
                status = commands.copyBufferToImage(
                    staging, uploads[index].image, copies);
            }
            const Status ended = commands.endFrame();
            if (status && !ended) status = ended;
        }
        if (staging)
        {
            const Status destroyed = mDevice.destroy(staging);
            if (status && !destroyed) status = destroyed;
        }
        if (!status)
        {
            destroy(uploads);
            return status;
        }

        std::array<Upload, 2> previous = mResources;
        mResources = uploads;
        for (auto& resource : mResources) resource.source = nullptr;
        mDependencies = {generation, uploads[0].view, uploads[1].view};
        mResourceEpoch = packet.resourceEpoch;
        status = destroy(previous);
        if (!status) return status;

        result.frameId = packet.frameId;
        result.resourceEpoch = packet.resourceEpoch;
        result.generation = generation;
        result.reflectionWidth = uploads[0].source->width;
        result.reflectionHeight = uploads[0].source->height;
        result.exclusionWidth = uploads[1].source->width;
        result.exclusionHeight = uploads[1].source->height;
        result.uploadBytes = uploadBytes;
        result.reflectionSha256 = sha256(uploads[0].source->decodedPixels);
        result.exclusionSha256 = sha256(uploads[1].source->decodedPixels);
        return Status::success();
    }

    const ProductionWaterDependencies& dependencies() const
    {
        return mDependencies;
    }

    Status shutdown()
    {
        std::array<Upload, 2> previous = mResources;
        mResources = {};
        mDependencies = {};
        mResourceEpoch = 0;
        return destroy(previous);
    }

private:
    Status destroy(std::array<Upload, 2>& resources)
    {
        Status first = Status::success();
        for (auto& resource : resources)
        {
            if (resource.view)
            {
                const Status status = mDevice.destroy(resource.view);
                if (first && !status) first = status;
                resource.view = {};
            }
            if (resource.image)
            {
                const Status status = mDevice.destroy(resource.image);
                if (first && !status) first = status;
                resource.image = {};
            }
        }
        return first;
    }

    Device& mDevice;
    std::array<Upload, 2> mResources{};
    ProductionWaterDependencies mDependencies;
    std::uint64_t mResourceEpoch = 0;
};

ProductionWaterResources::ProductionWaterResources(Device& device) :
    mImpl(std::make_unique<Impl>(device))
{
}

ProductionWaterResources::~ProductionWaterResources()
{
    if (mImpl) mImpl->shutdown();
}

Status ProductionWaterResources::update(
    const EnvironmentScenePacket& packet, std::uint64_t generation,
    const ProductionWaterResourceLimits& limits,
    ProductionWaterResourceResult& result)
{
    return mImpl->update(packet, generation, limits, result);
}

const ProductionWaterDependencies& ProductionWaterResources::dependencies() const
{
    return mImpl->dependencies();
}

Status ProductionWaterResources::shutdown()
{
    return mImpl->shutdown();
}
} // namespace LL::GHI
