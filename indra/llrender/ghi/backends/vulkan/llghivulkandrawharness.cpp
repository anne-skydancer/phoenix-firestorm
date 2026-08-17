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
        *creation.device, shaderPackage,
        {LL::GHI::Format::RGBA8UNorm, true,
         LL::GHI::ShaderPackageDesc::TargetProfile::VulkanSpirV13});
    if (!fixture.passed)
    {
        std::cerr << fixture.message << '\n';
        return 4;
    }
    std::cout << fixture.message << " backend=Vulkan depth-stencil="
              << static_cast<int>(fixture.depthStencilFormat)
              << " validation=" << (validation ? "on" : "off")
              << " color-sha256=" << fixture.colorSha256
              << " cache-id=" << fixture.pipelineCacheIdentity
              << " shader-us=" << fixture.shaderCreateMicroseconds
              << " pipeline-us=" << fixture.pipelineCreateMicroseconds << '\n';
    const auto warm = LL::GHI::Test::runDrawFixture(
        *creation.device, shaderPackage,
        {LL::GHI::Format::RGBA8UNorm, true,
         LL::GHI::ShaderPackageDesc::TargetProfile::VulkanSpirV13});
    if (!warm.passed || warm.colorSha256 != fixture.colorSha256 ||
        warm.pipelineCacheIdentity != fixture.pipelineCacheIdentity)
    {
        std::cerr << "Vulkan warm run: " << warm.message << '\n';
        return 5;
    }
    std::cout << warm.message << " backend=Vulkan cache=warm"
              << " validation=" << (validation ? "on" : "off")
              << " color-sha256=" << warm.colorSha256
              << " cache-id=" << warm.pipelineCacheIdentity
              << " shader-us=" << warm.shaderCreateMicroseconds
              << " pipeline-us=" << warm.pipelineCreateMicroseconds << '\n';
    const auto srgb = LL::GHI::Test::runDrawFixture(
        *creation.device, shaderPackage,
        {LL::GHI::Format::RGBA8SRGB, true,
         LL::GHI::ShaderPackageDesc::TargetProfile::VulkanSpirV13});
    if (!srgb.passed)
    {
        std::cerr << "Vulkan sRGB: " << srgb.message << '\n';
        return 6;
    }
    std::cout << srgb.message << " backend=Vulkan color=sRGB"
              << " validation=" << (validation ? "on" : "off")
              << " color-sha256=" << srgb.colorSha256 << '\n';
    creation.device.reset();
    return 0;
}
