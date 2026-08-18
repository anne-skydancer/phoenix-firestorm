/**
 * @file llghiproductiongbufferexecutor.cpp
 * @brief I8c2 shared-target material and terrain G-buffer execution.
 */

#include "linden_common.h"

#include "ghi/include/llghiproductiongbufferexecutor.h"

#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace LL::GHI
{
namespace
{
constexpr std::array<Format, PRODUCTION_GBUFFER_TARGETS> COLOR_FORMATS{{
    Format::RGBA8UNorm, Format::RGBA8UNorm,
    Format::RGBA16UNorm, Format::RGBA16Float}};
constexpr std::array<std::uint32_t, PRODUCTION_GBUFFER_TARGETS>
    COLOR_BYTES{{4, 4, 8, 8}};
constexpr Format DEPTH_FORMAT = Format::Depth32Float;
constexpr std::size_t MATERIAL_FLOATS = 44;
constexpr std::size_t OBJECT_FLOATS = 32;
constexpr std::size_t TERRAIN_FLOATS = 132;
using SkinData = std::array<float, MATERIAL_SKIN_FLOATS>;

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

Status unsupported(const char* message)
{
    return Status::failure(StatusCode::Unsupported, message);
}

bool supportedTextureCoordinates(const MaterialResource& material)
{
    return std::all_of(material.textures.begin(), material.textures.end(),
        [](const MaterialTextureBinding& binding)
        { return binding.texcoord == 0; });
}

const MaterialTextureBinding* findBinding(const MaterialResource& material,
                                           TextureSemantic semantic)
{
    const auto found = std::find_if(material.textures.begin(),
        material.textures.end(), [semantic](const MaterialTextureBinding& binding)
        { return binding.semantic == semantic; });
    return found == material.textures.end() ? nullptr : &*found;
}

bool validSkin(const SkinResource& skin)
{
    return skin.comparability == ResourceComparability::Comparable &&
           skin.jointCount > 0 && skin.jointCount <= MATERIAL_MAX_JOINTS &&
           skin.matrixPalette.size() ==
               static_cast<std::size_t>(skin.jointCount) * 12;
}

SkinData makeSkinData(const SkinResource* skin)
{
    SkinData data{};
    for (std::uint32_t joint = 0; joint < MATERIAL_MAX_JOINTS; ++joint)
    {
        data[joint * 12] = 1.f;
        data[joint * 12 + 5] = 1.f;
        data[joint * 12 + 10] = 1.f;
    }
    const std::uint32_t jointCount = skin ? skin->jointCount : 1u;
    if (skin)
        std::copy(skin->matrixPalette.begin(), skin->matrixPalette.end(),
                  data.begin());
    const std::array<std::uint32_t, 4> metadata{{jointCount, 0, 0, 0}};
    std::memcpy(data.data() + MATERIAL_MAX_JOINTS * 12,
                metadata.data(), sizeof(metadata));
    return data;
}

bool normalMatrix(const std::array<float, 16>& model,
                  std::array<float, 16>& output)
{
    const double a00 = model[0], a01 = model[4], a02 = model[8];
    const double a10 = model[1], a11 = model[5], a12 = model[9];
    const double a20 = model[2], a21 = model[6], a22 = model[10];
    const double c00 = a11 * a22 - a12 * a21;
    const double c01 = a12 * a20 - a10 * a22;
    const double c02 = a10 * a21 - a11 * a20;
    const double c10 = a02 * a21 - a01 * a22;
    const double c11 = a00 * a22 - a02 * a20;
    const double c12 = a01 * a20 - a00 * a21;
    const double c20 = a01 * a12 - a02 * a11;
    const double c21 = a02 * a10 - a00 * a12;
    const double c22 = a00 * a11 - a01 * a10;
    const double determinant = a00 * c00 + a01 * c01 + a02 * c02;
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.e-12)
        return false;
    output = {{1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
               0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f}};
    const double inverse = 1.0 / determinant;
    const std::array<double, 9> values{{
        c00 * inverse, c10 * inverse, c20 * inverse,
        c01 * inverse, c11 * inverse, c21 * inverse,
        c02 * inverse, c12 * inverse, c22 * inverse}};
    constexpr std::array<std::size_t, 9> offsets{{0, 1, 2, 4, 5, 6, 8, 9, 10}};
    for (std::size_t value = 0; value < values.size(); ++value)
    {
        if (!std::isfinite(values[value])) return false;
        output[offsets[value]] = static_cast<float>(values[value]);
    }
    return true;
}

bool makeObjectData(const MaterialSceneDraw& draw,
                    std::array<float, OBJECT_FLOATS>& data)
{
    std::copy(draw.modelTransform.begin(), draw.modelTransform.end(), data.begin());
    std::array<float, 16> normal{};
    if (!normalMatrix(draw.modelTransform, normal)) return false;
    std::copy(normal.begin(), normal.end(), data.begin() + 16);
    return true;
}

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment)
{
    if (value > std::numeric_limits<std::uint64_t>::max() - alignment + 1)
        return std::numeric_limits<std::uint64_t>::max();
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

class ProductionGBufferExecutor::Impl
{
public:
    Impl(Device& device, ShaderPackageDesc materialShader,
         ShaderPackageDesc terrainShader) :
        mDevice(device),
        mMaterialPackage(std::move(materialShader)),
        mTerrainPackage(std::move(terrainShader))
    {
    }

    ~Impl() { shutdown(); }

    Status submit(const ProductionFramePacket& frame,
                  const ProductionFrameTargetSet& targets,
                  const ProductionTextureResidency& residency,
                  const ProductionGBufferLimits& limits)
    {
        if (mPending)
            return Status::failure(StatusCode::NotReady,
                                   "production G-buffer execution is still pending");
        if (!limits.maxMaterialDraws || !limits.maxTerrainDraws ||
            !limits.maxUploadBytes)
            return invalid("production G-buffer limits must be nonzero");
        Status status = validateProductionFramePacket(frame);
        if (!status) return status;
        if (!productionFrameHasPass(frame.passes,
                                    ProductionFramePass::MaterialGBuffer) ||
            !productionFrameHasPass(frame.passes,
                                    ProductionFramePass::TerrainGBuffer))
            return invalid("production G-buffer execution requires both draw passes");
        if (!targets.width || !targets.height || !targets.generation ||
            targets.width > frame.sourceWidth || targets.height > frame.sourceHeight)
            return invalid("production G-buffer targets do not match the frame");
        if (std::any_of(targets.gbufferViews.begin(), targets.gbufferViews.end(),
                        [](ImageViewHandle view) { return !view; }) ||
            !targets.depthView)
            return invalid("production G-buffer target views are incomplete");

        status = initialize();
        if (!status) return status;
        status = ensureReadbacks(targets);
        if (!status) return status;

        struct MaterialDraw
        {
            std::size_t source = 0;
            std::array<ImageViewHandle, 4> views{};
            BindingSetHandle frameSet;
            BindingSetHandle objectSet;
            BindingSetHandle materialSet;
        };
        struct TerrainDraw
        {
            std::size_t source = 0;
            std::array<ImageViewHandle, 5> views{};
            BindingSetHandle set;
        };
        std::vector<MaterialDraw> materialDraws;
        std::vector<TerrainDraw> terrainDraws;
        std::uint32_t deferredMaterial = 0;
        std::uint32_t deferredTerrain = 0;
        std::uint32_t riggedDraws = 0;
        std::uint32_t pbrTerrainDraws = 0;

        constexpr std::array<TextureSemantic, 4> semantics{{
            TextureSemantic::BaseColor, TextureSemantic::Normal,
            TextureSemantic::MetallicRoughness, TextureSemantic::Emissive}};
        for (std::size_t source = 0;
             source < frame.materials.draws.size() &&
             materialDraws.size() < limits.maxMaterialDraws; ++source)
        {
            const MaterialSceneDraw& draw = frame.materials.draws[source];
            if (!draw.indexCount || draw.material >= frame.materials.materials.size())
                continue;
            const MaterialResource& material =
                frame.materials.materials[draw.material];
            if (draw.comparability != ResourceComparability::Comparable ||
                material.comparability != ResourceComparability::Comparable ||
                material.model != MaterialModel::MetallicRoughness ||
                material.alphaMode != MaterialAlphaMode::Opaque ||
                !supportedTextureCoordinates(material))
                continue;
            if (draw.skin != NO_RESOURCE &&
                (draw.skin >= frame.materials.skins.size() ||
                 !validSkin(frame.materials.skins[draw.skin])))
            {
                ++deferredMaterial;
                continue;
            }
            MaterialDraw executable;
            executable.source = source;
            bool ready = true;
            for (std::size_t texture = 0; texture < semantics.size(); ++texture)
            {
                executable.views[texture] = mFallbackViews[texture];
                const MaterialTextureBinding* binding =
                    findBinding(material, semantics[texture]);
                if (!binding) continue;
                if (binding->texture >= frame.materials.textures.size())
                {
                    ready = false;
                    break;
                }
                const auto resident = residency.find({
                    ProductionTextureDomain::Material,
                    frame.materials.textures[binding->texture].sourceIdentity});
                if (!resident)
                {
                    ready = false;
                    break;
                }
                executable.views[texture] = resident->view;
            }
            if (!ready)
            {
                ++deferredMaterial;
                continue;
            }
            riggedDraws += draw.skin != NO_RESOURCE;
            materialDraws.push_back(executable);
        }

        for (std::size_t source = 0;
             source < frame.terrain.draws.size() &&
             terrainDraws.size() < limits.maxTerrainDraws; ++source)
        {
            const TerrainSceneDraw& draw = frame.terrain.draws[source];
            if (!draw.indexCount || draw.region >= frame.terrain.regions.size())
                continue;
            const TerrainRegionResource& region = frame.terrain.regions[draw.region];
            if (draw.comparability != ResourceComparability::Comparable ||
                region.comparability != ResourceComparability::Comparable)
                continue;
            const std::array<std::uint32_t, 5> textureIndices{{
                region.compositionTexture, region.layers[0].baseColorTexture,
                region.layers[1].baseColorTexture,
                region.layers[2].baseColorTexture,
                region.layers[3].baseColorTexture}};
            TerrainDraw executable;
            executable.source = source;
            bool ready = true;
            for (std::size_t texture = 0; texture < textureIndices.size(); ++texture)
            {
                if (textureIndices[texture] >= frame.terrain.textures.size())
                {
                    ready = false;
                    break;
                }
                const auto resident = residency.find({
                    ProductionTextureDomain::Terrain,
                    frame.terrain.textures[textureIndices[texture]].sourceIdentity});
                if (!resident)
                {
                    ready = false;
                    break;
                }
                executable.views[texture] = resident->view;
            }
            if (!ready)
            {
                ++deferredTerrain;
                continue;
            }
            pbrTerrainDraws += region.model == MaterialModel::MetallicRoughness;
            terrainDraws.push_back(executable);
        }
        if (materialDraws.empty() || terrainDraws.empty())
            return invalid("production G-buffer frame has no executable material or terrain draws");

        const std::uint64_t alignment = std::max<std::uint64_t>(
            16, mDevice.capabilities().uniformBufferOffsetAlignment);
        const std::uint64_t materialVertexBytes =
            frame.materials.vertices.size() * sizeof(MaterialSceneVertex);
        const std::uint64_t materialIndexBytes =
            frame.materials.indices.size() * sizeof(std::uint32_t);
        const std::uint64_t terrainVertexBytes =
            frame.terrain.vertices.size() * sizeof(TerrainSceneVertex);
        const std::uint64_t terrainIndexBytes =
            frame.terrain.indices.size() * sizeof(std::uint32_t);
        const std::uint64_t materialVertexOffset = 0;
        const std::uint64_t materialIndexOffset =
            alignUp(materialVertexOffset + materialVertexBytes, alignment);
        const std::uint64_t terrainVertexOffset =
            alignUp(materialIndexOffset + materialIndexBytes, alignment);
        const std::uint64_t terrainIndexOffset =
            alignUp(terrainVertexOffset + terrainVertexBytes, alignment);
        const std::uint64_t frameStride = alignUp(16 * sizeof(float), alignment);
        const std::uint64_t objectStride = alignUp(OBJECT_FLOATS * sizeof(float), alignment);
        const std::uint64_t skinStride = alignUp(MATERIAL_SKIN_BYTES, alignment);
        const std::uint64_t materialStride = alignUp(MATERIAL_FLOATS * sizeof(float), alignment);
        const std::uint64_t terrainStride = alignUp(TERRAIN_FLOATS * sizeof(float), alignment);
        const std::uint64_t frameOffset =
            alignUp(terrainIndexOffset + terrainIndexBytes, alignment);
        const std::uint64_t objectOffset =
            alignUp(frameOffset + frameStride * materialDraws.size(), alignment);
        const std::uint64_t skinOffset =
            alignUp(objectOffset + objectStride * materialDraws.size(), alignment);
        const std::uint64_t materialOffset =
            alignUp(skinOffset + skinStride * materialDraws.size(), alignment);
        const std::uint64_t terrainOffset =
            alignUp(materialOffset + materialStride * materialDraws.size(), alignment);
        const std::uint64_t uploadBytes =
            alignUp(terrainOffset + terrainStride * terrainDraws.size(), alignment);
        if (uploadBytes == std::numeric_limits<std::uint64_t>::max() ||
            uploadBytes > limits.maxUploadBytes ||
            uploadBytes > mDevice.capabilities().maxBufferSize)
            return unsupported("production G-buffer upload exceeds its bounded limit");

        std::vector<std::byte> uploadData(static_cast<std::size_t>(uploadBytes));
        std::memcpy(uploadData.data() + materialVertexOffset,
                    frame.materials.vertices.data(), materialVertexBytes);
        std::memcpy(uploadData.data() + materialIndexOffset,
                    frame.materials.indices.data(), materialIndexBytes);
        std::memcpy(uploadData.data() + terrainVertexOffset,
                    frame.terrain.vertices.data(), terrainVertexBytes);
        std::memcpy(uploadData.data() + terrainIndexOffset,
                    frame.terrain.indices.data(), terrainIndexBytes);

        for (std::size_t item = 0; item < materialDraws.size(); ++item)
        {
            const MaterialSceneDraw& draw =
                frame.materials.draws[materialDraws[item].source];
            const MaterialResource& material = frame.materials.materials[draw.material];
            std::memcpy(uploadData.data() + frameOffset + frameStride * item,
                        draw.transform.data(), sizeof(draw.transform));
            std::array<float, OBJECT_FLOATS> object{};
            if (!makeObjectData(draw, object))
                return invalid("production material draw has a singular transform");
            std::memcpy(uploadData.data() + objectOffset + objectStride * item,
                        object.data(), sizeof(object));
            const SkinData skin = makeSkinData(draw.skin == NO_RESOURCE
                ? nullptr : &frame.materials.skins[draw.skin]);
            std::memcpy(uploadData.data() + skinOffset + skinStride * item,
                        skin.data(), sizeof(skin));
            std::array<float, MATERIAL_FLOATS> factors{{
                material.baseColor[0], material.baseColor[1],
                material.baseColor[2], material.baseColor[3],
                material.emissive[0], material.emissive[1],
                material.emissive[2], material.metallic,
                material.roughness, 1.f, 0.f, 0.f}};
            for (std::size_t texture = 0; texture < semantics.size(); ++texture)
            {
                constexpr std::array<float, 5> identity{{0.f, 0.f, 1.f, 1.f, 0.f}};
                const MaterialTextureBinding* binding =
                    findBinding(material, semantics[texture]);
                const auto& transform = binding ? binding->transform : identity;
                const std::size_t uv = 12 + texture * 4;
                factors[uv] = transform[0];
                factors[uv + 1] = transform[1];
                factors[uv + 2] = transform[2];
                factors[uv + 3] = transform[3];
                const std::size_t rotation = 28 + texture * 4;
                factors[rotation] = std::cos(transform[4]);
                factors[rotation + 1] = std::sin(transform[4]);
            }
            std::memcpy(uploadData.data() + materialOffset + materialStride * item,
                        factors.data(), sizeof(factors));
        }
        for (std::size_t item = 0; item < terrainDraws.size(); ++item)
        {
            const TerrainSceneDraw& draw =
                frame.terrain.draws[terrainDraws[item].source];
            const TerrainRegionResource& region = frame.terrain.regions[draw.region];
            std::array<float, TERRAIN_FLOATS> uniform{};
            std::copy(draw.viewProjection.begin(), draw.viewProjection.end(),
                      uniform.begin());
            std::copy(draw.modelTransform.begin(), draw.modelTransform.end(),
                      uniform.begin() + 16);
            std::array<float, 16> normal{};
            if (!normalMatrix(draw.modelTransform, normal))
                return invalid("production terrain draw has a singular transform");
            std::copy(normal.begin(), normal.end(), uniform.begin() + 32);
            for (std::size_t layer = 0; layer < TERRAIN_LAYER_COUNT; ++layer)
            {
                const TerrainLayerResource& source = region.layers[layer];
                const std::size_t uv = 48 + layer * 4;
                const std::size_t rotation = 64 + layer * 4;
                uniform[uv] = source.transform[0];
                uniform[uv + 1] = source.transform[1];
                uniform[uv + 2] = source.transform[2];
                uniform[uv + 3] = source.transform[3];
                uniform[rotation] = std::cos(source.transform[4]);
                uniform[rotation + 1] = std::sin(source.transform[4]);
                std::copy(source.baseColor.begin(), source.baseColor.end(),
                          uniform.begin() + 80 + layer * 4);
                uniform[96 + layer * 4] = source.emissive[0];
                uniform[97 + layer * 4] = source.emissive[1];
                uniform[98 + layer * 4] = source.emissive[2];
                uniform[99 + layer * 4] = source.metallic;
                uniform[112 + layer * 4] = source.roughness;
                uniform[113 + layer * 4] = source.alphaCutoff;
            }
            uniform[128] = region.regionScale;
            uniform[129] = region.model == MaterialModel::MetallicRoughness ? 1.f : 0.f;
            uniform[130] = region.paintMode == TerrainPaintMode::PBRPaintMap ? 1.f : 0.f;
            uniform[131] = static_cast<float>(region.projection ==
                TerrainProjection::Triplanar ? 3 : 1);
            std::memcpy(uploadData.data() + terrainOffset + terrainStride * item,
                        uniform.data(), sizeof(uniform));
        }

        BufferHandle upload, materialVertices, materialIndices, terrainVertices,
                     terrainIndices, frames, objects, skins, materials, terrains;
        auto destroyTransient = [&]()
        {
            Status first = Status::success();
            for (auto& draw : materialDraws)
            {
                destroy(draw.frameSet, first);
                destroy(draw.objectSet, first);
                destroy(draw.materialSet, first);
            }
            for (auto& draw : terrainDraws) destroy(draw.set, first);
            destroy(upload, first);
            destroy(materialVertices, first); destroy(materialIndices, first);
            destroy(terrainVertices, first); destroy(terrainIndices, first);
            destroy(frames, first); destroy(objects, first); destroy(skins, first);
            destroy(materials, first); destroy(terrains, first);
            return first;
        };

        upload = mDevice.createBuffer(
            {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
        if (status) materialVertices = mDevice.createBuffer(
            {materialVertexBytes, ResourceUsage::Vertex |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) materialIndices = mDevice.createBuffer(
            {materialIndexBytes, ResourceUsage::Index |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) terrainVertices = mDevice.createBuffer(
            {terrainVertexBytes, ResourceUsage::Vertex |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) terrainIndices = mDevice.createBuffer(
            {terrainIndexBytes, ResourceUsage::Index |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) frames = mDevice.createBuffer(
            {frameStride * materialDraws.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) objects = mDevice.createBuffer(
            {objectStride * materialDraws.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) skins = mDevice.createBuffer(
            {skinStride * materialDraws.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) materials = mDevice.createBuffer(
            {materialStride * materialDraws.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) terrains = mDevice.createBuffer(
            {terrainStride * terrainDraws.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (!status || !(status = mDevice.writeBuffer(upload, 0, uploadData)))
        {
            destroyTransient();
            return status;
        }

        for (std::size_t item = 0; status && item < materialDraws.size(); ++item)
        {
            BindingSetDesc frameDesc{mMaterialShader, 0, {{
                0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                frames, frameStride * item, 16 * sizeof(float), {}, {}}}};
            materialDraws[item].frameSet =
                mDevice.createBindingSet(frameDesc, status);
            BindingSetDesc objectDesc{mMaterialShader, 1, {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 objects, objectStride * item,
                 OBJECT_FLOATS * sizeof(float), {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 skins, skinStride * item, MATERIAL_SKIN_BYTES, {}, {}}}};
            if (status) materialDraws[item].objectSet =
                mDevice.createBindingSet(objectDesc, status);
            BindingSetDesc materialDesc;
            materialDesc.shader = mMaterialShader;
            materialDesc.group = 2;
            materialDesc.resources.push_back({
                0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                materials, materialStride * item,
                MATERIAL_FLOATS * sizeof(float), {}, {}});
            for (std::size_t texture = 0; texture < 4; ++texture)
                materialDesc.resources.push_back({
                    static_cast<std::uint16_t>(texture + 1), 0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,
                    {}, 0, 0, materialDraws[item].views[texture], mRepeatSampler});
            if (status) materialDraws[item].materialSet =
                mDevice.createBindingSet(materialDesc, status);
        }
        for (std::size_t item = 0; status && item < terrainDraws.size(); ++item)
        {
            BindingSetDesc terrainDesc;
            terrainDesc.shader = mTerrainShader;
            terrainDesc.group = 0;
            terrainDesc.resources.push_back({
                0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                terrains, terrainStride * item,
                TERRAIN_FLOATS * sizeof(float), {}, {}});
            for (std::size_t texture = 0; texture < 5; ++texture)
                terrainDesc.resources.push_back({
                    static_cast<std::uint16_t>(texture + 1), 0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,
                    {}, 0, 0, terrainDraws[item].views[texture],
                    texture ? mRepeatSampler : mClampSampler});
            terrainDraws[item].set =
                mDevice.createBindingSet(terrainDesc, status);
        }
        if (!status)
        {
            destroyTransient();
            return status;
        }

        CommandContext& commands = mDevice.commandContext();
        bool frameBegun = false;
        bool renderingBegun = false;
        status = commands.beginFrame();
        frameBegun = status.ok();
        auto copy = [&commands, upload](BufferHandle destination,
                                        std::uint64_t sourceOffset,
                                        std::uint64_t bytes) -> Status
        {
            const std::array<BufferCopyRegion, 1> regions{{
                {sourceOffset, 0, bytes}}};
            return commands.copyBuffer(upload, destination, regions);
        };
        if (status) status = copy(materialVertices, materialVertexOffset,
                                  materialVertexBytes);
        if (status) status = copy(materialIndices, materialIndexOffset,
                                  materialIndexBytes);
        if (status) status = copy(terrainVertices, terrainVertexOffset,
                                  terrainVertexBytes);
        if (status) status = copy(terrainIndices, terrainIndexOffset,
                                  terrainIndexBytes);
        if (status) status = copy(frames, frameOffset,
                                  frameStride * materialDraws.size());
        if (status) status = copy(objects, objectOffset,
                                  objectStride * materialDraws.size());
        if (status) status = copy(skins, skinOffset,
                                  skinStride * materialDraws.size());
        if (status) status = copy(materials, materialOffset,
                                  materialStride * materialDraws.size());
        if (status) status = copy(terrains, terrainOffset,
                                  terrainStride * terrainDraws.size());

        RenderingInfo rendering;
        rendering.semanticId = 0x493863325f474255ull; // "I8c2_GBU"
        rendering.width = targets.width;
        rendering.height = targets.height;
        for (std::size_t target = 0; target < PRODUCTION_GBUFFER_TARGETS; ++target)
            rendering.colors.push_back({
                targets.gbufferViews[target], COLOR_FORMATS[target],
                LoadOp::Clear, StoreOp::Store,
                {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
        rendering.depthStencil = AttachmentDesc{
            targets.depthView, DEPTH_FORMAT, LoadOp::Clear, StoreOp::Store,
            {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}};
        if (status)
        {
            status = commands.beginRendering(rendering);
            renderingBegun = status.ok();
        }
        if (status) status = commands.setViewport({
            0.f, 0.f, static_cast<float>(targets.width),
            static_cast<float>(targets.height), 0.f, 1.f});
        if (status) status = commands.setScissor(
            {0, 0, targets.width, targets.height});
        bool materialGeometryBound = false;
        for (const MaterialDraw& executable : materialDraws)
        {
            if (!status) break;
            const MaterialSceneDraw& draw =
                frame.materials.draws[executable.source];
            const MaterialResource& material =
                frame.materials.materials[draw.material];
            status = commands.bindPipeline(material.doubleSided
                ? mMaterialDoubleSidedPipeline : mMaterialCulledPipeline);
            if (status && !materialGeometryBound)
            {
                status = commands.bindVertexBuffer(0, materialVertices, 0);
                if (status) status = commands.bindIndexBuffer(
                    materialIndices, 0, IndexType::UInt32);
                materialGeometryBound = status.ok();
            }
            if (status) status = commands.bindBindingSet(0, executable.frameSet);
            if (status) status = commands.bindBindingSet(1, executable.objectSet);
            if (status) status = commands.bindBindingSet(2, executable.materialSet);
            if (status) status = commands.drawIndexed(
                {draw.indexCount, 1, draw.firstIndex, 0, 0});
        }
        if (status) status = commands.bindPipeline(mTerrainPipeline);
        if (status) status = commands.bindVertexBuffer(0, terrainVertices, 0);
        if (status) status = commands.bindIndexBuffer(
            terrainIndices, 0, IndexType::UInt32);
        for (const TerrainDraw& executable : terrainDraws)
        {
            if (!status) break;
            const TerrainSceneDraw& draw = frame.terrain.draws[executable.source];
            if (status) status = commands.bindBindingSet(0, executable.set);
            if (status) status = commands.drawIndexed(
                {draw.indexCount, 1, draw.firstIndex, 0, 0});
        }
        if (renderingBegun)
        {
            const Status ended = commands.endRendering();
            if (status && !ended) status = ended;
        }
        for (std::size_t target = 0;
             status && target < PRODUCTION_GBUFFER_TARGETS; ++target)
        {
            BufferImageCopyRegion imageCopy;
            imageCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
            imageCopy.imageExtent = {targets.width, targets.height, 1};
            const std::array<BufferImageCopyRegion, 1> copies{{imageCopy}};
            status = commands.copyImageToBuffer(
                targets.gbufferImages[target], mReadbacks[target], copies);
        }
        if (frameBegun)
        {
            const Status ended = commands.endFrame();
            if (status && !ended) status = ended;
        }
        const Status retired = destroyTransient();
        if (!status) return status;
        if (!retired) return retired;

        mPendingResult = {};
        mPendingResult.frameId = frame.frameId;
        mPendingResult.assemblyEpoch = frame.assemblyEpoch;
        mPendingResult.targetGeneration = targets.generation;
        mPendingResult.width = targets.width;
        mPendingResult.height = targets.height;
        mPendingResult.materialDraws =
            static_cast<std::uint32_t>(materialDraws.size());
        mPendingResult.riggedMaterialDraws = riggedDraws;
        mPendingResult.terrainDraws =
            static_cast<std::uint32_t>(terrainDraws.size());
        mPendingResult.pbrTerrainDraws = pbrTerrainDraws;
        mPendingResult.deferredMaterialDraws = deferredMaterial;
        mPendingResult.deferredTerrainDraws = deferredTerrain;
        mPendingResult.uploadBytes = uploadBytes;
        mPendingResult.frameSha256 = productionFramePacketSha256(frame);
        mPending = true;
        return Status::success();
    }

    Status poll(ProductionGBufferResult& result)
    {
        result = {};
        if (!mPending)
            return Status::failure(StatusCode::InvalidState,
                                   "production G-buffer has no pending execution");
        for (std::size_t target = 0; target < PRODUCTION_GBUFFER_TARGETS; ++target)
        {
            const Status status = mDevice.readBuffer(
                mReadbacks[target], 0, mPixels[target]);
            if (!status) return status;
        }
        for (std::size_t target = 0; target < PRODUCTION_GBUFFER_TARGETS; ++target)
        {
            mPendingResult.colorSha256[target] = sha256(mPixels[target]);
            const std::size_t bytes = COLOR_BYTES[target];
            for (std::size_t pixel = 0;
                 pixel < static_cast<std::size_t>(mWidth) * mHeight; ++pixel)
            {
                const auto begin = mPixels[target].begin() +
                    static_cast<std::ptrdiff_t>(pixel * bytes);
                if (std::any_of(begin, begin + bytes,
                                [](std::byte value)
                                { return value != std::byte{}; }))
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
        destroyReadbacks(first);
        destroy(mMaterialCulledPipeline, first);
        destroy(mMaterialDoubleSidedPipeline, first);
        destroy(mTerrainPipeline, first);
        destroy(mMaterialShader, first);
        destroy(mTerrainShader, first);
        destroy(mRepeatSampler, first);
        destroy(mClampSampler, first);
        for (auto& view : mFallbackViews) destroy(view, first);
        for (auto& image : mFallbackImages) destroy(image, first);
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

    void destroyReadbacks(Status& first)
    {
        for (auto& readback : mReadbacks) destroy(readback, first);
        for (auto& pixels : mPixels) pixels.clear();
        mReadbackGeneration = 0;
        mWidth = mHeight = 0;
    }

    Status ensureReadbacks(const ProductionFrameTargetSet& targets)
    {
        if (mReadbackGeneration == targets.generation &&
            mWidth == targets.width && mHeight == targets.height)
            return Status::success();
        std::array<BufferHandle, PRODUCTION_GBUFFER_TARGETS> replacement{};
        Status status = Status::success();
        for (std::size_t target = 0;
             status && target < PRODUCTION_GBUFFER_TARGETS; ++target)
            replacement[target] = mDevice.createBuffer({
                static_cast<std::uint64_t>(targets.width) * targets.height *
                    COLOR_BYTES[target],
                ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
        if (!status)
        {
            Status ignored = Status::success();
            for (auto& readback : replacement) destroy(readback, ignored);
            return status;
        }
        Status retired = Status::success();
        destroyReadbacks(retired);
        if (!retired)
        {
            for (auto& readback : replacement) destroy(readback, retired);
            return retired;
        }
        mReadbacks = replacement;
        mReadbackGeneration = targets.generation;
        mWidth = targets.width;
        mHeight = targets.height;
        for (std::size_t target = 0; target < PRODUCTION_GBUFFER_TARGETS; ++target)
            mPixels[target].resize(
                static_cast<std::size_t>(mWidth) * mHeight * COLOR_BYTES[target]);
        return Status::success();
    }

    Status initialize()
    {
        if (mMaterialCulledPipeline) return Status::success();
        if (mShutdown)
            return Status::failure(StatusCode::InvalidState,
                                   "production G-buffer executor is shut down");
        const RendererCapabilities& capabilities = mDevice.capabilities();
        if (capabilities.maxColorAttachments < PRODUCTION_GBUFFER_TARGETS ||
            capabilities.preferredDepthStencilFormat == Format::Undefined ||
            capabilities.maxSampledImagesPerStage < 5)
            return unsupported("device lacks production G-buffer capabilities");

        Status status = Status::success();
        mMaterialShader = mDevice.createShaderPackage(mMaterialPackage, status);
        if (status) mTerrainShader =
            mDevice.createShaderPackage(mTerrainPackage, status);
        SamplerDesc repeat;
        repeat.minFilter = repeat.magFilter = repeat.mipFilter = Filter::Linear;
        repeat.addressU = repeat.addressV = AddressMode::Repeat;
        if (status) mRepeatSampler = mDevice.createSampler(repeat, status);
        SamplerDesc clamp = repeat;
        clamp.addressU = clamp.addressV = AddressMode::ClampToEdge;
        if (status) mClampSampler = mDevice.createSampler(clamp, status);

        constexpr std::array<std::array<std::uint8_t, 4>, 4> fallbackPixels{{
            {{255, 255, 255, 255}}, {{128, 128, 255, 255}},
            {{255, 255, 0, 255}}, {{0, 0, 0, 255}}}};
        constexpr std::array<Format, 4> fallbackFormats{{
            Format::RGBA8SRGB, Format::RGBA8UNorm,
            Format::RGBA8UNorm, Format::RGBA8SRGB}};
        BufferHandle fallbackUpload;
        if (status) fallbackUpload = mDevice.createBuffer({
            16, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
        std::array<std::byte, 16> bytes{};
        for (std::size_t texture = 0; texture < fallbackPixels.size(); ++texture)
            for (std::size_t component = 0; component < 4; ++component)
                bytes[texture * 4 + component] =
                    static_cast<std::byte>(fallbackPixels[texture][component]);
        if (status) status = mDevice.writeBuffer(fallbackUpload, 0, bytes);
        for (std::size_t texture = 0;
             status && texture < mFallbackImages.size(); ++texture)
        {
            mFallbackImages[texture] = mDevice.createImage({
                {1, 1, 1}, fallbackFormats[texture],
                ResourceUsage::Sampled | ResourceUsage::TransferDestination,
                1, 1, 1}, status);
            if (status) mFallbackViews[texture] = mDevice.createImageView({
                mFallbackImages[texture], fallbackFormats[texture],
                {ImageAspect::Color, 0, 1, 0, 1}}, status);
        }
        if (status)
        {
            CommandContext& commands = mDevice.commandContext();
            bool begun = false;
            status = commands.beginFrame();
            begun = status.ok();
            for (std::size_t texture = 0;
                 status && texture < mFallbackImages.size(); ++texture)
            {
                BufferImageCopyRegion copy;
                copy.bufferOffset = texture * 4;
                copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                copy.imageExtent = {1, 1, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{copy}};
                status = commands.copyBufferToImage(
                    fallbackUpload, mFallbackImages[texture], copies);
            }
            if (begun)
            {
                const Status ended = commands.endFrame();
                if (status && !ended) status = ended;
            }
        }
        Status uploadRetired = Status::success();
        destroy(fallbackUpload, uploadRetired);
        if (status && !uploadRetired) status = uploadRetired;

        if (status)
        {
            PipelineDesc material;
            material.shader = mMaterialShader;
            material.cullMode = CullMode::Back;
            material.depthTest = true;
            material.depthWrite = true;
            material.depthCompare = CompareOp::GreaterEqual;
            material.colorFormats.assign(COLOR_FORMATS.begin(), COLOR_FORMATS.end());
            material.depthStencilFormat = DEPTH_FORMAT;
            material.blendStates.assign(PRODUCTION_GBUFFER_TARGETS, BlendState{});
            material.vertexBuffers = {{
                0, sizeof(MaterialSceneVertex), VertexInputRate::PerVertex}};
            material.vertexAttributes = {
                {0, 0, VertexFormat::Float32x3,
                 offsetof(MaterialSceneVertex, position)},
                {1, 0, VertexFormat::Float32x3,
                 offsetof(MaterialSceneVertex, normal)},
                {2, 0, VertexFormat::Float32x4,
                 offsetof(MaterialSceneVertex, tangent)},
                {3, 0, VertexFormat::Float32x2,
                 offsetof(MaterialSceneVertex, texCoord)},
                {4, 0, VertexFormat::UNorm8x4,
                 offsetof(MaterialSceneVertex, color)},
                {5, 0, VertexFormat::UInt16x4,
                 offsetof(MaterialSceneVertex, joints)},
                {6, 0, VertexFormat::Float32x4,
                 offsetof(MaterialSceneVertex, weights)}};
            mMaterialCulledPipeline = mDevice.createPipeline(material, status);
            if (status)
            {
                material.cullMode = CullMode::None;
                mMaterialDoubleSidedPipeline =
                    mDevice.createPipeline(material, status);
            }
            PipelineDesc terrain;
            terrain.shader = mTerrainShader;
            terrain.cullMode = CullMode::Back;
            terrain.depthTest = true;
            terrain.depthWrite = true;
            terrain.depthCompare = CompareOp::GreaterEqual;
            terrain.colorFormats.assign(COLOR_FORMATS.begin(), COLOR_FORMATS.end());
            terrain.depthStencilFormat = DEPTH_FORMAT;
            terrain.blendStates.assign(PRODUCTION_GBUFFER_TARGETS, BlendState{});
            terrain.vertexBuffers = {{
                0, sizeof(TerrainSceneVertex), VertexInputRate::PerVertex}};
            terrain.vertexAttributes = {
                {0, 0, VertexFormat::Float32x3,
                 offsetof(TerrainSceneVertex, position)},
                {1, 0, VertexFormat::Float32x3,
                 offsetof(TerrainSceneVertex, normal)},
                {3, 0, VertexFormat::Float32x2,
                 offsetof(TerrainSceneVertex, compositionCoord)}};
            if (status) mTerrainPipeline = mDevice.createPipeline(terrain, status);
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
    ShaderPackageDesc mMaterialPackage;
    ShaderPackageDesc mTerrainPackage;
    ShaderPackageHandle mMaterialShader;
    ShaderPackageHandle mTerrainShader;
    PipelineHandle mMaterialCulledPipeline;
    PipelineHandle mMaterialDoubleSidedPipeline;
    PipelineHandle mTerrainPipeline;
    SamplerHandle mRepeatSampler;
    SamplerHandle mClampSampler;
    std::array<ImageHandle, 4> mFallbackImages{};
    std::array<ImageViewHandle, 4> mFallbackViews{};
    std::array<BufferHandle, PRODUCTION_GBUFFER_TARGETS> mReadbacks{};
    std::array<std::vector<std::byte>, PRODUCTION_GBUFFER_TARGETS> mPixels;
    std::uint64_t mReadbackGeneration = 0;
    std::uint32_t mWidth = 0;
    std::uint32_t mHeight = 0;
    ProductionGBufferResult mPendingResult;
    bool mPending = false;
    bool mShutdown = false;
};

ProductionGBufferExecutor::ProductionGBufferExecutor(
    Device& device, ShaderPackageDesc materialShader,
    ShaderPackageDesc terrainShader) :
    mImpl(std::make_unique<Impl>(device, std::move(materialShader),
                                 std::move(terrainShader)))
{
}

ProductionGBufferExecutor::~ProductionGBufferExecutor() = default;

Status ProductionGBufferExecutor::submit(
    const ProductionFramePacket& frame,
    const ProductionFrameTargetSet& targets,
    const ProductionTextureResidency& residency,
    const ProductionGBufferLimits& limits)
{
    return mImpl->submit(frame, targets, residency, limits);
}

Status ProductionGBufferExecutor::poll(ProductionGBufferResult& result)
{
    return mImpl->poll(result);
}

bool ProductionGBufferExecutor::pending() const
{
    return mImpl->pending();
}

Status ProductionGBufferExecutor::shutdown()
{
    return mImpl->shutdown();
}

} // namespace LL::GHI
