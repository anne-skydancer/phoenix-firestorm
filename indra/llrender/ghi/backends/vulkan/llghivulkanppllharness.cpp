/**
 * @file llghivulkanppllharness.cpp
 * @brief Native Vulkan execution harness for the R6 PPLL fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghippllfixture.h"

#include <iostream>
#include <string>

#ifndef LL_GHI_R6_PPLL_SHADER_PACKAGE
#error LL_GHI_R6_PPLL_SHADER_PACKAGE must name the packaged R6 PPLL shader
#endif

int main(int argc, char** argv)
{
    bool validation = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--validation") validation = true;

    LL::GHI::ShaderPackageDesc package;
    LL::GHI::Status status = LL::GHI::loadShaderPackage(
        LL_GHI_R6_PPLL_SHADER_PACKAGE, package);
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
    const auto result = LL::GHI::Test::runPPLLFixture(*creation.device, package);
    if (!result.passed)
    {
        std::cerr << result.message << '\n';
        return 4;
    }
    std::cout << result.message << " backend=Vulkan validation="
              << (validation ? "on" : "off");
    for (std::size_t i = 0; i < result.colorSha256.size(); ++i)
        std::cout << " case" << i << "-sha256=" << result.colorSha256[i]
                  << " case" << i << "-coverage=" << result.shadedPixelCount[i]
                  << " case" << i << "-allocations=" << result.allocatedNodeCount[i]
                  << " case" << i << "-overflow=" << result.overflowFragmentCount[i];
    std::cout << '\n';
    creation.device.reset();
    return 0;
}
