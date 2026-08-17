/**
 * @file llghimaterialoffscreenprobe.cpp
 * @brief Asynchronous, non-presenting replay of live rigid opaque PBR draws.
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
constexpr std::array<float, 64> IDENTITY_SKIN{{
    1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
    0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f,
    1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
    0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f,
    1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
    0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f,
    1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
    0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f}};

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
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

bool potentiallyVisible(const MaterialScenePacket& packet,
                        const MaterialSceneDraw& draw)
{
    std::array<bool, 6> allOutside{{true, true, true, true, true, true}};
    for (std::uint32_t item = 0; item < draw.indexCount; ++item)
    {
        const MaterialSceneVertex& vertex =
            packet.vertices[packet.indices[draw.firstIndex + item]];
        const std::array<double, 4> local{{
            vertex.position[0], vertex.position[1], vertex.position[2], 1.0}};
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
} // namespace

class MaterialOffscreenProbe::Impl
{
public:
    Impl(Device& device, ShaderPackageDesc package) :
        mDevice(device), mShaderPackage(std::move(package)) {}
    ~Impl() { shutdown(); }

    Status submit(const MaterialScenePacket& packet,
                  const MaterialOffscreenProbeLimits& limits)
    {
        if (mPending)
            return Status::failure(StatusCode::NotReady,
                                   "material offscreen probe is still pending");
        if (!limits.maxDraws || !limits.maxVertices || !limits.maxIndices ||
            !limits.maxTextures || !limits.maxUploadBytes ||
            !limits.maxTextureBytes)
            return invalid("material offscreen limits must be nonzero");
        if (packet.vertices.empty() || packet.indices.empty() ||
            packet.draws.empty())
            return invalid("live material packet contains no drawable geometry");
        if (packet.vertices.size() > limits.maxVertices ||
            packet.indices.size() > limits.maxIndices)
            return invalid("live material packet exceeds geometry limits");

        std::vector<std::size_t> selected;
        std::size_t geometryDraws = 0;
        std::size_t rigidDraws = 0;
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
        for (std::size_t index = 0; index < packet.draws.size() &&
             selected.size() < limits.maxDraws; ++index)
        {
            const MaterialSceneDraw& draw = packet.draws[index];
            if (!draw.indexCount) continue;
            ++geometryDraws;
            if (draw.skin != NO_RESOURCE ||
                draw.material >= packet.materials.size())
                continue;
            ++rigidDraws;
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
            if (!potentiallyVisible(packet, draw)) continue;
            ++potentiallyVisibleDraws;
            selected.push_back(index);
            if (selected.size() * 4 > limits.maxTextures)
            {
                selected.pop_back();
                break;
            }
        }
        if (selected.empty())
        {
            std::ostringstream message;
            message << "live material packet has no executable rigid opaque PBR draws"
                    << " (packet/geometry/rigid/pbr/uv0/comparable/visible="
                    << packet.draws.size() << '/' << geometryDraws << '/'
                    << rigidDraws << '/' << opaquePbrDraws << '/'
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
        const std::uint64_t skinStride = align(sizeof(IDENTITY_SKIN));
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
        const std::uint64_t uploadBytes = textureOffset;
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
            std::memcpy(uploadData.data() + skinOffset + skinStride * item,
                        IDENTITY_SKIN.data(), sizeof(IDENTITY_SKIN));
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

        BufferHandle upload, vertices, indices, frames, objects, skin, materials;
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
            destroy(upload, first); destroy(vertices, first); destroy(indices, first);
            destroy(frames, first); destroy(objects, first); destroy(skin, first);
            destroy(materials, first);
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
                 skinStride * item, sizeof(IDENTITY_SKIN), {}, {}}}};
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
        if (status) status = commands.copyBuffer(upload, vertices, vertexCopy);
        if (status) status = commands.copyBuffer(upload, indices, indexCopy);
        if (status) status = commands.copyBuffer(upload, frames, frameCopy);
        if (status) status = commands.copyBuffer(upload, objects, objectCopy);
        if (status) status = commands.copyBuffer(upload, skin, skinCopy);
        if (status) status = commands.copyBuffer(upload, materials, materialCopy);
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

        RenderingInfo rendering;
        rendering.semanticId = 0x49345f4d41544cull; // "I4_MATL"
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
        mPendingResult.textureTransformedDraws = static_cast<std::uint32_t>(
            std::count_if(draws.begin(), draws.end(),
                [](const DrawResources& draw) { return draw.textureTransformed; }));
        mPendingResult.textures = static_cast<std::uint32_t>(draws.size() * 4);
        mPendingResult.packetSha256 = materialScenePacketSha256(packet);
        return Status::success();
    }

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
        result = std::move(mPendingResult);
        mPendingResult = {};
        mPending = false;
        return Status::success();
    }

    bool pending() const { return mPending; }

    Status shutdown()
    {
        if (mShutdown) return Status::success();
        mShutdown = true; mPending = false;
        Status first = Status::success();
        destroy(mCulledPipeline, first); destroy(mDoubleSidedPipeline, first);
        destroy(mShader, first); destroy(mRepeatSampler, first);
        destroy(mDepthView, first); destroy(mDepth, first);
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
            capabilities.preferredDepthStencilFormat == Format::Undefined)
            return Status::failure(StatusCode::Unsupported,
                                   "device lacks I4 material target capabilities");
        Status status = Status::success();
        for (std::size_t target = 0; target < 4; ++target)
        {
            mColors[target] = mDevice.createImage(
                {{PROBE_WIDTH, PROBE_HEIGHT, 1}, COLOR_FORMATS[target],
                 ResourceUsage::ColorAttachment | ResourceUsage::TransferSource,
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
             ResourceUsage::DepthStencilAttachment, 1, 1, 1}, status);
        if (status) mDepthView = mDevice.createImageView(
            {mDepth, mDepthFormat,
             {ImageAspect::DepthStencil, 0, 1, 0, 1}}, status);
        if (status) mShader = mDevice.createShaderPackage(mShaderPackage, status);
        SamplerDesc sampler;
        sampler.minFilter = sampler.magFilter = sampler.mipFilter = Filter::Linear;
        sampler.addressU = sampler.addressV = AddressMode::Repeat;
        if (status) mRepeatSampler = mDevice.createSampler(sampler, status);
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
    ImageHandle mDepth; ImageViewHandle mDepthView;
    Format mDepthFormat = Format::Undefined;
    ShaderPackageHandle mShader;
    SamplerHandle mRepeatSampler;
    PipelineHandle mCulledPipeline, mDoubleSidedPipeline;
    MaterialOffscreenProbeResult mPendingResult;
    bool mPending = false;
    bool mShutdown = false;
};

MaterialOffscreenProbe::MaterialOffscreenProbe(
    Device& device, ShaderPackageDesc package) :
    mImpl(std::make_unique<Impl>(device, std::move(package))) {}
MaterialOffscreenProbe::~MaterialOffscreenProbe() = default;
Status MaterialOffscreenProbe::submit(
    const MaterialScenePacket& packet, const MaterialOffscreenProbeLimits& limits)
{ return mImpl->submit(packet, limits); }
Status MaterialOffscreenProbe::poll(MaterialOffscreenProbeResult& result)
{ return mImpl->poll(result); }
bool MaterialOffscreenProbe::pending() const { return mImpl->pending(); }
Status MaterialOffscreenProbe::shutdown() { return mImpl->shutdown(); }

} // namespace LL::GHI
