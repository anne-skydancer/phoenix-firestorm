/**
 * @file llghivulkanvisibilityharness.cpp
 * @brief Standalone Vulkan execution harness for the R4c visibility fixture.
 */

#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghivisibilityfixture.h"

#include <iostream>
#include <string>

#ifndef LL_GHI_R4C_SHADER_PACKAGE
#error LL_GHI_R4C_SHADER_PACKAGE must name the packaged R4c visibility shader
#endif

int main(int argc, char** argv)
{
    bool validation = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--validation") validation = true;

    LL::GHI::ShaderPackageDesc shaderPackage;
    LL::GHI::Status status = LL::GHI::loadShaderPackage(
        LL_GHI_R4C_SHADER_PACKAGE, shaderPackage);
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
    const auto fixture = LL::GHI::Test::runVisibilityFixture(
        *creation.device, shaderPackage);
    if (!fixture.passed)
    {
        std::cerr << fixture.message << '\n';
        return 4;
    }
    std::cout << fixture.message << " backend=Vulkan validation="
              << (validation ? "on" : "off") << " depth-stencil="
              << static_cast<int>(fixture.depthStencilFormat)
              << " frame0-sha256=" << fixture.colorSha256[0]
              << " frame1-sha256=" << fixture.colorSha256[1]
              << " visible-samples=" << fixture.occlusionSamples[0]
              << ',' << fixture.occlusionSamples[2] << '\n';
    return 0;
}
