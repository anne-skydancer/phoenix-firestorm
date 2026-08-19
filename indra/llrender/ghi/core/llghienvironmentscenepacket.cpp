/**
 * @file llghienvironmentscenepacket.cpp
 * @brief Deterministic P0e2 environment scene packet encoding.
 */

#include "linden_common.h"

#include "ghi/include/llghienvironmentscenepacket.h"
#include "llghihash.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace LL::GHI
{
namespace
{
constexpr std::array<std::byte, 8> MAGIC{{
    std::byte{'L'}, std::byte{'L'}, std::byte{'G'}, std::byte{'H'},
    std::byte{'I'}, std::byte{'E'}, std::byte{'2'}, std::byte{'A'}}};
constexpr std::uint64_t MAX_TEXTURES = 64;
constexpr std::uint64_t MAX_TEXTURE_BYTES = 256ull * 1024ull * 1024ull;
constexpr std::uint64_t MAX_WATER_VERTICES = 4ull * 1024ull * 1024ull;
constexpr std::uint64_t MAX_WATER_INDICES = 12ull * 1024ull * 1024ull;
constexpr std::uint64_t MAX_WATER_DRAWS = 256ull * 1024ull;
constexpr std::uint32_t PASS_MASK =
    environmentPassBit(EnvironmentPass::Atmosphere) |
    environmentPassBit(EnvironmentPass::HdriSky) |
    environmentPassBit(EnvironmentPass::Sun) |
    environmentPassBit(EnvironmentPass::Moon) |
    environmentPassBit(EnvironmentPass::Stars) |
    environmentPassBit(EnvironmentPass::Clouds) |
    environmentPassBit(EnvironmentPass::WaterSurface) |
    environmentPassBit(EnvironmentPass::Underwater);
constexpr std::uint32_t DEPENDENCY_MASK =
    environmentDependencyBit(EnvironmentDependency::ProductionLighting) |
    environmentDependencyBit(EnvironmentDependency::ProductionDepth) |
    environmentDependencyBit(EnvironmentDependency::WaterExclusionMask) |
    environmentDependencyBit(EnvironmentDependency::ReflectionColor) |
    environmentDependencyBit(EnvironmentDependency::RefractionColor);
constexpr std::uint32_t COMPARABILITY_MASK =
    static_cast<std::uint32_t>(ResourceComparability::MissingCpuTexture) |
    static_cast<std::uint32_t>(ResourceComparability::TextureStillFetching);

void appendU32(std::vector<std::byte>& out, std::uint32_t value)
{
    for (unsigned shift = 0; shift != 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendU64(std::vector<std::byte>& out, std::uint64_t value)
{
    for (unsigned shift = 0; shift != 64; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendFloat(std::vector<std::byte>& out, float value)
{
    appendU32(out, std::bit_cast<std::uint32_t>(value));
}

template<std::size_t N>
void appendFloats(std::vector<std::byte>& out,
                  const std::array<float, N>& values)
{
    for (float value : values) appendFloat(out, value);
}

void appendBool(std::vector<std::byte>& out, bool value)
{
    appendU32(out, value ? 1u : 0u);
}

class Reader
{
public:
    explicit Reader(std::span<const std::byte> bytes) : mBytes(bytes) {}

    bool bytes(std::span<std::byte> out)
    {
        if (out.size() > mBytes.size() - mOffset) return false;
        std::copy_n(mBytes.begin() + static_cast<std::ptrdiff_t>(mOffset),
                    out.size(), out.begin());
        mOffset += out.size();
        return true;
    }

    bool u32(std::uint32_t& value)
    {
        if (4 > mBytes.size() - mOffset) return false;
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
            value |= std::to_integer<std::uint32_t>(mBytes[mOffset++]) << shift;
        return true;
    }

    bool u64(std::uint64_t& value)
    {
        if (8 > mBytes.size() - mOffset) return false;
        value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8)
            value |= std::to_integer<std::uint64_t>(mBytes[mOffset++]) << shift;
        return true;
    }

    bool floating(float& value)
    {
        std::uint32_t bits = 0;
        if (!u32(bits)) return false;
        value = std::bit_cast<float>(bits);
        return true;
    }

    template<std::size_t N>
    bool floats(std::array<float, N>& values)
    {
        for (float& value : values)
            if (!floating(value)) return false;
        return true;
    }

    bool boolean(bool& value)
    {
        std::uint32_t encoded = 0;
        if (!u32(encoded) || encoded > 1) return false;
        value = encoded != 0;
        return true;
    }

    bool finished() const { return mOffset == mBytes.size(); }

private:
    std::span<const std::byte> mBytes;
    std::size_t mOffset = 0;
};

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

template<std::size_t N>
bool finite(const std::array<float, N>& values)
{
    return std::all_of(values.begin(), values.end(),
                       [](float value) { return std::isfinite(value); });
}

bool zeroDigest(const ResourceDigest& digest)
{
    return std::all_of(digest.begin(), digest.end(),
                       [](std::byte value) { return value == std::byte{0}; });
}

bool validComparability(ResourceComparability comparability)
{
    const auto bits = static_cast<std::uint32_t>(comparability);
    return (bits & ~COMPARABILITY_MASK) == 0;
}

bool hasPass(const EnvironmentScenePacket& packet, EnvironmentPass pass)
{
    return (packet.passMask & environmentPassBit(pass)) != 0;
}

bool hasDependency(const EnvironmentScenePacket& packet,
                   EnvironmentDependency dependency)
{
    return (packet.dependencyMask & environmentDependencyBit(dependency)) != 0;
}

bool hasBinding(const std::vector<EnvironmentTextureBinding>& bindings,
                EnvironmentTextureSemantic semantic)
{
    return std::any_of(bindings.begin(), bindings.end(),
        [semantic](const EnvironmentTextureBinding& binding)
        { return binding.semantic == semantic; });
}

bool validBindings(const std::vector<EnvironmentTextureBinding>& bindings,
                   std::size_t textureCount)
{
    if (bindings.size() > static_cast<std::size_t>(EnvironmentTextureSemantic::WaterNormalNext) + 1)
        return false;
    for (std::size_t index = 0; index < bindings.size(); ++index)
    {
        const auto& binding = bindings[index];
        if (static_cast<std::uint32_t>(binding.semantic) >
                static_cast<std::uint32_t>(EnvironmentTextureSemantic::WaterNormalNext) ||
            binding.texture >= textureCount ||
            std::any_of(bindings.begin(),
                        bindings.begin() + static_cast<std::ptrdiff_t>(index),
                        [&binding](const EnvironmentTextureBinding& prior)
                        { return prior.semantic == binding.semantic; }))
            return false;
    }
    return true;
}

bool validAtmosphere(const AtmosphereState& state)
{
    return finite(state.ambient) && finite(state.blueDensity) &&
        finite(state.blueHorizon) && finite(state.sunlight) &&
        finite(state.moonlight) && finite(state.glow) &&
        std::isfinite(state.hazeDensity) && std::isfinite(state.hazeHorizon) &&
        std::isfinite(state.densityMultiplier) &&
        std::isfinite(state.distanceMultiplier) &&
        std::isfinite(state.maxAltitude) && std::isfinite(state.gamma) &&
        std::isfinite(state.planetRadius) &&
        std::isfinite(state.skyBottomRadius) &&
        std::isfinite(state.skyTopRadius) &&
        std::isfinite(state.sunArcRadians) &&
        std::isfinite(state.mieAnisotropy) &&
        std::isfinite(state.moisture) &&
        std::isfinite(state.dropletRadius) &&
        std::isfinite(state.iceLevel) &&
        std::isfinite(state.reflectionProbeAmbiance) &&
        std::isfinite(state.skyHdrScale) &&
        std::isfinite(state.skySunlightScale) &&
        std::isfinite(state.skyAmbientScale) &&
        std::isfinite(state.sceneLightStrength) &&
        std::isfinite(state.tonemapMix) && state.gamma > 0.f &&
        state.planetRadius >= 0.f && state.skyBottomRadius >= 0.f &&
        state.skyTopRadius >= state.skyBottomRadius;
}

bool validSky(const SkyLayerState& state)
{
    return finite(state.domeTransform) && finite(state.lightDirection) &&
        finite(state.sunDirection) && finite(state.moonDirection) &&
        finite(state.sunColor) && finite(state.moonColor) &&
        finite(state.cloudColor) && finite(state.cloudPositionDensity1) &&
        finite(state.cloudPositionDensity2) && finite(state.cloudScrollDelta) &&
        std::isfinite(state.cloudScale) && std::isfinite(state.cloudShadow) &&
        std::isfinite(state.cloudVariance) &&
        std::isfinite(state.sunMoonGlowFactor) &&
        std::isfinite(state.moonBrightness) &&
        std::isfinite(state.starBrightness) && std::isfinite(state.starPhase) &&
        std::isfinite(state.blendFactor) && std::isfinite(state.hdriExposure) &&
        std::isfinite(state.hdriRotation) &&
        std::isfinite(state.hdriSplitScreen) && state.cloudScale >= 0.f &&
        state.starBrightness >= 0.f && state.blendFactor >= 0.f &&
        state.blendFactor <= 1.f;
}

bool validWater(const WaterState& state)
{
    return finite(state.fogColor) && finite(state.normalScale) &&
        finite(state.waveDirection1) && finite(state.waveDirection2) &&
        finite(state.lightDirection) && finite(state.lightColor) &&
        finite(state.clampedLightNormal) && std::isfinite(state.waterHeight) &&
        std::isfinite(state.cameraToWaterHeight) &&
        std::isfinite(state.fogDensity) && std::isfinite(state.fresnelScale) &&
        std::isfinite(state.fresnelOffset) &&
        std::isfinite(state.blurMultiplier) && std::isfinite(state.scaleAbove) &&
        std::isfinite(state.scaleBelow) && std::isfinite(state.phase) &&
        std::isfinite(state.normalBlendFactor) &&
        std::isfinite(state.exposure) && std::isfinite(state.tonemapMix) &&
        state.fogDensity >= 0.f && state.blurMultiplier >= 0.f &&
        state.normalBlendFactor >= 0.f && state.normalBlendFactor <= 1.f &&
        state.exposure > 0.f;
}

void encodeTexture(std::vector<std::byte>& out,
                   const MaterialTextureResource& texture)
{
    out.insert(out.end(), texture.sourceIdentity.begin(), texture.sourceIdentity.end());
    out.insert(out.end(), texture.contentIdentity.begin(), texture.contentIdentity.end());
    appendU32(out, static_cast<std::uint32_t>(texture.colorSpace));
    appendU32(out, static_cast<std::uint32_t>(texture.comparability));
    appendU32(out, texture.width);
    appendU32(out, texture.height);
    appendU32(out, texture.components);
    appendU32(out, texture.discardLevel);
    appendU64(out, texture.decodedPixels.size());
    out.insert(out.end(), texture.decodedPixels.begin(), texture.decodedPixels.end());
}

bool decodeTexture(Reader& reader, MaterialTextureResource& texture,
                   std::uint64_t& totalBytes)
{
    std::uint32_t colorSpace = 0, comparability = 0;
    std::uint64_t size = 0;
    if (!reader.bytes(texture.sourceIdentity) ||
        !reader.bytes(texture.contentIdentity) || !reader.u32(colorSpace) ||
        !reader.u32(comparability) || !reader.u32(texture.width) ||
        !reader.u32(texture.height) || !reader.u32(texture.components) ||
        !reader.u32(texture.discardLevel) || !reader.u64(size) ||
        size > MAX_TEXTURE_BYTES - totalBytes ||
        size > std::numeric_limits<std::size_t>::max())
        return false;
    texture.colorSpace = static_cast<TextureColorSpace>(colorSpace);
    texture.comparability = static_cast<ResourceComparability>(comparability);
    texture.decodedPixels.resize(static_cast<std::size_t>(size));
    if (!reader.bytes(texture.decodedPixels)) return false;
    totalBytes += size;
    return true;
}

void encodeBindings(std::vector<std::byte>& out,
                    const std::vector<EnvironmentTextureBinding>& bindings)
{
    appendU64(out, bindings.size());
    for (const auto& binding : bindings)
    {
        appendU32(out, static_cast<std::uint32_t>(binding.semantic));
        appendU32(out, binding.texture);
    }
}

bool decodeBindings(Reader& reader,
                    std::vector<EnvironmentTextureBinding>& bindings)
{
    std::uint64_t count = 0;
    if (!reader.u64(count) || count > 32) return false;
    bindings.resize(static_cast<std::size_t>(count));
    for (auto& binding : bindings)
    {
        std::uint32_t semantic = 0;
        if (!reader.u32(semantic) || !reader.u32(binding.texture)) return false;
        binding.semantic = static_cast<EnvironmentTextureSemantic>(semantic);
    }
    return true;
}
} // namespace

Status validateEnvironmentScenePacket(const EnvironmentScenePacket& packet)
{
    if (packet.version != ENVIRONMENT_SCENE_PACKET_VERSION || !packet.frameId ||
        !packet.sourceWidth || !packet.sourceHeight ||
        packet.sourceWidth > 32768 || packet.sourceHeight > 32768 ||
        static_cast<std::uint32_t>(packet.viewKind) >
            static_cast<std::uint32_t>(EnvironmentViewKind::Preview) ||
        (packet.passMask & ~PASS_MASK) != 0 ||
        (packet.dependencyMask & ~DEPENDENCY_MASK) != 0 ||
        packet.textures.size() > MAX_TEXTURES ||
        packet.waterVertices.size() > MAX_WATER_VERTICES ||
        packet.waterIndices.size() > MAX_WATER_INDICES ||
        packet.waterDraws.size() > MAX_WATER_DRAWS ||
        !finite(packet.viewMatrix) || !finite(packet.projectionMatrix) ||
        !finite(packet.cameraOrigin) || !validAtmosphere(packet.atmosphere) ||
        !validSky(packet.sky) || !validWater(packet.water) ||
        !validBindings(packet.sky.textures, packet.textures.size()) ||
        !validBindings(packet.water.textures, packet.textures.size()))
        return invalid("invalid environment scene packet header or state");

    const bool atmosphere = hasPass(packet, EnvironmentPass::Atmosphere);
    const bool hdri = hasPass(packet, EnvironmentPass::HdriSky);
    const bool waterSurface = hasPass(packet, EnvironmentPass::WaterSurface);
    const bool underwater = hasPass(packet, EnvironmentPass::Underwater);
    if (atmosphere && hdri)
        return invalid("environment atmosphere and HDRI routes are exclusive");
    if (waterSurface && underwater)
        return invalid("environment water surface and underwater routes are exclusive");
    if (hdri && !hasBinding(packet.sky.textures, EnvironmentTextureSemantic::Hdri))
        return invalid("HDRI route requires an HDRI texture");
    if (hasPass(packet, EnvironmentPass::Sun) &&
        !hasBinding(packet.sky.textures, EnvironmentTextureSemantic::Sun))
        return invalid("sun route requires a sun texture");
    if (hasPass(packet, EnvironmentPass::Moon) &&
        !hasBinding(packet.sky.textures, EnvironmentTextureSemantic::Moon))
        return invalid("moon route requires a moon texture");
    if (hasPass(packet, EnvironmentPass::Stars) &&
        !hasBinding(packet.sky.textures, EnvironmentTextureSemantic::StarBloom))
        return invalid("star route requires a bloom texture");
    if (hasPass(packet, EnvironmentPass::Clouds) &&
        !hasBinding(packet.sky.textures, EnvironmentTextureSemantic::CloudNoise))
        return invalid("cloud route requires a noise texture");

    const bool hasWater = waterSurface || underwater;
    constexpr std::uint32_t WATER_DEPENDENCIES =
        environmentDependencyBit(EnvironmentDependency::ProductionLighting) |
        environmentDependencyBit(EnvironmentDependency::ProductionDepth) |
        environmentDependencyBit(EnvironmentDependency::WaterExclusionMask) |
        environmentDependencyBit(EnvironmentDependency::RefractionColor);
    if (hasWater &&
        ((packet.dependencyMask & WATER_DEPENDENCIES) != WATER_DEPENDENCIES ||
         !hasBinding(packet.water.textures, EnvironmentTextureSemantic::WaterNormal) ||
         packet.waterVertices.empty() || packet.waterIndices.empty() ||
         packet.waterDraws.empty()))
        return invalid("water route is missing dependencies, normals, or geometry");
    if (!hasWater && (!packet.waterVertices.empty() || !packet.waterIndices.empty() ||
                      !packet.waterDraws.empty()))
        return invalid("water geometry supplied without an active water route");

    std::uint64_t textureBytes = 0;
    for (std::size_t index = 0; index < packet.textures.size(); ++index)
    {
        const auto& texture = packet.textures[index];
        const std::uint64_t expected = static_cast<std::uint64_t>(texture.width) *
            texture.height * texture.components;
        const bool hasPixels = !texture.decodedPixels.empty();
        if (zeroDigest(texture.sourceIdentity) ||
            static_cast<std::uint32_t>(texture.colorSpace) >
                static_cast<std::uint32_t>(TextureColorSpace::SRGB) ||
            !validComparability(texture.comparability) || texture.components > 4 ||
            (hasPixels && (zeroDigest(texture.contentIdentity) ||
                           texture.decodedPixels.size() != expected)) ||
            (!hasPixels && !zeroDigest(texture.contentIdentity)) ||
            (!hasPixels && texture.comparability == ResourceComparability::Comparable) ||
            texture.decodedPixels.size() > MAX_TEXTURE_BYTES - textureBytes ||
            std::any_of(packet.textures.begin(),
                        packet.textures.begin() + static_cast<std::ptrdiff_t>(index),
                        [&texture](const MaterialTextureResource& prior)
                        { return prior.sourceIdentity == texture.sourceIdentity; }))
            return invalid("invalid environment texture resource");
        textureBytes += texture.decodedPixels.size();
    }

    for (const auto& vertex : packet.waterVertices)
        if (!finite(vertex.position) || !finite(vertex.normal) ||
            !finite(vertex.texCoord))
            return invalid("invalid water vertex");
    for (std::uint32_t index : packet.waterIndices)
        if (index >= packet.waterVertices.size())
            return invalid("water index is outside the vertex stream");
    for (const auto& draw : packet.waterDraws)
        if (!draw.semanticId || !draw.indexCount ||
            draw.firstIndex > packet.waterIndices.size() ||
            draw.indexCount > packet.waterIndices.size() - draw.firstIndex ||
            !finite(draw.modelTransform))
            return invalid("invalid water draw");

    return Status::success();
}

Status encodeEnvironmentScenePacket(const EnvironmentScenePacket& packet,
                                    std::vector<std::byte>& encoded)
{
    Status status = validateEnvironmentScenePacket(packet);
    if (!status) return status;
    encoded.clear();
    encoded.insert(encoded.end(), MAGIC.begin(), MAGIC.end());
    appendU32(encoded, packet.version);
    appendU32(encoded, static_cast<std::uint32_t>(packet.viewKind));
    appendU64(encoded, packet.frameId);
    appendU64(encoded, packet.sceneEpoch);
    appendU64(encoded, packet.resourceEpoch);
    appendU32(encoded, packet.sourceWidth);
    appendU32(encoded, packet.sourceHeight);
    appendU32(encoded, packet.passMask);
    appendU32(encoded, packet.dependencyMask);
    appendU64(encoded, packet.textures.size());
    appendU64(encoded, packet.waterVertices.size());
    appendU64(encoded, packet.waterIndices.size());
    appendU64(encoded, packet.waterDraws.size());
    appendFloats(encoded, packet.viewMatrix);
    appendFloats(encoded, packet.projectionMatrix);
    appendFloats(encoded, packet.cameraOrigin);

    const auto& atmosphere = packet.atmosphere;
    appendFloats(encoded, atmosphere.ambient);
    appendFloats(encoded, atmosphere.blueDensity);
    appendFloats(encoded, atmosphere.blueHorizon);
    appendFloats(encoded, atmosphere.sunlight);
    appendFloats(encoded, atmosphere.moonlight);
    appendFloats(encoded, atmosphere.glow);
    for (float value : {atmosphere.hazeDensity, atmosphere.hazeHorizon,
                        atmosphere.densityMultiplier, atmosphere.distanceMultiplier,
                        atmosphere.maxAltitude, atmosphere.gamma,
                        atmosphere.planetRadius, atmosphere.skyBottomRadius,
                        atmosphere.skyTopRadius, atmosphere.sunArcRadians,
                        atmosphere.mieAnisotropy, atmosphere.moisture,
                        atmosphere.dropletRadius, atmosphere.iceLevel,
                        atmosphere.reflectionProbeAmbiance, atmosphere.skyHdrScale,
                        atmosphere.skySunlightScale, atmosphere.skyAmbientScale,
                        atmosphere.sceneLightStrength, atmosphere.tonemapMix})
        appendFloat(encoded, value);
    appendBool(encoded, atmosphere.classicMode);

    const auto& sky = packet.sky;
    appendFloats(encoded, sky.domeTransform);
    appendFloats(encoded, sky.lightDirection);
    appendFloats(encoded, sky.sunDirection);
    appendFloats(encoded, sky.moonDirection);
    appendFloats(encoded, sky.sunColor);
    appendFloats(encoded, sky.moonColor);
    appendFloats(encoded, sky.cloudColor);
    appendFloats(encoded, sky.cloudPositionDensity1);
    appendFloats(encoded, sky.cloudPositionDensity2);
    appendFloats(encoded, sky.cloudScrollDelta);
    for (float value : {sky.cloudScale, sky.cloudShadow, sky.cloudVariance,
                        sky.sunMoonGlowFactor, sky.moonBrightness,
                        sky.starBrightness, sky.starPhase, sky.blendFactor,
                        sky.hdriExposure, sky.hdriRotation, sky.hdriSplitScreen})
        appendFloat(encoded, value);
    appendBool(encoded, sky.sunUp);
    appendBool(encoded, sky.moonUp);
    encodeBindings(encoded, sky.textures);

    const auto& water = packet.water;
    appendFloats(encoded, water.fogColor);
    appendFloats(encoded, water.normalScale);
    appendFloats(encoded, water.waveDirection1);
    appendFloats(encoded, water.waveDirection2);
    appendFloats(encoded, water.lightDirection);
    appendFloats(encoded, water.lightColor);
    appendFloats(encoded, water.clampedLightNormal);
    for (float value : {water.waterHeight, water.cameraToWaterHeight,
                        water.fogDensity, water.fresnelScale, water.fresnelOffset,
                        water.blurMultiplier, water.scaleAbove, water.scaleBelow,
                        water.phase, water.normalBlendFactor, water.exposure,
                        water.tonemapMix})
        appendFloat(encoded, value);
    appendU32(encoded, water.tonemapType);
    appendBool(encoded, water.normalMipFiltering);
    appendBool(encoded, water.sunUp);
    encodeBindings(encoded, water.textures);

    for (const auto& texture : packet.textures) encodeTexture(encoded, texture);
    for (const auto& vertex : packet.waterVertices)
    {
        appendFloats(encoded, vertex.position);
        appendFloats(encoded, vertex.normal);
        appendFloats(encoded, vertex.texCoord);
    }
    for (std::uint32_t index : packet.waterIndices) appendU32(encoded, index);
    for (const auto& draw : packet.waterDraws)
    {
        appendU64(encoded, draw.semanticId);
        appendU32(encoded, draw.firstIndex);
        appendU32(encoded, draw.indexCount);
        appendFloats(encoded, draw.modelTransform);
        appendBool(encoded, draw.edgePatch);
    }
    return Status::success();
}

Status decodeEnvironmentScenePacket(std::span<const std::byte> encoded,
                                    EnvironmentScenePacket& packet)
{
    Reader reader(encoded);
    EnvironmentScenePacket out;
    std::array<std::byte, 8> magic{};
    std::uint32_t viewKind = 0;
    std::uint64_t textureCount = 0, vertexCount = 0, indexCount = 0, drawCount = 0;
    if (!reader.bytes(magic) || magic != MAGIC || !reader.u32(out.version) ||
        !reader.u32(viewKind) || !reader.u64(out.frameId) ||
        !reader.u64(out.sceneEpoch) || !reader.u64(out.resourceEpoch) ||
        !reader.u32(out.sourceWidth) || !reader.u32(out.sourceHeight) ||
        !reader.u32(out.passMask) || !reader.u32(out.dependencyMask) ||
        !reader.u64(textureCount) || textureCount > MAX_TEXTURES ||
        !reader.u64(vertexCount) || vertexCount > MAX_WATER_VERTICES ||
        !reader.u64(indexCount) || indexCount > MAX_WATER_INDICES ||
        !reader.u64(drawCount) || drawCount > MAX_WATER_DRAWS ||
        !reader.floats(out.viewMatrix) || !reader.floats(out.projectionMatrix) ||
        !reader.floats(out.cameraOrigin))
        return invalid("truncated or unrecognized environment scene packet header");
    out.viewKind = static_cast<EnvironmentViewKind>(viewKind);

    auto& atmosphere = out.atmosphere;
    if (!reader.floats(atmosphere.ambient) ||
        !reader.floats(atmosphere.blueDensity) ||
        !reader.floats(atmosphere.blueHorizon) ||
        !reader.floats(atmosphere.sunlight) ||
        !reader.floats(atmosphere.moonlight) || !reader.floats(atmosphere.glow))
        return invalid("truncated atmosphere vectors");
    for (float* value : {&atmosphere.hazeDensity, &atmosphere.hazeHorizon,
                         &atmosphere.densityMultiplier, &atmosphere.distanceMultiplier,
                         &atmosphere.maxAltitude, &atmosphere.gamma,
                         &atmosphere.planetRadius, &atmosphere.skyBottomRadius,
                         &atmosphere.skyTopRadius, &atmosphere.sunArcRadians,
                         &atmosphere.mieAnisotropy, &atmosphere.moisture,
                         &atmosphere.dropletRadius, &atmosphere.iceLevel,
                         &atmosphere.reflectionProbeAmbiance, &atmosphere.skyHdrScale,
                         &atmosphere.skySunlightScale, &atmosphere.skyAmbientScale,
                         &atmosphere.sceneLightStrength, &atmosphere.tonemapMix})
        if (!reader.floating(*value)) return invalid("truncated atmosphere scalar");
    if (!reader.boolean(atmosphere.classicMode))
        return invalid("truncated atmosphere mode");

    auto& sky = out.sky;
    if (!reader.floats(sky.domeTransform) || !reader.floats(sky.lightDirection) ||
        !reader.floats(sky.sunDirection) || !reader.floats(sky.moonDirection) ||
        !reader.floats(sky.sunColor) || !reader.floats(sky.moonColor) ||
        !reader.floats(sky.cloudColor) ||
        !reader.floats(sky.cloudPositionDensity1) ||
        !reader.floats(sky.cloudPositionDensity2) ||
        !reader.floats(sky.cloudScrollDelta))
        return invalid("truncated sky vectors");
    for (float* value : {&sky.cloudScale, &sky.cloudShadow, &sky.cloudVariance,
                         &sky.sunMoonGlowFactor, &sky.moonBrightness,
                         &sky.starBrightness, &sky.starPhase, &sky.blendFactor,
                         &sky.hdriExposure, &sky.hdriRotation, &sky.hdriSplitScreen})
        if (!reader.floating(*value)) return invalid("truncated sky scalar");
    if (!reader.boolean(sky.sunUp) || !reader.boolean(sky.moonUp) ||
        !decodeBindings(reader, sky.textures))
        return invalid("truncated sky route state");

    auto& water = out.water;
    if (!reader.floats(water.fogColor) || !reader.floats(water.normalScale) ||
        !reader.floats(water.waveDirection1) ||
        !reader.floats(water.waveDirection2) ||
        !reader.floats(water.lightDirection) ||
        !reader.floats(water.lightColor) ||
        !reader.floats(water.clampedLightNormal))
        return invalid("truncated water vectors");
    for (float* value : {&water.waterHeight, &water.cameraToWaterHeight,
                         &water.fogDensity, &water.fresnelScale,
                         &water.fresnelOffset, &water.blurMultiplier,
                         &water.scaleAbove, &water.scaleBelow, &water.phase,
                         &water.normalBlendFactor, &water.exposure,
                         &water.tonemapMix})
        if (!reader.floating(*value)) return invalid("truncated water scalar");
    if (!reader.u32(water.tonemapType) ||
        !reader.boolean(water.normalMipFiltering) ||
        !reader.boolean(water.sunUp) ||
        !decodeBindings(reader, water.textures))
        return invalid("truncated water route state");

    out.textures.resize(static_cast<std::size_t>(textureCount));
    std::uint64_t textureBytes = 0;
    for (auto& texture : out.textures)
        if (!decodeTexture(reader, texture, textureBytes))
            return invalid("truncated environment texture resource");
    out.waterVertices.resize(static_cast<std::size_t>(vertexCount));
    for (auto& vertex : out.waterVertices)
        if (!reader.floats(vertex.position) || !reader.floats(vertex.normal) ||
            !reader.floats(vertex.texCoord))
            return invalid("truncated water vertex");
    out.waterIndices.resize(static_cast<std::size_t>(indexCount));
    for (auto& index : out.waterIndices)
        if (!reader.u32(index)) return invalid("truncated water index");
    out.waterDraws.resize(static_cast<std::size_t>(drawCount));
    for (auto& draw : out.waterDraws)
        if (!reader.u64(draw.semanticId) || !reader.u32(draw.firstIndex) ||
            !reader.u32(draw.indexCount) ||
            !reader.floats(draw.modelTransform) || !reader.boolean(draw.edgePatch))
            return invalid("truncated water draw");

    if (!reader.finished()) return invalid("environment scene packet has trailing data");
    Status status = validateEnvironmentScenePacket(out);
    if (!status) return status;
    packet = std::move(out);
    return Status::success();
}

std::string environmentScenePacketSha256(const EnvironmentScenePacket& packet)
{
    std::vector<std::byte> encoded;
    if (!encodeEnvironmentScenePacket(packet, encoded)) return {};
    return sha256(encoded);
}

} // namespace LL::GHI
