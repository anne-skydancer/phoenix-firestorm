/**
 * @file llghiproductiontextureresidency.cpp
 * @brief I8b retained and deduplicated decoded-texture residency.
 */

#include "linden_common.h"

#include "ghi/include/llghiproductiontextureresidency.h"

#include "ghi/include/llghidevice.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <utility>

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

bool zeroDigest(const ResourceDigest& digest)
{
    return std::all_of(digest.begin(), digest.end(),
                       [](std::byte value) { return value == std::byte{}; });
}

ResourceDigest projectorSource(
    const std::array<std::uint8_t, 16>& source)
{
    ResourceDigest result{};
    std::transform(source.begin(), source.end(), result.begin(),
                   [](std::uint8_t value)
                   { return static_cast<std::byte>(value); });
    return result;
}

std::uint16_t mipLevels(std::uint32_t width, std::uint32_t height)
{
    std::uint16_t levels = 1;
    while (width > 1 || height > 1)
    {
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        ++levels;
    }
    return levels;
}

struct ContentKey
{
    ResourceDigest identity{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Format format = Format::Undefined;

    friend auto operator<=>(const ContentKey&, const ContentKey&) = default;
};

struct TextureRequest
{
    ProductionTextureSourceIdentity source;
    ContentKey content;
    std::vector<std::byte> pixels;
    std::uint16_t mipLevelCount = 1;
};

Status normalizeTexture(
    const ProductionTextureSourceIdentity& source,
    const ResourceDigest& contentIdentity,
    TextureColorSpace colorSpace,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t components,
    std::span<const std::byte> decoded,
    TextureRequest& request)
{
    if (zeroDigest(source.source) || zeroDigest(contentIdentity) || !width ||
        !height || !components || components > 4 || decoded.empty())
        return invalid("invalid decoded texture residency request");
    const std::uint64_t texels = static_cast<std::uint64_t>(width) * height;
    if (texels > std::numeric_limits<std::size_t>::max() / components ||
        decoded.size() != texels * components)
        return invalid("decoded texture residency byte count mismatch");

    request = {};
    request.source = source;
    request.content.identity = contentIdentity;
    request.content.width = width;
    request.content.height = height;
    request.mipLevelCount = mipLevels(width, height);

    const bool srgb = colorSpace == TextureColorSpace::SRGB;
    if (!srgb && components == 1)
    {
        request.content.format = Format::R8UNorm;
        request.pixels.assign(decoded.begin(), decoded.end());
        return Status::success();
    }
    if (!srgb && components == 2)
    {
        request.content.format = Format::RG8UNorm;
        request.pixels.assign(decoded.begin(), decoded.end());
        return Status::success();
    }

    request.content.format = srgb ? Format::RGBA8SRGB : Format::RGBA8UNorm;
    request.pixels.resize(static_cast<std::size_t>(texels) * 4);
    for (std::size_t texel = 0; texel < texels; ++texel)
    {
        const std::size_t sourceOffset = texel * components;
        const std::size_t targetOffset = texel * 4;
        if (components == 1)
        {
            request.pixels[targetOffset] = decoded[sourceOffset];
            request.pixels[targetOffset + 1] = decoded[sourceOffset];
            request.pixels[targetOffset + 2] = decoded[sourceOffset];
            request.pixels[targetOffset + 3] = std::byte{255};
        }
        else if (components == 2)
        {
            request.pixels[targetOffset] = decoded[sourceOffset];
            request.pixels[targetOffset + 1] = decoded[sourceOffset];
            request.pixels[targetOffset + 2] = decoded[sourceOffset];
            request.pixels[targetOffset + 3] = decoded[sourceOffset + 1];
        }
        else
        {
            std::copy_n(decoded.begin() + static_cast<std::ptrdiff_t>(sourceOffset),
                        components, request.pixels.begin() +
                            static_cast<std::ptrdiff_t>(targetOffset));
            if (components == 3)
                request.pixels[targetOffset + 3] = std::byte{255};
        }
    }
    return Status::success();
}
} // namespace

class ProductionTextureResidency::Impl
{
public:
    explicit Impl(Device& device) : mDevice(device) {}

    struct Resident
    {
        ImageHandle image;
        ImageViewHandle view;
        std::uint16_t mipLevelCount = 1;
        std::uint64_t bytes = 0;
        std::uint64_t lastUsedEpoch = 0;
    };

    struct SourceRecord
    {
        ContentKey content;
        std::uint64_t generation = 1;
        std::uint64_t lastSeenEpoch = 0;
    };

    Status update(const ProductionFramePacket& frame,
                  const ProductionTextureResidencyLimits& limits,
                  ProductionTextureResidencyResult& result)
    {
        result = {};
        result.frameId = frame.frameId;
        result.assemblyEpoch = frame.assemblyEpoch;
        if (!limits.maxEntries || !limits.maxSourceRecords ||
            !limits.maxResidentBytes || !limits.maxUploadBytesPerFrame ||
            !limits.staleAfterAssemblyEpochs)
            return invalid("production texture residency limits must be nonzero");
        Status status = validateProductionFramePacket(frame);
        if (!status) return status;
        if (frame.assemblyEpoch <= mLastAssemblyEpoch)
            return invalid("production texture residency requires increasing assembly epochs");

        std::vector<TextureRequest> requests;
        auto addMaterial = [&](const MaterialTextureResource& texture,
                               ProductionTextureDomain domain) -> Status
        {
            ++result.requestedSources;
            if (texture.decodedPixels.empty())
            {
                ++result.deferredSources;
                return Status::success();
            }
            TextureRequest request;
            Status added = normalizeTexture(
                {domain, texture.sourceIdentity}, texture.contentIdentity,
                texture.colorSpace, texture.width, texture.height,
                texture.components, texture.decodedPixels, request);
            if (added) requests.push_back(std::move(request));
            return added;
        };
        for (const auto& texture : frame.materials.textures)
            if (!(status = addMaterial(texture,
                    ProductionTextureDomain::Material))) return status;
        for (const auto& texture : frame.terrain.textures)
            if (!(status = addMaterial(texture,
                    ProductionTextureDomain::Terrain))) return status;
        for (const auto& texture : frame.lighting.projectorTextures)
        {
            ++result.requestedSources;
            TextureRequest request;
            status = normalizeTexture(
                {ProductionTextureDomain::Projector,
                 projectorSource(texture.sourceIdentity)},
                texture.contentIdentity, TextureColorSpace::SRGB,
                texture.width, texture.height, texture.components,
                texture.decodedPixels, request);
            if (!status) return status;
            requests.push_back(std::move(request));
        }

        std::map<ProductionTextureSourceIdentity, ContentKey> sources;
        std::map<ContentKey, const TextureRequest*> contents;
        for (const TextureRequest& request : requests)
        {
            const auto [source, inserted] = sources.emplace(
                request.source, request.content);
            if (!inserted && source->second != request.content)
                return invalid("one texture source resolved to multiple contents in a frame");
            const auto [content, unique] = contents.emplace(
                request.content, &request);
            if (!unique && content->second->pixels != request.pixels)
                return invalid("one content identity resolved to different decoded bytes");
        }
        result.uniqueContents = static_cast<std::uint32_t>(contents.size());

        std::uint64_t uploadBytes = 0;
        std::vector<const TextureRequest*> missing;
        for (const auto& [content, request] : contents)
        {
            auto resident = mResidents.find(content);
            if (resident != mResidents.end())
            {
                resident->second.lastUsedEpoch = frame.assemblyEpoch;
                ++result.cacheHits;
            }
            else
            {
                if (request->pixels.size() >
                    std::numeric_limits<std::uint64_t>::max() - uploadBytes)
                    return unsupported("production texture upload byte count overflow");
                uploadBytes += request->pixels.size();
                missing.push_back(request);
            }
        }
        if (uploadBytes > limits.maxUploadBytesPerFrame)
            return unsupported("production texture upload exceeds per-frame limit");

        std::set<ContentKey> protectedContents;
        for (const auto& [content, request] : contents)
            protectedContents.insert(content);
        while (mResidents.size() + missing.size() > limits.maxEntries ||
               mResidentBytes + uploadBytes > limits.maxResidentBytes)
        {
            auto victim = mResidents.end();
            for (auto candidate = mResidents.begin();
                 candidate != mResidents.end(); ++candidate)
            {
                if (protectedContents.contains(candidate->first)) continue;
                if (victim == mResidents.end() ||
                    candidate->second.lastUsedEpoch < victim->second.lastUsedEpoch)
                    victim = candidate;
            }
            if (victim == mResidents.end())
                return unsupported("active production textures exceed residency limits");
            status = destroyResident(victim);
            if (!status) return status;
            ++result.evictions;
        }

        if (!missing.empty())
        {
            status = uploadMissing(missing, frame.assemblyEpoch);
            if (!status) return status;
            result.uploads = static_cast<std::uint32_t>(missing.size());
            result.uploadBytes = uploadBytes;
        }

        for (const auto& [source, content] : sources)
        {
            auto record = mSources.find(source);
            if (record == mSources.end())
            {
                if (mSources.size() >= limits.maxSourceRecords)
                {
                    auto oldest = std::min_element(
                        mSources.begin(), mSources.end(),
                        [](const auto& lhs, const auto& rhs)
                        { return lhs.second.lastSeenEpoch < rhs.second.lastSeenEpoch; });
                    if (oldest == mSources.end() ||
                        oldest->second.lastSeenEpoch == frame.assemblyEpoch)
                        return unsupported("active texture sources exceed residency limit");
                    mSources.erase(oldest);
                }
                mSources.emplace(source, SourceRecord{
                    content, 1, frame.assemblyEpoch});
            }
            else
            {
                if (record->second.content != content)
                {
                    record->second.content = content;
                    ++record->second.generation;
                    if (!record->second.generation) record->second.generation = 1;
                    ++result.generationChanges;
                }
                record->second.lastSeenEpoch = frame.assemblyEpoch;
            }
        }

        for (auto resident = mResidents.begin(); resident != mResidents.end();)
        {
            if (!protectedContents.contains(resident->first) &&
                frame.assemblyEpoch > resident->second.lastUsedEpoch &&
                frame.assemblyEpoch - resident->second.lastUsedEpoch >
                    limits.staleAfterAssemblyEpochs)
            {
                auto victim = resident++;
                status = destroyResident(victim);
                if (!status) return status;
                ++result.evictions;
            }
            else
            {
                ++resident;
            }
        }

        mLastAssemblyEpoch = frame.assemblyEpoch;
        result.residentEntries = static_cast<std::uint32_t>(mResidents.size());
        result.residentBytes = mResidentBytes;
        return Status::success();
    }

    std::optional<ProductionTextureBinding> find(
        const ProductionTextureSourceIdentity& source) const
    {
        const auto record = mSources.find(source);
        if (record == mSources.end()) return std::nullopt;
        const auto resident = mResidents.find(record->second.content);
        if (resident == mResidents.end()) return std::nullopt;
        return ProductionTextureBinding{
            resident->second.image, resident->second.view,
            record->second.content.format, record->second.content.width,
            record->second.content.height, resident->second.mipLevelCount,
            record->second.generation, record->second.content.identity};
    }

    Status shutdown()
    {
        Status first = Status::success();
        for (auto& [content, resident] : mResidents)
        {
            const Status viewStatus = mDevice.destroy(resident.view);
            const Status imageStatus = mDevice.destroy(resident.image);
            if (first && !viewStatus) first = viewStatus;
            if (first && !imageStatus) first = imageStatus;
        }
        mResidents.clear();
        mSources.clear();
        mResidentBytes = 0;
        mLastAssemblyEpoch = 0;
        return first;
    }

private:
    using ResidentMap = std::map<ContentKey, Resident>;

    Status destroyResident(ResidentMap::iterator resident)
    {
        Status status = mDevice.destroy(resident->second.view);
        const Status imageStatus = mDevice.destroy(resident->second.image);
        if (status && !imageStatus) status = imageStatus;
        mResidentBytes -= resident->second.bytes;
        mResidents.erase(resident);
        return status;
    }

    Status uploadMissing(const std::vector<const TextureRequest*>& missing,
                         std::uint64_t assemblyEpoch)
    {
        std::uint64_t total = 0;
        for (const TextureRequest* request : missing)
            total += request->pixels.size();
        std::vector<std::byte> stagingBytes;
        stagingBytes.reserve(static_cast<std::size_t>(total));
        std::vector<std::uint64_t> offsets;
        offsets.reserve(missing.size());
        for (const TextureRequest* request : missing)
        {
            offsets.push_back(stagingBytes.size());
            stagingBytes.insert(stagingBytes.end(), request->pixels.begin(),
                                request->pixels.end());
        }

        Status status = Status::success();
        BufferHandle staging = mDevice.createBuffer(
            {total, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
        struct Pending
        {
            ContentKey key;
            Resident resident;
        };
        std::vector<Pending> pending;
        pending.reserve(missing.size());
        for (const TextureRequest* request : missing)
        {
            if (!status) break;
            Resident resident;
            resident.mipLevelCount = request->mipLevelCount;
            resident.bytes = request->pixels.size();
            resident.lastUsedEpoch = assemblyEpoch;
            resident.image = mDevice.createImage(
                {{request->content.width, request->content.height, 1},
                 request->content.format,
                 ResourceUsage::Sampled | ResourceUsage::TransferDestination |
                     ResourceUsage::TransferSource,
                 request->mipLevelCount, 1, 1}, status);
            if (status) resident.view = mDevice.createImageView(
                {resident.image, request->content.format,
                 {ImageAspect::Color, 0, request->mipLevelCount, 0, 1}}, status);
            pending.push_back({request->content, resident});
        }
        if (status) status = mDevice.writeBuffer(staging, 0, stagingBytes);
        if (status)
        {
            CommandContext& commands = mDevice.commandContext();
            status = commands.beginFrame();
            for (std::size_t index = 0; status && index < pending.size(); ++index)
            {
                BufferImageCopyRegion copy;
                copy.bufferOffset = offsets[index];
                copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                copy.imageExtent = {
                    pending[index].key.width, pending[index].key.height, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{copy}};
                status = commands.copyBufferToImage(
                    staging, pending[index].resident.image, copies);
                if (status && pending[index].resident.mipLevelCount > 1)
                    status = commands.generateMipmaps(
                        pending[index].resident.image,
                        {ImageAspect::Color, 0,
                         pending[index].resident.mipLevelCount, 0, 1});
            }
            const Status ended = commands.endFrame();
            if (status && !ended) status = ended;
        }

        const Status stagingStatus = staging ? mDevice.destroy(staging)
                                              : Status::success();
        if (status && !stagingStatus) status = stagingStatus;
        if (!status)
        {
            for (const Pending& resource : pending)
            {
                if (resource.resident.view) mDevice.destroy(resource.resident.view);
                if (resource.resident.image) mDevice.destroy(resource.resident.image);
            }
            return status;
        }
        for (Pending& resource : pending)
        {
            mResidentBytes += resource.resident.bytes;
            mResidents.emplace(std::move(resource.key), resource.resident);
        }
        return Status::success();
    }

    Device& mDevice;
    ResidentMap mResidents;
    std::map<ProductionTextureSourceIdentity, SourceRecord> mSources;
    std::uint64_t mResidentBytes = 0;
    std::uint64_t mLastAssemblyEpoch = 0;
};

ProductionTextureResidency::ProductionTextureResidency(Device& device) :
    mImpl(std::make_unique<Impl>(device))
{
}

ProductionTextureResidency::~ProductionTextureResidency()
{
    if (mImpl) mImpl->shutdown();
}

Status ProductionTextureResidency::update(
    const ProductionFramePacket& frame,
    const ProductionTextureResidencyLimits& limits,
    ProductionTextureResidencyResult& result)
{
    return mImpl->update(frame, limits, result);
}

std::optional<ProductionTextureBinding> ProductionTextureResidency::find(
    const ProductionTextureSourceIdentity& source) const
{
    return mImpl->find(source);
}

Status ProductionTextureResidency::shutdown()
{
    return mImpl->shutdown();
}

} // namespace LL::GHI
