/**
 * @file llghivulkanmaterialskinharness.cpp
 * @brief Native Vulkan execution harness for the R5a material/skin fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghimaterialskinfixture.h"

#include <iostream>
#include <string>

#ifndef LL_GHI_R5A_SHADER_PACKAGE
#error LL_GHI_R5A_SHADER_PACKAGE must name the packaged R5a shader
#endif

int main(int argc, char** argv)
{
    bool validation = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--validation") validation = true;

    LL::GHI::ShaderPackageDesc package;
    LL::GHI::Status status = LL::GHI::loadShaderPackage(
        LL_GHI_R5A_SHADER_PACKAGE, package);
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
    const auto result = LL::GHI::Test::runMaterialSkinFixture(*creation.device, package);
    if (!result.passed)
    {
        std::cerr << result.message << '\n';
        return 4;
    }
    std::cout << result.message << " backend=Vulkan validation="
              << (validation ? "on" : "off") << " depth-stencil="
              << static_cast<int>(result.depthStencilFormat);
    for (std::size_t target = 0; target < result.colorSha256.size(); ++target)
        std::cout << " target" << target << "-sha256=" << result.colorSha256[target]
                  << " target" << target << "-coverage=" << result.shadedPixelCount[target];
    std::cout << '\n';
    creation.device.reset();
    return 0;
}
