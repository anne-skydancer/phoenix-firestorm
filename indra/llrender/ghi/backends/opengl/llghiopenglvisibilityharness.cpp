/**
 * @file llghiopenglvisibilityharness.cpp
 * @brief Standalone OpenGL execution harness for the R4c visibility fixture.
 */

#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghivisibilityfixture.h"

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <iostream>

#ifndef LL_GHI_R4C_SHADER_PACKAGE
#error LL_GHI_R4C_SHADER_PACKAGE must name the packaged R4c visibility shader
#endif

namespace
{
LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProc(window, message, wparam, lparam);
}
}

int main()
{
    const HINSTANCE instance = GetModuleHandle(nullptr);
    const wchar_t* className = L"VulkanstormR4cOpenGLVisibilityHarness";
    WNDCLASSW windowClass{};
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 2;
    HWND window = CreateWindowExW(0, className, L"R4c OpenGL visibility",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 128, 128,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 2;
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR descriptor{};
    descriptor.nSize = sizeof(descriptor);
    descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.iPixelType = PFD_TYPE_RGBA;
    descriptor.cColorBits = 32;
    descriptor.cDepthBits = 24;
    descriptor.cStencilBits = 8;
    descriptor.iLayerType = PFD_MAIN_PLANE;
    const int format = ChoosePixelFormat(dc, &descriptor);
    HGLRC context = nullptr;
    if (!format || !SetPixelFormat(dc, format, &descriptor) ||
        !(context = wglCreateContext(dc)) || !wglMakeCurrent(dc, context))
        return 3;

    LL::GHI::ShaderPackageDesc shaderPackage;
    LL::GHI::Status status = LL::GHI::loadShaderPackage(
        LL_GHI_R4C_SHADER_PACKAGE, shaderPackage);
    int exitCode = 0;
    if (!status)
    {
        std::cerr << status.message() << '\n';
        exitCode = 4;
    }
    else
    {
        LL::GHI::DeviceCreateInfo createInfo;
        createInfo.backend = LL::GHI::Backend::OpenGL;
        createInfo.framesInFlight = 2;
        auto creation = LL::GHI::createDevice(createInfo);
        if (!creation.status)
        {
            std::cerr << creation.status.message() << '\n';
            exitCode = 5;
        }
        else
        {
            const auto fixture = LL::GHI::Test::runVisibilityFixture(
                *creation.device, shaderPackage);
            if (!fixture.passed)
            {
                std::cerr << fixture.message << '\n';
                exitCode = 6;
            }
            else
            {
                std::cout << fixture.message << " backend=OpenGL depth-stencil="
                          << static_cast<int>(fixture.depthStencilFormat)
                          << " frame0-sha256=" << fixture.colorSha256[0]
                          << " frame1-sha256=" << fixture.colorSha256[1]
                          << " visible-samples=" << fixture.occlusionSamples[0]
                          << ',' << fixture.occlusionSamples[2] << '\n';

                LL::GHI::ShaderPackageDesc fallbackPackage = shaderPackage;
                for (auto& stage : fallbackPackage.stages)
                {
                    std::erase_if(stage.artifacts, [](const auto& artifact)
                    {
                        return artifact.target ==
                            LL::GHI::ShaderPackageDesc::TargetProfile::OpenGL46;
                    });
                }
                const auto fallback = LL::GHI::Test::runVisibilityFixture(
                    *creation.device, fallbackPackage);
                if (!fallback.passed ||
                    fallback.colorSha256 != fixture.colorSha256)
                {
                    std::cerr << "OpenGL 4.1 fallback: "
                              << fallback.message << '\n';
                    exitCode = 7;
                }
                else
                {
                    std::cout << fallback.message
                              << " backend=OpenGL profile=OpenGL41 hashes=exact\n";
                }
            }
        }
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    return exitCode;
}
