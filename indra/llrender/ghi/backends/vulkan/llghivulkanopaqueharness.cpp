/**
 * @file llghivulkanopaqueharness.cpp
 * @brief Standalone Vulkan execution harness for the R4 opaque fixture.
 */

#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghiopaquefixture.h"
#include "tests/ghi/llghiopaquescenefixture.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifndef LL_GHI_R4_SHADER_PACKAGE
#error LL_GHI_R4_SHADER_PACKAGE must name the packaged R4 opaque shader
#endif

int main(int argc, char** argv)
{
    bool validation = false;
    std::string packetPath;
    std::string dumpPrefix;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--validation") validation = true;
        else if (std::string(argv[i]) == "--packet" && i + 1 < argc)
            packetPath = argv[++i];
        else if (std::string(argv[i]) == "--dump-prefix" && i + 1 < argc)
            dumpPrefix = argv[++i];
    }

    LL::GHI::ShaderPackageDesc shaderPackage;
    LL::GHI::Status status = LL::GHI::loadShaderPackage(
        LL_GHI_R4_SHADER_PACKAGE, shaderPackage);
    if (!status)
    {
        std::cerr << status.message() << '\n';
        return 2;
    }
    LL::GHI::DeviceCreateInfo createInfo;
    createInfo.backend = LL::GHI::Backend::Vulkan;
    createInfo.framesInFlight = 2;
    createInfo.enableValidation = validation;
    auto creation = LL::GHI::createDevice(createInfo);
    if (!creation.status)
    {
        std::cerr << creation.status.message() << '\n';
        return 3;
    }
    if (!packetPath.empty())
    {
        std::ifstream input(packetPath, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
        std::vector<std::byte> encoded(raw.size());
        std::transform(raw.begin(), raw.end(), encoded.begin(),
            [](char value) { return static_cast<std::byte>(value); });
        LL::GHI::OpaqueScenePacket packet;
        status = LL::GHI::decodeOpaqueScenePacket(encoded, packet);
        if (!input || !status)
        {
            std::cerr << (status ? "could not read scene packet" : status.message()) << '\n';
            return 4;
        }
        const auto scene = LL::GHI::Test::runOpaqueSceneFixture(
            *creation.device, shaderPackage, packet);
        if (!scene.passed)
        {
            std::cerr << scene.message << '\n';
            return 4;
        }
        std::cout << scene.message << " backend=Vulkan validation="
                  << (validation ? "on" : "off") << " packet-sha256="
                  << LL::GHI::sha256(encoded) << " draws="
                  << packet.statistics.capturedDraws << " triangles="
                  << packet.statistics.capturedTriangles << " submitted-draws="
                  << packet.statistics.submittedDraws << " submitted-triangles="
                  << packet.statistics.submittedTriangles << " source="
                  << packet.sourceWidth << "x" << packet.sourceHeight << " frame="
                  << packet.frameId << " production-occlusion="
                  << (packet.productionOcclusionEnabled ? "on" : "off")
                  << " skipped-rigged="
                  << packet.statistics.skippedRiggedDraws << " skipped-material="
                  << packet.statistics.skippedMaterialDraws << " invalid="
                  << packet.statistics.invalidDraws;
        for (std::size_t target = 0; target < scene.colorSha256.size(); ++target)
        {
            std::cout << " target" << target << "-sha256=" << scene.colorSha256[target]
                      << " target" << target << "-coverage=" << scene.nonClearPixels[target];
            if (!dumpPrefix.empty())
            {
                std::ofstream dump(dumpPrefix + "-target" + std::to_string(target) + ".bin",
                                   std::ios::binary);
                dump.write(reinterpret_cast<const char*>(scene.colorPixels[target].data()),
                           static_cast<std::streamsize>(scene.colorPixels[target].size()));
            }
        }
        std::cout << '\n';
        return 0;
    }
    const auto fixture = LL::GHI::Test::runOpaqueFixture(*creation.device, shaderPackage);
    if (!fixture.passed)
    {
        std::cerr << fixture.message << '\n';
        return 4;
    }
    std::cout << fixture.message << " backend=Vulkan validation="
              << (validation ? "on" : "off") << " depth-stencil="
              << static_cast<int>(fixture.depthStencilFormat);
    for (std::size_t target = 0; target < fixture.colorSha256.size(); ++target)
        std::cout << " target" << target << "-sha256="
                  << fixture.colorSha256[target];
    std::cout << '\n';
    return 0;
}
