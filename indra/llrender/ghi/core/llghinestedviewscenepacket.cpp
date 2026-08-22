/**
 * @file llghinestedviewscenepacket.cpp
 * @brief Deterministic P0e4 nested-view scene packet encoding.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "ghi/include/llghinestedviewscenepacket.h"
#include "llghihash.h"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>
#include <unordered_set>

namespace LL::GHI
{
namespace
{
constexpr std::array<std::byte, 8> MAGIC{{
    std::byte{'L'}, std::byte{'L'}, std::byte{'G'}, std::byte{'H'},
    std::byte{'I'}, std::byte{'N'}, std::byte{'V'}, std::byte{'1'}}};
constexpr std::uint64_t MAX_PASSES = 64ull * 1024ull;

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

    bool finished() const { return mOffset == mBytes.size(); }

private:
    std::span<const std::byte> mBytes;
    std::size_t mOffset = 0;
};

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

auto scheduleKey(const OffscreenPassDesc& pass)
{
    const std::uint16_t cubeIndex = isCubeView(pass.view)
        ? static_cast<std::uint16_t>(pass.arrayLayer / 6u) : 0;
    return std::tuple{static_cast<std::uint8_t>(pass.view), cubeIndex,
                      pass.mipLevel, static_cast<std::uint8_t>(pass.probePhase)};
}

} // namespace

Status validateNestedViewScenePacket(const NestedViewScenePacket& packet)
{
    if (packet.version != NESTED_VIEW_SCENE_PACKET_VERSION)
        return invalid("nested-view packet version is unsupported");
    if (!packet.frameId || !packet.sceneGeneration || !packet.resourceGeneration)
        return invalid("nested-view packet generations must be nonzero");
    if (packet.passes.empty() || packet.passes.size() > MAX_PASSES)
        return invalid("nested-view packet pass count is invalid");

    std::unordered_set<std::uint64_t> semanticIds;
    std::size_t index = 0;
    std::tuple<std::uint8_t, std::uint16_t, std::uint16_t, std::uint8_t> priorKey{};
    bool havePriorKey = false;
    while (index < packet.passes.size())
    {
        const NestedViewPass& nested = packet.passes[index];
        const OffscreenPassDesc& pass = nested.pass;
        if (!nested.resourceGeneration)
            return invalid("nested-view pass resource generation is zero");
        if (!validOffscreenPass(pass) || pass.view == RenderViewClass::Main)
            return invalid("nested-view packet contains an invalid offscreen pass");
        if (pass.updateEpoch != packet.sceneGeneration)
            return invalid("nested-view pass has a stale scene generation");
        if (!nested.semanticId || nested.semanticId != offscreenSemanticId(pass) ||
            !semanticIds.insert(nested.semanticId).second)
            return invalid("nested-view semantic identity is invalid or duplicated");

        const auto key = scheduleKey(pass);
        if (havePriorKey && key < priorKey)
            return invalid("nested-view schedule is not canonical");
        priorKey = key;
        havePriorKey = true;

        if (!isCubeView(pass.view))
        {
            if (pass.arrayLayer || pass.mipLevel)
                return invalid("non-cube nested view has an attachment subresource");
            ++index;
            continue;
        }

        if (pass.face != CubeFace::PositiveX || index + 6 > packet.passes.size())
            return invalid("cube nested view is missing its canonical face group");
        const std::uint16_t firstLayer = pass.arrayLayer;
        for (std::uint8_t faceIndex = 0; faceIndex < 6; ++faceIndex)
        {
            const NestedViewPass& face = packet.passes[index + faceIndex];
            const OffscreenPassDesc& facePass = face.pass;
            if (facePass.view != pass.view ||
                facePass.recursionDepth != pass.recursionDepth ||
                facePass.face != static_cast<CubeFace>(faceIndex) ||
                facePass.probePhase != pass.probePhase ||
                facePass.arrayLayer != firstLayer + faceIndex ||
                facePass.mipLevel != pass.mipLevel ||
                facePass.updateEpoch != pass.updateEpoch ||
                face.resourceGeneration != nested.resourceGeneration ||
                face.semanticId != offscreenSemanticId(facePass) ||
                !semanticIds.insert(face.semanticId).second && faceIndex != 0)
                return invalid("cube nested view face group is incomplete or inconsistent");
        }
        index += 6;
    }
    return Status::success();
}

Status encodeNestedViewScenePacket(const NestedViewScenePacket& packet,
                                   std::vector<std::byte>& encoded)
{
    Status status = validateNestedViewScenePacket(packet);
    if (!status) return status;
    encoded.clear();
    encoded.insert(encoded.end(), MAGIC.begin(), MAGIC.end());
    appendU32(encoded, packet.version);
    appendU64(encoded, packet.frameId);
    appendU64(encoded, packet.sceneGeneration);
    appendU64(encoded, packet.resourceGeneration);
    appendU64(encoded, packet.passes.size());
    for (const NestedViewPass& nested : packet.passes)
    {
        appendU64(encoded, nested.semanticId);
        appendU64(encoded, nested.resourceGeneration);
        appendU32(encoded, static_cast<std::uint32_t>(nested.pass.view));
        appendU32(encoded, nested.pass.recursionDepth);
        appendU32(encoded, static_cast<std::uint32_t>(nested.pass.face));
        appendU32(encoded, static_cast<std::uint32_t>(nested.pass.probePhase));
        appendU32(encoded, nested.pass.arrayLayer);
        appendU32(encoded, nested.pass.mipLevel);
        appendU64(encoded, nested.pass.updateEpoch);
    }
    return Status::success();
}

Status decodeNestedViewScenePacket(std::span<const std::byte> encoded,
                                   NestedViewScenePacket& packet)
{
    Reader reader(encoded);
    std::array<std::byte, MAGIC.size()> magic{};
    std::uint64_t passCount = 0;
    NestedViewScenePacket decoded;
    if (!reader.bytes(magic) || magic != MAGIC || !reader.u32(decoded.version) ||
        !reader.u64(decoded.frameId) || !reader.u64(decoded.sceneGeneration) ||
        !reader.u64(decoded.resourceGeneration) || !reader.u64(passCount) ||
        passCount > MAX_PASSES || passCount > std::numeric_limits<std::size_t>::max())
        return invalid("nested-view packet header is malformed");

    decoded.passes.resize(static_cast<std::size_t>(passCount));
    for (NestedViewPass& nested : decoded.passes)
    {
        std::uint32_t view = 0, depth = 0, face = 0, phase = 0;
        std::uint32_t layer = 0, mip = 0;
        if (!reader.u64(nested.semanticId) ||
            !reader.u64(nested.resourceGeneration) || !reader.u32(view) ||
            !reader.u32(depth) || !reader.u32(face) || !reader.u32(phase) ||
            !reader.u32(layer) || !reader.u32(mip) ||
            !reader.u64(nested.pass.updateEpoch) ||
            depth > std::numeric_limits<std::uint8_t>::max() ||
            layer > std::numeric_limits<std::uint16_t>::max() ||
            mip > std::numeric_limits<std::uint16_t>::max())
            return invalid("nested-view packet pass is malformed");
        nested.pass.view = static_cast<RenderViewClass>(view);
        nested.pass.recursionDepth = static_cast<std::uint8_t>(depth);
        nested.pass.face = static_cast<CubeFace>(face);
        nested.pass.probePhase = static_cast<ProbePhase>(phase);
        nested.pass.arrayLayer = static_cast<std::uint16_t>(layer);
        nested.pass.mipLevel = static_cast<std::uint16_t>(mip);
    }
    if (!reader.finished()) return invalid("nested-view packet has trailing bytes");
    Status status = validateNestedViewScenePacket(decoded);
    if (!status) return status;
    packet = std::move(decoded);
    return Status::success();
}

std::string nestedViewScenePacketSha256(const NestedViewScenePacket& packet)
{
    std::vector<std::byte> encoded;
    if (!encodeNestedViewScenePacket(packet, encoded)) return {};
    return sha256(encoded);
}

} // namespace LL::GHI