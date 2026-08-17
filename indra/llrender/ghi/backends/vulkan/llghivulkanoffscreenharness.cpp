#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghioffscreenfixture.h"

#include <iostream>
#include <string>

#ifndef LL_GHI_R7_OFFSCREEN_SHADER_PACKAGE
#error LL_GHI_R7_OFFSCREEN_SHADER_PACKAGE must name the R7 offscreen package
#endif

int main(int argc, char** argv)
{
    bool validation = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--validation") validation = true;
    LL::GHI::ShaderPackageDesc package;
    auto status = LL::GHI::loadShaderPackage(LL_GHI_R7_OFFSCREEN_SHADER_PACKAGE, package);
    if (!status) { std::cerr << status.message() << '\n'; return 2; }
    LL::GHI::DeviceCreateInfo info;
    info.backend = LL::GHI::Backend::Vulkan;
    info.framesInFlight = 2;
    info.enableValidation = validation;
    auto creation = LL::GHI::createDevice(info);
    if (!creation.status) { std::cerr << creation.status.message() << '\n'; return 3; }
    auto result = LL::GHI::Test::runOffscreenFixture(*creation.device, package);
    if (!result.passed) { std::cerr << result.message << '\n'; return 4; }
    std::cout << result.message << " backend=Vulkan validation="
              << (validation ? "on" : "off") << " sha256=" << result.colorSha256
              << " coverage=" << result.shadedPixelCount << '\n';
    creation.device.reset();
    return 0;
}
