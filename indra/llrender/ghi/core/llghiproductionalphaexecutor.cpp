/**
 * @file llghiproductionalphaexecutor.cpp
 * @brief P0e3c shared-target legacy alpha execution.
 */

#include "linden_common.h"

#include "ghi/include/llghiproductionalphaexecutor.h"

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
constexpr Format COLOR_FORMAT = Format::RGBA16Float;
constexpr Format DEPTH_FORMAT = Format::Depth32Float;
constexpr std::uint32_t COLOR_BYTES = 8;
constexpr std::size_t ALPHA_FLOATS = 20;
constexpr std::size_t OBJECT_FLOATS = 32;
constexpr std::size_t MATERIAL_FLOATS = 48;
using SkinData = std::array<float, MATERIAL_SKIN_FLOATS>;

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

Status unsupported(const char* message)
{
    return Status::failure(StatusCode::Unsupported, message);
}

bool finite(const std::array<float, 3>& values)
{
    return std::all_of(values.begin(), values.end(),
                       [](float value) { return std::isfinite(value); });
}

bool boundedLighting(const ProductionAlphaLighting& lighting)
{
    if (!finite(lighting.direction) || !finite(lighting.ambient) ||
        !finite(lighting.directional))
        return false;
    float lengthSquared = 0.f;
    for (float value : lighting.direction)
    {
        if (value < -1.f || value > 1.f) return false;
        lengthSquared += value * value;
    }
    if (lengthSquared < 1.e-6f || lengthSquared > 1.001f) return false;
    const auto color = [](const std::array<float, 3>& values)
    {
        return std::all_of(values.begin(), values.end(),
            [](float value) { return value >= 0.f && value <= 16.f; });
    };
    return color(lighting.ambient) && color(lighting.directional);
}

bool hasPPLLArtifact(const ShaderPackageDesc& package, Backend backend)
{
    const ShaderPackageDesc::TargetProfile required = backend == Backend::OpenGL
        ? ShaderPackageDesc::TargetProfile::OpenGL44
        : ShaderPackageDesc::TargetProfile::VulkanSpirV13;
    const auto fragment = std::find_if(package.stages.begin(), package.stages.end(),
        [](const ShaderPackageDesc::StageArtifact& stage)
        { return stage.stage == ShaderPackageDesc::Stage::Fragment; });
    const auto binding = [&](std::uint16_t index,
                             ShaderPackageDesc::BindingType type)
    {
        return std::any_of(package.bindings.begin(), package.bindings.end(),
            [index, type](const ShaderPackageDesc::Binding& value)
            { return value.group == 3 && value.binding == index &&
                     value.type == type; });
    };
    return fragment != package.stages.end() &&
        std::any_of(fragment->artifacts.begin(), fragment->artifacts.end(),
            [required](const ShaderPackageDesc::CodeArtifact& artifact)
            { return artifact.target == required; }) &&
        binding(0, ShaderPackageDesc::BindingType::StorageImage) &&
        binding(1, ShaderPackageDesc::BindingType::StorageBuffer) &&
        binding(2, ShaderPackageDesc::BindingType::StorageBuffer);
}

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment)
{
    if (value > std::numeric_limits<std::uint64_t>::max() - alignment + 1)
        return std::numeric_limits<std::uint64_t>::max();
    return (value + alignment - 1) / alignment * alignment;
}

bool supportedComparability(ResourceComparability value)
{
    constexpr std::uint32_t ALPHA_SUPPORTED =
        static_cast<std::uint32_t>(ResourceComparability::AlphaDeferred) |
        static_cast<std::uint32_t>(ResourceComparability::MissingCpuTexture) |
        static_cast<std::uint32_t>(ResourceComparability::TextureStillFetching);
    return (static_cast<std::uint32_t>(value) & ~ALPHA_SUPPORTED) == 0;
}

bool validSkin(const SkinResource& skin)
{
    return skin.comparability == ResourceComparability::Comparable &&
           skin.jointCount && skin.jointCount <= MATERIAL_MAX_JOINTS &&
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
    if (skin)
        std::copy(skin->matrixPalette.begin(), skin->matrixPalette.end(),
                  data.begin());
    const std::array<std::uint32_t, 4> metadata{{
        skin ? skin->jointCount : 1u, 0, 0, 0}};
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

const MaterialTextureBinding* findBinding(
    const MaterialResource& material, TextureSemantic semantic)
{
    const auto found = std::find_if(material.textures.begin(),
        material.textures.end(), [semantic](const MaterialTextureBinding& binding)
        { return binding.semantic == semantic; });
    return found == material.textures.end() ? nullptr : &*found;
}

BlendFactor blendFactor(AlphaBlendFactor value)
{
    return static_cast<BlendFactor>(value);
}
static_assert(static_cast<std::uint8_t>(AlphaBlendFactor::Zero) ==
              static_cast<std::uint8_t>(BlendFactor::Zero));
static_assert(static_cast<std::uint8_t>(AlphaBlendFactor::OneMinusDestinationAlpha) ==
              static_cast<std::uint8_t>(BlendFactor::OneMinusDestinationAlpha));
static_assert(static_cast<std::uint8_t>(AlphaBlendOperation::Maximum) ==
              static_cast<std::uint8_t>(BlendOp::Maximum));

BlendOp blendOperation(AlphaBlendOperation value)
{
    return static_cast<BlendOp>(value);
}

BlendState blendState(const AlphaBlendDescription& source)
{
    BlendState output;
    output.enabled = true;
    output.sourceColor = blendFactor(source.sourceColor);
    output.destinationColor = blendFactor(source.destinationColor);
    output.colorOp = blendOperation(source.colorOperation);
    output.sourceAlpha = blendFactor(source.sourceAlpha);
    output.destinationAlpha = blendFactor(source.destinationAlpha);
    output.alphaOp = blendOperation(source.alphaOperation);
    return output;
}

BlendState emissiveBlendState()
{
    BlendState output;
    output.enabled = true;
    output.sourceColor = BlendFactor::Zero;
    output.destinationColor = BlendFactor::One;
    output.sourceAlpha = BlendFactor::One;
    output.destinationAlpha = BlendFactor::One;
    return output;
}

struct Texture
{
    Format format = Format::Undefined;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> pixels;
    std::uint64_t uploadOffset = 0;
    ImageHandle image;
    ImageViewHandle view;
};

Status decodeTexture(const MaterialTextureResource& source, Texture& output)
{
    if (source.comparability != ResourceComparability::Comparable ||
        source.decodedPixels.empty() || !source.width || !source.height ||
        !source.components || source.components > 4)
        return unsupported("alpha texture has no comparable decoded content");
    const std::uint64_t texels =
        static_cast<std::uint64_t>(source.width) * source.height;
    if (texels > std::numeric_limits<std::size_t>::max() / source.components ||
        source.decodedPixels.size() != texels * source.components ||
        texels > std::numeric_limits<std::size_t>::max() / 4)
        return invalid("alpha texture byte count is invalid");
    output = {};
    output.width = source.width;
    output.height = source.height;
    output.format = source.colorSpace == TextureColorSpace::SRGB
        ? Format::RGBA8SRGB : Format::RGBA8UNorm;
    output.pixels.resize(static_cast<std::size_t>(texels) * 4);
    for (std::size_t texel = 0; texel < texels; ++texel)
    {
        const std::size_t input = texel * source.components;
        const std::size_t result = texel * 4;
        const std::byte luma = source.decodedPixels[input];
        output.pixels[result] = luma;
        output.pixels[result + 1] = source.components < 3
            ? luma : source.decodedPixels[input + 1];
        output.pixels[result + 2] = source.components < 3
            ? luma : source.decodedPixels[input + 2];
        output.pixels[result + 3] = source.components == 2
            ? source.decodedPixels[input + 1]
            : source.components == 4 ? source.decodedPixels[input + 3]
                                     : std::byte{255};
    }
    return Status::success();
}
} // namespace

class ProductionAlphaExecutor::Impl
{
public:
    Impl(Device& device, ShaderPackageDesc shader) :
        mDevice(device), mShaderPackage(std::move(shader))
    {
    }

    ~Impl() { shutdown(); }

    Status submit(const AlphaScenePacket& packet,
                  const ProductionFrameTargetSet& targets,
                  const ProductionAlphaLighting& lighting,
                  const ProductionAlphaLimits& limits)
    {
        if (mPending)
            return Status::failure(StatusCode::NotReady,
                                   "production alpha execution is pending");
        Status status = validateAlphaScenePacket(packet);
        if (!status) return status;
        if (!targets.generation || !targets.width || !targets.height ||
            !targets.lightingImage || !targets.lightingView ||
            !targets.depthImage || !targets.depthView ||
            targets.width > packet.sourceWidth ||
            targets.height > packet.sourceHeight)
            return invalid("production alpha shared targets are incomplete");
        if (lighting.generation != targets.generation ||
            !boundedLighting(lighting))
            return invalid("production alpha lighting is invalid or stale");
        if (!limits.maxDraws || !limits.maxVertices || !limits.maxIndices ||
            !limits.maxUploadBytes || !limits.maxTextureBytes)
            return invalid("production alpha limits must be nonzero");
        if (packet.draws.size() > limits.maxDraws ||
            packet.materials.vertices.size() > limits.maxVertices ||
            packet.materials.indices.size() > limits.maxIndices)
            return unsupported("production alpha geometry exceeds limits");
        if (!(status = initialize())) return status;
        if (!(status = ensureReadbacks(targets))) return status;

        const AlphaPPLLAllocation ppllAllocation = planAlphaPPLLAllocation(
            targets.width, targets.height,
            mDevice.capabilities().maxBufferSize, packet.ppllPolicy);
        const bool ppllAvailable = supportsPPLL(mDevice.capabilities()) &&
            hasPPLLArtifact(mShaderPackage, mDevice.backend()) &&
            ppllAllocation.usable;

        struct Draw
        {
            std::size_t source = 0;
            AlphaRoute route = AlphaRoute::LegacySorted;
            ImageViewHandle baseView;
            ImageViewHandle emissiveView;
            BindingSetHandle frameSet;
            BindingSetHandle replayFrameSet;
            BindingSetHandle objectSet;
            BindingSetHandle materialSet;
            PipelineHandle pipeline;
            PipelineHandle replayPipeline;
        };

        std::vector<Texture> textures(packet.materials.textures.size());
        std::uint64_t textureBytes = 0;
        for (std::size_t index = 0; index < textures.size(); ++index)
        {
            status = decodeTexture(packet.materials.textures[index],
                                   textures[index]);
            if (!status)
            {
                status = Status::success();
                textures[index] = {};
                continue;
            }
            if (textures[index].pixels.size() >
                std::numeric_limits<std::uint64_t>::max() - textureBytes)
                return unsupported("production alpha texture byte count overflow");
            textureBytes += textures[index].pixels.size();
        }
        if (textureBytes > limits.maxTextureBytes)
            return unsupported("production alpha textures exceed limits");
        status = createTextures(textures);
        if (!status) return status;

        const AlphaRoutingState routing{
            packet.requestedMethod, ppllAvailable, false, packet.transientLoad};
        std::vector<Draw> draws;
        std::uint32_t maskDraws = 0;
        std::uint32_t deferredDraws = 0;
        std::uint32_t deferredRouteOrMaterialDraws = 0;
        std::uint32_t deferredSkinDraws = 0;
        std::uint32_t deferredTextureDraws = 0;
        std::uint32_t sortedDraws = 0;
        std::uint32_t residualDraws = 0;
        std::uint32_t ppllDraws = 0;
        std::uint32_t emissiveReplays = 0;
        for (std::size_t source = 0; source < packet.draws.size(); ++source)
        {
            const AlphaSceneDraw& alpha = packet.draws[source];
            const MaterialSceneDraw& geometry = packet.materials.draws[source];
            const AlphaRoute route = routeAlphaSubmission(
                {packet.phase, alpha.classification, alpha.rigged,
                 alpha.fullbright, alpha.emissive}, routing).route;
            if (route == AlphaRoute::Mask)
            {
                ++maskDraws;
                continue;
            }
            if (route != AlphaRoute::LegacySorted &&
                route != AlphaRoute::LegacyResidual &&
                route != AlphaRoute::PPLLCapture)
            {
                ++deferredDraws;
                ++deferredRouteOrMaterialDraws;
                continue;
            }
            if (geometry.material >= packet.materials.materials.size() ||
                !supportedComparability(geometry.comparability))
            {
                ++deferredDraws;
                ++deferredRouteOrMaterialDraws;
                continue;
            }
            const MaterialResource& material =
                packet.materials.materials[geometry.material];
            if (!supportedComparability(material.comparability))
            {
                ++deferredDraws;
                ++deferredRouteOrMaterialDraws;
                continue;
            }
            if (geometry.skin != NO_RESOURCE &&
                (geometry.skin >= packet.materials.skins.size() ||
                 !validSkin(packet.materials.skins[geometry.skin])))
            {
                ++deferredDraws;
                ++deferredSkinDraws;
                continue;
            }
            Draw executable;
            executable.source = source;
            executable.route = route;
            executable.baseView = mFallbackViews[0];
            executable.emissiveView = mFallbackViews[1];
            bool ready = true;
            for (const auto& binding : material.textures)
            {
                if (binding.texcoord != 0 ||
                    binding.texture >= textures.size())
                {
                    ready = false;
                    break;
                }
                if (binding.semantic == TextureSemantic::BaseColor ||
                    binding.semantic == TextureSemantic::Emissive)
                {
                    if (!textures[binding.texture].view)
                    {
                        ready = false;
                        break;
                    }
                    if (binding.semantic == TextureSemantic::BaseColor)
                        executable.baseView = textures[binding.texture].view;
                    else
                        executable.emissiveView = textures[binding.texture].view;
                }
            }
            if (!ready)
            {
                ++deferredDraws;
                ++deferredTextureDraws;
                continue;
            }
            if (route == AlphaRoute::LegacyResidual) ++residualDraws;
            else if (route == AlphaRoute::PPLLCapture) ++ppllDraws;
            else ++sortedDraws;
            emissiveReplays += alpha.emissive;
            draws.push_back(executable);
        }
        if (draws.empty())
        {
            destroyTextures(textures);
            return invalid("production alpha packet has no executable blended draws");
        }

        const std::uint64_t alignment = std::max<std::uint64_t>(
            16, mDevice.capabilities().uniformBufferOffsetAlignment);
        std::vector<MaterialSceneVertex> uploadVertices =
            packet.materials.vertices;
        std::vector<std::uint32_t> uploadIndices = packet.materials.indices;
        const std::uint32_t resolveFirstIndex =
            static_cast<std::uint32_t>(uploadIndices.size());
        if (ppllDraws)
        {
            const std::uint32_t firstVertex =
                static_cast<std::uint32_t>(uploadVertices.size());
            std::array<MaterialSceneVertex, 3> fullscreen{};
            fullscreen[0].position = {{-1.f, -1.f, 0.f}};
            fullscreen[1].position = {{3.f, -1.f, 0.f}};
            fullscreen[2].position = {{-1.f, 3.f, 0.f}};
            for (auto& vertex : fullscreen)
            {
                vertex.normal = {{0.f, 0.f, 1.f}};
                uploadVertices.push_back(vertex);
            }
            uploadIndices.insert(uploadIndices.end(),
                {firstVertex, firstVertex + 1u, firstVertex + 2u});
        }
        const std::uint64_t vertexBytes =
            uploadVertices.size() * sizeof(MaterialSceneVertex);
        const std::uint64_t indexBytes =
            uploadIndices.size() * sizeof(std::uint32_t);
        const std::uint64_t frameStride = alignUp(16 * sizeof(float), alignment);
        const std::uint64_t alphaStride = alignUp(ALPHA_FLOATS * sizeof(float), alignment);
        const std::uint64_t objectStride = alignUp(OBJECT_FLOATS * sizeof(float), alignment);
        const std::uint64_t skinStride = alignUp(MATERIAL_SKIN_BYTES, alignment);
        const std::uint64_t materialStride = alignUp(MATERIAL_FLOATS * sizeof(float), alignment);
        const std::uint64_t vertexOffset = 0;
        const std::uint64_t indexOffset = alignUp(vertexBytes, alignment);
        const std::uint64_t frameOffset = alignUp(indexOffset + indexBytes, alignment);
        const std::uint64_t alphaOffset = alignUp(
            frameOffset + frameStride * draws.size(), alignment);
        const std::size_t alphaSlots = draws.size() * 2 + (ppllDraws ? 1 : 0);
        const std::uint64_t objectOffset = alignUp(
            alphaOffset + alphaStride * alphaSlots, alignment);
        const std::uint64_t skinOffset = alignUp(
            objectOffset + objectStride * draws.size(), alignment);
        const std::uint64_t materialOffset = alignUp(
            skinOffset + skinStride * draws.size(), alignment);
        const std::uint64_t textureOffset = alignUp(
            materialOffset + materialStride * draws.size(), alignment);
        std::uint64_t total = textureOffset;
        for (Texture& texture : textures)
        {
            if (texture.pixels.empty()) continue;
            texture.uploadOffset = total;
            total = alignUp(total + texture.pixels.size(), alignment);
        }
        const std::uint64_t headOffset = total;
        const std::uint64_t headBytes = ppllDraws
            ? static_cast<std::uint64_t>(targets.width) * targets.height *
                sizeof(std::uint32_t) : 0;
        if (ppllDraws) total = alignUp(total + headBytes, alignment);
        const std::uint64_t counterOffset = total;
        if (ppllDraws) total = alignUp(total + 2u * sizeof(std::uint32_t), alignment);
        if (total == std::numeric_limits<std::uint64_t>::max() ||
            total > limits.maxUploadBytes ||
            total > mDevice.capabilities().maxBufferSize)
        {
            destroyTextures(textures);
            return unsupported("production alpha upload exceeds limits");
        }

        std::vector<std::byte> bytes(static_cast<std::size_t>(total));
        std::memcpy(bytes.data() + vertexOffset, uploadVertices.data(),
                    static_cast<std::size_t>(vertexBytes));
        std::memcpy(bytes.data() + indexOffset, uploadIndices.data(),
                    static_cast<std::size_t>(indexBytes));
        for (std::size_t item = 0; item < draws.size(); ++item)
        {
            const std::size_t source = draws[item].source;
            const MaterialSceneDraw& draw = packet.materials.draws[source];
            const AlphaSceneDraw& alpha = packet.draws[source];
            const MaterialResource& material =
                packet.materials.materials[draw.material];
            std::memcpy(bytes.data() + frameOffset + frameStride * item,
                        draw.transform.data(), sizeof(draw.transform));
            std::array<float, OBJECT_FLOATS> object{};
            std::copy(draw.modelTransform.begin(), draw.modelTransform.end(),
                      object.begin());
            std::array<float, 16> normal{};
            if (!normalMatrix(draw.modelTransform, normal))
            {
                destroyTextures(textures);
                return invalid("production alpha draw has a singular transform");
            }
            std::copy(normal.begin(), normal.end(), object.begin() + 16);
            std::memcpy(bytes.data() + objectOffset + objectStride * item,
                        object.data(), sizeof(object));
            const SkinData skin = makeSkinData(draw.skin == NO_RESOURCE
                ? nullptr : &packet.materials.skins[draw.skin]);
            std::memcpy(bytes.data() + skinOffset + skinStride * item,
                        skin.data(), sizeof(skin));

            std::array<float, MATERIAL_FLOATS> factors{};
            std::copy(material.baseColor.begin(), material.baseColor.end(),
                      factors.begin());
            const bool legacy = material.model == MaterialModel::Legacy;
            factors[4] = legacy ? material.legacySpecular[0] : material.emissive[0];
            factors[5] = legacy ? material.legacySpecular[1] : material.emissive[1];
            factors[6] = legacy ? material.legacySpecular[2] : material.emissive[2];
            factors[7] = legacy ? material.legacySpecular[3] : material.metallic;
            factors[8] = material.roughness;
            factors[12] = legacy ? 1.f : 0.f;
            factors[13] = material.fullbright || alpha.fullbright ? 1.f : 0.f;
            factors[15] = material.environmentIntensity;
            constexpr std::array<float, 5> identity{{0.f, 0.f, 1.f, 1.f, 0.f}};
            for (const auto [semantic, slot] : std::array<std::pair<TextureSemantic,
                     std::size_t>, 2>{{{TextureSemantic::BaseColor, 0},
                                       {TextureSemantic::Emissive, 3}}})
            {
                const MaterialTextureBinding* binding = findBinding(material, semantic);
                const auto& transform = binding ? binding->transform : identity;
                const std::size_t uv = 16 + slot * 4;
                factors[uv] = transform[0]; factors[uv + 1] = transform[1];
                factors[uv + 2] = transform[2]; factors[uv + 3] = transform[3];
                const std::size_t rotation = 32 + slot * 4;
                factors[rotation] = std::cos(transform[4]);
                factors[rotation + 1] = std::sin(transform[4]);
            }
            std::memcpy(bytes.data() + materialOffset + materialStride * item,
                        factors.data(), sizeof(factors));

            std::array<float, ALPHA_FLOATS> alphaData{{
                lighting.direction[0], lighting.direction[1], lighting.direction[2], 0.f,
                lighting.ambient[0], lighting.ambient[1], lighting.ambient[2],
                alpha.minimumAlpha,
                lighting.directional[0], lighting.directional[1],
                lighting.directional[2], legacy ? 1.f : 0.f}};
            const std::array<std::uint32_t, 4> config{{
                draws[item].route == AlphaRoute::PPLLCapture ? 1u : 0u,
                static_cast<std::uint32_t>(ppllAllocation.nodeCapacity),
                ppllAllocation.exactLayersPerPixel, 0u}};
            std::memcpy(alphaData.data() + 12, config.data(), sizeof(config));
            alphaData[16] = 0.f;
            std::memcpy(bytes.data() + alphaOffset + alphaStride * item,
                        alphaData.data(), sizeof(alphaData));
            const std::array<std::uint32_t, 4> replayConfig{{
                3u, config[1], config[2], 0u}};
            std::memcpy(alphaData.data() + 12, replayConfig.data(),
                        sizeof(replayConfig));
            std::memcpy(bytes.data() + alphaOffset +
                            alphaStride * (draws.size() + item),
                        alphaData.data(), sizeof(alphaData));
        }
        if (ppllDraws)
        {
            std::array<float, ALPHA_FLOATS> resolveData{};
            const std::array<std::uint32_t, 4> resolveConfig{{
                2u, static_cast<std::uint32_t>(ppllAllocation.nodeCapacity),
                ppllAllocation.exactLayersPerPixel, 0u}};
            std::memcpy(resolveData.data() + 12, resolveConfig.data(),
                        sizeof(resolveConfig));
            resolveData[16] = 0.f;
            std::memcpy(bytes.data() + alphaOffset +
                            alphaStride * (draws.size() * 2),
                        resolveData.data(), sizeof(resolveData));
            std::fill_n(reinterpret_cast<std::uint32_t*>(
                            bytes.data() + headOffset),
                        static_cast<std::size_t>(targets.width) * targets.height,
                        0xffffffffu);
            std::fill_n(reinterpret_cast<std::uint32_t*>(
                            bytes.data() + counterOffset), 2, 0u);
        }
        for (const Texture& texture : textures)
            if (!texture.pixels.empty())
                std::memcpy(bytes.data() + texture.uploadOffset,
                            texture.pixels.data(), texture.pixels.size());

        BufferHandle staging, vertices, indices, frames, alphaData,
                 objects, skins, materials, ppllNodes, ppllCounter;
        ImageHandle ppllHead;
        ImageViewHandle ppllHeadView;
        BindingSetHandle ppllSet, resolveFrameSet;
        PipelineHandle resolvePipeline;
        auto cleanup = [&]()
        {
            Status first = Status::success();
            for (Draw& draw : draws)
            {
                destroy(draw.frameSet, first);
                destroy(draw.replayFrameSet, first);
                destroy(draw.objectSet, first);
                destroy(draw.materialSet, first);
            }
            destroy(resolveFrameSet, first); destroy(ppllSet, first);
            destroy(staging, first); destroy(vertices, first);
            destroy(indices, first); destroy(frames, first);
            destroy(alphaData, first); destroy(objects, first);
            destroy(skins, first); destroy(materials, first);
            destroy(ppllHeadView, first); destroy(ppllHead, first);
            destroy(ppllNodes, first); destroy(ppllCounter, first);
            destroyTextures(textures, first);
            return first;
        };
        staging = mDevice.createBuffer(
            {total, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
        if (status) vertices = mDevice.createBuffer(
            {vertexBytes, ResourceUsage::Vertex | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
        if (status) indices = mDevice.createBuffer(
            {indexBytes, ResourceUsage::Index | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
        if (status) frames = mDevice.createBuffer(
            {frameStride * draws.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) alphaData = mDevice.createBuffer(
            {alphaStride * alphaSlots, ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) objects = mDevice.createBuffer(
            {objectStride * draws.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) skins = mDevice.createBuffer(
            {skinStride * draws.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status) materials = mDevice.createBuffer(
            {materialStride * draws.size(), ResourceUsage::Uniform |
             ResourceUsage::TransferDestination, MemoryClass::DeviceLocal}, status);
        if (status && ppllDraws) ppllNodes = mDevice.createBuffer(
            {ppllAllocation.nodeCapacity * ALPHA_PPLL_NODE_BYTES,
             ResourceUsage::Storage, MemoryClass::DeviceLocal}, status);
        if (status && ppllDraws) ppllCounter = mDevice.createBuffer(
            {8u, ResourceUsage::Storage | ResourceUsage::TransferDestination |
                 ResourceUsage::TransferSource, MemoryClass::DeviceLocal}, status);
        if (status && ppllDraws) ppllHead = mDevice.createImage(
            {{targets.width, targets.height, 1}, Format::R32UInt,
             ResourceUsage::Storage | ResourceUsage::TransferDestination,
             1, 1, 1}, status);
        if (status && ppllDraws) ppllHeadView = mDevice.createImageView(
            {ppllHead, Format::R32UInt,
             {ImageAspect::Color, 0, 1, 0, 1}}, status);
        if (status) status = mDevice.writeBuffer(staging, 0, bytes);

        if (status && ppllDraws)
        {
            BindingSetDesc ppllDesc;
            ppllDesc.shader = mShader;
            ppllDesc.group = 3;
            ppllDesc.resources = {
                {0, 0, ShaderPackageDesc::BindingType::StorageImage,
                 {}, 0, 0, ppllHeadView, {}},
                {1, 0, ShaderPackageDesc::BindingType::StorageBuffer,
                 ppllNodes, 0,
                 ppllAllocation.nodeCapacity * ALPHA_PPLL_NODE_BYTES,
                 {}, {}},
                {2, 0, ShaderPackageDesc::BindingType::StorageBuffer,
                 ppllCounter, 0, 8u, {}, {}}};
            ppllSet = mDevice.createBindingSet(ppllDesc, status);
            BlendState resolveBlend;
            resolveBlend.enabled = true;
            resolveBlend.sourceColor = BlendFactor::One;
            resolveBlend.destinationColor = BlendFactor::OneMinusSourceAlpha;
            resolveBlend.sourceAlpha = BlendFactor::One;
            resolveBlend.destinationAlpha = BlendFactor::OneMinusSourceAlpha;
            if (status) resolvePipeline = pipelineFor(
                resolveBlend, true, false, status);
        }

        for (std::size_t item = 0; status && item < draws.size(); ++item)
        {
            const AlphaSceneDraw& alpha = packet.draws[draws[item].source];
            const MaterialSceneDraw& geometry =
                packet.materials.draws[draws[item].source];
            const MaterialResource& material =
                packet.materials.materials[geometry.material];
            const bool noCull = material.doubleSided ||
                alpha.classification == AlphaSubmissionClass::Particle;
            draws[item].pipeline = pipelineFor(
                blendState(alpha.blend), noCull, true, status);
            if (alpha.emissive)
                draws[item].replayPipeline = pipelineFor(
                    emissiveBlendState(), noCull, true, status);
            BindingSetDesc frameDesc;
            frameDesc.shader = mShader;
            frameDesc.group = 0;
            frameDesc.resources = {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 frames, frameStride * item, 16 * sizeof(float), {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 alphaData, alphaStride * item,
                 ALPHA_FLOATS * sizeof(float), {}, {}}};
            draws[item].frameSet = mDevice.createBindingSet(frameDesc, status);
            frameDesc.resources[1].bufferOffset =
                alphaStride * (draws.size() + item);
            if (status && alpha.emissive)
                draws[item].replayFrameSet =
                    mDevice.createBindingSet(frameDesc, status);
            BindingSetDesc objectDesc;
            objectDesc.shader = mShader;
            objectDesc.group = 1;
            objectDesc.resources = {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 objects, objectStride * item,
                 OBJECT_FLOATS * sizeof(float), {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 skins, skinStride * item, MATERIAL_SKIN_BYTES, {}, {}}};
            if (status) draws[item].objectSet =
                mDevice.createBindingSet(objectDesc, status);
            BindingSetDesc materialDesc;
            materialDesc.shader = mShader;
            materialDesc.group = 2;
            materialDesc.resources = {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 materials, materialStride * item,
                 MATERIAL_FLOATS * sizeof(float), {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                 {}, 0, 0, draws[item].baseView, mSampler},
                {4, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
                 {}, 0, 0, draws[item].emissiveView, mSampler}};
            if (status) draws[item].materialSet =
                mDevice.createBindingSet(materialDesc, status);
        }
        if (status && ppllDraws)
        {
            BindingSetDesc resolveDesc;
            resolveDesc.shader = mShader;
            resolveDesc.group = 0;
            resolveDesc.resources = {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 frames, 0, 16 * sizeof(float), {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 alphaData, alphaStride * (draws.size() * 2),
                 ALPHA_FLOATS * sizeof(float), {}, {}}};
            resolveFrameSet = mDevice.createBindingSet(resolveDesc, status);
        }
        if (!status) { cleanup(); return status; }

        CommandContext& commands = mDevice.commandContext();
        bool frame = false;
        bool rendering = false;
        status = commands.beginFrame();
        frame = status.ok();
        const auto copy = [&](BufferHandle target, std::uint64_t offset,
                              std::uint64_t size)
        {
            const std::array<BufferCopyRegion, 1> regions{{{offset, 0, size}}};
            return commands.copyBuffer(staging, target, regions);
        };
        if (status) status = copy(vertices, vertexOffset, vertexBytes);
        if (status) status = copy(indices, indexOffset, indexBytes);
        if (status) status = copy(frames, frameOffset, frameStride * draws.size());
        if (status) status = copy(alphaData, alphaOffset,
                                  alphaStride * alphaSlots);
        if (status) status = copy(objects, objectOffset,
                                  objectStride * draws.size());
        if (status) status = copy(skins, skinOffset, skinStride * draws.size());
        if (status) status = copy(materials, materialOffset,
                                  materialStride * draws.size());
        for (Texture& texture : textures)
        {
            if (!status || !texture.image) continue;
            BufferImageCopyRegion region;
            region.bufferOffset = texture.uploadOffset;
            region.imageSubresource = {ImageAspect::Color, 0, 0, 1};
            region.imageExtent = {texture.width, texture.height, 1};
            const std::array<BufferImageCopyRegion, 1> regions{{region}};
            status = commands.copyBufferToImage(staging, texture.image, regions);
        }
        if (status && ppllDraws)
        {
            status = copy(ppllCounter, counterOffset, 8u);
            BufferImageCopyRegion headCopy;
            headCopy.bufferOffset = headOffset;
            headCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
            headCopy.imageExtent = {targets.width, targets.height, 1};
            const std::array<BufferImageCopyRegion, 1> copies{{headCopy}};
            if (status) status = commands.copyBufferToImage(
                staging, ppllHead, copies);
        }
        if (status)
        {
            BufferImageCopyRegion region;
            region.imageSubresource = {ImageAspect::Color, 0, 0, 1};
            region.imageExtent = {targets.width, targets.height, 1};
            const std::array<BufferImageCopyRegion, 1> regions{{region}};
            status = commands.copyImageToBuffer(
                targets.lightingImage, mInputReadback, regions);
        }
        const auto beginPass = [&](bool depth, std::uint64_t semanticId)
        {
            RenderingInfo info;
            info.semanticId = semanticId;
            info.width = targets.width;
            info.height = targets.height;
            info.colors.push_back({targets.lightingView, COLOR_FORMAT,
                LoadOp::Load, StoreOp::Store, {}});
            if (depth)
                info.depthStencil = AttachmentDesc{
                    targets.depthView, DEPTH_FORMAT, LoadOp::Load,
                    StoreOp::Store, {}};
            Status begun = commands.beginRendering(info);
            if (begun) begun = commands.setViewport({
                0.f, 0.f, static_cast<float>(targets.width),
                static_cast<float>(targets.height), 0.f, 1.f});
            if (begun) begun = commands.setScissor(
                {0, 0, targets.width, targets.height});
            rendering = begun.ok();
            return begun;
        };
        const auto endPass = [&]()
        {
            if (!rendering) return Status::success();
            rendering = false;
            return commands.endRendering();
        };
        const auto issue = [&](const Draw& executable, bool replay)
        {
            const MaterialSceneDraw& draw =
                packet.materials.draws[executable.source];
            Status issued = commands.bindPipeline(
                replay ? executable.replayPipeline : executable.pipeline);
            if (issued) issued = commands.bindVertexBuffer(0, vertices, 0);
            if (issued) issued = commands.bindIndexBuffer(
                indices, 0, IndexType::UInt32);
            if (issued) issued = commands.bindBindingSet(
                0, replay ? executable.replayFrameSet : executable.frameSet);
            if (issued) issued = commands.bindBindingSet(1, executable.objectSet);
            if (issued) issued = commands.bindBindingSet(2, executable.materialSet);
            if (issued && ppllDraws)
                issued = commands.bindBindingSet(3, ppllSet);
            if (issued) issued = commands.drawIndexed(
                {draw.indexCount, 1, draw.firstIndex, 0, 0});
            return issued;
        };

        if (status && ppllDraws)
        {
            status = beginPass(true, 0x50306533645f4341ull); // "P0e3d_CA"
            for (const Draw& draw : draws)
                if (status && draw.route == AlphaRoute::PPLLCapture)
                    status = issue(draw, false);
            const Status captured = endPass();
            if (status && !captured) status = captured;
            if (status) status = commands.resourceBarrier(
                ResourceBarrier::StorageWriteToRead);

            if (status) status = beginPass(false, 0x50306533645f5253ull); // "P0e3d_RS"
            if (status) status = commands.bindPipeline(resolvePipeline);
            if (status) status = commands.bindVertexBuffer(0, vertices, 0);
            if (status) status = commands.bindIndexBuffer(
                indices, 0, IndexType::UInt32);
            if (status) status = commands.bindBindingSet(0, resolveFrameSet);
            if (status) status = commands.bindBindingSet(1, draws[0].objectSet);
            if (status) status = commands.bindBindingSet(2, draws[0].materialSet);
            if (status) status = commands.bindBindingSet(3, ppllSet);
            if (status) status = commands.drawIndexed(
                {3, 1, resolveFirstIndex, 0, 0});
            const Status resolved = endPass();
            if (status && !resolved) status = resolved;
        }

        if (status) status = beginPass(true, 0x50306533645f4c52ull); // "P0e3d_LR"
        for (const Draw& draw : draws)
            if (status && draw.route != AlphaRoute::PPLLCapture)
                status = issue(draw, false);
        for (const Draw& draw : draws)
            if (status && packet.draws[draw.source].emissive)
                status = issue(draw, true);
        const Status legacyEnded = endPass();
        if (status && !legacyEnded) status = legacyEnded;

        if (status)
        {
            BufferImageCopyRegion region;
            region.imageSubresource = {ImageAspect::Color, 0, 0, 1};
            region.imageExtent = {targets.width, targets.height, 1};
            const std::array<BufferImageCopyRegion, 1> regions{{region}};
            status = commands.copyImageToBuffer(
                targets.lightingImage, mReadback, regions);
        }
            if (status && ppllDraws)
            {
                const std::array<BufferCopyRegion, 1> counterCopy{{{0, 0, 8u}}};
                status = commands.copyBuffer(
                ppllCounter, mPPLLCounterReadback, counterCopy);
            }
        if (frame)
        {
            const Status ended = commands.endFrame();
            if (status && !ended) status = ended;
        }
        const Status cleaned = cleanup();
        if (!status) return status;
        if (!cleaned) return cleaned;

        mResult = {};
        mResult.frameId = packet.frameId;
        mResult.sceneEpoch = packet.sceneEpoch;
        mResult.resourceEpoch = packet.resourceEpoch;
        mResult.targetGeneration = targets.generation;
        mResult.maskDraws = maskDraws;
        mResult.sortedDraws = sortedDraws;
        mResult.residualDraws = residualDraws;
        mResult.ppllDraws = ppllDraws;
        mResult.emissiveReplays = emissiveReplays;
        mResult.deferredDraws = deferredDraws;
        mResult.deferredRouteOrMaterialDraws = deferredRouteOrMaterialDraws;
        mResult.deferredSkinDraws = deferredSkinDraws;
        mResult.deferredTextureDraws = deferredTextureDraws;
        mResult.uploadBytes = total;
        mResult.ppllNodeCapacity = ppllDraws
            ? ppllAllocation.nodeCapacity : 0;
        mResult.ppllExactLayers = ppllDraws
            ? ppllAllocation.exactLayersPerPixel : 0;
        mResult.ppllAvailable = ppllAvailable;
        mResult.packetSha256 = alphaScenePacketSha256(packet);
        mPending = true;
        return Status::success();
    }

    Status poll(ProductionAlphaResult& result)
    {
        result = {};
        if (!mPending)
            return Status::failure(StatusCode::InvalidState,
                                   "production alpha executor has no pending result");
        std::vector<std::byte> input(
            static_cast<std::size_t>(mWidth) * mHeight * COLOR_BYTES);
        std::vector<std::byte> pixels(input.size());
        Status status = mDevice.readBuffer(mInputReadback, 0, input);
        if (!status) return status;
        status = mDevice.readBuffer(mReadback, 0, pixels);
        if (!status) return status;
        if (mResult.ppllDraws)
        {
            std::array<std::byte, 8> counters{};
            status = mDevice.readBuffer(mPPLLCounterReadback, 0, counters);
            if (!status) return status;
            std::memcpy(&mResult.ppllAllocatedNodes, counters.data(), 4u);
            std::memcpy(&mResult.ppllOverflowFragments,
                        counters.data() + 4u, 4u);
        }
        if (mDevice.backend() == Backend::OpenGL)
        {
            const std::size_t row =
                static_cast<std::size_t>(mWidth) * COLOR_BYTES;
            for (std::uint32_t y = 0; y < mHeight / 2; ++y)
            {
                std::swap_ranges(pixels.begin() + y * row,
                    pixels.begin() + (y + 1) * row,
                    pixels.begin() + (mHeight - 1 - y) * row);
                std::swap_ranges(input.begin() + y * row,
                    input.begin() + (y + 1) * row,
                    input.begin() + (mHeight - 1 - y) * row);
            }
        }
        mResult.colorSha256 = sha256(pixels);
        for (std::size_t pixel = 0;
             pixel < static_cast<std::size_t>(mWidth) * mHeight; ++pixel)
        {
            const auto begin = pixels.begin() + pixel * COLOR_BYTES;
            const auto before = input.begin() + pixel * COLOR_BYTES;
            if (!std::equal(begin, begin + COLOR_BYTES, before))
                ++mResult.modifiedPixels;
        }
        result = std::move(mResult);
        mResult = {};
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
        destroy(mReadback, first); destroy(mInputReadback, first);
        destroy(mPPLLCounterReadback, first);
        for (Pipeline& pipeline : mPipelines) destroy(pipeline.handle, first);
        destroy(mSampler, first); destroy(mShader, first);
        for (auto& view : mFallbackViews) destroy(view, first);
        for (auto& image : mFallbackImages) destroy(image, first);
        return first;
    }

private:
    struct Pipeline
    {
        BlendState blend;
        bool noCull = false;
        bool depthTest = true;
        PipelineHandle handle;
    };

    template<typename Handle>
    void destroy(Handle& handle, Status& first)
    {
        if (!handle) return;
        const Status status = mDevice.destroy(handle);
        if (first && !status) first = status;
        handle = {};
    }

    void destroyTextures(std::vector<Texture>& textures)
    {
        Status ignored = Status::success();
        destroyTextures(textures, ignored);
    }

    void destroyTextures(std::vector<Texture>& textures, Status& first)
    {
        for (Texture& texture : textures)
        {
            destroy(texture.view, first);
            destroy(texture.image, first);
        }
    }

    Status createTextures(std::vector<Texture>& textures)
    {
        Status status = Status::success();
        for (Texture& texture : textures)
        {
            if (texture.pixels.empty()) continue;
            texture.image = mDevice.createImage({
                {texture.width, texture.height, 1}, texture.format,
                ResourceUsage::Sampled | ResourceUsage::TransferDestination,
                1, 1, 1}, status);
            if (status) texture.view = mDevice.createImageView({
                texture.image, texture.format,
                {ImageAspect::Color, 0, 1, 0, 1}}, status);
            if (!status)
            {
                destroyTextures(textures);
                return status;
            }
        }
        return status;
    }

    PipelineHandle pipelineFor(const BlendState& blend, bool noCull,
                               bool depthTest, Status& status)
    {
        const auto found = std::find_if(mPipelines.begin(), mPipelines.end(),
            [&](const Pipeline& pipeline)
            { return pipeline.blend == blend && pipeline.noCull == noCull &&
                     pipeline.depthTest == depthTest; });
        if (found != mPipelines.end()) return found->handle;
        PipelineDesc desc;
        desc.shader = mShader;
        desc.cullMode = noCull ? CullMode::None : CullMode::Back;
        desc.depthTest = depthTest;
        desc.depthWrite = false;
        desc.depthCompare = CompareOp::GreaterEqual;
        desc.colorFormats = {COLOR_FORMAT};
        if (depthTest) desc.depthStencilFormat = DEPTH_FORMAT;
        desc.blendStates = {blend};
        desc.vertexBuffers = {{
            0, sizeof(MaterialSceneVertex), VertexInputRate::PerVertex}};
        desc.vertexAttributes = {
            {0, 0, VertexFormat::Float32x3,
             offsetof(MaterialSceneVertex, position)},
            {1, 0, VertexFormat::Float32x3,
             offsetof(MaterialSceneVertex, normal)},
            {3, 0, VertexFormat::Float32x2,
             offsetof(MaterialSceneVertex, texCoord)},
            {4, 0, VertexFormat::UNorm8x4,
             offsetof(MaterialSceneVertex, color)},
            {5, 0, VertexFormat::UInt16x4,
             offsetof(MaterialSceneVertex, joints)},
            {6, 0, VertexFormat::Float32x4,
             offsetof(MaterialSceneVertex, weights)}};
        Pipeline created{blend, noCull, depthTest,
                         mDevice.createPipeline(desc, status)};
        if (!status) return {};
        mPipelines.push_back(created);
        return created.handle;
    }

    Status ensureReadbacks(const ProductionFrameTargetSet& targets)
    {
        if (mGeneration == targets.generation &&
            mWidth == targets.width && mHeight == targets.height)
            return Status::success();
        Status first = Status::success();
        destroy(mReadback, first); destroy(mInputReadback, first);
        destroy(mPPLLCounterReadback, first);
        if (!first) return first;
        const std::uint64_t bytes =
            static_cast<std::uint64_t>(targets.width) * targets.height *
            COLOR_BYTES;
        Status status = Status::success();
        mReadback = mDevice.createBuffer(
            {bytes, ResourceUsage::TransferDestination,
             MemoryClass::Readback}, status);
        if (status) mInputReadback = mDevice.createBuffer(
            {bytes, ResourceUsage::TransferDestination,
             MemoryClass::Readback}, status);
        if (status) mPPLLCounterReadback = mDevice.createBuffer(
            {8u, ResourceUsage::TransferDestination,
             MemoryClass::Readback}, status);
        if (!status)
        {
            Status ignored = Status::success();
            destroy(mReadback, ignored); destroy(mInputReadback, ignored);
            destroy(mPPLLCounterReadback, ignored);
            return status;
        }
        mGeneration = targets.generation;
        mWidth = targets.width;
        mHeight = targets.height;
        return status;
    }

    Status initialize()
    {
        if (mShader) return Status::success();
        if (mShutdown)
            return Status::failure(StatusCode::InvalidState,
                                   "production alpha executor is shut down");
        Status status = Status::success();
        mShader = mDevice.createShaderPackage(mShaderPackage, status);
        SamplerDesc repeat;
        repeat.minFilter = repeat.magFilter = Filter::Linear;
        repeat.addressU = repeat.addressV = AddressMode::Repeat;
        if (status) mSampler = mDevice.createSampler(repeat, status);
        constexpr std::array<std::array<std::uint8_t, 4>, 2> fallback{{
            {{255, 255, 255, 255}}, {{0, 0, 0, 255}}}};
        BufferHandle upload;
        if (status) upload = mDevice.createBuffer(
            {8, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
        std::array<std::byte, 8> bytes{};
        for (std::size_t texture = 0; texture < fallback.size(); ++texture)
            for (std::size_t component = 0; component < 4; ++component)
                bytes[texture * 4 + component] =
                    static_cast<std::byte>(fallback[texture][component]);
        if (status) status = mDevice.writeBuffer(upload, 0, bytes);
        for (std::size_t texture = 0; status && texture < fallback.size(); ++texture)
        {
            mFallbackImages[texture] = mDevice.createImage({
                {1, 1, 1}, Format::RGBA8SRGB,
                ResourceUsage::Sampled | ResourceUsage::TransferDestination,
                1, 1, 1}, status);
            if (status) mFallbackViews[texture] = mDevice.createImageView({
                mFallbackImages[texture], Format::RGBA8SRGB,
                {ImageAspect::Color, 0, 1, 0, 1}}, status);
        }
        if (status)
        {
            CommandContext& commands = mDevice.commandContext();
            bool begun = false;
            status = commands.beginFrame();
            begun = status.ok();
            for (std::size_t texture = 0;
                 status && texture < fallback.size(); ++texture)
            {
                BufferImageCopyRegion copy;
                copy.bufferOffset = texture * 4;
                copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
                copy.imageExtent = {1, 1, 1};
                const std::array<BufferImageCopyRegion, 1> copies{{copy}};
                status = commands.copyBufferToImage(
                    upload, mFallbackImages[texture], copies);
            }
            if (begun)
            {
                const Status ended = commands.endFrame();
                if (status && !ended) status = ended;
            }
        }
        Status retired = Status::success();
        destroy(upload, retired);
        if (status && !retired) status = retired;
        if (!status)
        {
            const Status failure = status;
            shutdown();
            return failure;
        }
        return status;
    }

    Device& mDevice;
    ShaderPackageDesc mShaderPackage;
    ShaderPackageHandle mShader;
    SamplerHandle mSampler;
    std::array<ImageHandle, 2> mFallbackImages{};
    std::array<ImageViewHandle, 2> mFallbackViews{};
    std::vector<Pipeline> mPipelines;
    BufferHandle mReadback;
    BufferHandle mInputReadback;
    BufferHandle mPPLLCounterReadback;
    std::uint64_t mGeneration = 0;
    std::uint32_t mWidth = 0;
    std::uint32_t mHeight = 0;
    ProductionAlphaResult mResult;
    bool mPending = false;
    bool mShutdown = false;
};

ProductionAlphaExecutor::ProductionAlphaExecutor(
    Device& device, ShaderPackageDesc shader) :
    mImpl(std::make_unique<Impl>(device, std::move(shader)))
{
}

ProductionAlphaExecutor::~ProductionAlphaExecutor() = default;

Status ProductionAlphaExecutor::submit(
    const AlphaScenePacket& packet,
    const ProductionFrameTargetSet& targets,
    const ProductionAlphaLighting& lighting,
    const ProductionAlphaLimits& limits)
{
    return mImpl->submit(packet, targets, lighting, limits);
}

Status ProductionAlphaExecutor::poll(ProductionAlphaResult& result)
{
    return mImpl->poll(result);
}

bool ProductionAlphaExecutor::pending() const { return mImpl->pending(); }
Status ProductionAlphaExecutor::shutdown() { return mImpl->shutdown(); }
} // namespace LL::GHI