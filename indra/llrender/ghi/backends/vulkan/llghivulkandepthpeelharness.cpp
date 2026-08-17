/**
 * @file llghivulkandepthpeelharness.cpp
 * @brief Native Vulkan execution harness for R6 depth peeling.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghidepthpeelfixture.h"

#include <iostream>
#include <string>

#ifndef LL_GHI_R6_PEEL_SHADER_PACKAGE
#error LL_GHI_R6_PEEL_SHADER_PACKAGE must name the R6 depth-peel package
#endif

int main(int argc, char** argv)
{
    bool validation = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--validation") validation = true;
    LL::GHI::ShaderPackageDesc package;
    LL::GHI::Status status = LL::GHI::loadShaderPackage(
        LL_GHI_R6_PEEL_SHADER_PACKAGE, package);
    if (!status) { std::cerr << status.message() << '\n'; return 2; }
    LL::GHI::DeviceCreateInfo createInfo;
    createInfo.backend = LL::GHI::Backend::Vulkan;
    createInfo.framesInFlight = 2;
    createInfo.enableValidation = validation;
    auto creation = LL::GHI::createDevice(createInfo);
    if (!creation.status) { std::cerr << creation.status.message() << '\n'; return 3; }
    const auto result = LL::GHI::Test::runDepthPeelFixture(*creation.device, package);
    if (!result.passed) { std::cerr << result.message << '\n'; return 4; }
    std::cout << result.message << " backend=Vulkan validation="
              << (validation ? "on" : "off")
              << " sha256=" << result.colorSha256
              << " coverage=" << result.shadedPixelCount
              << " peeled-layers=" << result.peeledLayers
              << " residual-tail=" << (result.residualTailRendered ? "yes" : "no") << '\n';
    creation.device.reset();
    return 0;
}
