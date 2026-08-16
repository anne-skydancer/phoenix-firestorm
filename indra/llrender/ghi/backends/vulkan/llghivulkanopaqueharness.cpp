/**
 * @file llghivulkanopaqueharness.cpp
 * @brief Standalone Vulkan execution harness for the R4 opaque fixture.
 */

#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghiopaquefixture.h"

#include <iostream>
#include <string>

#ifndef LL_GHI_R4_SHADER_PACKAGE
#error LL_GHI_R4_SHADER_PACKAGE must name the packaged R4 opaque shader
#endif

int main(int argc, char** argv)
{
    bool validation = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--validation") validation = true;

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
    const auto fixture = LL::GHI::Test::runOpaqueFixture(
        *creation.device, shaderPackage);
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
