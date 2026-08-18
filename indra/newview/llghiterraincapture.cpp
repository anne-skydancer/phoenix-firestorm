/**
 * @file llghiterraincapture.cpp
 * @brief Live I6 terrain observer. Visible rendering remains OpenGL.
 */

#include "llviewerprecompiledheaders.h"

#include "llghiterraincapture.h"
#include "llghimaterialcapture.h"
#include "llghiruntime.h"

#include "llface.h"
#include "llfetchedgltfmaterial.h"
#include "llrender.h"
#include "llviewerregion.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvlcomposition.h"
#include "llvertexbuffer.h"
#include "ghi/core/llghihash.h"
#include "ghi/include/llghiterrainscenepacket.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

#include <glm/gtc/type_ptr.hpp>

using namespace std::chrono_literals;

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

LL::GHI::TerrainDetailMode detailMode(std::int32_t value)
{
    if (value >= TERRAIN_PBR_DETAIL_EMISSIVE)
        return LL::GHI::TerrainDetailMode::Emissive;
    if (value >= TERRAIN_PBR_DETAIL_OCCLUSION)
        return LL::GHI::TerrainDetailMode::Occlusion;
    if (value >= TERRAIN_PBR_DETAIL_NORMAL)
        return LL::GHI::TerrainDetailMode::Normal;
    if (value >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
        return LL::GHI::TerrainDetailMode::MetallicRoughness;
    return LL::GHI::TerrainDetailMode::BaseColor;
}
} // namespace

class LLGHITerrainCapture::Impl
{
public:
    enum class State { Disabled, Warming, Recording, Complete, Failed };

    void configure()
    {
        if (mConfigured) return;
        mConfigured = true;
        const char* output = std::getenv("VULKANSTORM_GHI_I6_CAPTURE");
        if (!output || !*output) return;
        mOutput = std::filesystem::path(output);
        mWarmup = 120s;
        if (const char* value = std::getenv("VULKANSTORM_GHI_I6_WARMUP_SECONDS"))
        {
            char* end = nullptr;
            const double seconds = std::strtod(value, &end);
            if (end != value && seconds >= 0.0 && seconds <= 3600.0)
                mWarmup = std::chrono::milliseconds(
                    static_cast<std::int64_t>(seconds * 1000.0));
        }
        mState = State::Warming;
        mWarmupStart = std::chrono::steady_clock::now();
        LL_INFOS("GHI") << "I6 terrain capture armed; warmup="
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
        mCaptureRuntime = LLGHIRuntime::shouldCaptureLiveTerrainPacket(frameId);
        if (!mCaptureFile && !mCaptureRuntime) return false;
        mPacket = {};
        mPacket.frameId = frameId;
        mPacket.sceneEpoch = ++mSceneEpoch;
        mPacket.sourceWidth = width;
        mPacket.sourceHeight = height;
        mTextureIndices.clear();
        mRegionIndices.clear();
        mRuntimeTextureBytes = 0;
        mBudgetLimited = false;
        mInFrame = true;
        return true;
    }

    std::uint32_t texture(LLViewerTexture* source,
                          LL::GHI::TextureColorSpace colorSpace,
                          const char* semantic)
    {
        if (!source) return LL::GHI::NO_RESOURCE;
        const std::string sourceKey = source->getID().asString() + ':' + semantic + ':' +
            std::to_string(static_cast<std::uint32_t>(colorSpace));
        const auto sourceDigest = digestString(sourceKey);
        const std::string key = LL::GHI::sha256(sourceDigest);
        if (auto found = mTextureIndices.find(key); found != mTextureIndices.end())
            return found->second;
        if (mCaptureRuntime && mPacket.textures.size() >= MAX_TEXTURES)
        {
            mBudgetLimited = true;
            return LL::GHI::NO_RESOURCE;
        }
        LL::GHI::MaterialTextureResource resource;
        resource.sourceIdentity = sourceDigest;
        resource.colorSpace = colorSpace;
        resource.width = llmax(0, source->getFullWidth());
        resource.height = llmax(0, source->getFullHeight());
        resource.components = llmax<S32>(0, source->getComponents());
        resource.discardLevel = llmax<S32>(0, source->getDiscardLevel());
        if (auto* fetched = dynamic_cast<LLViewerFetchedTexture*>(source))
        {
            LL::GHI::MaterialTextureResource observed;
            if (LLGHIMaterialCapture::instance().copyDecodedTexture(*fetched, observed) &&
                (!mCaptureRuntime ||
                 (observed.decodedPixels.size() <= MAX_TEXTURE_BYTES -
                      llmin(mRuntimeTextureBytes, MAX_TEXTURE_BYTES))))
            {
                resource.width = observed.width;
                resource.height = observed.height;
                resource.components = observed.components;
                resource.discardLevel = observed.discardLevel;
                resource.contentIdentity = observed.contentIdentity;
                resource.decodedPixels = std::move(observed.decodedPixels);
                mRuntimeTextureBytes += resource.decodedPixels.size();
            }
            else
            {
                resource.comparability =
                    LL::GHI::ResourceComparability::MissingCpuTexture;
                if (fetched->isFetching())
                    resource.comparability = resource.comparability |
                        LL::GHI::ResourceComparability::TextureStillFetching;
            }
        }
        else
        {
            resource.comparability =
                LL::GHI::ResourceComparability::MissingCpuTexture;
        }
        const auto index = static_cast<std::uint32_t>(mPacket.textures.size());
        mPacket.textures.push_back(std::move(resource));
        mTextureIndices.emplace(key, index);
        return index;
    }

    std::uint32_t regionResource(LLViewerRegion& viewerRegion,
                                 LLTerrainMaterials& materials,
                                 LLViewerTexture* composition, bool pbr,
                                 std::uint32_t paintType,
                                 std::int32_t pbrDetailMode, float detailScale)
    {
        std::ostringstream identity;
        identity.precision(9);
        identity << viewerRegion.getRegionID().asString() << ':' << pbr << ':'
                 << paintType << ':' << pbrDetailMode << ':' << detailScale
                 << ':' << composition->getID().asString() << ':'
                 << gSavedSettings.getS32("RenderTerrainPBRPlanarSampleCount");
        for (S32 layer = 0; layer < LLTerrainMaterials::ASSET_COUNT; ++layer)
        {
            identity << ':' << materials.getDetailAssetID(layer).asString();
            if (pbr)
            {
                LLFetchedGLTFMaterial* material =
                    materials.getDetailRenderMaterial(layer);
                identity << ':' << (material
                    ? material->getHash().asString()
                    : std::string("terrain-default-pbr"));
            }
        }
        const std::string key = identity.str();
        if (auto found = mRegionIndices.find(key); found != mRegionIndices.end())
            return found->second;

        LL::GHI::TerrainRegionResource output;
        output.identity = digestString(key);
        output.model = pbr ? LL::GHI::MaterialModel::MetallicRoughness
                           : LL::GHI::MaterialModel::Legacy;
        output.paintMode = pbr && paintType == TERRAIN_PAINT_TYPE_PBR_PAINTMAP
            ? LL::GHI::TerrainPaintMode::PBRPaintMap
            : LL::GHI::TerrainPaintMode::HeightmapWithNoise;
        output.projection = pbr && gSavedSettings.getS32("RenderTerrainPBRPlanarSampleCount") == 3
            ? LL::GHI::TerrainProjection::Triplanar
            : LL::GHI::TerrainProjection::Planar;
        output.detailMode = pbr ? detailMode(pbrDetailMode)
                                : LL::GHI::TerrainDetailMode::BaseColor;
        output.regionScale = viewerRegion.getWidth();
        output.detailScale = detailScale;
        output.compositionTexture = texture(
            composition, LL::GHI::TextureColorSpace::Linear, "composition");

        for (S32 index = 0; index < LLTerrainMaterials::ASSET_COUNT; ++index)
        {
            auto& layer = output.layers[index];
            layer.model = output.model;
            if (pbr)
            {
                LLFetchedGLTFMaterial* material =
                    materials.getDetailRenderMaterial(index);
                const LLGLTFMaterial* source = material
                    ? static_cast<const LLGLTFMaterial*>(material)
                    : &LLGLTFMaterial::sDefault;
                layer.identity = digestString(material
                    ? material->getHash().asString()
                    : std::string("terrain-default-pbr-") + std::to_string(index));
                std::copy_n(source->mBaseColor.mV, 4, layer.baseColor.begin());
                std::copy_n(source->mEmissiveColor.mV, 3, layer.emissive.begin());
                layer.metallic = source->mMetallicFactor;
                layer.roughness = source->mRoughnessFactor;
                layer.alphaCutoff = source->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK
                    ? source->mAlphaCutoff : 0.f;
                const auto& transform = source->mTextureTransform[
                    LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
                layer.transform = {{transform.mOffset.mV[0], transform.mOffset.mV[1],
                    transform.mScale.mV[0] * detailScale,
                    transform.mScale.mV[1] * detailScale, transform.mRotation}};
                layer.baseColorTexture = texture(
                    material && material->mBaseColorTexture
                        ? material->mBaseColorTexture.get()
                        : LLViewerFetchedTexture::sWhiteImagep.get(),
                    LL::GHI::TextureColorSpace::SRGB, "terrain-base");
                if (pbrDetailMode >= TERRAIN_PBR_DETAIL_NORMAL)
                    layer.normalTexture = texture(
                        material && material->mNormalTexture
                            ? material->mNormalTexture.get()
                            : LLViewerFetchedTexture::sFlatNormalImagep.get(),
                        LL::GHI::TextureColorSpace::Linear, "terrain-normal");
                if (pbrDetailMode >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
                    layer.metallicRoughnessTexture = texture(
                        material && material->mMetallicRoughnessTexture
                            ? material->mMetallicRoughnessTexture.get()
                            : LLViewerFetchedTexture::sWhiteImagep.get(),
                        LL::GHI::TextureColorSpace::Linear, "terrain-orm");
                if (pbrDetailMode >= TERRAIN_PBR_DETAIL_EMISSIVE)
                    layer.emissiveTexture = texture(
                        material && material->mEmissiveTexture
                            ? material->mEmissiveTexture.get()
                            : LLViewerFetchedTexture::sWhiteImagep.get(),
                        LL::GHI::TextureColorSpace::SRGB, "terrain-emissive");
            }
            else
            {
                LLViewerFetchedTexture* detail = materials.getDetailTexture(index);
                layer.identity = digestString(detail
                    ? detail->getID().asString()
                    : std::string("terrain-missing-legacy-") + std::to_string(index));
                layer.baseColorTexture = texture(detail,
                    LL::GHI::TextureColorSpace::SRGB, "terrain-detail");
            }
            const std::uint32_t textureIndices[] = {
                layer.baseColorTexture, layer.normalTexture,
                layer.metallicRoughnessTexture, layer.emissiveTexture};
            for (std::uint32_t textureIndex : textureIndices)
                if (textureIndex != LL::GHI::NO_RESOURCE)
                    layer.comparability = layer.comparability |
                        mPacket.textures[textureIndex].comparability;
            output.comparability = output.comparability | layer.comparability;
        }
        if (output.compositionTexture != LL::GHI::NO_RESOURCE)
            output.comparability = output.comparability |
                mPacket.textures[output.compositionTexture].comparability;
        if (output.compositionTexture == LL::GHI::NO_RESOURCE ||
            std::any_of(output.layers.begin(), output.layers.end(),
                [](const LL::GHI::TerrainLayerResource& layer)
                {
                    return layer.baseColorTexture == LL::GHI::NO_RESOURCE;
                }))
        {
            mBudgetLimited = true;
            return LL::GHI::NO_RESOURCE;
        }
        const auto index = static_cast<std::uint32_t>(mPacket.regions.size());
        mPacket.regions.push_back(std::move(output));
        mRegionIndices.emplace(key, index);
        return index;
    }

    bool geometry(const LLFace& face, std::uint32_t regionIndex,
                  LLViewerRegion& region)
    {
        const LLVertexBuffer* buffer = face.getVertexBuffer();
        constexpr std::uint32_t required = LLVertexBuffer::MAP_VERTEX |
            LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TANGENT |
            LLVertexBuffer::MAP_TEXCOORD1;
        const std::uint32_t indexCount = face.getIndicesCount();
        const std::uint32_t firstIndex = face.getIndicesStart();
        const std::uint32_t firstVertex = face.getGeomIndex();
        const std::uint32_t vertexRange = face.getGeomCount();
        if (!buffer || !indexCount || indexCount % 3 || !vertexRange ||
            (buffer->getTypeMask() & required) != required ||
            !buffer->getMappedData() || !buffer->getMappedIndices() ||
            firstVertex > buffer->getNumVerts() ||
            vertexRange > buffer->getNumVerts() - firstVertex ||
            firstIndex > buffer->getNumIndices() ||
            indexCount > buffer->getNumIndices() - firstIndex ||
            (buffer->getIndexStride() != 2 && buffer->getIndexStride() != 4))
            return false;
        if (mCaptureRuntime && (mPacket.draws.size() >= MAX_DRAWS ||
            mPacket.vertices.size() + vertexRange > MAX_VERTICES ||
            mPacket.indices.size() + indexCount > MAX_INDICES))
        {
            mBudgetLimited = true;
            return false;
        }
        const auto* rawIndices = buffer->getMappedIndices();
        std::vector<bool> referenced(vertexRange, false);
        for (std::uint32_t item = 0; item < indexCount; ++item)
        {
            const std::uint32_t value = buffer->getIndexStride() == 2
                ? reinterpret_cast<const std::uint16_t*>(rawIndices)[firstIndex + item]
                : reinterpret_cast<const std::uint32_t*>(rawIndices)[firstIndex + item];
            if (value < firstVertex || value >= firstVertex + vertexRange) return false;
            referenced[value - firstVertex] = true;
        }
        const auto* data = buffer->getMappedData();
        const auto* positions = reinterpret_cast<const float*>(
            data + buffer->getOffset(LLVertexBuffer::TYPE_VERTEX));
        const auto* normals = reinterpret_cast<const float*>(
            data + buffer->getOffset(LLVertexBuffer::TYPE_NORMAL));
        const auto* tangents = reinterpret_cast<const float*>(
            data + buffer->getOffset(LLVertexBuffer::TYPE_TANGENT));
        const auto* composition = reinterpret_cast<const float*>(
            data + buffer->getOffset(LLVertexBuffer::TYPE_TEXCOORD1));
        std::vector<std::uint32_t> remap(
            vertexRange, std::numeric_limits<std::uint32_t>::max());
        for (std::uint32_t local = 0; local < vertexRange; ++local)
        {
            if (!referenced[local]) continue;
            const std::uint32_t source = firstVertex + local;
            remap[local] = static_cast<std::uint32_t>(mPacket.vertices.size());
            LL::GHI::TerrainSceneVertex vertex;
            std::copy_n(positions + static_cast<std::size_t>(source) * 4, 3,
                        vertex.position.begin());
            std::copy_n(normals + static_cast<std::size_t>(source) * 4, 3,
                        vertex.normal.begin());
            std::copy_n(tangents + static_cast<std::size_t>(source) * 4, 4,
                        vertex.tangent.begin());
            std::copy_n(composition + static_cast<std::size_t>(source) * 2, 2,
                        vertex.compositionCoord.begin());
            mPacket.vertices.push_back(vertex);
        }
        LL::GHI::TerrainSceneDraw draw;
        draw.semanticId = 0x49365f0000000000ull |
            static_cast<std::uint64_t>(mPacket.draws.size());
        draw.region = regionIndex;
        draw.comparability = mPacket.regions[regionIndex].comparability;
        draw.firstIndex = static_cast<std::uint32_t>(mPacket.indices.size());
        draw.indexCount = indexCount;
        for (std::uint32_t item = 0; item < indexCount; ++item)
        {
            const std::uint32_t value = buffer->getIndexStride() == 2
                ? reinterpret_cast<const std::uint16_t*>(rawIndices)[firstIndex + item]
                : reinterpret_cast<const std::uint32_t*>(rawIndices)[firstIndex + item];
            mPacket.indices.push_back(remap[value - firstVertex]);
        }
        const glm::mat4 viewProjection = glm::make_mat4(gGLProjection) *
                                         glm::make_mat4(gGLModelView);
        std::copy_n(glm::value_ptr(viewProjection), 16, draw.viewProjection.begin());
        std::copy_n(&region.mRenderMatrix.mMatrix[0][0], 16,
                    draw.modelTransform.begin());
        mPacket.draws.push_back(draw);
        return true;
    }

    void record(const std::vector<LLFace*>& faces, LLViewerRegion& viewerRegion,
                LLTerrainMaterials& materials, LLViewerTexture* composition,
                bool pbr, std::uint32_t paintType,
                std::int32_t pbrDetailMode, float detailScale)
    {
        if (!mInFrame || faces.empty() || !composition) return;
        const std::uint32_t region = regionResource(
            viewerRegion, materials, composition, pbr, paintType,
            pbrDetailMode, detailScale);
        if (region == LL::GHI::NO_RESOURCE) return;
        for (LLFace* face : faces)
            if (face) geometry(*face, region, viewerRegion);
    }

    void end()
    {
        if (!mInFrame) return;
        mInFrame = false;
        const bool captureFile = mCaptureFile;
        const bool captureRuntime = mCaptureRuntime;
        mCaptureFile = false;
        mCaptureRuntime = false;

        // Resource lifetime is independent of frame lifetime. This gives the
        // eventual peer backend a stable invalidation signal across camera
        // motion while still detecting texture refinement, region hand-off,
        // terrain overrides, and PBR transform changes.
        std::ostringstream signature;
        for (const auto& texture : mPacket.textures)
            signature << LL::GHI::sha256(texture.sourceIdentity)
                      << LL::GHI::sha256(texture.contentIdentity) << ':'
                      << texture.width << ':' << texture.height << ':'
                      << texture.components << ':' << texture.discardLevel << ':'
                      << static_cast<std::uint32_t>(texture.colorSpace) << ':'
                      << static_cast<std::uint32_t>(texture.comparability) << ';';
        for (const auto& region : mPacket.regions)
            signature << LL::GHI::sha256(region.identity) << ':'
                      << static_cast<std::uint32_t>(region.model) << ':'
                      << static_cast<std::uint32_t>(region.paintMode) << ':'
                      << static_cast<std::uint32_t>(region.projection) << ':'
                      << static_cast<std::uint32_t>(region.detailMode) << ':'
                      << static_cast<std::uint32_t>(region.comparability) << ';';
        const std::string resourceSignature = signature.str();
        if (resourceSignature != mLastResourceSignature)
        {
            ++mResourceEpoch;
            mLastResourceSignature = resourceSignature;
        }
        mPacket.resourceEpoch = mResourceEpoch;

        if (captureRuntime)
            LLGHIRuntime::consumeLiveTerrainPacket(mPacket, mBudgetLimited);
        if (!captureFile || mPacket.draws.empty()) return;
        std::vector<std::byte> bytes;
        const LL::GHI::Status status =
            LL::GHI::encodeTerrainScenePacket(mPacket, bytes);
        if (!status) { fail(status.message()); return; }
        std::error_code error;
        if (mOutput.has_parent_path())
            std::filesystem::create_directories(mOutput.parent_path(), error);
        std::ofstream stream(mOutput, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream) { fail("could not write terrain capture file"); return; }
        mState = State::Complete;
        LL_INFOS("GHI") << "I6 terrain capture complete: frame="
                         << mPacket.frameId << " draws=" << mPacket.draws.size()
                         << " regions=" << mPacket.regions.size() << " textures="
                         << mPacket.textures.size() << " vertices="
                         << mPacket.vertices.size() << " indices="
                         << mPacket.indices.size() << " bytes=" << bytes.size()
                         << " sha256=" << LL::GHI::sha256(bytes) << LL_ENDL;
    }

private:
    void fail(const std::string& message)
    {
        mState = State::Failed;
        LL_WARNS("GHI") << "I6 terrain capture failed: " << message << LL_ENDL;
    }

    static constexpr std::size_t MAX_DRAWS = 64;
    static constexpr std::size_t MAX_VERTICES = 131072;
    static constexpr std::size_t MAX_INDICES = 393216;
    static constexpr std::size_t MAX_TEXTURES = 32;
    static constexpr std::size_t MAX_TEXTURE_BYTES = 16ull * 1024ull * 1024ull;
    bool mConfigured = false;
    bool mInFrame = false;
    bool mCaptureFile = false;
    bool mCaptureRuntime = false;
    bool mBudgetLimited = false;
    State mState = State::Disabled;
    std::filesystem::path mOutput;
    std::chrono::milliseconds mWarmup{0};
    std::chrono::steady_clock::time_point mWarmupStart{};
    std::uint64_t mSceneEpoch = 0;
    std::uint64_t mResourceEpoch = 0;
    std::size_t mRuntimeTextureBytes = 0;
    std::string mLastResourceSignature;
    std::map<std::string, std::uint32_t> mTextureIndices;
    std::map<std::string, std::uint32_t> mRegionIndices;
    LL::GHI::TerrainScenePacket mPacket;
};

LLGHITerrainCapture::LLGHITerrainCapture() : mImpl(std::make_unique<Impl>()) {}
LLGHITerrainCapture::~LLGHITerrainCapture() = default;
bool LLGHITerrainCapture::sActive = false;

bool LLGHITerrainCapture::beginFrame(std::uint32_t width,
                                     std::uint32_t height,
                                     std::uint64_t frame_id)
{
    sActive = mImpl->begin(width, height, frame_id);
    return sActive;
}

void LLGHITerrainCapture::record(
    const std::vector<LLFace*>& faces, LLViewerRegion& region,
    LLTerrainMaterials& materials, LLViewerTexture* composition, bool pbr,
    std::uint32_t paint_type, std::int32_t pbr_detail_mode, float detail_scale)
{
    mImpl->record(faces, region, materials, composition, pbr, paint_type,
                  pbr_detail_mode, detail_scale);
}

void LLGHITerrainCapture::endFrame()
{
    mImpl->end();
    sActive = false;
}
