/**
 * @file llghiproductionlightingexecutor.cpp
 * @brief I8c3 shared-target native shadow and deferred-light execution.
 */

#include "linden_common.h"

#include "ghi/include/llghiproductionlightingexecutor.h"

#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace LL::GHI
{
namespace
{
constexpr Format LIGHTING_FORMAT = Format::RGBA16Float;
constexpr Format SHADOW_FORMAT = Format::Depth32Float;
constexpr std::uint32_t LIGHTING_BYTES = 8;
constexpr std::uint32_t SHADOW_BYTES = 4;
constexpr std::size_t MAX_POINT_LIGHTS = 64;
constexpr std::size_t LIGHTING_HEADER_FLOATS = 16 * 2 + 4 * 6;
constexpr std::size_t LIGHTING_POINT_FLOATS =
    LIGHTING_HEADER_FLOATS + MAX_POINT_LIGHTS * 8;
constexpr std::size_t LIGHTING_DATA_FLOATS = LIGHTING_POINT_FLOATS +
    LIGHTING_DIRECTIONAL_SHADOW_CASCADES * 16 + 8;
constexpr std::size_t PROJECTOR_DATA_FLOATS = 76;
constexpr std::size_t OBJECT_FLOATS = 32;
using LightingData = std::array<float, LIGHTING_DATA_FLOATS>;
using ProjectorData = std::array<float, PROJECTOR_DATA_FLOATS>;
using SkinData = std::array<float, MATERIAL_SKIN_FLOATS>;
using ShadowMaterialData = std::array<float, 12>;

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

Status unsupported(const char* message)
{
    return Status::failure(StatusCode::Unsupported, message);
}

bool hasComparability(ResourceComparability value,
                      ResourceComparability flag)
{
    return (static_cast<std::uint32_t>(value) &
            static_cast<std::uint32_t>(flag)) != 0;
}

const MaterialTextureBinding* findBinding(const MaterialResource& material,
                                          TextureSemantic semantic)
{
    const auto found = std::find_if(material.textures.begin(),
        material.textures.end(), [semantic](const MaterialTextureBinding& binding)
        { return binding.semantic == semantic; });
    return found == material.textures.end() ? nullptr : &*found;
}

bool supportedTextureCoordinates(const MaterialResource& material)
{
    return std::all_of(material.textures.begin(), material.textures.end(),
        [](const MaterialTextureBinding& binding)
        { return binding.texcoord == 0; });
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

ShadowMaterialData makeShadowMaterialData(const MaterialResource& material)
{
    ShadowMaterialData data{};
    data[0] = material.alphaCutoff;
    data[1] = material.baseColor[3];
    data[2] = material.alphaMode == MaterialAlphaMode::Mask ? 1.f : 0.f;
    data[6] = data[7] = data[8] = 1.f;
    if (const MaterialTextureBinding* base =
            findBinding(material, TextureSemantic::BaseColor))
    {
        data[4] = base->transform[0];
        data[5] = base->transform[1];
        data[6] = base->transform[2];
        data[7] = base->transform[3];
        data[8] = std::cos(base->transform[4]);
        data[9] = std::sin(base->transform[4]);
    }
    return data;
}

std::array<float, 16> multiplyMatrix(const std::array<float, 16>& lhs,
                                     const std::array<float, 16>& rhs)
{
    std::array<float, 16> output{};
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t inner = 0; inner < 4; ++inner)
                output[column * 4 + row] +=
                    lhs[inner * 4 + row] * rhs[column * 4 + inner];
    return output;
}

std::array<float, 16> shadowClipMatrix(const LightingScenePacket& packet,
                                       std::size_t shadow)
{
    constexpr std::array<float, 16> textureToClip{{
        2.f, 0.f, 0.f, 0.f, 0.f, 2.f, 0.f, 0.f,
        0.f, 0.f, 2.f, 0.f, -1.f, -1.f, -1.f, 1.f}};
    return multiplyMatrix(textureToClip,
        multiplyMatrix(packet.shadows.matrices[shadow], packet.viewMatrix));
}

LightingData makeLightingData(const LightingScenePacket& packet,
                              std::uint32_t maxPointLights,
                              bool nativeShadows,
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
        directionalLights += light.active ? 1u : 0u;
    };
    copyDirectional(packet.sun, 40);
    copyDirectional(packet.moon, 48);
    const std::uint32_t limit = std::min<std::uint32_t>(
        maxPointLights, static_cast<std::uint32_t>(MAX_POINT_LIGHTS));
    for (const LocalLightRecord& light : packet.localLights)
    {
        if (light.kind != LocalLightKind::Point || pointLights == limit) continue;
        const std::size_t position = LIGHTING_HEADER_FLOATS + pointLights * 4;
        const std::size_t color = LIGHTING_HEADER_FLOATS +
            MAX_POINT_LIGHTS * 4 + pointLights * 4;
        std::copy(light.position.begin(), light.position.end(),
                  data.begin() + static_cast<std::ptrdiff_t>(position));
        data[position + 3] = light.radius;
        std::copy(light.color.begin(), light.color.end(),
                  data.begin() + static_cast<std::ptrdiff_t>(color));
        data[color + 3] = light.falloff;
        ++pointLights;
    }
    data[35] = static_cast<float>(pointLights);
    for (std::size_t cascade = 0;
         cascade < LIGHTING_DIRECTIONAL_SHADOW_CASCADES; ++cascade)
        std::copy(packet.shadows.matrices[cascade].begin(),
                  packet.shadows.matrices[cascade].end(),
                  data.begin() + static_cast<std::ptrdiff_t>(
                      LIGHTING_POINT_FLOATS + cascade * 16));
    const std::size_t shadowClip = LIGHTING_POINT_FLOATS +
        LIGHTING_DIRECTIONAL_SHADOW_CASCADES * 16;
    std::copy(packet.shadows.clipPlanes.begin(), packet.shadows.clipPlanes.end(),
              data.begin() + static_cast<std::ptrdiff_t>(shadowClip));
    data[shadowClip + 4] = packet.shadows.directionalBias;
    data[shadowClip + 5] = nativeShadows && packet.shadows.enabled &&
        packet.shadows.directionalCascadeCount ? 1.f : 0.f;
    data[shadowClip + 6] = static_cast<float>(
        std::min<std::uint32_t>(packet.shadows.directionalCascadeCount,
            LIGHTING_DIRECTIONAL_SHADOW_CASCADES));
    return data;
}

ProjectorData makeProjectorData(const LightingScenePacket& packet,
                                const LocalLightRecord& light,
                                std::uint32_t levels,
                                bool nativeShadows)
{
    ProjectorData data{};
    std::copy(packet.viewMatrix.begin(), packet.viewMatrix.end(), data.begin());
    std::copy(packet.projectionMatrix.begin(), packet.projectionMatrix.end(),
              data.begin() + 16);
    std::copy(packet.cameraOrigin.begin(), packet.cameraOrigin.end(),
              data.begin() + 32);
    std::copy(light.position.begin(), light.position.end(), data.begin() + 36);
    data[39] = light.radius;
    std::copy(light.color.begin(), light.color.end(), data.begin() + 40);
    data[43] = light.falloff;
    std::copy(light.rotation.begin(), light.rotation.end(), data.begin() + 44);
    std::copy(light.scale.begin(), light.scale.end(), data.begin() + 48);
    std::copy(light.projectorParams.begin(), light.projectorParams.end(),
              data.begin() + 52);
    data[55] = static_cast<float>(levels);
    const bool shadowed = nativeShadows && packet.shadows.enabled &&
        light.shadowSlot >= 0 &&
        light.shadowSlot < static_cast<std::int32_t>(LIGHTING_PROJECTOR_SHADOWS);
    if (shadowed)
    {
        const std::size_t matrix = LIGHTING_DIRECTIONAL_SHADOW_CASCADES +
            static_cast<std::size_t>(light.shadowSlot);
        std::copy(packet.shadows.matrices[matrix].begin(),
                  packet.shadows.matrices[matrix].end(), data.begin() + 56);
    }
    data[72] = packet.shadows.spotShadowBias;
    data[73] = packet.shadows.spotShadowOffset;
    data[74] = light.shadowFade;
    data[75] = shadowed ? 1.f : 0.f;
    return data;
}

std::array<double, 4> transformPoint(const std::array<float, 16>& matrix,
                                     const std::array<double, 4>& point)
{
    std::array<double, 4> result{};
    for (std::size_t row = 0; row < 4; ++row)
        result[row] = matrix[row] * point[0] + matrix[4 + row] * point[1] +
                      matrix[8 + row] * point[2] + matrix[12 + row] * point[3];
    return result;
}

ScissorRect projectorScissor(const LightingScenePacket& packet,
                             const LocalLightRecord& light,
                             std::uint32_t width, std::uint32_t height,
                             bool& fullscreen)
{
    const double dx = packet.cameraOrigin[0] - light.position[0];
    const double dy = packet.cameraOrigin[1] - light.position[1];
    const double dz = packet.cameraOrigin[2] - light.position[2];
    fullscreen = dx * dx + dy * dy + dz * dz <
        static_cast<double>(light.radius) * light.radius;
    if (fullscreen) return {0, 0, width, height};
    double minX = 1., minY = 1., maxX = -1., maxY = -1.;
    for (int z = -1; z <= 1; z += 2)
        for (int y = -1; y <= 1; y += 2)
            for (int x = -1; x <= 1; x += 2)
            {
                const std::array<double, 4> world{{
                    light.position[0] + x * light.radius,
                    light.position[1] + y * light.radius,
                    light.position[2] + z * light.radius, 1.}};
                const auto view = transformPoint(packet.viewMatrix, world);
                const auto clip = transformPoint(packet.projectionMatrix, view);
                if (!std::isfinite(clip[3]) || clip[3] <= 1.e-6)
                {
                    fullscreen = true;
                    return {0, 0, width, height};
                }
                minX = std::min(minX, clip[0] / clip[3]);
                maxX = std::max(maxX, clip[0] / clip[3]);
                minY = std::min(minY, clip[1] / clip[3]);
                maxY = std::max(maxY, clip[1] / clip[3]);
            }
    minX = std::clamp(minX, -1., 1.);
    maxX = std::clamp(maxX, -1., 1.);
    minY = std::clamp(minY, -1., 1.);
    maxY = std::clamp(maxY, -1., 1.);
    if (maxX <= minX || maxY <= minY) return {};
    const auto left = static_cast<std::int32_t>(std::floor(
        (minX * .5 + .5) * width));
    const auto right = static_cast<std::int32_t>(std::ceil(
        (maxX * .5 + .5) * width));
    const auto top = static_cast<std::int32_t>(std::floor(
        (.5 - maxY * .5) * height));
    const auto bottom = static_cast<std::int32_t>(std::ceil(
        (.5 - minY * .5) * height));
    return {left, top, static_cast<std::uint32_t>(std::max(0, right - left)),
            static_cast<std::uint32_t>(std::max(0, bottom - top))};
}

ResourceDigest projectorSource(const std::array<std::uint8_t, 16>& source)
{
    ResourceDigest result{};
    std::transform(source.begin(), source.end(), result.begin(),
        [](std::uint8_t value) { return static_cast<std::byte>(value); });
    return result;
}

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment)
{
    if (value > std::numeric_limits<std::uint64_t>::max() - alignment + 1)
        return std::numeric_limits<std::uint64_t>::max();
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

class ProductionLightingExecutor::Impl
{
public:
    Impl(Device& device, ShaderPackageDesc lightingShader,
         ShaderPackageDesc projectorShader, ShaderPackageDesc shadowShader) :
        mDevice(device), mLightingPackage(std::move(lightingShader)),
        mProjectorPackage(std::move(projectorShader)),
        mShadowPackage(std::move(shadowShader))
    {
    }

    ~Impl() { shutdown(); }

    Status submit(const ProductionFramePacket& frame,
                  const ProductionFrameTargetSet& targets,
                  const ProductionTextureResidency& residency,
                  const ProductionLightingLimits& limits)
    {
        if (mPending)
            return Status::failure(StatusCode::NotReady,
                "production lighting execution is still pending");
        if (!limits.maxPointLights || !limits.maxProjectorLights ||
            !limits.maxShadowDraws || !limits.maxUploadBytes)
            return invalid("production lighting limits must be nonzero");
        Status status = validateProductionFramePacket(frame);
        if (!status) return status;
        if (!productionFrameHasPass(frame.passes,
                                    ProductionFramePass::DeferredLighting))
            return invalid("production lighting requires the deferred-light pass");
        if (!targets.width || !targets.height || !targets.generation ||
            !targets.depthView || !targets.lightingView ||
            std::any_of(targets.gbufferViews.begin(), targets.gbufferViews.end(),
                        [](ImageViewHandle view) { return !view; }))
            return invalid("production lighting target views are incomplete");

        status = initialize();
        if (!status) return status;
        status = ensureReadbacks(targets);
        if (!status) return status;

        struct ShadowPass
        {
            bool active = false;
            std::array<float, 16> matrix{};
            BindingSetHandle set;
        };
        std::array<ShadowPass, PRODUCTION_SHADOW_TARGETS> shadowPasses{};
        if (frame.lighting.shadows.enabled &&
            productionFrameHasPass(frame.passes,
                                   ProductionFramePass::DirectionalShadow))
        {
            const std::uint32_t cascades = std::min<std::uint32_t>(
                frame.lighting.shadows.directionalCascadeCount,
                LIGHTING_DIRECTIONAL_SHADOW_CASCADES);
            for (std::uint32_t shadow = 0; shadow < cascades; ++shadow)
            {
                if (!targets.shadowViews[shadow])
                    return invalid("directional shadow target is absent");
                shadowPasses[shadow].active = true;
                shadowPasses[shadow].matrix = shadowClipMatrix(
                    frame.lighting, shadow);
            }
        }
        if (frame.lighting.shadows.enabled &&
            productionFrameHasPass(frame.passes,
                                   ProductionFramePass::ProjectorShadow))
        {
            const std::uint32_t count = std::min<std::uint32_t>(
                frame.lighting.shadows.projectorShadowCount,
                LIGHTING_PROJECTOR_SHADOWS);
            for (std::uint32_t slot = 0; slot < count; ++slot)
            {
                const std::size_t shadow =
                    LIGHTING_DIRECTIONAL_SHADOW_CASCADES + slot;
                if (!targets.shadowViews[shadow])
                    return invalid("projector shadow target is absent");
                shadowPasses[shadow].active = true;
                shadowPasses[shadow].matrix = shadowClipMatrix(
                    frame.lighting, shadow);
            }
        }
        const bool renderShadows = std::any_of(
            shadowPasses.begin(), shadowPasses.end(),
            [](const ShadowPass& shadow) { return shadow.active; });

        struct Caster
        {
            std::size_t source = 0;
            bool masked = false;
            ImageViewHandle baseColorView;
            BindingSetHandle objectSet;
            BindingSetHandle materialSet;
        };
        std::vector<Caster> casters;
        std::uint32_t deferredCasters = 0;
        if (renderShadows)
        {
            for (std::size_t source = 0;
                 source < frame.materials.draws.size(); ++source)
            {
                const MaterialSceneDraw& draw = frame.materials.draws[source];
                if (!draw.indexCount ||
                    draw.material >= frame.materials.materials.size()) continue;
                const MaterialResource& material =
                    frame.materials.materials[draw.material];
                if (material.alphaMode == MaterialAlphaMode::Blend) continue;
                if (hasComparability(draw.comparability,
                        ResourceComparability::UnsupportedVertexLayout) ||
                    hasComparability(draw.comparability,
                        ResourceComparability::MissingSkinPalette))
                {
                    ++deferredCasters;
                    continue;
                }
                if (draw.skin != NO_RESOURCE &&
                    (draw.skin >= frame.materials.skins.size() ||
                     !validSkin(frame.materials.skins[draw.skin])))
                {
                    ++deferredCasters;
                    continue;
                }
                Caster caster;
                caster.source = source;
                caster.masked = material.alphaMode == MaterialAlphaMode::Mask;
                caster.baseColorView = mWhiteView;
                if (caster.masked)
                {
                    if (!supportedTextureCoordinates(material))
                    {
                        ++deferredCasters;
                        continue;
                    }
                    const MaterialTextureBinding* base = findBinding(
                        material, TextureSemantic::BaseColor);
                    if (!base || base->texture >= frame.materials.textures.size())
                    {
                        ++deferredCasters;
                        continue;
                    }
                    const auto resident = residency.find({
                        ProductionTextureDomain::Material,
                        frame.materials.textures[base->texture].sourceIdentity});
                    if (!resident)
                    {
                        ++deferredCasters;
                        continue;
                    }
                    caster.baseColorView = resident->view;
                }
                casters.push_back(caster);
            }
            std::stable_sort(casters.begin(), casters.end(),
                [&frame](const Caster& lhs, const Caster& rhs)
                {
                    if (lhs.masked != rhs.masked) return lhs.masked > rhs.masked;
                    return (frame.materials.draws[lhs.source].skin != NO_RESOURCE) >
                           (frame.materials.draws[rhs.source].skin != NO_RESOURCE);
                });
            if (casters.size() > limits.maxShadowDraws)
                casters.resize(limits.maxShadowDraws);
            if (casters.empty())
                return invalid("production shadow frame has no executable casters");
        }

        struct Projector
        {
            std::size_t sourceLight = 0;
            bool fullscreen = false;
            ScissorRect scissor;
            ImageViewHandle imageView;
            std::uint16_t mipLevels = 1;
            ProjectorData data{};
            BindingSetHandle set;
        };
        std::vector<Projector> projectors;
        std::set<ResourceDigest> projectorImages;
        if (productionFrameHasPass(frame.passes,
                                   ProductionFramePass::ProjectorLighting))
        {
            for (std::size_t source = 0;
                 source < frame.lighting.localLights.size() &&
                 projectors.size() < limits.maxProjectorLights; ++source)
            {
                const LocalLightRecord& light = frame.lighting.localLights[source];
                if (light.kind != LocalLightKind::Projector ||
                    light.comparability != LightingComparability::Comparable)
                    continue;
                const ResourceDigest identity = projectorSource(
                    light.projectorTextureIdentity);
                const auto resident = residency.find({
                    ProductionTextureDomain::Projector, identity});
                if (!resident) continue;
                Projector projector;
                projector.sourceLight = source;
                projector.imageView = resident->view;
                projector.mipLevels = resident->mipLevels;
                projector.scissor = projectorScissor(
                    frame.lighting, light, targets.width, targets.height,
                    projector.fullscreen);
                if (!projector.scissor.width || !projector.scissor.height)
                    continue;
                projector.data = makeProjectorData(
                    frame.lighting, light, resident->mipLevels, renderShadows);
                projectorImages.insert(identity);
                projectors.push_back(projector);
            }
        }

        std::uint32_t directionalLights = 0;
        std::uint32_t pointLights = 0;
        const LightingData lightingData = makeLightingData(
            frame.lighting, limits.maxPointLights, renderShadows,
            directionalLights, pointLights);
        const std::uint64_t alignment = std::max<std::uint64_t>(
            16, mDevice.capabilities().uniformBufferOffsetAlignment);
        const std::uint64_t vertexBytes = renderShadows
            ? frame.materials.vertices.size() * sizeof(MaterialSceneVertex) : 0;
        const std::uint64_t indexBytes = renderShadows
            ? frame.materials.indices.size() * sizeof(std::uint32_t) : 0;
        const std::uint64_t vertexOffset = 0;
        const std::uint64_t indexOffset = alignUp(vertexBytes, alignment);
        const std::uint64_t shadowFrameStride = alignUp(
            16 * sizeof(float), alignment);
        const std::uint64_t shadowFrameOffset = alignUp(
            indexOffset + indexBytes, alignment);
        const std::uint64_t objectStride = alignUp(
            OBJECT_FLOATS * sizeof(float), alignment);
        const std::uint64_t objectOffset = alignUp(
            shadowFrameOffset + shadowFrameStride * PRODUCTION_SHADOW_TARGETS,
            alignment);
        const std::uint64_t skinStride = alignUp(MATERIAL_SKIN_BYTES, alignment);
        const std::uint64_t skinOffset = alignUp(
            objectOffset + objectStride * casters.size(), alignment);
        const std::uint64_t materialStride = alignUp(
            sizeof(ShadowMaterialData), alignment);
        const std::uint64_t materialOffset = alignUp(
            skinOffset + skinStride * casters.size(), alignment);
        const std::uint64_t lightingOffset = alignUp(
            materialOffset + materialStride * casters.size(), alignment);
        const std::uint64_t projectorStride = alignUp(
            sizeof(ProjectorData), alignment);
        const std::uint64_t projectorOffset = alignUp(
            lightingOffset + sizeof(LightingData), alignment);
        const std::uint64_t uploadBytes = alignUp(
            projectorOffset + projectorStride * projectors.size(), alignment);
        if (uploadBytes == std::numeric_limits<std::uint64_t>::max() ||
            uploadBytes > limits.maxUploadBytes ||
            uploadBytes > mDevice.capabilities().maxBufferSize)
            return unsupported("production lighting upload exceeds its bounded limit");

        std::vector<std::byte> uploadData(static_cast<std::size_t>(uploadBytes));
        if (renderShadows)
        {
            std::memcpy(uploadData.data() + vertexOffset,
                        frame.materials.vertices.data(), vertexBytes);
            std::memcpy(uploadData.data() + indexOffset,
                        frame.materials.indices.data(), indexBytes);
        }
        for (std::size_t shadow = 0; shadow < shadowPasses.size(); ++shadow)
            if (shadowPasses[shadow].active)
                std::memcpy(uploadData.data() + shadowFrameOffset +
                                shadowFrameStride * shadow,
                            shadowPasses[shadow].matrix.data(),
                            sizeof(shadowPasses[shadow].matrix));
        for (std::size_t item = 0; item < casters.size(); ++item)
        {
            const MaterialSceneDraw& draw =
                frame.materials.draws[casters[item].source];
            const MaterialResource& material =
                frame.materials.materials[draw.material];
            std::array<float, OBJECT_FLOATS> object{};
            if (!makeObjectData(draw, object))
                return invalid("production shadow caster has a singular transform");
            std::memcpy(uploadData.data() + objectOffset + objectStride * item,
                        object.data(), sizeof(object));
            const SkinData skin = makeSkinData(draw.skin == NO_RESOURCE
                ? nullptr : &frame.materials.skins[draw.skin]);
            std::memcpy(uploadData.data() + skinOffset + skinStride * item,
                        skin.data(), sizeof(skin));
            const ShadowMaterialData shadowMaterial =
                makeShadowMaterialData(material);
            std::memcpy(uploadData.data() + materialOffset +
                            materialStride * item,
                        shadowMaterial.data(), sizeof(shadowMaterial));
        }
        std::memcpy(uploadData.data() + lightingOffset,
                    lightingData.data(), sizeof(lightingData));
        for (std::size_t item = 0; item < projectors.size(); ++item)
            std::memcpy(uploadData.data() + projectorOffset +
                            projectorStride * item,
                        projectors[item].data.data(),
                        sizeof(projectors[item].data));

        BufferHandle upload, vertices, indices, shadowFrames, objects, skins,
                     materials, lighting, projectorUniforms;
        BindingSetHandle lightingSet;
        auto destroyTransient = [&]()
        {
            Status first = Status::success();
            for (auto& shadow : shadowPasses) destroy(shadow.set, first);
            for (auto& caster : casters)
            {
                destroy(caster.objectSet, first);
                destroy(caster.materialSet, first);
            }
            for (auto& projector : projectors) destroy(projector.set, first);
            destroy(lightingSet, first);
            destroy(upload, first); destroy(vertices, first);
            destroy(indices, first); destroy(shadowFrames, first);
            destroy(objects, first); destroy(skins, first);
            destroy(materials, first); destroy(lighting, first);
            destroy(projectorUniforms, first);
            return first;
        };

        upload = mDevice.createBuffer({
            uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
        if (status && renderShadows) vertices = mDevice.createBuffer({
            vertexBytes, ResourceUsage::Vertex | ResourceUsage::TransferDestination,
            MemoryClass::DeviceLocal}, status);
        if (status && renderShadows) indices = mDevice.createBuffer({
            indexBytes, ResourceUsage::Index | ResourceUsage::TransferDestination,
            MemoryClass::DeviceLocal}, status);
        if (status && renderShadows) shadowFrames = mDevice.createBuffer({
            shadowFrameStride * PRODUCTION_SHADOW_TARGETS,
            ResourceUsage::Uniform | ResourceUsage::TransferDestination,
            MemoryClass::DeviceLocal}, status);
        if (status && renderShadows) objects = mDevice.createBuffer({
            objectStride * casters.size(),
            ResourceUsage::Uniform | ResourceUsage::TransferDestination,
            MemoryClass::DeviceLocal}, status);
        if (status && renderShadows) skins = mDevice.createBuffer({
            skinStride * casters.size(),
            ResourceUsage::Uniform | ResourceUsage::TransferDestination,
            MemoryClass::DeviceLocal}, status);
        if (status && renderShadows) materials = mDevice.createBuffer({
            materialStride * casters.size(),
            ResourceUsage::Uniform | ResourceUsage::TransferDestination,
            MemoryClass::DeviceLocal}, status);
        if (status) lighting = mDevice.createBuffer({
            sizeof(LightingData),
            ResourceUsage::Uniform | ResourceUsage::TransferDestination,
            MemoryClass::DeviceLocal}, status);
        if (status && !projectors.empty()) projectorUniforms = mDevice.createBuffer({
            projectorStride * projectors.size(),
            ResourceUsage::Uniform | ResourceUsage::TransferDestination,
            MemoryClass::DeviceLocal}, status);
        if (!status || !(status = mDevice.writeBuffer(upload, 0, uploadData)))
        {
            destroyTransient();
            return status;
        }

        for (std::size_t shadow = 0;
             status && shadow < shadowPasses.size(); ++shadow)
        {
            if (!shadowPasses[shadow].active) continue;
            BindingSetDesc desc{mShadowShader, 0, {{
                0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                shadowFrames, shadowFrameStride * shadow,
                16 * sizeof(float), {}, {}}}};
            shadowPasses[shadow].set = mDevice.createBindingSet(desc, status);
        }
        for (std::size_t item = 0; status && item < casters.size(); ++item)
        {
            BindingSetDesc objectDesc{mShadowShader, 1, {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 objects, objectStride * item,
                 OBJECT_FLOATS * sizeof(float), {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 skins, skinStride * item, MATERIAL_SKIN_BYTES, {}, {}}}};
            casters[item].objectSet = mDevice.createBindingSet(objectDesc, status);
            BindingSetDesc materialDesc{mShadowShader, 2, {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 materials, materialStride * item,
                 sizeof(ShadowMaterialData), {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                 {}, 0, 0, casters[item].baseColorView, mRepeatSampler}}};
            if (status) casters[item].materialSet =
                mDevice.createBindingSet(materialDesc, status);
        }
        if (status)
        {
            BindingSetDesc lightingDesc;
            lightingDesc.shader = mLightingShader;
            lightingDesc.group = 0;
            lightingDesc.resources.push_back({
                0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                lighting, 0, sizeof(LightingData), {}, {}});
            for (std::size_t target = 0;
                 target < PRODUCTION_GBUFFER_TARGETS; ++target)
                lightingDesc.resources.push_back({
                    static_cast<std::uint16_t>(target + 1), 0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,
                    {}, 0, 0, targets.gbufferViews[target], mClampSampler});
            lightingDesc.resources.push_back({
                5, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                {}, 0, 0, targets.depthView, mClampSampler});
            for (std::size_t shadow = 0;
                 shadow < LIGHTING_DIRECTIONAL_SHADOW_CASCADES; ++shadow)
                lightingDesc.resources.push_back({
                    static_cast<std::uint16_t>(6 + shadow), 0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,
                    {}, 0, 0, shadowPasses[shadow].active
                        ? targets.shadowViews[shadow] : mShadowFallbackView,
                    mShadowSampler});
            lightingSet = mDevice.createBindingSet(lightingDesc, status);
        }
        for (std::size_t item = 0; status && item < projectors.size(); ++item)
        {
            const LocalLightRecord& light =
                frame.lighting.localLights[projectors[item].sourceLight];
            const std::size_t shadow = light.shadowSlot >= 0 &&
                light.shadowSlot < static_cast<std::int32_t>(
                    LIGHTING_PROJECTOR_SHADOWS)
                ? LIGHTING_DIRECTIONAL_SHADOW_CASCADES +
                    static_cast<std::size_t>(light.shadowSlot)
                : PRODUCTION_SHADOW_TARGETS;
            BindingSetDesc projectorDesc;
            projectorDesc.shader = mProjectorShader;
            projectorDesc.group = 0;
            projectorDesc.resources.push_back({
                0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                projectorUniforms, projectorStride * item,
                sizeof(ProjectorData), {}, {}});
            for (std::size_t target = 0; target < 3; ++target)
                projectorDesc.resources.push_back({
                    static_cast<std::uint16_t>(target + 1), 0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,
                    {}, 0, 0, targets.gbufferViews[target], mClampSampler});
            projectorDesc.resources.push_back({
                5, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                {}, 0, 0, targets.depthView, mClampSampler});
            projectorDesc.resources.push_back({
                6, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                {}, 0, 0, projectors[item].imageView, mProjectorSampler});
            projectorDesc.resources.push_back({
                7, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                {}, 0, 0,
                shadow < PRODUCTION_SHADOW_TARGETS &&
                        shadowPasses[shadow].active
                    ? targets.shadowViews[shadow] : mShadowFallbackView,
                mShadowSampler});
            projectors[item].set =
                mDevice.createBindingSet(projectorDesc, status);
        }
        if (!status)
        {
            destroyTransient();
            return status;
        }

        CommandContext& commands = mDevice.commandContext();
        bool frameBegun = false;
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
        if (status && renderShadows) status = copy(
            vertices, vertexOffset, vertexBytes);
        if (status && renderShadows) status = copy(
            indices, indexOffset, indexBytes);
        if (status && renderShadows) status = copy(
            shadowFrames, shadowFrameOffset,
            shadowFrameStride * PRODUCTION_SHADOW_TARGETS);
        if (status && renderShadows) status = copy(
            objects, objectOffset, objectStride * casters.size());
        if (status && renderShadows) status = copy(
            skins, skinOffset, skinStride * casters.size());
        if (status && renderShadows) status = copy(
            materials, materialOffset, materialStride * casters.size());
        if (status) status = copy(lighting, lightingOffset,
                                  sizeof(LightingData));
        if (status && !projectors.empty()) status = copy(
            projectorUniforms, projectorOffset,
            projectorStride * projectors.size());

        for (std::size_t shadow = 0;
             status && shadow < shadowPasses.size(); ++shadow)
        {
            if (!shadowPasses[shadow].active) continue;
            RenderingInfo rendering;
            rendering.semanticId = 0x493863335f534844ull + shadow;
            rendering.width = targets.width;
            rendering.height = targets.height;
            rendering.depthStencil = AttachmentDesc{
                targets.shadowViews[shadow], SHADOW_FORMAT,
                LoadOp::Clear, StoreOp::Store,
                {{0.f, 0.f, 0.f, 0.f}, 1.f, 0}};
            bool begun = false;
            status = commands.beginRendering(rendering);
            begun = status.ok();
            if (status) status = commands.setViewport({
                0.f, 0.f, static_cast<float>(targets.width),
                static_cast<float>(targets.height), 0.f, 1.f});
            if (status) status = commands.setScissor(
                {0, 0, targets.width, targets.height});
            bool geometryBound = false;
            for (const Caster& caster : casters)
            {
                if (!status) break;
                const MaterialSceneDraw& draw =
                    frame.materials.draws[caster.source];
                const MaterialResource& material =
                    frame.materials.materials[draw.material];
                status = commands.bindPipeline(material.doubleSided
                    ? mShadowDoubleSidedPipeline : mShadowCulledPipeline);
                if (status) status = commands.bindBindingSet(
                    0, shadowPasses[shadow].set);
                if (status && !geometryBound)
                {
                    status = commands.bindVertexBuffer(0, vertices, 0);
                    if (status) status = commands.bindIndexBuffer(
                        indices, 0, IndexType::UInt32);
                    geometryBound = status.ok();
                }
                if (status) status = commands.bindBindingSet(
                    1, caster.objectSet);
                if (status) status = commands.bindBindingSet(
                    2, caster.materialSet);
                if (status) status = commands.drawIndexed(
                    {draw.indexCount, 1, draw.firstIndex, 0, 0});
            }
            if (begun)
            {
                const Status ended = commands.endRendering();
                if (status && !ended) status = ended;
            }
        }
        if (status && renderShadows) status = commands.resourceBarrier(
            ResourceBarrier::DepthAttachmentWriteToSampledRead);
        if (status) status = commands.resourceBarrier(
            ResourceBarrier::ColorAttachmentWriteToSampledRead);
        if (status) status = commands.resourceBarrier(
            ResourceBarrier::DepthAttachmentWriteToSampledRead);

        RenderingInfo lightingRendering;
        lightingRendering.semanticId = 0x493863335f4c4954ull;
        lightingRendering.width = targets.width;
        lightingRendering.height = targets.height;
        lightingRendering.colors.push_back({
            targets.lightingView, LIGHTING_FORMAT,
            LoadOp::Clear, StoreOp::Store,
            {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
        bool lightingBegun = false;
        if (status)
        {
            status = commands.beginRendering(lightingRendering);
            lightingBegun = status.ok();
        }
        if (status) status = commands.setViewport({
            0.f, 0.f, static_cast<float>(targets.width),
            static_cast<float>(targets.height), 0.f, 1.f});
        if (status) status = commands.setScissor(
            {0, 0, targets.width, targets.height});
        if (status) status = commands.bindPipeline(mLightingPipeline);
        if (status) status = commands.bindBindingSet(0, lightingSet);
        if (status) status = commands.draw({3, 1, 0, 0});
        if (lightingBegun)
        {
            const Status ended = commands.endRendering();
            if (status && !ended) status = ended;
        }
        if (status && !projectors.empty())
        {
            RenderingInfo projectorRendering;
            projectorRendering.semanticId = 0x493863335f50524aull;
            projectorRendering.width = targets.width;
            projectorRendering.height = targets.height;
            projectorRendering.colors.push_back({
                targets.lightingView, LIGHTING_FORMAT,
                LoadOp::Load, StoreOp::Store,
                {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
            bool begun = false;
            status = commands.beginRendering(projectorRendering);
            begun = status.ok();
            if (status) status = commands.setViewport({
                0.f, 0.f, static_cast<float>(targets.width),
                static_cast<float>(targets.height), 0.f, 1.f});
            if (status) status = commands.bindPipeline(mProjectorPipeline);
            for (const Projector& projector : projectors)
            {
                if (status) status = commands.setScissor(projector.scissor);
                if (status) status = commands.bindBindingSet(0, projector.set);
                if (status) status = commands.draw({3, 1, 0, 0});
            }
            if (begun)
            {
                const Status ended = commands.endRendering();
                if (status && !ended) status = ended;
            }
        }
        if (status)
        {
            BufferImageCopyRegion copyRegion;
            copyRegion.imageSubresource = {ImageAspect::Color, 0, 0, 1};
            copyRegion.imageExtent = {targets.width, targets.height, 1};
            const std::array<BufferImageCopyRegion, 1> copies{{copyRegion}};
            status = commands.copyImageToBuffer(
                targets.lightingImage, mLightingReadback, copies);
        }
        for (std::size_t shadow = 0;
             status && shadow < shadowPasses.size(); ++shadow)
        {
            if (!shadowPasses[shadow].active) continue;
            BufferImageCopyRegion copyRegion;
            copyRegion.imageSubresource = {ImageAspect::Depth, 0, 0, 1};
            copyRegion.imageExtent = {targets.width, targets.height, 1};
            const std::array<BufferImageCopyRegion, 1> copies{{copyRegion}};
            status = commands.copyImageToBuffer(
                targets.shadowImages[shadow], mShadowReadbacks[shadow], copies);
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
        mPendingResult.directionalLights = directionalLights;
        mPendingResult.pointLights = pointLights;
        mPendingResult.projectorLights =
            static_cast<std::uint32_t>(projectors.size());
        mPendingResult.projectorTextures =
            static_cast<std::uint32_t>(projectorImages.size());
        mPendingResult.projectorFullscreenLights =
            static_cast<std::uint32_t>(std::count_if(
                projectors.begin(), projectors.end(),
                [](const Projector& projector) { return projector.fullscreen; }));
        mPendingResult.projectorVolumeLights =
            mPendingResult.projectorLights -
            mPendingResult.projectorFullscreenLights;
        mPendingResult.shadowCasterDraws =
            static_cast<std::uint32_t>(casters.size());
        mPendingResult.deferredShadowDraws = deferredCasters;
        for (const Caster& caster : casters)
        {
            const MaterialSceneDraw& draw =
                frame.materials.draws[caster.source];
            mPendingResult.shadowRiggedDraws += draw.skin != NO_RESOURCE;
            mPendingResult.shadowMaskedDraws += caster.masked;
        }
        for (std::size_t shadow = 0; shadow < shadowPasses.size(); ++shadow)
        {
            mPendingResult.shadowActive[shadow] = shadowPasses[shadow].active;
            if (!shadowPasses[shadow].active) continue;
            ++mPendingResult.shadowMaps;
            if (shadow < LIGHTING_DIRECTIONAL_SHADOW_CASCADES)
                ++mPendingResult.directionalShadowMaps;
            else
                ++mPendingResult.projectorShadowMaps;
        }
        mPendingResult.uploadBytes = uploadBytes;
        mPendingResult.frameSha256 = productionFramePacketSha256(frame);
        mPending = true;
        return Status::success();
    }

    Status poll(ProductionLightingResult& result)
    {
        result = {};
        if (!mPending)
            return Status::failure(StatusCode::InvalidState,
                "production lighting has no pending execution");
        Status status = mDevice.readBuffer(
            mLightingReadback, 0, mLightingPixels);
        if (!status) return status;
        mPendingResult.lightingSha256 = sha256(mLightingPixels);
        for (std::size_t pixel = 0;
             pixel < static_cast<std::size_t>(mWidth) * mHeight; ++pixel)
        {
            const auto begin = mLightingPixels.begin() +
                static_cast<std::ptrdiff_t>(pixel * LIGHTING_BYTES);
            if (std::any_of(begin, begin + LIGHTING_BYTES,
                    [](std::byte value) { return value != std::byte{}; }))
                ++mPendingResult.litNonClearPixels;
        }
        for (std::size_t shadow = 0;
             shadow < PRODUCTION_SHADOW_TARGETS; ++shadow)
        {
            if (!mPendingResult.shadowActive[shadow]) continue;
            status = mDevice.readBuffer(
                mShadowReadbacks[shadow], 0, mShadowPixels[shadow]);
            if (!status) return status;
            mPendingResult.shadowDepthSha256[shadow] =
                sha256(mShadowPixels[shadow]);
            for (std::size_t pixel = 0;
                 pixel < static_cast<std::size_t>(mWidth) * mHeight; ++pixel)
            {
                float depth = 1.f;
                std::memcpy(&depth, mShadowPixels[shadow].data() +
                    static_cast<std::ptrdiff_t>(pixel * SHADOW_BYTES),
                    sizeof(depth));
                if (std::isfinite(depth) && depth < .999999f)
                    ++mPendingResult.shadowNonClearPixels[shadow];
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
        destroy(mShadowDoubleSidedPipeline, first);
        destroy(mShadowCulledPipeline, first);
        destroy(mProjectorPipeline, first);
        destroy(mLightingPipeline, first);
        destroy(mShadowShader, first);
        destroy(mProjectorShader, first);
        destroy(mLightingShader, first);
        destroy(mRepeatSampler, first);
        destroy(mClampSampler, first);
        destroy(mProjectorSampler, first);
        destroy(mShadowSampler, first);
        destroy(mWhiteView, first);
        destroy(mWhiteImage, first);
        destroy(mShadowFallbackView, first);
        destroy(mShadowFallbackImage, first);
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
        destroy(mLightingReadback, first);
        for (auto& readback : mShadowReadbacks) destroy(readback, first);
        mLightingPixels.clear();
        for (auto& pixels : mShadowPixels) pixels.clear();
        mReadbackGeneration = 0;
        mWidth = mHeight = 0;
    }

    Status ensureReadbacks(const ProductionFrameTargetSet& targets)
    {
        if (mReadbackGeneration == targets.generation &&
            mWidth == targets.width && mHeight == targets.height)
            return Status::success();
        BufferHandle lighting;
        std::array<BufferHandle, PRODUCTION_SHADOW_TARGETS> shadows{};
        Status status = Status::success();
        lighting = mDevice.createBuffer({
            static_cast<std::uint64_t>(targets.width) * targets.height *
                LIGHTING_BYTES,
            ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
        for (std::size_t shadow = 0;
             status && shadow < shadows.size(); ++shadow)
            shadows[shadow] = mDevice.createBuffer({
                static_cast<std::uint64_t>(targets.width) * targets.height *
                    SHADOW_BYTES,
                ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
        if (!status)
        {
            Status ignored = Status::success();
            destroy(lighting, ignored);
            for (auto& shadow : shadows) destroy(shadow, ignored);
            return status;
        }
        Status retired = Status::success();
        destroyReadbacks(retired);
        if (!retired)
        {
            destroy(lighting, retired);
            for (auto& shadow : shadows) destroy(shadow, retired);
            return retired;
        }
        mLightingReadback = lighting;
        mShadowReadbacks = shadows;
        mReadbackGeneration = targets.generation;
        mWidth = targets.width;
        mHeight = targets.height;
        mLightingPixels.resize(
            static_cast<std::size_t>(mWidth) * mHeight * LIGHTING_BYTES);
        for (auto& pixels : mShadowPixels)
            pixels.resize(
                static_cast<std::size_t>(mWidth) * mHeight * SHADOW_BYTES);
        return Status::success();
    }

    Status initialize()
    {
        if (mLightingPipeline) return Status::success();
        if (mShutdown)
            return Status::failure(StatusCode::InvalidState,
                "production lighting executor is shut down");
        const RendererCapabilities& capabilities = mDevice.capabilities();
        if (capabilities.maxSampledImagesPerStage < 10 ||
            capabilities.maxTexture2DSize < 1 ||
            capabilities.preferredDepthStencilFormat == Format::Undefined)
            return unsupported("device lacks production lighting capabilities");
        Status status = Status::success();
        mLightingShader = mDevice.createShaderPackage(mLightingPackage, status);
        if (status) mProjectorShader =
            mDevice.createShaderPackage(mProjectorPackage, status);
        if (status) mShadowShader =
            mDevice.createShaderPackage(mShadowPackage, status);

        SamplerDesc repeat;
        repeat.minFilter = repeat.magFilter = repeat.mipFilter = Filter::Linear;
        repeat.addressU = repeat.addressV = AddressMode::Repeat;
        if (status) mRepeatSampler = mDevice.createSampler(repeat, status);
        SamplerDesc clamp = repeat;
        clamp.minFilter = clamp.magFilter = clamp.mipFilter = Filter::Nearest;
        clamp.addressU = clamp.addressV = clamp.addressW = AddressMode::ClampToEdge;
        if (status) mClampSampler = mDevice.createSampler(clamp, status);
        if (status) mShadowSampler = mDevice.createSampler(clamp, status);
        SamplerDesc projector = clamp;
        projector.minFilter = projector.magFilter =
            projector.mipFilter = Filter::Linear;
        if (status) mProjectorSampler =
            mDevice.createSampler(projector, status);

        BufferHandle upload;
        if (status) upload = mDevice.createBuffer({
            4, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
        const std::array<std::byte, 4> white{{
            std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}}};
        if (status) status = mDevice.writeBuffer(upload, 0, white);
        if (status) mWhiteImage = mDevice.createImage({
            {1, 1, 1}, Format::RGBA8SRGB,
            ResourceUsage::Sampled | ResourceUsage::TransferDestination,
            1, 1, 1}, status);
        if (status) mWhiteView = mDevice.createImageView({
            mWhiteImage, Format::RGBA8SRGB,
            {ImageAspect::Color, 0, 1, 0, 1}}, status);
        if (status) mShadowFallbackImage = mDevice.createImage({
            {1, 1, 1}, SHADOW_FORMAT,
            ResourceUsage::DepthStencilAttachment | ResourceUsage::Sampled,
            1, 1, 1}, status);
        if (status) mShadowFallbackView = mDevice.createImageView({
            mShadowFallbackImage, SHADOW_FORMAT,
            {ImageAspect::Depth, 0, 1, 0, 1}}, status);
        if (status)
        {
            CommandContext& commands = mDevice.commandContext();
            bool frameBegun = false;
            status = commands.beginFrame();
            frameBegun = status.ok();
            if (status)
            {
                BufferImageCopyRegion copy;
                copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                copy.imageExtent = {1, 1, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{copy}};
                status = commands.copyBufferToImage(upload, mWhiteImage, copies);
            }
            RenderingInfo clear;
            clear.semanticId = 0x493863335f46424bull;
            clear.width = clear.height = 1;
            clear.depthStencil = AttachmentDesc{
                mShadowFallbackView, SHADOW_FORMAT,
                LoadOp::Clear, StoreOp::Store,
                {{0.f, 0.f, 0.f, 0.f}, 1.f, 0}};
            bool begun = false;
            if (status)
            {
                status = commands.beginRendering(clear);
                begun = status.ok();
            }
            if (begun)
            {
                const Status ended = commands.endRendering();
                if (status && !ended) status = ended;
            }
            if (status) status = commands.resourceBarrier(
                ResourceBarrier::DepthAttachmentWriteToSampledRead);
            if (frameBegun)
            {
                const Status ended = commands.endFrame();
                if (status && !ended) status = ended;
            }
        }
        Status uploadRetired = Status::success();
        destroy(upload, uploadRetired);
        if (status && !uploadRetired) status = uploadRetired;

        if (status)
        {
            PipelineDesc lighting;
            lighting.shader = mLightingShader;
            lighting.cullMode = CullMode::None;
            lighting.depthTest = false;
            lighting.depthWrite = false;
            lighting.colorFormats = {LIGHTING_FORMAT};
            lighting.blendStates = {BlendState{}};
            mLightingPipeline = mDevice.createPipeline(lighting, status);
            if (status)
            {
                lighting.shader = mProjectorShader;
                BlendState additive;
                additive.enabled = true;
                additive.sourceColor = BlendFactor::One;
                additive.destinationColor = BlendFactor::One;
                additive.sourceAlpha = BlendFactor::One;
                additive.destinationAlpha = BlendFactor::One;
                lighting.blendStates = {additive};
                mProjectorPipeline = mDevice.createPipeline(lighting, status);
            }
            PipelineDesc shadow;
            shadow.shader = mShadowShader;
            shadow.cullMode = CullMode::Back;
            shadow.depthTest = true;
            shadow.depthWrite = true;
            shadow.depthCompare = CompareOp::Less;
            shadow.depthStencilFormat = SHADOW_FORMAT;
            shadow.vertexBuffers = {{
                0, sizeof(MaterialSceneVertex), VertexInputRate::PerVertex}};
            shadow.vertexAttributes = {
                {0, 0, VertexFormat::Float32x3,
                 offsetof(MaterialSceneVertex, position)},
                {3, 0, VertexFormat::Float32x2,
                 offsetof(MaterialSceneVertex, texCoord)},
                {4, 0, VertexFormat::UNorm8x4,
                 offsetof(MaterialSceneVertex, color)},
                {5, 0, VertexFormat::UInt16x4,
                 offsetof(MaterialSceneVertex, joints)},
                {6, 0, VertexFormat::Float32x4,
                 offsetof(MaterialSceneVertex, weights)}};
            if (status) mShadowCulledPipeline =
                mDevice.createPipeline(shadow, status);
            if (status)
            {
                shadow.cullMode = CullMode::None;
                mShadowDoubleSidedPipeline =
                    mDevice.createPipeline(shadow, status);
            }
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
    ShaderPackageDesc mLightingPackage;
    ShaderPackageDesc mProjectorPackage;
    ShaderPackageDesc mShadowPackage;
    ShaderPackageHandle mLightingShader;
    ShaderPackageHandle mProjectorShader;
    ShaderPackageHandle mShadowShader;
    PipelineHandle mLightingPipeline;
    PipelineHandle mProjectorPipeline;
    PipelineHandle mShadowCulledPipeline;
    PipelineHandle mShadowDoubleSidedPipeline;
    SamplerHandle mRepeatSampler;
    SamplerHandle mClampSampler;
    SamplerHandle mProjectorSampler;
    SamplerHandle mShadowSampler;
    ImageHandle mWhiteImage;
    ImageViewHandle mWhiteView;
    ImageHandle mShadowFallbackImage;
    ImageViewHandle mShadowFallbackView;
    BufferHandle mLightingReadback;
    std::array<BufferHandle, PRODUCTION_SHADOW_TARGETS> mShadowReadbacks{};
    std::vector<std::byte> mLightingPixels;
    std::array<std::vector<std::byte>, PRODUCTION_SHADOW_TARGETS> mShadowPixels;
    std::uint64_t mReadbackGeneration = 0;
    std::uint32_t mWidth = 0;
    std::uint32_t mHeight = 0;
    ProductionLightingResult mPendingResult;
    bool mPending = false;
    bool mShutdown = false;
};

ProductionLightingExecutor::ProductionLightingExecutor(
    Device& device, ShaderPackageDesc lightingShader,
    ShaderPackageDesc projectorShader, ShaderPackageDesc shadowShader) :
    mImpl(std::make_unique<Impl>(device, std::move(lightingShader),
                                 std::move(projectorShader),
                                 std::move(shadowShader)))
{
}

ProductionLightingExecutor::~ProductionLightingExecutor() = default;

Status ProductionLightingExecutor::submit(
    const ProductionFramePacket& frame,
    const ProductionFrameTargetSet& targets,
    const ProductionTextureResidency& residency,
    const ProductionLightingLimits& limits)
{
    return mImpl->submit(frame, targets, residency, limits);
}

Status ProductionLightingExecutor::poll(ProductionLightingResult& result)
{
    return mImpl->poll(result);
}

bool ProductionLightingExecutor::pending() const
{
    return mImpl->pending();
}

Status ProductionLightingExecutor::shutdown()
{
    return mImpl->shutdown();
}

} // namespace LL::GHI
