/**
 * @file llghishaderpackage.cpp
 * @brief Runtime decoder for offline-built GHI shader packages.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llghishaderpackage.h"

#include <boost/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <set>
#include <utility>
#include <vector>

namespace LL::GHI
{
namespace
{

using JsonObject = boost::json::object;

const JsonObject& objectAt(const JsonObject& object, std::string_view key)
{
    return object.at(key).as_object();
}

const boost::json::array& arrayAt(const JsonObject& object, std::string_view key)
{
    return object.at(key).as_array();
}

std::string stringAt(const JsonObject& object, std::string_view key)
{
    return std::string(object.at(key).as_string());
}

std::uint64_t unsignedAt(const JsonObject& object, std::string_view key)
{
    const auto& value = object.at(key);
    if (value.is_uint64()) return value.as_uint64();
    if (value.is_int64() && value.as_int64() >= 0)
    {
        return static_cast<std::uint64_t>(value.as_int64());
    }
    throw std::runtime_error(std::string(key) + " must be an unsigned integer");
}

template<typename Integer>
Integer narrowUnsigned(const JsonObject& object, std::string_view key)
{
    const std::uint64_t value = unsignedAt(object, key);
    if (value > std::numeric_limits<Integer>::max())
    {
        throw std::runtime_error(std::string(key) + " is out of range");
    }
    return static_cast<Integer>(value);
}

unsigned char hexNibble(char value)
{
    if (value >= '0' && value <= '9') return static_cast<unsigned char>(value - '0');
    value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    if (value >= 'a' && value <= 'f') return static_cast<unsigned char>(value - 'a' + 10);
    throw std::runtime_error("hash contains a non-hexadecimal character");
}

std::array<std::uint8_t, 32> decodeHash(std::string_view encoded)
{
    if (encoded.size() != 64) throw std::runtime_error("SHA-256 must contain 64 hexadecimal characters");
    std::array<std::uint8_t, 32> result{};
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        result[index] = static_cast<std::uint8_t>(
            (hexNibble(encoded[index * 2]) << 4) | hexNibble(encoded[index * 2 + 1]));
    }
    return result;
}

std::array<std::uint8_t, 32> digest(std::string_view data)
{
    std::array<std::uint8_t, EVP_MAX_MD_SIZE> storage{};
    unsigned int size = 0;
    if (EVP_Digest(data.data(), data.size(), storage.data(), &size, EVP_sha256(), nullptr) != 1 || size != 32)
    {
        throw std::runtime_error("unable to calculate shader artifact SHA-256");
    }
    std::array<std::uint8_t, 32> result{};
    std::copy_n(storage.begin(), result.size(), result.begin());
    return result;
}

std::vector<std::uint8_t> decodeBase64(std::string_view encoded)
{
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (encoded.empty() || encoded.size() % 4 != 0)
    {
        throw std::runtime_error("base64 SPIR-V payload has invalid length");
    }
    std::vector<std::uint8_t> result;
    result.reserve(encoded.size() / 4 * 3);
    for (std::size_t offset = 0; offset < encoded.size(); offset += 4)
    {
        std::array<unsigned int, 4> values{};
        unsigned int padding = 0;
        for (std::size_t index = 0; index < 4; ++index)
        {
            const char character = encoded[offset + index];
            if (character == '=')
            {
                if (index < 2 || offset + 4 != encoded.size())
                    throw std::runtime_error("base64 SPIR-V payload has invalid padding");
                values[index] = 0;
                ++padding;
                continue;
            }
            if (padding) throw std::runtime_error("base64 SPIR-V data follows padding");
            const std::size_t position = alphabet.find(character);
            if (position == std::string_view::npos)
                throw std::runtime_error("base64 SPIR-V payload has an invalid character");
            values[index] = static_cast<unsigned int>(position);
        }
        if (padding > 2) throw std::runtime_error("base64 SPIR-V payload has excessive padding");
        const std::uint32_t value =
            (values[0] << 18) | (values[1] << 12) | (values[2] << 6) | values[3];
        result.push_back(static_cast<std::uint8_t>(value >> 16));
        if (padding < 2) result.push_back(static_cast<std::uint8_t>(value >> 8));
        if (padding < 1) result.push_back(static_cast<std::uint8_t>(value));
    }
    return result;
}

ShaderPackageDesc::Stage decodeStage(std::string_view value)
{
    if (value == "vertex") return ShaderPackageDesc::Stage::Vertex;
    if (value == "fragment") return ShaderPackageDesc::Stage::Fragment;
    if (value == "compute") return ShaderPackageDesc::Stage::Compute;
    throw std::runtime_error("unknown shader stage");
}

ShaderPackageDesc::TargetProfile decodeTarget(std::string_view value)
{
    if (value == "opengl_41") return ShaderPackageDesc::TargetProfile::OpenGL41;
    if (value == "opengl_46") return ShaderPackageDesc::TargetProfile::OpenGL46;
    if (value == "vulkan_spirv_1_3") return ShaderPackageDesc::TargetProfile::VulkanSpirV13;
    throw std::runtime_error("unknown shader target profile");
}

ShaderPackageDesc::BindingType decodeBindingType(std::string_view value)
{
    using Type = ShaderPackageDesc::BindingType;
    if (value == "uniform_buffer") return Type::UniformBuffer;
    if (value == "storage_buffer") return Type::StorageBuffer;
    if (value == "sampler") return Type::Sampler;
    if (value == "sampled_image") return Type::SampledImage;
    if (value == "combined_image_sampler") return Type::CombinedImageSampler;
    if (value == "storage_image") return Type::StorageImage;
    throw std::runtime_error("unknown shader binding type");
}

ShaderValueType decodeShaderValueType(std::string_view value)
{
    if (value == "float") return ShaderValueType::Float;
    if (value == "float2") return ShaderValueType::Float2;
    if (value == "float3") return ShaderValueType::Float3;
    if (value == "float4") return ShaderValueType::Float4;
    if (value == "uint") return ShaderValueType::UInt;
    if (value == "uint2") return ShaderValueType::UInt2;
    if (value == "uint3") return ShaderValueType::UInt3;
    if (value == "uint4") return ShaderValueType::UInt4;
    if (value == "sint") return ShaderValueType::SInt;
    if (value == "sint2") return ShaderValueType::SInt2;
    if (value == "sint3") return ShaderValueType::SInt3;
    if (value == "sint4") return ShaderValueType::SInt4;
    throw std::runtime_error("unknown reflected shader value type");
}

ShaderPackageDesc::StageVisibility decodeVisibility(const boost::json::array& stages)
{
    using Visibility = ShaderPackageDesc::StageVisibility;
    Visibility result = Visibility::None;
    for (const auto& stage : stages)
    {
        const std::string value(stage.as_string());
        if (value == "vertex") result = result | Visibility::Vertex;
        else if (value == "fragment") result = result | Visibility::Fragment;
        else if (value == "compute") result = result | Visibility::Compute;
        else throw std::runtime_error("unknown binding stage visibility");
    }
    return result;
}

} // namespace

Status decodeShaderPackage(std::string_view encoded, ShaderPackageDesc& package)
{
    try
    {
        boost::system::error_code error;
        boost::json::value parsed = boost::json::parse(encoded, error);
        if (error) throw std::runtime_error("invalid shader package JSON: " + error.message());
        const JsonObject& root = parsed.as_object();

        ShaderPackageDesc decoded;
        decoded.schemaVersion = narrowUnsigned<std::uint32_t>(root, "schema_version");
        if (decoded.schemaVersion != ShaderPackageDesc::CURRENT_SCHEMA_VERSION)
            throw std::runtime_error("unsupported shader package schema version");
        decoded.semanticHash = decodeHash(stringAt(root, "semantic_hash"));
        decoded.toolchainHash = decodeHash(stringAt(root, "toolchain_hash"));
        if (std::all_of(decoded.semanticHash.begin(), decoded.semanticHash.end(),
                        [](std::uint8_t value) { return value == 0; }) ||
            std::all_of(decoded.toolchainHash.begin(), decoded.toolchainHash.end(),
                        [](std::uint8_t value) { return value == 0; }))
        {
            throw std::runtime_error("shader package identity hashes must not be zero");
        }

        std::set<ShaderPackageDesc::Stage> seenStages;
        for (const auto& stageValue : arrayAt(root, "stages"))
        {
            const JsonObject& stageObject = stageValue.as_object();
            ShaderPackageDesc::StageArtifact stage;
            stage.stage = decodeStage(stringAt(stageObject, "stage"));
            if (!seenStages.insert(stage.stage).second)
                throw std::runtime_error("shader package contains a duplicate stage");
            stage.entryPoint = stringAt(stageObject, "entry_point");
            if (stage.entryPoint.empty()) throw std::runtime_error("shader entry point must not be empty");
            std::set<ShaderPackageDesc::TargetProfile> seenTargets;
            for (const auto& artifactValue : arrayAt(stageObject, "artifacts"))
            {
                const JsonObject& artifactObject = artifactValue.as_object();
                ShaderPackageDesc::CodeArtifact artifact;
                artifact.target = decodeTarget(stringAt(artifactObject, "target"));
                if (!seenTargets.insert(artifact.target).second)
                    throw std::runtime_error("shader stage contains a duplicate target profile");
                artifact.artifactHash = decodeHash(stringAt(artifactObject, "artifact_hash"));
                const std::string encoding = stringAt(artifactObject, "encoding");
                std::string artifactBytes;
                if (artifact.target == ShaderPackageDesc::TargetProfile::VulkanSpirV13)
                {
                    if (encoding != "base64") throw std::runtime_error("SPIR-V artifact must use base64 encoding");
                    const std::vector<std::uint8_t> bytes = decodeBase64(stringAt(artifactObject, "spirv"));
                    artifactBytes.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                    if (bytes.size() < 20 || bytes.size() % sizeof(std::uint32_t) != 0)
                        throw std::runtime_error("SPIR-V artifact has invalid size");
                    artifact.spirv.resize(bytes.size() / sizeof(std::uint32_t));
                    for (std::size_t index = 0; index < artifact.spirv.size(); ++index)
                    {
                        const std::size_t offset = index * 4;
                        artifact.spirv[index] = static_cast<std::uint32_t>(bytes[offset]) |
                            (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                            (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
                            (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
                    }
                    if (artifact.spirv.front() != 0x07230203u)
                        throw std::runtime_error("SPIR-V artifact has invalid magic");
                }
                else
                {
                    if (encoding != "utf8") throw std::runtime_error("OpenGL artifact must use UTF-8 encoding");
                    artifact.source = stringAt(artifactObject, "source");
                    if (artifact.source.empty()) throw std::runtime_error("OpenGL artifact is empty");
                    artifactBytes = artifact.source;
                }
                if (digest(artifactBytes) != artifact.artifactHash)
                    throw std::runtime_error("shader artifact SHA-256 mismatch");
                stage.artifacts.push_back(std::move(artifact));
            }
            if (stage.artifacts.empty()) throw std::runtime_error("shader stage has no target artifacts");
            decoded.stages.push_back(std::move(stage));
        }
        if (decoded.stages.empty()) throw std::runtime_error("shader package has no stages");

        std::set<std::pair<std::uint8_t, std::uint16_t>> seenBindings;
        for (const auto& bindingValue : arrayAt(root, "bindings"))
        {
            const JsonObject& bindingObject = bindingValue.as_object();
            ShaderPackageDesc::Binding binding{
                narrowUnsigned<std::uint8_t>(bindingObject, "group"),
                narrowUnsigned<std::uint16_t>(bindingObject, "binding"),
                decodeBindingType(stringAt(bindingObject, "type")),
                decodeVisibility(arrayAt(bindingObject, "stages")),
                narrowUnsigned<std::uint16_t>(bindingObject, "array_count"),
                bindingObject.at("dynamic_offset").as_bool(),
                stringAt(bindingObject, "name"),
            };
            if (!seenBindings.emplace(binding.group, binding.binding).second)
                throw std::runtime_error("shader package contains a duplicate binding");
            if (binding.arrayCount == 0 || binding.name.empty() ||
                binding.visibility == ShaderPackageDesc::StageVisibility::None)
                throw std::runtime_error("shader package contains an incomplete binding");
            decoded.bindings.push_back(std::move(binding));
        }
        std::set<std::uint16_t> seenLocations;
        for (const auto& inputValue : arrayAt(root, "vertex_inputs"))
        {
            const JsonObject& inputObject = inputValue.as_object();
            const std::uint16_t location = narrowUnsigned<std::uint16_t>(inputObject, "location");
            if (!seenLocations.insert(location).second)
                throw std::runtime_error("shader package contains a duplicate vertex input location");
            decoded.vertexInputs.push_back({
                location,
                decodeShaderValueType(stringAt(inputObject, "type")),
            });
        }
        std::set<std::uint16_t> seenOutputLocations;
        for (const auto& outputValue : arrayAt(root, "fragment_outputs"))
        {
            const JsonObject& outputObject = outputValue.as_object();
            const std::uint16_t location = narrowUnsigned<std::uint16_t>(
                outputObject, "location");
            if (!seenOutputLocations.insert(location).second)
                throw std::runtime_error(
                    "shader package contains a duplicate fragment output location");
            decoded.fragmentOutputs.push_back({
                location,
                decodeShaderValueType(stringAt(outputObject, "type")),
            });
        }
        decoded.pushConstantBytes = narrowUnsigned<std::uint16_t>(root, "push_constant_bytes");
        if ((decoded.pushConstantBytes & 3u) != 0)
            throw std::runtime_error("push-constant byte count must be four-byte aligned");
        package = std::move(decoded);
        return Status::success();
    }
    catch (const std::exception& error)
    {
        return Status::failure(StatusCode::InvalidArgument, error.what());
    }
}

Status loadShaderPackage(const std::filesystem::path& path, ShaderPackageDesc& package)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return Status::failure(StatusCode::InvalidArgument, "unable to open shader package: " + path.string());
    const std::string encoded((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof())
        return Status::failure(StatusCode::InvalidArgument, "unable to read shader package: " + path.string());
    return decodeShaderPackage(encoded, package);
}

} // namespace LL::GHI
