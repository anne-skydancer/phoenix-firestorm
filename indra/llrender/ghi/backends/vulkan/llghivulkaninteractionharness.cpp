#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghiinteractionfixture.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#ifndef LL_GHI_R8_INTERACTION_SHADER_PACKAGE
#error LL_GHI_R8_INTERACTION_SHADER_PACKAGE must name the R8 interaction package
#endif

int main(int argc, char** argv)
{
    bool validation = false;
    int iterations = 1;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument(argv[i]);
        if (argument == "--validation") validation = true;
        else if (argument == "--iterations" && i + 1 < argc)
            iterations = std::max(1, std::atoi(argv[++i]));
        else
        {
            std::cerr << "usage: llrender_vulkan_interaction_harness "
                         "[--validation] [--iterations N]\n";
            return 2;
        }
    }
    LL::GHI::ShaderPackageDesc package;
    auto status = LL::GHI::loadShaderPackage(
        LL_GHI_R8_INTERACTION_SHADER_PACKAGE, package);
    if (!status) { std::cerr << status.message() << '\n'; return 3; }
    LL::GHI::DeviceCreateInfo info;
    info.backend = LL::GHI::Backend::Vulkan;
    info.framesInFlight = 2;
    info.enableValidation = validation;
    auto creation = LL::GHI::createDevice(info);
    if (!creation.status) { std::cerr << creation.status.message() << '\n'; return 4; }
    LL::GHI::Test::InteractionFixtureResult last;
    double maximumNanoseconds = 0.0;
    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        last = LL::GHI::Test::runInteractionFixture(*creation.device, package);
        if (!last.passed) { std::cerr << last.message << '\n'; return 5; }
        maximumNanoseconds = std::max(maximumNanoseconds, last.gpuNanoseconds);
    }
    std::cout << last.message << " backend=Vulkan validation="
              << (validation ? "on" : "off")
              << " iterations=" << iterations
              << " snapshot=" << last.snapshotSha256
              << " selection=" << last.selectionSha256
              << " pick-depth=" << last.pickDepthSha256
              << " selected-pixels=" << last.selectedPixelCount
              << " max-gpu-ns=" << maximumNanoseconds << '\n';
    creation.device.reset();
    return 0;
}
