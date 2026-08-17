#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghioffscreenfixture.h"

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <iostream>

#ifndef LL_GHI_R7_OFFSCREEN_SHADER_PACKAGE
#error LL_GHI_R7_OFFSCREEN_SHADER_PACKAGE must name the R7 offscreen package
#endif

namespace
{
LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{ return DefWindowProc(window, message, wparam, lparam); }

void report(const LL::GHI::Test::OffscreenFixtureResult& result, const char* profile)
{
    std::cout << result.message << " backend=OpenGL profile=" << profile
              << " sha256=" << result.colorSha256
              << " coverage=" << result.shadedPixelCount << '\n';
}
}

int main()
{
    HINSTANCE instance = GetModuleHandle(nullptr);
    const wchar_t* className = L"VulkanstormR7OffscreenOpenGLHarness";
    WNDCLASSW windowClass{};
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;
    HWND window = CreateWindowExW(0, className, L"R7 OpenGL offscreen",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 128, 128,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 2;
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR descriptor{};
    descriptor.nSize = sizeof(descriptor); descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.iPixelType = PFD_TYPE_RGBA; descriptor.cColorBits = 32;
    descriptor.cDepthBits = 24; descriptor.cStencilBits = 8;
    descriptor.iLayerType = PFD_MAIN_PLANE;
    HGLRC context = nullptr;
    int format = ChoosePixelFormat(dc, &descriptor);
    if (!format || !SetPixelFormat(dc, format, &descriptor) ||
        !(context = wglCreateContext(dc)) || !wglMakeCurrent(dc, context)) return 3;
    LL::GHI::ShaderPackageDesc package;
    auto status = LL::GHI::loadShaderPackage(LL_GHI_R7_OFFSCREEN_SHADER_PACKAGE, package);
    int exitCode = 0;
    if (!status) { std::cerr << status.message() << '\n'; exitCode = 4; }
    else
    {
        LL::GHI::DeviceCreateInfo info;
        info.backend = LL::GHI::Backend::OpenGL; info.framesInFlight = 2;
        auto creation = LL::GHI::createDevice(info);
        if (!creation.status) { std::cerr << creation.status.message() << '\n'; exitCode = 5; }
        else
        {
            auto result = LL::GHI::Test::runOffscreenFixture(*creation.device, package);
            if (!result.passed) { std::cerr << result.message << '\n'; exitCode = 6; }
            else report(result, "OpenGL46");
            auto fallback = package;
            for (auto& stage : fallback.stages)
                std::erase_if(stage.artifacts, [](const auto& artifact)
                { return artifact.target == LL::GHI::ShaderPackageDesc::TargetProfile::OpenGL46; });
            if (!exitCode)
            {
                auto peer = LL::GHI::Test::runOffscreenFixture(*creation.device, fallback);
                if (!peer.passed || peer.colorSha256 != result.colorSha256)
                { std::cerr << "OpenGL 4.1 fallback: " << peer.message << '\n'; exitCode = 7; }
                else report(peer, "OpenGL41");
            }
            creation.device.reset();
        }
    }
    wglMakeCurrent(nullptr, nullptr); wglDeleteContext(context);
    ReleaseDC(window, dc); DestroyWindow(window);
    return exitCode;
}
