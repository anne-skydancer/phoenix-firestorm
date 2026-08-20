#include "ghi/core/llghishaderpackage.h"
#include "ghi/core/llghihash.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghiproductionalphaharness.h"

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifndef LL_GHI_P0_ALPHA_SHADER_PACKAGE
#error LL_GHI_P0_ALPHA_SHADER_PACKAGE must name the production alpha package
#endif
#ifndef LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE
#error LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE must name the legacy alpha package
#endif

namespace
{
LRESULT CALLBACK windowProcedure(HWND window, UINT message,
                                 WPARAM wparam, LPARAM lparam)
{
    return DefWindowProc(window, message, wparam, lparam);
}

bool readPacket(const std::string& path, LL::GHI::AlphaScenePacket& packet,
                std::string& sourceHash, LL::GHI::Status& status)
{
    std::ifstream input(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    std::vector<std::byte> encoded(raw.size());
    std::transform(raw.begin(), raw.end(), encoded.begin(), [](char value)
    { return static_cast<std::byte>(static_cast<unsigned char>(value)); });
    if (!input && !input.eof()) return false;
    sourceHash = LL::GHI::sha256(encoded);
    status = LL::GHI::decodeAlphaScenePacket(encoded, packet);
    return status.ok();
}

void report(const LL::GHI::Test::ProductionAlphaHarnessResult& result,
            const char* profile, const std::string& sourceHash)
{
    const auto& value = result.execution;
    std::cout << result.message << " backend=OpenGL profile=" << profile
              << " frame=" << value.frameId
              << " source-sha256=" << sourceHash
              << " packet-sha256=" << value.packetSha256
              << " routes=" << value.maskDraws << ',' << value.sortedDraws
              << ',' << value.residualDraws << ',' << value.emissiveReplays
              << ',' << value.deferredDraws
              << " deferred-reasons="
              << value.deferredRouteOrMaterialDraws << ','
              << value.deferredSkinDraws << ','
              << value.deferredTextureDraws
              << " ppll=" << value.ppllAvailable << ',' << value.ppllDraws
              << ',' << value.ppllNodeCapacity << ',' << value.ppllExactLayers
              << ',' << value.ppllAllocatedNodes << ','
              << value.ppllOverflowFragments
              << " modified=" << value.modifiedPixels
              << " color-sha256=" << value.colorSha256 << '\n';
}
} // namespace

int main(int argc, char** argv)
{
    std::string packetPath;
    bool forcePPLL = false;
    bool stressPPLL = false;
    bool tailPPLL = false;
    for (int index = 1; index < argc; ++index)
        if (std::string(argv[index]) == "--packet" && index + 1 < argc)
            packetPath = argv[++index];
        else if (std::string(argv[index]) == "--ppll") forcePPLL = true;
        else if (std::string(argv[index]) == "--ppll-stress")
        { forcePPLL = true; stressPPLL = true; }
        else if (std::string(argv[index]) == "--ppll-tail")
        { forcePPLL = true; tailPPLL = true; }
    if (packetPath.empty())
    {
        std::cerr << "usage: llrender_opengl_production_alpha_harness --packet <file>\n";
        return 2;
    }
    LL::GHI::Status status = LL::GHI::Status::success();
    std::string sourceHash;
    LL::GHI::AlphaScenePacket packet;
    if (!readPacket(packetPath, packet, sourceHash, status))
    {
        std::cerr << (status ? "could not read alpha packet" : status.message()) << '\n';
        return 3;
    }
    if (forcePPLL) packet.requestedMethod = LL::GHI::AlphaMethod::PPLL;
    if (stressPPLL || tailPPLL)
        LL::GHI::Test::stressProductionAlphaPacket(packet, stressPPLL);

    HINSTANCE instance = GetModuleHandle(nullptr);
    const wchar_t* className = L"VulkanstormProductionAlphaOpenGLHarness";
    WNDCLASSW windowClass{};
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 4;
    HWND window = CreateWindowExW(0, className, L"Production alpha",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 128, 128,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 4;
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR descriptor{};
    descriptor.nSize = sizeof(descriptor); descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.iPixelType = PFD_TYPE_RGBA; descriptor.cColorBits = 32;
    descriptor.cDepthBits = 24; descriptor.cStencilBits = 8;
    descriptor.iLayerType = PFD_MAIN_PLANE;
    HGLRC context = nullptr;
    const int format = ChoosePixelFormat(dc, &descriptor);
    if (!format || !SetPixelFormat(dc, format, &descriptor) ||
        !(context = wglCreateContext(dc)) || !wglMakeCurrent(dc, context)) return 5;

    LL::GHI::ShaderPackageDesc package;
    status = LL::GHI::loadShaderPackage(forcePPLL
        ? LL_GHI_P0_ALPHA_SHADER_PACKAGE
        : LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE, package);
    int exitCode = 0;
    if (!status) { std::cerr << status.message() << '\n'; exitCode = 6; }
    else
    {
        LL::GHI::DeviceCreationResult creation = LL::GHI::createDevice(
            {LL::GHI::Backend::OpenGL, 0, 2, false});
        if (!creation.status) { std::cerr << creation.status.message() << '\n'; exitCode = 7; }
        else
        {
            auto result = LL::GHI::Test::runProductionAlphaHarness(
                *creation.device, package, packet);
            if (!result.passed) { std::cerr << result.message << '\n'; exitCode = 8; }
            else report(result, "OpenGL44", sourceHash);
            LL::GHI::ShaderPackageDesc fallback;
            status = LL::GHI::loadShaderPackage(
                LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE, fallback);
            for (auto& stage : fallback.stages)
                std::erase_if(stage.artifacts, [](const auto& artifact)
                { return artifact.target == LL::GHI::ShaderPackageDesc::TargetProfile::OpenGL44; });
            if (!exitCode)
            {
                auto peer = LL::GHI::Test::runProductionAlphaHarness(
                    *creation.device, std::move(fallback), packet);
                if (!peer.passed) { std::cerr << peer.message << '\n'; exitCode = 9; }
                else report(peer, "OpenGL41", sourceHash);
            }
            creation.device.reset();
        }
    }
    wglMakeCurrent(nullptr, nullptr); wglDeleteContext(context);
    ReleaseDC(window, dc); DestroyWindow(window);
    return exitCode;
}