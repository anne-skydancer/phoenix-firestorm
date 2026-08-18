/**
 * @file llghimaterialoffscreenprobe.cpp
 * @brief Asynchronous, non-presenting replay of live rigid and rigged opaque PBR draws.
 */

#include "linden_common.h"

#include "ghi/include/llghimaterialoffscreenprobe.h"

#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
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
constexpr std::array<std::uint32_t, 4> COLOR_BYTES{{4, 4, 8, 8}};
constexpr Format LIGHTING_FORMAT = Format::RGBA16Float;
constexpr std::uint32_t LIGHTING_BYTES = 8;
constexpr std::size_t SHADOW_MAP_COUNT =
    LIGHTING_DIRECTIONAL_SHADOW_CASCADES + LIGHTING_PROJECTOR_SHADOWS;
constexpr Format SHADOW_FORMAT = Format::Depth32Float;
constexpr std::uint32_t SHADOW_BYTES = 4;
constexpr std::size_t MAX_LIGHTING_POINT_LIGHTS = 64;
constexpr std::size_t LIGHTING_HEADER_FLOATS = 16 * 2 + 4 * 6;
constexpr std::size_t LIGHTING_POINT_DATA_FLOATS =
    LIGHTING_HEADER_FLOATS + MAX_LIGHTING_POINT_LIGHTS * 8;
constexpr std::size_t LIGHTING_DATA_FLOATS = LIGHTING_POINT_DATA_FLOATS +
    LIGHTING_DIRECTIONAL_SHADOW_CASCADES * 16 + 8;
using LightingData = std::array<float, LIGHTING_DATA_FLOATS>;
constexpr std::size_t PROJECTOR_DATA_FLOATS = 76;
using ProjectorData = std::array<float, PROJECTOR_DATA_FLOATS>;
using SkinData = std::array<float, MATERIAL_SKIN_FLOATS>;
using ShadowMaterialData = std::array<float, 12>;

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

LightingData makeLightingData(const LightingScenePacket& packet,
                              std::uint32_t max_point_lights,
                              bool native_shadows,
                              std::uint32_t& directional_lights,
                              std::uint32_t& point_lights)
{
    LightingData data{};
    std::copy(packet.viewMatrix.begin(), packet.viewMatrix.end(), data.begin());
    std::copy(packet.projectionMatrix.begin(), packet.projectionMatrix.end(),
              data.begin() + 16);
    std::copy(packet.cameraOrigin.begin(), packet.cameraOrigin.end(),
              data.begin() + 32);
    std::copy(packet.ambientColor.begin(), packet.ambientColor.end(),
              data.begin() + 36);

    auto copyDirectional = [&data, &directional_lights](
        const DirectionalLightRecord& light, std::size_t offset)
    {
        std::copy(light.direction.begin(), light.direction.end(),
                  data.begin() + static_cast<std::ptrdiff_t>(offset));
        data[offset + 3] = light.active ? 1.f : 0.f;
        std::copy(light.color.begin(), light.color.end(),
                  data.begin() + static_cast<std::ptrdiff_t>(offset + 4));
        data[offset + 7] = light.intensity;
        if (light.active) ++directional_lights;
    };
    copyDirectional(packet.sun, 40);
    copyDirectional(packet.moon, 48);

    const std::uint32_t limit = std::min<std::uint32_t>(
        max_point_lights, static_cast<std::uint32_t>(MAX_LIGHTING_POINT_LIGHTS));
    for (const LocalLightRecord& light : packet.localLights)
    {
        if (light.kind != LocalLightKind::Point || point_lights == limit) continue;
        const std::size_t position = LIGHTING_HEADER_FLOATS + point_lights * 4;
        const std::size_t color = LIGHTING_HEADER_FLOATS +
                                  MAX_LIGHTING_POINT_LIGHTS * 4 +
                                  point_lights * 4;
        std::copy(light.position.begin(), light.position.end(),
                  data.begin() + static_cast<std::ptrdiff_t>(position));
        data[position + 3] = light.radius;
        std::copy(light.color.begin(), light.color.end(),
                  data.begin() + static_cast<std::ptrdiff_t>(color));
        data[color + 3] = light.falloff;
        ++point_lights;
    }
    data[35] = static_cast<float>(point_lights);
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
    data[shadowClip + 5] = native_shadows && packet.shadows.enabled &&
        packet.shadows.directionalCascadeCount ? 1.f : 0.f;
    data[shadowClip + 6] = static_cast<float>(
        std::min<std::uint32_t>(packet.shadows.directionalCascadeCount,
            LIGHTING_DIRECTIONAL_SHADOW_CASCADES));
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
    // Lighting matrices map main-camera view space to [0,1] shadow texture
    // coordinates. Remove that scale/bias and restore the main view transform
    // to obtain the backend-neutral world-to-shadow clip matrix used to
    // produce the native depth map.
    constexpr std::array<float, 16> textureToClip{{
        2.f, 0.f, 0.f, 0.f, 0.f, 2.f, 0.f, 0.f,
        0.f, 0.f, 2.f, 0.f, -1.f, -1.f, -1.f, 1.f}};
    return multiplyMatrix(textureToClip,
        multiplyMatrix(packet.shadows.matrices[shadow], packet.viewMatrix));
}

bool supportedTextureCoordinates(const MaterialResource& material)
{
    for (const auto& binding : material.textures)
        if (binding.texcoord != 0) return false;
    return true;
}

bool hasTextureTransform(const MaterialResource& material)
{
    constexpr std::array<float, 5> identity{{0.f, 0.f, 1.f, 1.f, 0.f}};
    return std::any_of(material.textures.begin(), material.textures.end(),
        [&identity](const MaterialTextureBinding& binding)
        {
            return binding.transform != identity;
        });
}

std::array<double, 4> transformPoint(const std::array<float, 16>& matrix,
                                     const std::array<double, 4>& point)
{
    std::array<double, 4> result{};
    for (std::size_t row = 0; row < result.size(); ++row)
        result[row] = matrix[row] * point[0] + matrix[4 + row] * point[1] +
                      matrix[8 + row] * point[2] + matrix[12 + row] * point[3];
    return result;
}

bool validSkin(const SkinResource& skin)
{
    return skin.comparability == ResourceComparability::Comparable &&
           skin.jointCount > 0 && skin.jointCount <= MATERIAL_MAX_JOINTS &&
           skin.matrixPalette.size() ==
               static_cast<std::size_t>(skin.jointCount) * 12;
}

std::array<double, 4> skinPoint(const MaterialSceneVertex& vertex,
                                const SkinResource* skin)
{
    if (!skin)
        return {{vertex.position[0], vertex.position[1], vertex.position[2], 1.0}};
    double weightSum = 0.0;
    for (float weight : vertex.weights)
        weightSum += std::max(0.0, static_cast<double>(weight));
    if (!std::isfinite(weightSum) || weightSum <= 1.e-6)
        return {{0.0, 0.0, 0.0, 1.0}};
    std::array<double, 4> result{{0.0, 0.0, 0.0, 1.0}};
    for (std::size_t influence = 0; influence < vertex.weights.size(); ++influence)
    {
        const double weight = std::max(0.0,
            static_cast<double>(vertex.weights[influence])) / weightSum;
        const std::uint32_t joint = std::min<std::uint32_t>(
            vertex.joints[influence], skin->jointCount - 1);
        const float* matrix = skin->matrixPalette.data() + joint * 12;
        result[0] += weight * (matrix[0] * vertex.position[0] +
                               matrix[4] * vertex.position[1] +
                               matrix[8] * vertex.position[2] + matrix[3]);
        result[1] += weight * (matrix[1] * vertex.position[0] +
                               matrix[5] * vertex.position[1] +
                               matrix[9] * vertex.position[2] + matrix[7]);
        result[2] += weight * (matrix[2] * vertex.position[0] +
                               matrix[6] * vertex.position[1] +
                               matrix[10] * vertex.position[2] + matrix[11]);
    }
    return result;
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
        std::copy(skin->matrixPalette.begin(), skin->matrixPalette.end(), data.begin());
    const std::array<std::uint32_t, 4> meta{{jointCount, 0, 0, 0}};
    std::memcpy(data.data() + MATERIAL_MAX_JOINTS * 12,
                meta.data(), sizeof(meta));
    return data;
}

bool potentiallyVisible(const MaterialScenePacket& packet,
                        const MaterialSceneDraw& draw,
                        const SkinResource* skin)
{
    std::array<bool, 6> allOutside{{true, true, true, true, true, true}};
    for (std::uint32_t item = 0; item < draw.indexCount; ++item)
    {
        const MaterialSceneVertex& vertex =
            packet.vertices[packet.indices[draw.firstIndex + item]];
        const std::array<double, 4> local = skinPoint(vertex, skin);
        const auto world = transformPoint(draw.modelTransform, local);
        const auto clip = transformPoint(draw.transform, world);
        if (!std::all_of(clip.begin(), clip.end(),
                         [](double value) { return std::isfinite(value); }))
            continue;
        const double w = clip[3];
        const std::array<bool, 6> inside{{
            clip[0] >= -w, clip[0] <= w, clip[1] >= -w,
            clip[1] <= w, clip[2] >= -w, clip[2] <= w}};
        for (std::size_t plane = 0; plane < allOutside.size(); ++plane)
            if (inside[plane]) allOutside[plane] = false;
    }
    return std::none_of(allOutside.begin(), allOutside.end(),
                        [](bool outside) { return outside; });
}

bool makeObjectData(const MaterialSceneDraw& draw, std::array<float, 32>& data)
{
    std::copy(draw.modelTransform.begin(), draw.modelTransform.end(), data.begin());
    const auto& model = draw.modelTransform;
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
    const double inverseDeterminant = 1.0 / determinant;
    const std::array<double, 9> normal{{
        c00 * inverseDeterminant, c10 * inverseDeterminant,
        c20 * inverseDeterminant, c01 * inverseDeterminant,
        c11 * inverseDeterminant, c21 * inverseDeterminant,
        c02 * inverseDeterminant, c12 * inverseDeterminant,
        c22 * inverseDeterminant}};
    constexpr std::array<float, 16> identity{{
        1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f}};
    std::copy(identity.begin(), identity.end(), data.begin() + 16);
    constexpr std::array<std::size_t, 9> indices{{0, 1, 2, 4, 5, 6, 8, 9, 10}};
    for (std::size_t index = 0; index < normal.size(); ++index)
    {
        if (!std::isfinite(normal[index])) return false;
        data[16 + indices[index]] = static_cast<float>(normal[index]);
    }
    return true;
}

bool hasComparability(ResourceComparability value, ResourceComparability flag)
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

ShadowMaterialData makeShadowMaterialData(const MaterialResource& material)
{
    ShadowMaterialData data{};
    data[0] = material.alphaCutoff;
    data[1] = material.baseColor[3];
    data[2] = material.alphaMode == MaterialAlphaMode::Mask ? 1.f : 0.f;
    data[6] = data[7] = 1.f;
    data[8] = 1.f;
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

std::vector<std::byte> rgbaPixels(const MaterialScenePacket& packet,
                                  const MaterialResource& material,
                                  TextureSemantic semantic,
                                  const std::array<std::uint8_t, 4>& fallback,
                                  std::uint32_t& width, std::uint32_t& height,
                                  const MaterialOffscreenProbeLimits& limits,
                                  Status& status)
{
    const MaterialTextureBinding* binding = findBinding(material, semantic);
    if (!binding)
    {
        width = height = 1;
        return {static_cast<std::byte>(fallback[0]),
                static_cast<std::byte>(fallback[1]),
                static_cast<std::byte>(fallback[2]),
                static_cast<std::byte>(fallback[3])};
    }
    if (binding->texture >= packet.textures.size())
    {
        status = invalid("material binding references an absent texture");
        return {};
    }
    const MaterialTextureResource& texture = packet.textures[binding->texture];
    if (texture.comparability != ResourceComparability::Comparable ||
        texture.decodedPixels.empty() || !texture.width || !texture.height ||
        texture.components < 1 || texture.components > 4)
    {
        status = invalid("material texture has no comparable decoded pixels");
        return {};
    }
    const std::uint64_t rgbaBytes = static_cast<std::uint64_t>(texture.width) *
                                    texture.height * 4;
    if (rgbaBytes > limits.maxTextureBytes ||
        rgbaBytes > std::numeric_limits<std::size_t>::max())
    {
        status = invalid("material texture exceeds the per-image byte limit");
        return {};
    }
    width = texture.width;
    height = texture.height;
    std::vector<std::byte> pixels(static_cast<std::size_t>(rgbaBytes));
    for (std::uint64_t pixel = 0;
         pixel < static_cast<std::uint64_t>(width) * height; ++pixel)
    {
        const std::size_t source = static_cast<std::size_t>(pixel) * texture.components;
        const std::size_t target = static_cast<std::size_t>(pixel) * 4;
        const std::byte luminance = texture.decodedPixels[source];
        pixels[target] = texture.components == 1 || texture.components == 2
            ? luminance : texture.decodedPixels[source];
        pixels[target + 1] = texture.components == 1 || texture.components == 2
            ? luminance : texture.decodedPixels[source + 1];
        pixels[target + 2] = texture.components == 1 || texture.components == 2
            ? luminance : texture.decodedPixels[source + 2];
        pixels[target + 3] = texture.components == 2
            ? texture.decodedPixels[source + 1]
            : texture.components == 4 ? texture.decodedPixels[source + 3]
                                      : std::byte{255};
    }
    return pixels;
}

std::vector<std::byte> projectorRgbaPixels(
    const ProjectorTextureResource& texture,
    const MaterialOffscreenProbeLimits& limits, Status& status)
{
    const std::uint64_t rgbaBytes = static_cast<std::uint64_t>(texture.width) *
                                    texture.height * 4;
    if (!texture.width || !texture.height || texture.components < 1 ||
        texture.components > 4 || texture.decodedPixels.empty() ||
        rgbaBytes > limits.maxProjectorTextureBytes ||
        rgbaBytes > std::numeric_limits<std::size_t>::max())
    {
        status = invalid("projector texture exceeds the executable image limit");
        return {};
    }
    std::vector<std::byte> pixels(static_cast<std::size_t>(rgbaBytes));
    for (std::uint64_t pixel = 0;
         pixel < static_cast<std::uint64_t>(texture.width) * texture.height;
         ++pixel)
    {
        const std::size_t source = static_cast<std::size_t>(pixel) *
                                   texture.components;
        const std::size_t target = static_cast<std::size_t>(pixel) * 4;
        const std::byte luminance = texture.decodedPixels[source];
        pixels[target] = texture.components < 3
            ? luminance : texture.decodedPixels[source];
        pixels[target + 1] = texture.components < 3
            ? luminance : texture.decodedPixels[source + 1];
        pixels[target + 2] = texture.components < 3
            ? luminance : texture.decodedPixels[source + 2];
        pixels[target + 3] = texture.components == 2
            ? texture.decodedPixels[source + 1]
            : texture.components == 4 ? texture.decodedPixels[source + 3]
                                      : std::byte{255};
    }
    return pixels;
}

std::uint32_t mipLevels(std::uint32_t width, std::uint32_t height)
{
    std::uint32_t levels = 1;
    for (std::uint32_t size = std::max(width, height); size > 1; size >>= 1)
        ++levels;
    return levels;
}

ProjectorData makeProjectorData(const LightingScenePacket& packet,
                                const LocalLightRecord& light,
                                std::uint32_t levels,
                                bool native_shadows)
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
    const bool shadowed = native_shadows && packet.shadows.enabled &&
        light.shadowSlot >= 0 &&
        light.shadowSlot < static_cast<std::int32_t>(
            LIGHTING_PROJECTOR_SHADOWS);
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

ScissorRect projectorScissor(const LightingScenePacket& packet,
                             const LocalLightRecord& light,
                             bool& fullscreen)
{
    const double dx = packet.cameraOrigin[0] - light.position[0];
    const double dy = packet.cameraOrigin[1] - light.position[1];
    const double dz = packet.cameraOrigin[2] - light.position[2];
    fullscreen = dx * dx + dy * dy + dz * dz <
                 static_cast<double>(light.radius) * light.radius;
    if (fullscreen) return {0, 0, PROBE_WIDTH, PROBE_HEIGHT};

    double minX = 1.0, minY = 1.0, maxX = -1.0, maxY = -1.0;
    for (int z = -1; z <= 1; z += 2)
        for (int y = -1; y <= 1; y += 2)
            for (int x = -1; x <= 1; x += 2)
            {
                const std::array<double, 4> world{{
                    light.position[0] + x * light.radius,
                    light.position[1] + y * light.radius,
                    light.position[2] + z * light.radius, 1.0}};
                const auto view = transformPoint(packet.viewMatrix, world);
                const auto clip = transformPoint(packet.projectionMatrix, view);
                if (!std::isfinite(clip[3]) || clip[3] <= 1.e-6)
                {
                    fullscreen = true;
                    return {0, 0, PROBE_WIDTH, PROBE_HEIGHT};
                }
                minX = std::min(minX, clip[0] / clip[3]);
                maxX = std::max(maxX, clip[0] / clip[3]);
                minY = std::min(minY, clip[1] / clip[3]);
                maxY = std::max(maxY, clip[1] / clip[3]);
            }
    minX = std::clamp(minX, -1.0, 1.0);
    maxX = std::clamp(maxX, -1.0, 1.0);
    minY = std::clamp(minY, -1.0, 1.0);
    maxY = std::clamp(maxY, -1.0, 1.0);
    if (maxX <= minX || maxY <= minY) return {};
    const auto left = static_cast<std::int32_t>(std::floor(
        (minX * .5 + .5) * PROBE_WIDTH));
    const auto right = static_cast<std::int32_t>(std::ceil(
        (maxX * .5 + .5) * PROBE_WIDTH));
    const auto top = static_cast<std::int32_t>(std::floor(
        (.5 - maxY * .5) * PROBE_HEIGHT));
    const auto bottom = static_cast<std::int32_t>(std::ceil(
        (.5 - minY * .5) * PROBE_HEIGHT));
    return {left, top, static_cast<std::uint32_t>(std::max(0, right - left)),
            static_cast<std::uint32_t>(std::max(0, bottom - top))};
}
} // namespace

class MaterialOffscreenProbe::Impl
{
public:
    Impl(Device& device, ShaderPackageDesc package,
         std::optional<ShaderPackageDesc> lighting_package = std::nullopt,
         std::optional<ShaderPackageDesc> projector_package = std::nullopt,
         std::optional<ShaderPackageDesc> shadow_package = std::nullopt) :
        mDevice(device), mShaderPackage(std::move(package)),
        mLightingShaderPackage(std::move(lighting_package)),
        mProjectorShaderPackage(std::move(projector_package)),
        mShadowShaderPackage(std::move(shadow_package)) {}
    ~Impl() { shutdown(); }

    Status submit(const MaterialScenePacket& packet,
                  const MaterialOffscreenProbeLimits& limits)
    {
        return submitImpl(packet, nullptr, limits);
    }

    Status submit(const MaterialScenePacket& material_packet,
                  const LightingScenePacket& lighting_packet,
                  const MaterialOffscreenProbeLimits& limits)
    {
        return submitImpl(material_packet, &lighting_packet, limits);
    }

private:
    Status submitImpl(const MaterialScenePacket& packet,
                      const LightingScenePacket* lighting_packet,
                      const MaterialOffscreenProbeLimits& limits)
    {
        if (mPending)
            return Status::failure(StatusCode::NotReady,
                                   "material offscreen probe is still pending");
        if (!limits.maxDraws || !limits.maxVertices || !limits.maxIndices ||
            !limits.maxTextures || !limits.maxUploadBytes ||
            !limits.maxTextureBytes || !limits.maxPointLights ||
            !limits.maxProjectorLights || !limits.maxProjectorTextureBytes ||
            !limits.maxShadowDraws)
            return invalid("material offscreen limits must be nonzero");
        if (lighting_packet)
        {
            if (!mLightingShaderPackage)
                return Status::failure(StatusCode::InvalidState,
                    "material probe was not configured for deferred lighting");
            if (lighting_packet->frameId != packet.frameId)
                return invalid("material and lighting packets are not from the same frame");
            if (lighting_packet->sourceWidth != packet.sourceWidth ||
                lighting_packet->sourceHeight != packet.sourceHeight)
                return invalid("material and lighting packet extents do not match");
            std::vector<std::byte> encodedLighting;
            const Status lightingStatus =
                encodeLightingScenePacket(*lighting_packet, encodedLighting);
            if (!lightingStatus) return lightingStatus;
        }
        if (packet.vertices.empty() || packet.indices.empty() ||
            packet.draws.empty())
            return invalid("live material packet contains no drawable geometry");
        if (packet.vertices.size() > limits.maxVertices ||
            packet.indices.size() > limits.maxIndices)
            return invalid("live material packet exceeds geometry limits");

        std::vector<std::size_t> selected;
        std::size_t geometryDraws = 0;
        std::size_t rigidDraws = 0;
        std::size_t riggedDraws = 0;
        std::size_t validSkinDraws = 0;
        std::size_t comparableDraws = 0;
        std::size_t opaquePbrDraws = 0;
        std::size_t uv0Draws = 0;
        std::size_t potentiallyVisibleDraws = 0;
        std::array<std::size_t, 5> comparabilityCauses{};
        struct TextureDiagnostic
        {
            std::size_t bound = 0;
            std::size_t decoded = 0;
            std::size_t missing = 0;
            std::size_t fetching = 0;
            std::uint64_t bytes = 0;
        };
        std::array<TextureDiagnostic, 4> textureDiagnostics{};
        std::size_t materialsWithoutBindings = 0;
        constexpr std::array<TextureSemantic, 4> diagnosticSemantics{{
            TextureSemantic::BaseColor, TextureSemantic::Normal,
            TextureSemantic::MetallicRoughness, TextureSemantic::Emissive}};
        for (std::size_t index = 0; index < packet.draws.size(); ++index)
        {
            const MaterialSceneDraw& draw = packet.draws[index];
            if (!draw.indexCount) continue;
            ++geometryDraws;
            if (draw.material >= packet.materials.size()) continue;
            const SkinResource* skin = nullptr;
            if (draw.skin == NO_RESOURCE)
            {
                ++rigidDraws;
            }
            else
            {
                ++riggedDraws;
                if (draw.skin >= packet.skins.size() ||
                    !validSkin(packet.skins[draw.skin]))
                    continue;
                skin = &packet.skins[draw.skin];
                ++validSkinDraws;
            }
            const MaterialResource& material = packet.materials[draw.material];
            if (material.model != MaterialModel::MetallicRoughness ||
                material.alphaMode != MaterialAlphaMode::Opaque)
                continue;
            ++opaquePbrDraws;
            if (material.textures.empty()) ++materialsWithoutBindings;
            for (std::size_t semantic = 0;
                 semantic < diagnosticSemantics.size(); ++semantic)
            {
                const MaterialTextureBinding* binding =
                    findBinding(material, diagnosticSemantics[semantic]);
                if (!binding) continue;
                TextureDiagnostic& diagnostic = textureDiagnostics[semantic];
                ++diagnostic.bound;
                if (binding->texture >= packet.textures.size())
                {
                    ++diagnostic.missing;
                    continue;
                }
                const MaterialTextureResource& texture =
                    packet.textures[binding->texture];
                if (!texture.decodedPixels.empty())
                {
                    ++diagnostic.decoded;
                    diagnostic.bytes += texture.decodedPixels.size();
                }
                if (hasComparability(texture.comparability,
                                     ResourceComparability::MissingCpuTexture))
                    ++diagnostic.missing;
                if (hasComparability(texture.comparability,
                                     ResourceComparability::TextureStillFetching))
                    ++diagnostic.fetching;
            }
            constexpr std::array<ResourceComparability, 5> causes{{
                ResourceComparability::MissingCpuTexture,
                ResourceComparability::TextureStillFetching,
                ResourceComparability::MissingSkinPalette,
                ResourceComparability::AlphaDeferred,
                ResourceComparability::UnsupportedVertexLayout}};
            for (std::size_t cause = 0; cause < causes.size(); ++cause)
                if (hasComparability(draw.comparability, causes[cause]))
                    ++comparabilityCauses[cause];
            if (!supportedTextureCoordinates(material)) continue;
            ++uv0Draws;
            if (draw.comparability != ResourceComparability::Comparable ||
                material.comparability != ResourceComparability::Comparable)
                continue;
            ++comparableDraws;
            if (!potentiallyVisible(packet, draw, skin)) continue;
            ++potentiallyVisibleDraws;
            selected.push_back(index);
        }
        // Capture reserves room for rigged draws and the consumer gives those
        // draws first claim on its smaller executable budget. Rigid draws keep
        // their original order within the remaining capacity.
        std::stable_sort(selected.begin(), selected.end(),
            [&packet](std::size_t lhs, std::size_t rhs)
            {
                return (packet.draws[lhs].skin != NO_RESOURCE) >
                       (packet.draws[rhs].skin != NO_RESOURCE);
            });
        const std::size_t executableLimit = std::min<std::size_t>(
            limits.maxDraws, limits.maxTextures / 4);
        if (selected.size() > executableLimit) selected.resize(executableLimit);

        std::vector<std::size_t> shadowSelected;
        if (mShadowShaderPackage)
        {
            for (std::size_t index = 0; index < packet.draws.size(); ++index)
            {
                const MaterialSceneDraw& draw = packet.draws[index];
                if (!draw.indexCount || draw.material >= packet.materials.size())
                    continue;
                const MaterialResource& material = packet.materials[draw.material];
                if (material.alphaMode == MaterialAlphaMode::Blend) continue;
                const SkinResource* skin = nullptr;
                if (draw.skin != NO_RESOURCE)
                {
                    if (draw.skin >= packet.skins.size() ||
                        !validSkin(packet.skins[draw.skin])) continue;
                    skin = &packet.skins[draw.skin];
                }
                if (hasComparability(draw.comparability,
                        ResourceComparability::UnsupportedVertexLayout) ||
                    hasComparability(draw.comparability,
                        ResourceComparability::MissingSkinPalette) ||
                    !potentiallyVisible(packet, draw, skin)) continue;
                if (material.alphaMode == MaterialAlphaMode::Mask)
                {
                    if (!supportedTextureCoordinates(material)) continue;
                    const MaterialTextureBinding* base = findBinding(
                        material, TextureSemantic::BaseColor);
                    if (!base || base->texture >= packet.textures.size() ||
                        packet.textures[base->texture].comparability !=
                            ResourceComparability::Comparable ||
                        packet.textures[base->texture].decodedPixels.empty())
                        continue;
                }
                shadowSelected.push_back(index);
            }
            std::stable_sort(shadowSelected.begin(), shadowSelected.end(),
                [&packet](std::size_t lhs, std::size_t rhs)
                {
                    const MaterialSceneDraw& a = packet.draws[lhs];
                    const MaterialSceneDraw& b = packet.draws[rhs];
                    const bool aMask = packet.materials[a.material].alphaMode ==
                        MaterialAlphaMode::Mask;
                    const bool bMask = packet.materials[b.material].alphaMode ==
                        MaterialAlphaMode::Mask;
                    if (aMask != bMask) return aMask > bMask;
                    return (a.skin != NO_RESOURCE) > (b.skin != NO_RESOURCE);
                });
            if (shadowSelected.size() > limits.maxShadowDraws)
                shadowSelected.resize(limits.maxShadowDraws);
            if (shadowSelected.empty())
                return invalid("live material packet contains no executable native shadow casters");
        }
        if (selected.empty())
        {
            std::ostringstream message;
            message << "live material packet has no executable rigid or rigged opaque PBR draws"
                    << " (packet/geometry/rigid/rigged/valid-skin/pbr/uv0/comparable/visible="
                    << packet.draws.size() << '/' << geometryDraws << '/'
                    << rigidDraws << '/' << riggedDraws << '/'
                    << validSkinDraws << '/' << opaquePbrDraws << '/'
                    << uv0Draws << '/' << comparableDraws << '/'
                    << potentiallyVisibleDraws
                    << "; causes=missing/fetching/skin/alpha/layout="
                    << comparabilityCauses[0] << '/' << comparabilityCauses[1]
                    << '/' << comparabilityCauses[2] << '/'
                    << comparabilityCauses[3] << '/' << comparabilityCauses[4]
                    << "; textures=base,normal,mr,emissive(bound/decoded/missing/fetching/bytes)=";
            for (std::size_t semantic = 0;
                 semantic < textureDiagnostics.size(); ++semantic)
            {
                if (semantic) message << ',';
                const TextureDiagnostic& diagnostic =
                    textureDiagnostics[semantic];
                message << diagnostic.bound << '/' << diagnostic.decoded << '/'
                        << diagnostic.missing << '/' << diagnostic.fetching << '/'
                        << diagnostic.bytes;
            }
            message << "; no-bindings=" << materialsWithoutBindings << ')';
            return Status::failure(StatusCode::InvalidArgument, message.str());
        }

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
        const std::uint64_t vertexBytes = packet.vertices.size() *
                                          sizeof(MaterialSceneVertex);
        const std::uint64_t indexBytes = packet.indices.size() * sizeof(std::uint32_t);
        const std::uint64_t vertexOffset = 0;
        const std::uint64_t indexOffset = align(vertexBytes);
        const std::uint64_t frameOffset = align(indexOffset + indexBytes);
        const std::uint64_t frameStride = align(sizeof(packet.draws.front().transform));
        const std::uint64_t objectOffset = align(
            frameOffset + frameStride * selected.size());
        constexpr std::size_t OBJECT_FLOATS = 32;
        const std::uint64_t objectStride = align(OBJECT_FLOATS * sizeof(float));
        const std::uint64_t skinOffset = align(
            objectOffset + objectStride * selected.size());
        const std::uint64_t skinStride = align(MATERIAL_SKIN_BYTES);
        const std::uint64_t materialOffset = align(
            skinOffset + skinStride * selected.size());
        constexpr std::size_t MATERIAL_FLOATS = 44;
        const std::uint64_t materialStride = align(MATERIAL_FLOATS * sizeof(float));
        std::uint64_t textureOffset = align(materialOffset + materialStride * selected.size());
        if (textureOffset == std::numeric_limits<std::uint64_t>::max())
            return invalid("material offscreen upload size overflow");

        struct DrawResources
        {
            std::size_t sourceDraw = 0;
            bool textureTransformed = false;
            BindingSetHandle frameSet;
            BindingSetHandle skinSet;
            BindingSetHandle materialSet;
            std::array<ImageHandle, 4> images{};
            std::array<ImageViewHandle, 4> views{};
            std::array<std::uint64_t, 4> textureOffsets{};
            std::array<std::uint32_t, 4> widths{};
            std::array<std::uint32_t, 4> heights{};
            std::array<std::vector<std::byte>, 4> pixels;
        };
        std::vector<DrawResources> draws;
        draws.reserve(selected.size());
        constexpr std::array<TextureSemantic, 4> semantics{{
            TextureSemantic::BaseColor, TextureSemantic::Normal,
            TextureSemantic::MetallicRoughness, TextureSemantic::Emissive}};
        constexpr std::array<std::array<std::uint8_t, 4>, 4> fallbacks{{
            {{255, 255, 255, 255}}, {{128, 128, 255, 255}},
            {{255, 255, 0, 255}}, {{0, 0, 0, 255}}}};
        for (std::size_t item = 0; item < selected.size(); ++item)
        {
            DrawResources candidate;
            candidate.sourceDraw = selected[item];
            const MaterialResource& material =
                packet.materials[packet.draws[selected[item]].material];
            candidate.textureTransformed = hasTextureTransform(material);
            const std::uint64_t drawTextureOffset = textureOffset;
            bool drawFits = true;
            for (std::size_t texture = 0; texture < 4; ++texture)
            {
                candidate.pixels[texture] = rgbaPixels(
                    packet, material, semantics[texture], fallbacks[texture],
                    candidate.widths[texture], candidate.heights[texture],
                    limits, status);
                if (!status) return status;
                candidate.textureOffsets[texture] = textureOffset;
                if (candidate.pixels[texture].size() >
                    std::numeric_limits<std::uint64_t>::max() - textureOffset)
                    return invalid("material texture upload size overflow");
                textureOffset = align(textureOffset +
                                      candidate.pixels[texture].size());
                if (textureOffset > limits.maxUploadBytes)
                {
                    drawFits = false;
                    break;
                }
            }
            if (!drawFits)
            {
                textureOffset = drawTextureOffset;
                if (draws.empty())
                    return invalid("one material draw exceeds the upload byte limit");
                break;
            }
            draws.push_back(std::move(candidate));
        }
        std::uint32_t directionalLights = 0;
        std::uint32_t pointLights = 0;
        LightingData lightingData{};
        struct ProjectorImageResources
        {
            std::array<std::uint8_t, 16> sourceIdentity{};
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t levels = 1;
            std::vector<std::byte> pixels;
            std::uint64_t textureOffset = 0;
            ImageHandle image;
            ImageViewHandle view;
        };
        struct ProjectorResources
        {
            std::size_t sourceLight = 0;
            std::size_t image = 0;
            bool fullscreen = false;
            ScissorRect scissor;
            ProjectorData data{};
            std::uint64_t uniformOffset = 0;
            BufferHandle uniform;
            BindingSetHandle set;
        };
        std::vector<ProjectorImageResources> projectorImages;
        std::vector<ProjectorResources> projectors;
        struct ShadowPassResources
        {
            bool active = false;
            std::array<float, 16> matrix{};
            std::uint64_t uniformOffset = 0;
            BufferHandle uniform;
            BindingSetHandle set;
        };
        struct ShadowDrawResources
        {
            std::size_t sourceDraw = 0;
            bool masked = false;
            ShadowMaterialData materialData{};
            std::array<float, 32> objectData{};
            SkinData skinData{};
            std::uint32_t width = 1;
            std::uint32_t height = 1;
            std::vector<std::byte> pixels;
            std::uint64_t objectOffset = 0;
            std::uint64_t skinOffset = 0;
            std::uint64_t materialOffset = 0;
            std::uint64_t textureOffset = 0;
            BufferHandle object;
            BufferHandle skin;
            BufferHandle material;
            ImageHandle image;
            ImageViewHandle view;
            BindingSetHandle objectSet;
            BindingSetHandle materialSet;
        };
        std::array<ShadowPassResources, SHADOW_MAP_COUNT> shadowPasses{};
        std::vector<ShadowDrawResources> shadowDraws;
        std::uint64_t projectorTextureBytes = 0;
        if (lighting_packet && mProjectorShaderPackage)
        {
            for (std::size_t lightIndex = 0;
                 lightIndex < lighting_packet->localLights.size() &&
                 projectors.size() < limits.maxProjectorLights;
                 ++lightIndex)
            {
                const LocalLightRecord& light =
                    lighting_packet->localLights[lightIndex];
                if (light.kind != LocalLightKind::Projector ||
                    light.comparability != LightingComparability::Comparable)
                    continue;
                const auto resource = std::find_if(
                    lighting_packet->projectorTextures.begin(),
                    lighting_packet->projectorTextures.end(),
                    [&light](const ProjectorTextureResource& candidate)
                    {
                        return candidate.sourceIdentity ==
                            light.projectorTextureIdentity;
                    });
                if (resource == lighting_packet->projectorTextures.end())
                    continue;
                ProjectorResources candidate;
                candidate.sourceLight = lightIndex;
                candidate.scissor = projectorScissor(
                    *lighting_packet, light, candidate.fullscreen);
                if (!candidate.scissor.width || !candidate.scissor.height)
                    continue;
                const auto existingImage = std::find_if(
                    projectorImages.begin(), projectorImages.end(),
                    [&resource](const ProjectorImageResources& image)
                    { return image.sourceIdentity == resource->sourceIdentity; });
                if (existingImage == projectorImages.end())
                {
                    ProjectorImageResources image;
                    image.sourceIdentity = resource->sourceIdentity;
                    image.width = resource->width;
                    image.height = resource->height;
                    image.levels = mipLevels(resource->width, resource->height);
                    image.pixels = projectorRgbaPixels(
                        *resource, limits, status);
                    if (!status) return status;
                    if (image.pixels.size() >
                        limits.maxProjectorTextureBytes - projectorTextureBytes)
                        continue;
                    projectorTextureBytes += image.pixels.size();
                    candidate.image = projectorImages.size();
                    projectorImages.push_back(std::move(image));
                }
                else
                {
                    candidate.image = static_cast<std::size_t>(
                        existingImage - projectorImages.begin());
                }
                candidate.data = makeProjectorData(
                    *lighting_packet, light,
                    projectorImages[candidate.image].levels,
                    mShadowShaderPackage.has_value());
                projectors.push_back(std::move(candidate));
            }
        }
        const std::uint64_t lightingOffset = align(textureOffset);
        if (lighting_packet)
            lightingData = makeLightingData(*lighting_packet,
                limits.maxPointLights, mShadowShaderPackage.has_value(),
                directionalLights, pointLights);
        std::uint64_t uploadBytes = lighting_packet
            ? align(lightingOffset + sizeof(lightingData)) : textureOffset;
        for (ProjectorResources& projector : projectors)
        {
            projector.uniformOffset = uploadBytes;
            uploadBytes = align(uploadBytes + sizeof(projector.data));
        }
        for (ProjectorImageResources& image : projectorImages)
        {
            image.textureOffset = uploadBytes;
            uploadBytes = align(uploadBytes + image.pixels.size());
        }
        if (lighting_packet && mShadowShaderPackage)
        {
            const std::uint32_t cascades = std::min<std::uint32_t>(
                lighting_packet->shadows.directionalCascadeCount,
                LIGHTING_DIRECTIONAL_SHADOW_CASCADES);
            for (std::size_t shadow = 0; shadow < SHADOW_MAP_COUNT; ++shadow)
            {
                shadowPasses[shadow].active = lighting_packet->shadows.enabled &&
                    (shadow < cascades ||
                     (shadow >= LIGHTING_DIRECTIONAL_SHADOW_CASCADES &&
                      lighting_packet->shadows.projectorLightIds[
                        shadow - LIGHTING_DIRECTIONAL_SHADOW_CASCADES] != 0));
                if (!shadowPasses[shadow].active) continue;
                shadowPasses[shadow].matrix = shadowClipMatrix(
                    *lighting_packet, shadow);
                shadowPasses[shadow].uniformOffset = uploadBytes;
                uploadBytes = align(uploadBytes + sizeof(
                    shadowPasses[shadow].matrix));
            }
            shadowDraws.reserve(shadowSelected.size());
            for (std::size_t selectedDraw : shadowSelected)
            {
                ShadowDrawResources resources;
                resources.sourceDraw = selectedDraw;
                const MaterialSceneDraw& draw = packet.draws[selectedDraw];
                const MaterialResource& material = packet.materials[draw.material];
                resources.masked = material.alphaMode == MaterialAlphaMode::Mask;
                resources.materialData = makeShadowMaterialData(material);
                if (!makeObjectData(draw, resources.objectData))
                    return invalid("shadow caster has a singular model transform");
                resources.skinData = makeSkinData(draw.skin == NO_RESOURCE
                    ? nullptr : &packet.skins[draw.skin]);
                if (resources.masked)
                {
                    resources.pixels = rgbaPixels(packet, material,
                        TextureSemantic::BaseColor, {{255, 255, 255, 255}},
                        resources.width, resources.height, limits, status);
                    if (!status) return status;
                }
                else
                {
                    resources.pixels = {std::byte{255}, std::byte{255},
                                       std::byte{255}, std::byte{255}};
                }
                resources.objectOffset = uploadBytes;
                uploadBytes = align(uploadBytes + sizeof(resources.objectData));
                resources.skinOffset = uploadBytes;
                uploadBytes = align(uploadBytes + sizeof(resources.skinData));
                resources.materialOffset = uploadBytes;
                uploadBytes = align(uploadBytes + sizeof(resources.materialData));
                resources.textureOffset = uploadBytes;
                uploadBytes = align(uploadBytes + resources.pixels.size());
                shadowDraws.push_back(std::move(resources));
            }
        }
        if (uploadBytes > limits.maxUploadBytes ||
            uploadBytes > mDevice.capabilities().maxBufferSize)
            return invalid("material offscreen sample exceeds upload byte limit");

        std::vector<std::byte> uploadData(static_cast<std::size_t>(uploadBytes));
        std::memcpy(uploadData.data() + vertexOffset, packet.vertices.data(),
                    static_cast<std::size_t>(vertexBytes));
        std::memcpy(uploadData.data() + indexOffset, packet.indices.data(),
                    static_cast<std::size_t>(indexBytes));
        for (std::size_t item = 0; item < draws.size(); ++item)
        {
            const MaterialSceneDraw& draw = packet.draws[draws[item].sourceDraw];
            const MaterialResource& material = packet.materials[draw.material];
            std::memcpy(uploadData.data() + frameOffset + frameStride * item,
                        draw.transform.data(), sizeof(draw.transform));
            std::array<float, OBJECT_FLOATS> objectData{};
            if (!makeObjectData(draw, objectData))
                return invalid("material draw has a singular model transform");
            std::memcpy(uploadData.data() + objectOffset + objectStride * item,
                        objectData.data(), sizeof(objectData));
            const SkinResource* drawSkin = draw.skin == NO_RESOURCE
                ? nullptr : &packet.skins[draw.skin];
            const SkinData skinData = makeSkinData(drawSkin);
            std::memcpy(uploadData.data() + skinOffset + skinStride * item,
                        skinData.data(), MATERIAL_SKIN_BYTES);
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
                const std::size_t offsetScale = 12 + texture * 4;
                factors[offsetScale] = transform[0];
                factors[offsetScale + 1] = transform[1];
                factors[offsetScale + 2] = transform[2];
                factors[offsetScale + 3] = transform[3];
                const std::size_t rotation = 28 + texture * 4;
                factors[rotation] = std::cos(transform[4]);
                factors[rotation + 1] = std::sin(transform[4]);
            }
            std::memcpy(uploadData.data() + materialOffset + materialStride * item,
                        factors.data(), sizeof(factors));
            for (std::size_t texture = 0; texture < 4; ++texture)
                std::memcpy(uploadData.data() + draws[item].textureOffsets[texture],
                            draws[item].pixels[texture].data(),
                            draws[item].pixels[texture].size());
        }
        if (lighting_packet)
            std::memcpy(uploadData.data() + lightingOffset,
                        lightingData.data(), sizeof(lightingData));
        for (const ProjectorResources& projector : projectors)
            std::memcpy(uploadData.data() + projector.uniformOffset,
                        projector.data.data(), sizeof(projector.data));
        for (const ProjectorImageResources& image : projectorImages)
            std::memcpy(uploadData.data() + image.textureOffset,
                        image.pixels.data(), image.pixels.size());
        for (const ShadowPassResources& shadow : shadowPasses)
            if (shadow.active)
                std::memcpy(uploadData.data() + shadow.uniformOffset,
                            shadow.matrix.data(), sizeof(shadow.matrix));
        for (const ShadowDrawResources& shadow : shadowDraws)
        {
            std::memcpy(uploadData.data() + shadow.objectOffset,
                        shadow.objectData.data(), sizeof(shadow.objectData));
            std::memcpy(uploadData.data() + shadow.skinOffset,
                        shadow.skinData.data(), sizeof(shadow.skinData));
            std::memcpy(uploadData.data() + shadow.materialOffset,
                        shadow.materialData.data(), sizeof(shadow.materialData));
            std::memcpy(uploadData.data() + shadow.textureOffset,
                        shadow.pixels.data(), shadow.pixels.size());
        }

        BufferHandle upload, vertices, indices, frames, objects, skin, materials,
                     lightingBuffer;
        BindingSetHandle lightingSet;
        auto cleanup = [&]()
        {
            Status first = Status::success();
            for (auto& draw : draws)
            {
                destroy(draw.frameSet, first);
                destroy(draw.skinSet, first);
                destroy(draw.materialSet, first);
                for (std::size_t texture = 0; texture < 4; ++texture)
                {
                    destroy(draw.views[texture], first);
                    destroy(draw.images[texture], first);
                }
            }
            for (auto& projector : projectors)
            {
                destroy(projector.set, first);
                destroy(projector.uniform, first);
            }
            for (auto& image : projectorImages)
            {
                destroy(image.view, first);
                destroy(image.image, first);
            }
            for (auto& shadow : shadowPasses)
            {
                destroy(shadow.set, first);
                destroy(shadow.uniform, first);
            }
            for (auto& shadow : shadowDraws)
            {
                destroy(shadow.objectSet, first);
                destroy(shadow.materialSet, first);
                destroy(shadow.view, first);
                destroy(shadow.image, first);
                destroy(shadow.object, first);
                destroy(shadow.skin, first);
                destroy(shadow.material, first);
            }
            destroy(upload, first); destroy(vertices, first); destroy(indices, first);
            destroy(frames, first); destroy(objects, first); destroy(skin, first);
            destroy(materials, first); destroy(lightingSet, first);
            destroy(lightingBuffer, first);
            return first;
        };
        upload = mDevice.createBuffer(
            {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
        if (status) vertices = mDevice.createBuffer(
            {vertexBytes, ResourceUsage::Vertex | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
        if (status) indices = mDevice.createBuffer(
            {indexBytes, ResourceUsage::Index | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
        if (status) frames = mDevice.createBuffer(
            {frameStride * selected.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) objects = mDevice.createBuffer(
            {objectStride * selected.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) skin = mDevice.createBuffer(
            {skinStride * selected.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) materials = mDevice.createBuffer(
            {materialStride * selected.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status && lighting_packet) lightingBuffer = mDevice.createBuffer(
            {sizeof(lightingData), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        for (auto& projector : projectors)
        {
            if (status) projector.uniform = mDevice.createBuffer(
                {sizeof(projector.data), ResourceUsage::Uniform |
                 ResourceUsage::TransferDestination, MemoryClass::DeviceLocal},
                status);
        }
        for (auto& image : projectorImages)
        {
            if (status) image.image = mDevice.createImage(
                {{image.width, image.height, 1}, Format::RGBA8SRGB,
                 ResourceUsage::Sampled | ResourceUsage::TransferDestination |
                     ResourceUsage::TransferSource,
                 static_cast<std::uint16_t>(image.levels), 1, 1}, status);
            if (status) image.view = mDevice.createImageView(
                {image.image, Format::RGBA8SRGB,
                 {ImageAspect::Color, 0,
                  static_cast<std::uint16_t>(image.levels), 0, 1}}, status);
        }
        for (auto& shadow : shadowPasses)
            if (status && shadow.active)
                shadow.uniform = mDevice.createBuffer(
                    {sizeof(shadow.matrix), ResourceUsage::Uniform |
                     ResourceUsage::TransferDestination,
                     MemoryClass::DeviceLocal}, status);
        for (auto& shadow : shadowDraws)
        {
            if (status) shadow.object = mDevice.createBuffer(
                {sizeof(shadow.objectData), ResourceUsage::Uniform |
                 ResourceUsage::TransferDestination,
                 MemoryClass::DeviceLocal}, status);
            if (status) shadow.skin = mDevice.createBuffer(
                {sizeof(shadow.skinData), ResourceUsage::Uniform |
                 ResourceUsage::TransferDestination,
                 MemoryClass::DeviceLocal}, status);
            if (status) shadow.material = mDevice.createBuffer(
                {sizeof(shadow.materialData), ResourceUsage::Uniform |
                 ResourceUsage::TransferDestination,
                 MemoryClass::DeviceLocal}, status);
            if (status) shadow.image = mDevice.createImage(
                {{shadow.width, shadow.height, 1}, Format::RGBA8SRGB,
                 ResourceUsage::Sampled | ResourceUsage::TransferDestination,
                 1, 1, 1}, status);
            if (status) shadow.view = mDevice.createImageView(
                {shadow.image, Format::RGBA8SRGB,
                 {ImageAspect::Color, 0, 1, 0, 1}}, status);
        }
        if (!status || !(status = mDevice.writeBuffer(upload, 0, uploadData)))
        {
            cleanup(); return status;
        }

        for (std::size_t item = 0; status && item < draws.size(); ++item)
        {
            BindingSetDesc frameDesc{mShader, 0, {{
                0, 0, ShaderPackageDesc::BindingType::UniformBuffer, frames,
                frameStride * item, sizeof(packet.draws.front().transform), {}, {}}}};
            draws[item].frameSet = mDevice.createBindingSet(frameDesc, status);
            BindingSetDesc skinDesc{mShader, 1, {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer, objects,
                 objectStride * item, OBJECT_FLOATS * sizeof(float), {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::UniformBuffer, skin,
                 skinStride * item, MATERIAL_SKIN_BYTES, {}, {}}}};
            if (status) draws[item].skinSet =
                mDevice.createBindingSet(skinDesc, status);
            BindingSetDesc materialDesc;
            materialDesc.shader = mShader;
            materialDesc.group = 2;
            materialDesc.resources.push_back({
                0, 0, ShaderPackageDesc::BindingType::UniformBuffer, materials,
                materialStride * item, MATERIAL_FLOATS * sizeof(float), {}, {}});
            for (std::size_t texture = 0; status && texture < 4; ++texture)
            {
                const Format format = texture == 0 || texture == 3
                    ? Format::RGBA8SRGB : Format::RGBA8UNorm;
                draws[item].images[texture] = mDevice.createImage(
                    {{draws[item].widths[texture], draws[item].heights[texture], 1},
                     format, ResourceUsage::Sampled | ResourceUsage::TransferDestination,
                     1, 1, 1}, status);
                if (status) draws[item].views[texture] = mDevice.createImageView(
                    {draws[item].images[texture], format,
                     {ImageAspect::Color, 0, 1, 0, 1}}, status);
                if (status) materialDesc.resources.push_back({
                    static_cast<std::uint16_t>(texture + 1), 0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,
                    {}, 0, 0, draws[item].views[texture], mRepeatSampler});
            }
            if (status) draws[item].materialSet =
                mDevice.createBindingSet(materialDesc, status);
        }
        for (auto& shadow : shadowPasses)
        {
            if (!status || !shadow.active) continue;
            BindingSetDesc frameDesc{mShadowShader, 0, {{
                0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                shadow.uniform, 0, sizeof(shadow.matrix), {}, {}}}};
            shadow.set = mDevice.createBindingSet(frameDesc, status);
        }
        for (auto& shadow : shadowDraws)
        {
            if (!status) break;
            BindingSetDesc objectDesc{mShadowShader, 1, {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 shadow.object, 0, sizeof(shadow.objectData), {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 shadow.skin, 0, sizeof(shadow.skinData), {}, {}}}};
            shadow.objectSet = mDevice.createBindingSet(objectDesc, status);
            BindingSetDesc materialDesc{mShadowShader, 2, {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 shadow.material, 0, sizeof(shadow.materialData), {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                 {}, 0, 0, shadow.view, mRepeatSampler}}};
            if (status) shadow.materialSet =
                mDevice.createBindingSet(materialDesc, status);
        }
        if (status && lighting_packet)
        {
            BindingSetDesc lightingDesc;
            lightingDesc.shader = mLightingShader;
            lightingDesc.group = 0;
            lightingDesc.resources.push_back({
                0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                lightingBuffer, 0, sizeof(lightingData), {}, {}});
            for (std::size_t target = 0; target < 4; ++target)
                lightingDesc.resources.push_back({
                    static_cast<std::uint16_t>(target + 1), 0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,
                    {}, 0, 0, mColorViews[target], mClampSampler});
            lightingDesc.resources.push_back({
                5, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                {}, 0, 0, mDepthSampleView, mClampSampler});
            for (std::size_t shadow = 0;
                 shadow < LIGHTING_DIRECTIONAL_SHADOW_CASCADES; ++shadow)
                lightingDesc.resources.push_back({
                    static_cast<std::uint16_t>(6 + shadow), 0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,
                    {}, 0, 0, mShadowViews[shadow], mShadowSampler});
            lightingSet = mDevice.createBindingSet(lightingDesc, status);
        }
        for (auto& projector : projectors)
        {
            if (!status) break;
            BindingSetDesc projectorDesc;
            projectorDesc.shader = mProjectorShader;
            projectorDesc.group = 0;
            projectorDesc.resources.push_back({
                0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                projector.uniform, 0, sizeof(projector.data), {}, {}});
            for (std::size_t target = 0; target < 3; ++target)
                projectorDesc.resources.push_back({
                    static_cast<std::uint16_t>(target + 1), 0,
                    ShaderPackageDesc::BindingType::CombinedImageSampler,
                    {}, 0, 0, mColorViews[target], mClampSampler});
            projectorDesc.resources.push_back({
                5, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                {}, 0, 0, mDepthSampleView, mClampSampler});
            projectorDesc.resources.push_back({
                6, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                {}, 0, 0, projectorImages[projector.image].view,
                mProjectorSampler});
            const LocalLightRecord& light =
                lighting_packet->localLights[projector.sourceLight];
            const std::size_t shadow = light.shadowSlot >= 0 &&
                light.shadowSlot < static_cast<std::int32_t>(
                    LIGHTING_PROJECTOR_SHADOWS)
                ? LIGHTING_DIRECTIONAL_SHADOW_CASCADES +
                    static_cast<std::size_t>(light.shadowSlot)
                : LIGHTING_DIRECTIONAL_SHADOW_CASCADES;
            projectorDesc.resources.push_back({
                7, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                {}, 0, 0, mShadowViews[shadow], mShadowSampler});
            projector.set = mDevice.createBindingSet(projectorDesc, status);
        }
        if (!status) { cleanup(); return status; }

        CommandContext& commands = mDevice.commandContext();
        bool frameBegun = false, renderingBegun = false;
        status = commands.beginFrame(); frameBegun = status.ok();
        const std::array<BufferCopyRegion, 1> vertexCopy{{{vertexOffset, 0, vertexBytes}}};
        const std::array<BufferCopyRegion, 1> indexCopy{{{indexOffset, 0, indexBytes}}};
        const std::array<BufferCopyRegion, 1> frameCopy{{
            {frameOffset, 0, frameStride * selected.size()}}};
        const std::array<BufferCopyRegion, 1> objectCopy{{
            {objectOffset, 0, objectStride * selected.size()}}};
        const std::array<BufferCopyRegion, 1> skinCopy{{
            {skinOffset, 0, skinStride * selected.size()}}};
        const std::array<BufferCopyRegion, 1> materialCopy{{
            {materialOffset, 0, materialStride * selected.size()}}};
        const std::array<BufferCopyRegion, 1> lightingCopy{{
            {lightingOffset, 0, sizeof(lightingData)}}};
        if (status) status = commands.copyBuffer(upload, vertices, vertexCopy);
        if (status) status = commands.copyBuffer(upload, indices, indexCopy);
        if (status) status = commands.copyBuffer(upload, frames, frameCopy);
        if (status) status = commands.copyBuffer(upload, objects, objectCopy);
        if (status) status = commands.copyBuffer(upload, skin, skinCopy);
        if (status) status = commands.copyBuffer(upload, materials, materialCopy);
        if (status && lighting_packet)
            status = commands.copyBuffer(upload, lightingBuffer, lightingCopy);
        for (auto& projector : projectors)
        {
            if (status)
            {
                const std::array<BufferCopyRegion, 1> uniformCopy{{
                    {projector.uniformOffset, 0, sizeof(projector.data)}}};
                status = commands.copyBuffer(
                    upload, projector.uniform, uniformCopy);
            }
        }
        for (auto& image : projectorImages)
        {
            if (status)
            {
                BufferImageCopyRegion copy;
                copy.bufferOffset = image.textureOffset;
                copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                copy.imageExtent = {image.width, image.height, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{copy}};
                status = commands.copyBufferToImage(
                    upload, image.image, copies);
            }
            if (status && image.levels > 1)
                status = commands.generateMipmaps(
                    image.image,
                    {ImageAspect::Color, 0,
                     static_cast<std::uint16_t>(image.levels), 0, 1});
        }
        for (auto& draw : draws)
            for (std::size_t texture = 0; status && texture < 4; ++texture)
            {
                BufferImageCopyRegion copy;
                copy.bufferOffset = draw.textureOffsets[texture];
                copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                copy.imageExtent = {draw.widths[texture], draw.heights[texture], 1};
                const std::array<BufferImageCopyRegion, 1> copies{{copy}};
                status = commands.copyBufferToImage(upload, draw.images[texture], copies);
            }
        for (auto& shadow : shadowPasses)
        {
            if (!status || !shadow.active) continue;
            const std::array<BufferCopyRegion, 1> copy{{
                {shadow.uniformOffset, 0, sizeof(shadow.matrix)}}};
            status = commands.copyBuffer(upload, shadow.uniform, copy);
        }
        for (auto& shadow : shadowDraws)
        {
            if (!status) break;
            const std::array<BufferCopyRegion, 1> objectUniform{{
                {shadow.objectOffset, 0, sizeof(shadow.objectData)}}};
            const std::array<BufferCopyRegion, 1> skinUniform{{
                {shadow.skinOffset, 0, sizeof(shadow.skinData)}}};
            const std::array<BufferCopyRegion, 1> materialUniform{{
                {shadow.materialOffset, 0, sizeof(shadow.materialData)}}};
            status = commands.copyBuffer(upload, shadow.object, objectUniform);
            if (status) status = commands.copyBuffer(
                upload, shadow.skin, skinUniform);
            if (status) status = commands.copyBuffer(
                upload, shadow.material, materialUniform);
            if (status)
            {
                BufferImageCopyRegion imageCopy;
                imageCopy.bufferOffset = shadow.textureOffset;
                imageCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                imageCopy.imageExtent = {shadow.width, shadow.height, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{imageCopy}};
                status = commands.copyBufferToImage(
                    upload, shadow.image, copies);
            }
        }

        if (status && mShadowShaderPackage)
        {
            bool renderedShadow = false;
            for (std::size_t shadow = 0;
                 status && shadow < shadowPasses.size(); ++shadow)
            {
                if (!shadowPasses[shadow].active) continue;
                RenderingInfo shadowRendering;
                shadowRendering.semanticId = 0x4937645f53484457ull + shadow;
                shadowRendering.width = PROBE_WIDTH;
                shadowRendering.height = PROBE_HEIGHT;
                shadowRendering.depthStencil = AttachmentDesc{
                    mShadowViews[shadow], SHADOW_FORMAT,
                    LoadOp::Clear, StoreOp::Store,
                    {{0.f, 0.f, 0.f, 0.f}, 1.f, 0}};
                bool begun = false;
                status = commands.beginRendering(shadowRendering);
                begun = status.ok();
                if (status) status = commands.setViewport(
                    {0.f, 0.f, static_cast<float>(PROBE_WIDTH),
                     static_cast<float>(PROBE_HEIGHT), 0.f, 1.f});
                if (status) status = commands.setScissor(
                    {0, 0, PROBE_WIDTH, PROBE_HEIGHT});
                bool geometryBound = false;
                for (const ShadowDrawResources& caster : shadowDraws)
                {
                    if (!status) break;
                    const MaterialSceneDraw& draw =
                        packet.draws[caster.sourceDraw];
                    const MaterialResource& material =
                        packet.materials[draw.material];
                    status = commands.bindPipeline(material.doubleSided
                        ? mShadowDoubleSidedPipeline : mShadowCulledPipeline);
                    // GHI pipeline binding invalidates descriptor state. Bind
                    // the pass set after every pipeline choice so a cull-mode
                    // change cannot leave the shadow draw without group 0.
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
                renderedShadow = renderedShadow || status.ok();
            }
            if (status && renderedShadow)
                status = commands.resourceBarrier(
                    ResourceBarrier::DepthAttachmentWriteToSampledRead);
        }
        else if (status && lighting_packet)
        {
            // The I7b/I7c packages share the I7d-capable descriptor contract.
            // Clear and transition inert maps so validation never observes an
            // undefined sampled image while the shadow-enable uniform is off.
            for (std::size_t shadow = 0;
                 status && shadow < SHADOW_MAP_COUNT; ++shadow)
            {
                RenderingInfo clearShadow;
                clearShadow.semanticId = 0x4937645f434c4541ull + shadow;
                clearShadow.width = PROBE_WIDTH;
                clearShadow.height = PROBE_HEIGHT;
                clearShadow.depthStencil = AttachmentDesc{
                    mShadowViews[shadow], SHADOW_FORMAT,
                    LoadOp::Clear, StoreOp::Store,
                    {{0.f, 0.f, 0.f, 0.f}, 1.f, 0}};
                bool begun = false;
                status = commands.beginRendering(clearShadow);
                begun = status.ok();
                if (begun)
                {
                    const Status ended = commands.endRendering();
                    if (status && !ended) status = ended;
                }
            }
            if (status) status = commands.resourceBarrier(
                ResourceBarrier::DepthAttachmentWriteToSampledRead);
        }

        RenderingInfo rendering;
        rendering.semanticId = 0x49355f4d41544cull; // "I5_MATL"
        rendering.width = PROBE_WIDTH; rendering.height = PROBE_HEIGHT;
        for (std::size_t target = 0; target < 4; ++target)
            rendering.colors.push_back({mColorViews[target], COLOR_FORMATS[target],
                LoadOp::Clear, StoreOp::Store, {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
        rendering.depthStencil = AttachmentDesc{
            mDepthView, mDepthFormat, LoadOp::Clear, StoreOp::Store,
            {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}};
        if (status) { status = commands.beginRendering(rendering); renderingBegun = status.ok(); }
        if (status) status = commands.setViewport(
            {0.f, 0.f, static_cast<float>(PROBE_WIDTH),
             static_cast<float>(PROBE_HEIGHT), 0.f, 1.f});
        if (status) status = commands.setScissor({0, 0, PROBE_WIDTH, PROBE_HEIGHT});
        for (std::size_t item = 0; status && item < draws.size(); ++item)
        {
            const MaterialSceneDraw& draw = packet.draws[draws[item].sourceDraw];
            const MaterialResource& material = packet.materials[draw.material];
            status = commands.bindPipeline(material.doubleSided ? mDoubleSidedPipeline
                                                                 : mCulledPipeline);
            if (status && item == 0)
                status = commands.bindVertexBuffer(0, vertices, 0);
            if (status && item == 0)
                status = commands.bindIndexBuffer(indices, 0, IndexType::UInt32);
            if (status) status = commands.bindBindingSet(1, draws[item].skinSet);
            if (status) status = commands.bindBindingSet(0, draws[item].frameSet);
            if (status) status = commands.bindBindingSet(2, draws[item].materialSet);
            if (status) status = commands.drawIndexed(
                {draw.indexCount, 1, draw.firstIndex, 0, 0});
        }
        if (renderingBegun)
        {
            const Status ended = commands.endRendering();
            if (status && !ended) status = ended;
        }
        if (status && lighting_packet)
        {
            status = commands.resourceBarrier(
                ResourceBarrier::ColorAttachmentWriteToSampledRead);
            if (status) status = commands.resourceBarrier(
                ResourceBarrier::DepthAttachmentWriteToSampledRead);
            RenderingInfo lightingRendering;
            lightingRendering.semanticId = 0x4937625f4c495447ull; // "I7b_LITG"
            lightingRendering.width = PROBE_WIDTH;
            lightingRendering.height = PROBE_HEIGHT;
            lightingRendering.colors.push_back({
                mLightingView, LIGHTING_FORMAT, LoadOp::Clear, StoreOp::Store,
                {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
            bool lightingBegun = false;
            if (status)
            {
                status = commands.beginRendering(lightingRendering);
                lightingBegun = status.ok();
            }
            if (status) status = commands.setViewport(
                {0.f, 0.f, static_cast<float>(PROBE_WIDTH),
                 static_cast<float>(PROBE_HEIGHT), 0.f, 1.f});
            if (status) status = commands.setScissor(
                {0, 0, PROBE_WIDTH, PROBE_HEIGHT});
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
                projectorRendering.semanticId =
                    0x4937635f50524f4aull; // "I7c_PROJ"
                projectorRendering.width = PROBE_WIDTH;
                projectorRendering.height = PROBE_HEIGHT;
                projectorRendering.colors.push_back({
                    mLightingView, LIGHTING_FORMAT, LoadOp::Load,
                    StoreOp::Store, {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
                bool projectorBegun = false;
                status = commands.beginRendering(projectorRendering);
                projectorBegun = status.ok();
                if (status) status = commands.setViewport(
                    {0.f, 0.f, static_cast<float>(PROBE_WIDTH),
                     static_cast<float>(PROBE_HEIGHT), 0.f, 1.f});
                if (status) status = commands.bindPipeline(mProjectorPipeline);
                for (const ProjectorResources& projector : projectors)
                {
                    if (status) status = commands.setScissor(projector.scissor);
                    if (status) status = commands.bindBindingSet(
                        0, projector.set);
                    if (status) status = commands.draw({3, 1, 0, 0});
                }
                if (projectorBegun)
                {
                    const Status ended = commands.endRendering();
                    if (status && !ended) status = ended;
                }
            }
            if (status)
            {
                BufferImageCopyRegion copy;
                copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                copy.imageExtent = {PROBE_WIDTH, PROBE_HEIGHT, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{copy}};
                status = commands.copyImageToBuffer(
                    mLightingColor, mLightingReadback, copies);
            }
        }
        if (status && mShadowShaderPackage)
            for (std::size_t shadow = 0;
                 status && shadow < shadowPasses.size(); ++shadow)
            {
                if (!shadowPasses[shadow].active) continue;
                BufferImageCopyRegion copy;
                copy.imageSubresource = {ImageAspect::Depth, 0, 0, 1};
                copy.imageExtent = {PROBE_WIDTH, PROBE_HEIGHT, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{copy}};
                status = commands.copyImageToBuffer(
                    mShadowImages[shadow], mShadowReadbacks[shadow], copies);
            }
        if (status)
            for (std::size_t target = 0; status && target < 4; ++target)
            {
                BufferImageCopyRegion copy;
                copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                copy.imageExtent = {PROBE_WIDTH, PROBE_HEIGHT, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{copy}};
                status = commands.copyImageToBuffer(mColors[target], mReadbacks[target], copies);
            }
        if (frameBegun)
        {
            const Status ended = commands.endFrame();
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
        mPendingResult.draws = static_cast<std::uint32_t>(draws.size());
        for (const DrawResources& resources : draws)
        {
            const MaterialSceneDraw& draw = packet.draws[resources.sourceDraw];
            if (draw.skin == NO_RESOURCE) continue;
            ++mPendingResult.riggedDraws;
            mPendingResult.maxJointCount = std::max(
                mPendingResult.maxJointCount, packet.skins[draw.skin].jointCount);
        }
        mPendingResult.textureTransformedDraws = static_cast<std::uint32_t>(
            std::count_if(draws.begin(), draws.end(),
                [](const DrawResources& draw) { return draw.textureTransformed; }));
        mPendingResult.textures = static_cast<std::uint32_t>(draws.size() * 4);
        mPendingResult.packetSha256 = materialScenePacketSha256(packet);
        if (lighting_packet)
        {
            mPendingResult.lightingExecuted = true;
            mPendingResult.directionalLights = directionalLights;
            mPendingResult.pointLights = pointLights;
            mPendingResult.projectorLights =
                static_cast<std::uint32_t>(projectors.size());
            mPendingResult.projectorTextures =
                static_cast<std::uint32_t>(projectorImages.size());
            mPendingResult.projectorFullscreenLights =
                static_cast<std::uint32_t>(std::count_if(
                    projectors.begin(), projectors.end(),
                    [](const ProjectorResources& projector)
                    { return projector.fullscreen; }));
            mPendingResult.projectorVolumeLights =
                mPendingResult.projectorLights -
                mPendingResult.projectorFullscreenLights;
            mPendingResult.lightingPacketSha256 =
                lightingScenePacketSha256(*lighting_packet);
        }
        if (mShadowShaderPackage)
        {
            mPendingResult.shadowsExecuted = true;
            mPendingResult.shadowCasterDraws =
                static_cast<std::uint32_t>(shadowDraws.size());
            for (const ShadowDrawResources& caster : shadowDraws)
            {
                const MaterialSceneDraw& draw = packet.draws[caster.sourceDraw];
                if (draw.skin != NO_RESOURCE)
                    ++mPendingResult.shadowRiggedDraws;
                if (caster.masked) ++mPendingResult.shadowMaskedDraws;
            }
            for (std::size_t shadow = 0;
                 shadow < shadowPasses.size(); ++shadow)
            {
                mPendingShadowActive[shadow] = shadowPasses[shadow].active;
                if (!shadowPasses[shadow].active) continue;
                ++mPendingResult.shadowMaps;
                if (shadow < LIGHTING_DIRECTIONAL_SHADOW_CASCADES)
                    ++mPendingResult.directionalShadowMaps;
                else
                    ++mPendingResult.projectorShadowMaps;
            }
        }
        return Status::success();
    }

public:
    Status poll(MaterialOffscreenProbeResult& result)
    {
        result = {};
        if (!mPending)
            return Status::failure(StatusCode::InvalidState,
                                   "material offscreen probe has no pending sample");
        for (std::size_t target = 0; target < 4; ++target)
        {
            const Status status = mDevice.readBuffer(
                mReadbacks[target], 0, mPixels[target]);
            if (!status) return status;
        }
        for (std::size_t target = 0; target < 4; ++target)
        {
            mPendingResult.colorSha256[target] = sha256(mPixels[target]);
            for (std::size_t pixel = 0; pixel <
                 static_cast<std::size_t>(PROBE_WIDTH) * PROBE_HEIGHT; ++pixel)
            {
                const auto begin = mPixels[target].begin() +
                    static_cast<std::ptrdiff_t>(pixel * COLOR_BYTES[target]);
                if (std::any_of(begin, begin + COLOR_BYTES[target],
                    [](std::byte value) { return value != std::byte{0}; }))
                    ++mPendingResult.nonClearPixels[target];
            }
        }
        if (mPendingResult.lightingExecuted)
        {
            const Status status = mDevice.readBuffer(
                mLightingReadback, 0, mLightingPixels);
            if (!status) return status;
            mPendingResult.litColorSha256 = sha256(mLightingPixels);
            for (std::size_t pixel = 0; pixel <
                 static_cast<std::size_t>(PROBE_WIDTH) * PROBE_HEIGHT; ++pixel)
            {
                const auto begin = mLightingPixels.begin() +
                    static_cast<std::ptrdiff_t>(pixel * LIGHTING_BYTES);
                if (std::any_of(begin, begin + LIGHTING_BYTES,
                    [](std::byte value) { return value != std::byte{0}; }))
                    ++mPendingResult.litNonClearPixels;
            }
        }
        if (mPendingResult.shadowsExecuted)
        {
            for (std::size_t shadow = 0;
                 shadow < mPendingShadowActive.size(); ++shadow)
            {
                if (!mPendingShadowActive[shadow]) continue;
                const Status status = mDevice.readBuffer(
                    mShadowReadbacks[shadow], 0, mShadowPixels[shadow]);
                if (!status) return status;
                mPendingResult.shadowDepthSha256[shadow] =
                    sha256(mShadowPixels[shadow]);
                for (std::size_t pixel = 0;
                     pixel < static_cast<std::size_t>(PROBE_WIDTH) *
                         PROBE_HEIGHT; ++pixel)
                {
                    float depth = 1.f;
                    std::memcpy(&depth,
                        mShadowPixels[shadow].data() +
                            static_cast<std::ptrdiff_t>(pixel * SHADOW_BYTES),
                        sizeof(depth));
                    if (std::isfinite(depth) && depth < 0.999999f)
                        ++mPendingResult.shadowNonClearPixels[shadow];
                }
            }
        }
        result = std::move(mPendingResult);
        mPendingResult = {};
        mPendingShadowActive.fill(false);
        mPending = false;
        return Status::success();
    }

    bool pending() const { return mPending; }

    Status shutdown()
    {
        if (mShutdown) return Status::success();
        mShutdown = true; mPending = false;
        Status first = Status::success();
        destroy(mShadowDoubleSidedPipeline, first);
        destroy(mShadowCulledPipeline, first);
        destroy(mShadowShader, first);
        destroy(mProjectorPipeline, first); destroy(mProjectorShader, first);
        destroy(mLightingPipeline, first); destroy(mLightingShader, first);
        destroy(mCulledPipeline, first); destroy(mDoubleSidedPipeline, first);
        destroy(mShader, first); destroy(mRepeatSampler, first);
        destroy(mClampSampler, first); destroy(mProjectorSampler, first);
        destroy(mShadowSampler, first);
        destroy(mLightingView, first); destroy(mLightingColor, first);
        destroy(mLightingReadback, first);
        destroy(mDepthSampleView, first); destroy(mDepthView, first);
        destroy(mDepth, first);
        for (std::size_t shadow = 0; shadow < SHADOW_MAP_COUNT; ++shadow)
        {
            destroy(mShadowViews[shadow], first);
            destroy(mShadowImages[shadow], first);
            destroy(mShadowReadbacks[shadow], first);
        }
        for (std::size_t target = 0; target < 4; ++target)
        {
            destroy(mColorViews[target], first); destroy(mColors[target], first);
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

    Status initialize()
    {
        if (mCulledPipeline) return Status::success();
        if (mShutdown)
            return Status::failure(StatusCode::InvalidState,
                                   "material offscreen probe is shut down");
        const RendererCapabilities& capabilities = mDevice.capabilities();
        if (capabilities.maxColorAttachments < 4 ||
            capabilities.maxTexture2DSize < PROBE_WIDTH ||
            capabilities.preferredDepthStencilFormat == Format::Undefined ||
            (mLightingShaderPackage &&
             capabilities.maxSampledImagesPerStage < 10u))
            return Status::failure(StatusCode::Unsupported,
                                   "device lacks I5 material target capabilities");
        Status status = Status::success();
        for (std::size_t target = 0; target < 4; ++target)
        {
            mColors[target] = mDevice.createImage(
                {{PROBE_WIDTH, PROBE_HEIGHT, 1}, COLOR_FORMATS[target],
                 ResourceUsage::ColorAttachment | ResourceUsage::TransferSource |
                     ResourceUsage::Sampled,
                 1, 1, 1}, status);
            if (status) mColorViews[target] = mDevice.createImageView(
                {mColors[target], COLOR_FORMATS[target],
                 {ImageAspect::Color, 0, 1, 0, 1}}, status);
            if (status) mReadbacks[target] = mDevice.createBuffer(
                {static_cast<std::uint64_t>(PROBE_WIDTH) * PROBE_HEIGHT *
                 COLOR_BYTES[target], ResourceUsage::TransferDestination,
                 MemoryClass::Readback}, status);
            mPixels[target].resize(static_cast<std::size_t>(PROBE_WIDTH) *
                                   PROBE_HEIGHT * COLOR_BYTES[target]);
            if (!status) break;
        }
        mDepthFormat = capabilities.preferredDepthStencilFormat;
        if (status) mDepth = mDevice.createImage(
            {{PROBE_WIDTH, PROBE_HEIGHT, 1}, mDepthFormat,
             ResourceUsage::DepthStencilAttachment | ResourceUsage::Sampled,
             1, 1, 1}, status);
        if (status) mDepthView = mDevice.createImageView(
            {mDepth, mDepthFormat,
             {ImageAspect::DepthStencil, 0, 1, 0, 1}}, status);
        if (status && mLightingShaderPackage)
            mDepthSampleView = mDevice.createImageView(
                {mDepth, mDepthFormat,
                 {ImageAspect::Depth, 0, 1, 0, 1}}, status);
        if (status) mShader = mDevice.createShaderPackage(mShaderPackage, status);
        SamplerDesc sampler;
        sampler.minFilter = sampler.magFilter = sampler.mipFilter = Filter::Linear;
        sampler.addressU = sampler.addressV = AddressMode::Repeat;
        if (status) mRepeatSampler = mDevice.createSampler(sampler, status);
        if (status && mLightingShaderPackage)
        {
            sampler.minFilter = sampler.magFilter = sampler.mipFilter = Filter::Nearest;
            sampler.addressU = sampler.addressV = sampler.addressW =
                AddressMode::ClampToEdge;
            mClampSampler = mDevice.createSampler(sampler, status);
            if (status) mShadowSampler = mDevice.createSampler(sampler, status);
            for (std::size_t shadow = 0;
                 status && shadow < SHADOW_MAP_COUNT; ++shadow)
            {
                mShadowImages[shadow] = mDevice.createImage(
                    {{PROBE_WIDTH, PROBE_HEIGHT, 1}, SHADOW_FORMAT,
                     ResourceUsage::DepthStencilAttachment |
                         ResourceUsage::Sampled | ResourceUsage::TransferSource,
                     1, 1, 1}, status);
                if (status) mShadowViews[shadow] = mDevice.createImageView(
                    {mShadowImages[shadow], SHADOW_FORMAT,
                     {ImageAspect::Depth, 0, 1, 0, 1}}, status);
                if (status) mShadowReadbacks[shadow] = mDevice.createBuffer(
                    {static_cast<std::uint64_t>(PROBE_WIDTH) * PROBE_HEIGHT *
                         SHADOW_BYTES,
                     ResourceUsage::TransferDestination,
                     MemoryClass::Readback}, status);
                mShadowPixels[shadow].resize(
                    static_cast<std::size_t>(PROBE_WIDTH) * PROBE_HEIGHT *
                    SHADOW_BYTES);
            }
            if (status) mLightingColor = mDevice.createImage(
                {{PROBE_WIDTH, PROBE_HEIGHT, 1}, LIGHTING_FORMAT,
                 ResourceUsage::ColorAttachment | ResourceUsage::TransferSource,
                 1, 1, 1}, status);
            if (status) mLightingView = mDevice.createImageView(
                {mLightingColor, LIGHTING_FORMAT,
                 {ImageAspect::Color, 0, 1, 0, 1}}, status);
            if (status) mLightingReadback = mDevice.createBuffer(
                {static_cast<std::uint64_t>(PROBE_WIDTH) * PROBE_HEIGHT *
                     LIGHTING_BYTES,
                 ResourceUsage::TransferDestination, MemoryClass::Readback},
                status);
            mLightingPixels.resize(static_cast<std::size_t>(PROBE_WIDTH) *
                                   PROBE_HEIGHT * LIGHTING_BYTES);
            if (status) mLightingShader = mDevice.createShaderPackage(
                *mLightingShaderPackage, status);
            if (status && mProjectorShaderPackage)
            {
                sampler.minFilter = sampler.magFilter =
                    sampler.mipFilter = Filter::Linear;
                sampler.addressU = sampler.addressV = sampler.addressW =
                    AddressMode::ClampToEdge;
                mProjectorSampler = mDevice.createSampler(sampler, status);
                if (status) mProjectorShader = mDevice.createShaderPackage(
                    *mProjectorShaderPackage, status);
            }
            if (status && mShadowShaderPackage)
                mShadowShader = mDevice.createShaderPackage(
                    *mShadowShaderPackage, status);
        }
        if (status)
        {
            PipelineDesc pipeline;
            pipeline.shader = mShader;
            pipeline.cullMode = CullMode::Back;
            pipeline.depthTest = true; pipeline.depthWrite = true;
            pipeline.depthCompare = CompareOp::GreaterEqual;
            pipeline.colorFormats.assign(COLOR_FORMATS.begin(), COLOR_FORMATS.end());
            pipeline.depthStencilFormat = mDepthFormat;
            pipeline.blendStates.assign(4, BlendState{});
            pipeline.vertexBuffers = {{0, sizeof(MaterialSceneVertex),
                                       VertexInputRate::PerVertex}};
            pipeline.vertexAttributes = {
                {0, 0, VertexFormat::Float32x3, offsetof(MaterialSceneVertex, position)},
                {1, 0, VertexFormat::Float32x3, offsetof(MaterialSceneVertex, normal)},
                {2, 0, VertexFormat::Float32x4, offsetof(MaterialSceneVertex, tangent)},
                {3, 0, VertexFormat::Float32x2, offsetof(MaterialSceneVertex, texCoord)},
                {4, 0, VertexFormat::UNorm8x4, offsetof(MaterialSceneVertex, color)},
                {5, 0, VertexFormat::UInt16x4, offsetof(MaterialSceneVertex, joints)},
                {6, 0, VertexFormat::Float32x4, offsetof(MaterialSceneVertex, weights)}};
            mCulledPipeline = mDevice.createPipeline(pipeline, status);
            if (status)
            {
                pipeline.cullMode = CullMode::None;
                mDoubleSidedPipeline = mDevice.createPipeline(pipeline, status);
            }
            if (status && mLightingShaderPackage)
            {
                PipelineDesc lightingPipeline;
                lightingPipeline.shader = mLightingShader;
                lightingPipeline.cullMode = CullMode::None;
                lightingPipeline.depthTest = false;
                lightingPipeline.depthWrite = false;
                lightingPipeline.colorFormats = {LIGHTING_FORMAT};
                lightingPipeline.blendStates = {BlendState{}};
                mLightingPipeline = mDevice.createPipeline(
                    lightingPipeline, status);
                if (status && mProjectorShaderPackage)
                {
                    lightingPipeline.shader = mProjectorShader;
                    BlendState additive;
                    additive.enabled = true;
                    additive.sourceColor = BlendFactor::One;
                    additive.destinationColor = BlendFactor::One;
                    additive.sourceAlpha = BlendFactor::One;
                    additive.destinationAlpha = BlendFactor::One;
                    lightingPipeline.blendStates = {additive};
                    mProjectorPipeline = mDevice.createPipeline(
                        lightingPipeline, status);
                }
                if (status && mShadowShaderPackage)
                {
                    PipelineDesc shadowPipeline;
                    shadowPipeline.shader = mShadowShader;
                    shadowPipeline.cullMode = CullMode::Back;
                    shadowPipeline.depthTest = true;
                    shadowPipeline.depthWrite = true;
                    shadowPipeline.depthCompare = CompareOp::Less;
                    shadowPipeline.depthStencilFormat = SHADOW_FORMAT;
                    shadowPipeline.vertexBuffers = {{
                        0, sizeof(MaterialSceneVertex),
                        VertexInputRate::PerVertex}};
                    shadowPipeline.vertexAttributes = {
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
                    mShadowCulledPipeline = mDevice.createPipeline(
                        shadowPipeline, status);
                    if (status)
                    {
                        shadowPipeline.cullMode = CullMode::None;
                        mShadowDoubleSidedPipeline = mDevice.createPipeline(
                            shadowPipeline, status);
                    }
                }
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
    ShaderPackageDesc mShaderPackage;
    std::optional<ShaderPackageDesc> mLightingShaderPackage;
    std::optional<ShaderPackageDesc> mProjectorShaderPackage;
    std::optional<ShaderPackageDesc> mShadowShaderPackage;
    std::array<ImageHandle, 4> mColors{};
    std::array<ImageViewHandle, 4> mColorViews{};
    std::array<BufferHandle, 4> mReadbacks{};
    std::array<std::vector<std::byte>, 4> mPixels;
    ImageHandle mDepth; ImageViewHandle mDepthView, mDepthSampleView;
    Format mDepthFormat = Format::Undefined;
    ShaderPackageHandle mShader, mLightingShader, mProjectorShader,
                        mShadowShader;
    SamplerHandle mRepeatSampler, mClampSampler, mProjectorSampler,
                  mShadowSampler;
    PipelineHandle mCulledPipeline, mDoubleSidedPipeline, mLightingPipeline,
                   mProjectorPipeline, mShadowCulledPipeline,
                   mShadowDoubleSidedPipeline;
    std::array<ImageHandle, SHADOW_MAP_COUNT> mShadowImages{};
    std::array<ImageViewHandle, SHADOW_MAP_COUNT> mShadowViews{};
    std::array<BufferHandle, SHADOW_MAP_COUNT> mShadowReadbacks{};
    std::array<std::vector<std::byte>, SHADOW_MAP_COUNT> mShadowPixels;
    std::array<bool, SHADOW_MAP_COUNT> mPendingShadowActive{};
    ImageHandle mLightingColor;
    ImageViewHandle mLightingView;
    BufferHandle mLightingReadback;
    std::vector<std::byte> mLightingPixels;
    MaterialOffscreenProbeResult mPendingResult;
    bool mPending = false;
    bool mShutdown = false;
};

MaterialOffscreenProbe::MaterialOffscreenProbe(
    Device& device, ShaderPackageDesc package) :
    mImpl(std::make_unique<Impl>(device, std::move(package))) {}
MaterialOffscreenProbe::MaterialOffscreenProbe(
    Device& device, ShaderPackageDesc material_package,
    ShaderPackageDesc lighting_package) :
    mImpl(std::make_unique<Impl>(device, std::move(material_package),
                                std::move(lighting_package))) {}
MaterialOffscreenProbe::MaterialOffscreenProbe(
    Device& device, ShaderPackageDesc material_package,
    ShaderPackageDesc lighting_package, ShaderPackageDesc projector_package) :
    mImpl(std::make_unique<Impl>(device, std::move(material_package),
                                std::move(lighting_package),
                                std::move(projector_package))) {}
MaterialOffscreenProbe::MaterialOffscreenProbe(
    Device& device, ShaderPackageDesc material_package,
    ShaderPackageDesc lighting_package, ShaderPackageDesc projector_package,
    ShaderPackageDesc shadow_package) :
    mImpl(std::make_unique<Impl>(device, std::move(material_package),
                                std::move(lighting_package),
                                std::move(projector_package),
                                std::move(shadow_package))) {}
MaterialOffscreenProbe::~MaterialOffscreenProbe() = default;
Status MaterialOffscreenProbe::submit(
    const MaterialScenePacket& packet, const MaterialOffscreenProbeLimits& limits)
{ return mImpl->submit(packet, limits); }
Status MaterialOffscreenProbe::submit(
    const MaterialScenePacket& material_packet,
    const LightingScenePacket& lighting_packet,
    const MaterialOffscreenProbeLimits& limits)
{ return mImpl->submit(material_packet, lighting_packet, limits); }
Status MaterialOffscreenProbe::poll(MaterialOffscreenProbeResult& result)
{ return mImpl->poll(result); }
bool MaterialOffscreenProbe::pending() const { return mImpl->pending(); }
Status MaterialOffscreenProbe::shutdown() { return mImpl->shutdown(); }

} // namespace LL::GHI
