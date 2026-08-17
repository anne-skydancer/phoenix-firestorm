/**
 * @file llghivulkanresourceharness.cpp
 * @brief Vulkan execution harness for the shared R2 resource fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/include/llghi.h"
#include "tests/ghi/llghiresourcefixture.h"

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    bool validation = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--validation") validation = true;
    }

    LL::GHI::DeviceCreateInfo createInfo;
    createInfo.backend = LL::GHI::Backend::Vulkan;
    createInfo.framesInFlight = 2;
    createInfo.enableValidation = validation;
    auto creation = LL::GHI::createDevice(createInfo);
    if (!creation.status)
    {
        std::cerr << creation.status.message() << '\n';
        return 2;
    }
    const auto fixture = LL::GHI::Test::runResourceFixture(*creation.device);
    if (!fixture.passed)
    {
        std::cerr << fixture.message << '\n';
        return 3;
    }
    std::cout << fixture.message << '\n';
    creation.device.reset();
    return 0;
}
