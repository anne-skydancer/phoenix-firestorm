/**
 * @file llghivulkandrawharness.cpp
 * @brief Vulkan validation harness for the shared R3 draw fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghidrawfixture.h"

#include <iostream>
#include <string>

#ifndef LL_GHI_R3_SHADER_PACKAGE
#error LL_GHI_R3_SHADER_PACKAGE must name the packaged R3 diagnostic shader
#endif

int main(int argc, char** argv)
{
    bool validation = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--validation") validation = true;

    LL::GHI::ShaderPackageDesc shaderPackage;
    LL::GHI::Status status = LL::GHI::loadShaderPackage(
        LL_GHI_R3_SHADER_PACKAGE, shaderPackage);
    if (!status)
    {
        std::cerr << "shader package: " << status.message() << '\n';
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
    const auto fixture = LL::GHI::Test::runDrawFixture(
        *creation.device, shaderPackage);
    if (!fixture.passed)
    {
        std::cerr << fixture.message << '\n';
        return 4;
    }
    std::cout << fixture.message << " backend=Vulkan depth-stencil="
              << static_cast<int>(fixture.depthStencilFormat)
              << " validation=" << (validation ? "on" : "off") << '\n';
    creation.device.reset();
    return 0;
}
