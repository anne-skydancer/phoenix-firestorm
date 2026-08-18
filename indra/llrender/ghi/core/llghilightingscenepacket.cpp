/**
 * @file llghilightingscenepacket.cpp
 * @brief Deterministic I7 live-lighting packet encoding.
 */

#include "linden_common.h"

#include "ghi/include/llghilightingscenepacket.h"
#include "llghihash.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace LL::GHI
{
namespace
{
constexpr std::array<std::byte, 8> MAGIC{{
    std::byte{'L'}, std::byte{'L'}, std::byte{'G'}, std::byte{'H'},
    std::byte{'I'}, std::byte{'L'}, std::byte{'7'}, std::byte{'A'}}};
constexpr std::uint64_t MAX_LOCAL_LIGHTS = 4096;
constexpr std::uint32_t COMPARABILITY_MASK =
    static_cast<std::uint32_t>(LightingComparability::ShadowImagesDeferred) |
    static_cast<std::uint32_t>(LightingComparability::ProjectorImageDeferred);

void appendU32(std::vector<std::byte>& output, std::uint32_t value)
{
    for (unsigned shift = 0; shift != 32; shift += 8)
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendI32(std::vector<std::byte>& output, std::int32_t value)
{
    appendU32(output, std::bit_cast<std::uint32_t>(value));
}

void appendU64(std::vector<std::byte>& output, std::uint64_t value)
{
    for (unsigned shift = 0; shift != 64; shift += 8)
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendFloat(std::vector<std::byte>& output, float value)
{
    appendU32(output, std::bit_cast<std::uint32_t>(value));
}

template<std::size_t N>
void appendFloats(std::vector<std::byte>& output,
                  const std::array<float, N>& values)
{
    for (float value : values) appendFloat(output, value);
}

class Reader
{
public:
    explicit Reader(std::span<const std::byte> input) : mInput(input) {}

    bool bytes(std::span<std::byte> output)
    {
        if (output.size() > mInput.size() - mOffset) return false;
        std::copy_n(mInput.begin() + static_cast<std::ptrdiff_t>(mOffset),
                    output.size(), output.begin());
        mOffset += output.size();
        return true;
    }

    bool u32(std::uint32_t& value)
    {
        if (4 > mInput.size() - mOffset) return false;
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
            value |= std::to_integer<std::uint32_t>(mInput[mOffset++]) << shift;
        return true;
    }

    bool i32(std::int32_t& value)
    {
        std::uint32_t bits = 0;
        if (!u32(bits)) return false;
        value = std::bit_cast<std::int32_t>(bits);
        return true;
    }

    bool u64(std::uint64_t& value)
    {
        if (8 > mInput.size() - mOffset) return false;
        value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8)
            value |= std::to_integer<std::uint64_t>(mInput[mOffset++]) << shift;
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

    bool finished() const { return mOffset == mInput.size(); }

private:
    std::span<const std::byte> mInput;
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

bool validComparability(LightingComparability comparability)
{
    return (static_cast<std::uint32_t>(comparability) & ~COMPARABILITY_MASK) == 0;
}

bool validDirectional(const DirectionalLightRecord& light)
{
    return finite(light.direction) && finite(light.color) &&
           std::isfinite(light.intensity) && light.intensity >= 0.f;
}

bool validPacket(const LightingScenePacket& packet)
{
    if (packet.version != LIGHTING_SCENE_PACKET_VERSION ||
        !packet.sourceWidth || !packet.sourceHeight ||
        packet.localLights.size() > MAX_LOCAL_LIGHTS ||
        !finite(packet.viewMatrix) || !finite(packet.projectionMatrix) ||
        !finite(packet.cameraOrigin) || !finite(packet.ambientColor) ||
        !validDirectional(packet.sun) || !validDirectional(packet.moon) ||
        !validComparability(packet.shadows.comparability) ||
        packet.shadows.directionalCascadeCount >
            LIGHTING_DIRECTIONAL_SHADOW_CASCADES ||
        packet.shadows.projectorShadowCount > LIGHTING_PROJECTOR_SHADOWS ||
        !finite(packet.shadows.clipPlanes) ||
        !std::isfinite(packet.shadows.directionalBias) ||
        !std::isfinite(packet.shadows.spotShadowOffset) ||
        !std::isfinite(packet.shadows.spotShadowBias) ||
        !finite(packet.shadows.projectorFade))
        return false;

    for (const auto& matrix : packet.shadows.matrices)
        if (!finite(matrix)) return false;

    for (const LocalLightRecord& light : packet.localLights)
    {
        if (!light.semanticId ||
            static_cast<std::uint32_t>(light.kind) >
                static_cast<std::uint32_t>(LocalLightKind::Projector) ||
            !validComparability(light.comparability) ||
            !finite(light.position) || !finite(light.color) ||
            !finite(light.rotation) || !finite(light.scale) ||
            !finite(light.projectorParams) || !std::isfinite(light.radius) ||
            !std::isfinite(light.falloff) || !std::isfinite(light.shadowFade) ||
            light.radius <= 0.f || light.falloff < 0.f ||
            light.shadowSlot < -1 || light.shadowSlot >=
                static_cast<std::int32_t>(LIGHTING_PROJECTOR_SHADOWS) ||
            (light.kind == LocalLightKind::Point && light.shadowSlot != -1))
            return false;
    }
    return true;
}

void encodeDirectional(std::vector<std::byte>& output,
                       const DirectionalLightRecord& light)
{
    appendFloats(output, light.direction);
    appendFloat(output, light.intensity);
    appendFloats(output, light.color);
    appendU32(output, light.active ? 1u : 0u);
}

bool decodeDirectional(Reader& reader, DirectionalLightRecord& light)
{
    std::uint32_t active = 0;
    if (!reader.floats(light.direction) || !reader.floating(light.intensity) ||
        !reader.floats(light.color) || !reader.u32(active) || active > 1)
        return false;
    light.active = active != 0;
    return true;
}
} // namespace

Status encodeLightingScenePacket(const LightingScenePacket& packet,
                                 std::vector<std::byte>& encoded)
{
    if (!validPacket(packet)) return invalid("invalid lighting scene packet");
    encoded.clear();
    encoded.insert(encoded.end(), MAGIC.begin(), MAGIC.end());
    appendU32(encoded, packet.version);
    appendU32(encoded, 0);
    appendU64(encoded, packet.frameId);
    appendU64(encoded, packet.sceneEpoch);
    appendU64(encoded, packet.resourceEpoch);
    appendU32(encoded, packet.sourceWidth);
    appendU32(encoded, packet.sourceHeight);
    appendU64(encoded, packet.localLights.size());
    appendFloats(encoded, packet.viewMatrix);
    appendFloats(encoded, packet.projectionMatrix);
    appendFloats(encoded, packet.cameraOrigin);
    appendFloats(encoded, packet.ambientColor);
    encodeDirectional(encoded, packet.sun);
    encodeDirectional(encoded, packet.moon);

    const LightingShadowState& shadows = packet.shadows;
    appendU32(encoded, shadows.enabled ? 1u : 0u);
    appendU32(encoded, shadows.directionalCascadeCount);
    appendU32(encoded, shadows.projectorShadowCount);
    appendU32(encoded, static_cast<std::uint32_t>(shadows.comparability));
    for (const auto& matrix : shadows.matrices) appendFloats(encoded, matrix);
    appendFloats(encoded, shadows.clipPlanes);
    appendFloat(encoded, shadows.directionalBias);
    appendFloat(encoded, shadows.spotShadowOffset);
    appendFloat(encoded, shadows.spotShadowBias);
    for (std::uint64_t id : shadows.projectorLightIds) appendU64(encoded, id);
    appendFloats(encoded, shadows.projectorFade);

    for (const LocalLightRecord& light : packet.localLights)
    {
        appendU64(encoded, light.semanticId);
        appendU32(encoded, static_cast<std::uint32_t>(light.kind));
        appendU32(encoded, static_cast<std::uint32_t>(light.comparability));
        appendFloats(encoded, light.position);
        appendFloat(encoded, light.radius);
        appendFloats(encoded, light.color);
        appendFloat(encoded, light.falloff);
        appendFloats(encoded, light.rotation);
        appendFloats(encoded, light.scale);
        appendFloats(encoded, light.projectorParams);
        for (std::uint8_t value : light.projectorTextureIdentity)
            encoded.push_back(static_cast<std::byte>(value));
        appendI32(encoded, light.shadowSlot);
        appendFloat(encoded, light.shadowFade);
    }
    return Status::success();
}

Status decodeLightingScenePacket(std::span<const std::byte> encoded,
                                 LightingScenePacket& packet)
{
    Reader reader(encoded);
    std::array<std::byte, 8> magic{};
    std::uint32_t reserved = 0;
    std::uint64_t lightCount = 0;
    LightingScenePacket output;
    if (!reader.bytes(magic) || magic != MAGIC || !reader.u32(output.version) ||
        !reader.u32(reserved) || !reader.u64(output.frameId) ||
        !reader.u64(output.sceneEpoch) || !reader.u64(output.resourceEpoch) ||
        !reader.u32(output.sourceWidth) || !reader.u32(output.sourceHeight) ||
        !reader.u64(lightCount) || lightCount > MAX_LOCAL_LIGHTS ||
        output.version != LIGHTING_SCENE_PACKET_VERSION ||
        !reader.floats(output.viewMatrix) ||
        !reader.floats(output.projectionMatrix) ||
        !reader.floats(output.cameraOrigin) ||
        !reader.floats(output.ambientColor) ||
        !decodeDirectional(reader, output.sun) ||
        !decodeDirectional(reader, output.moon))
        return invalid("truncated or unrecognized lighting scene packet header");

    std::uint32_t enabled = 0, comparability = 0;
    LightingShadowState& shadows = output.shadows;
    if (!reader.u32(enabled) || enabled > 1 ||
        !reader.u32(shadows.directionalCascadeCount) ||
        !reader.u32(shadows.projectorShadowCount) ||
        !reader.u32(comparability))
        return invalid("truncated lighting shadow state");
    shadows.enabled = enabled != 0;
    shadows.comparability = static_cast<LightingComparability>(comparability);
    for (auto& matrix : shadows.matrices)
        if (!reader.floats(matrix)) return invalid("truncated lighting shadow matrix");
    if (!reader.floats(shadows.clipPlanes) ||
        !reader.floating(shadows.directionalBias) ||
        !reader.floating(shadows.spotShadowOffset) ||
        !reader.floating(shadows.spotShadowBias))
        return invalid("truncated lighting shadow parameters");
    for (std::uint64_t& id : shadows.projectorLightIds)
        if (!reader.u64(id)) return invalid("truncated projector shadow identity");
    if (!reader.floats(shadows.projectorFade))
        return invalid("truncated projector shadow fade");

    output.localLights.resize(static_cast<std::size_t>(lightCount));
    for (LocalLightRecord& light : output.localLights)
    {
        std::uint32_t kind = 0, lightComparability = 0;
        if (!reader.u64(light.semanticId) || !reader.u32(kind) ||
            !reader.u32(lightComparability) || !reader.floats(light.position) ||
            !reader.floating(light.radius) || !reader.floats(light.color) ||
            !reader.floating(light.falloff) || !reader.floats(light.rotation) ||
            !reader.floats(light.scale) ||
            !reader.floats(light.projectorParams))
            return invalid("truncated local-light record");
        light.kind = static_cast<LocalLightKind>(kind);
        light.comparability =
            static_cast<LightingComparability>(lightComparability);
        for (std::uint8_t& value : light.projectorTextureIdentity)
        {
            std::array<std::byte, 1> byte{};
            if (!reader.bytes(byte)) return invalid("truncated projector identity");
            value = std::to_integer<std::uint8_t>(byte[0]);
        }
        if (!reader.i32(light.shadowSlot) || !reader.floating(light.shadowFade))
            return invalid("truncated local-light shadow state");
    }

    if (!reader.finished() || !validPacket(output))
        return invalid("lighting scene packet has trailing or invalid data");
    packet = std::move(output);
    return Status::success();
}

std::string lightingScenePacketSha256(const LightingScenePacket& packet)
{
    std::vector<std::byte> encoded;
    if (!encodeLightingScenePacket(packet, encoded)) return {};
    return sha256(encoded);
}

} // namespace LL::GHI
