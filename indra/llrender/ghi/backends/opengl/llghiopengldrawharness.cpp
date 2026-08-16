/**
 * @file llghiopengldrawharness.cpp
 * @brief Standalone OpenGL execution harness for the shared R3 draw fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghidrawfixture.h"

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <iostream>

#ifndef LL_GHI_R3_SHADER_PACKAGE
#error LL_GHI_R3_SHADER_PACKAGE must name the packaged R3 diagnostic shader
#endif

namespace
{

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProc(window, message, wparam, lparam);
}

} // namespace

int main()
{
    const HINSTANCE instance = GetModuleHandle(nullptr);
    const wchar_t* className = L"VulkanstormR3OpenGLDrawHarness";
    WNDCLASSW windowClass{};
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        std::cerr << "RegisterClassW failed" << '\n';
        return 2;
    }
    HWND window = CreateWindowExW(0, className, L"R3 OpenGL draw", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 128, 128, nullptr, nullptr, instance, nullptr);
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
    {
        std::cerr << "WGL context creation failed" << '\n';
        if (context) wglDeleteContext(context);
        ReleaseDC(window, dc);
        DestroyWindow(window);
        return 3;
    }

    std::cout << "OpenGL vendor=" << glGetString(GL_VENDOR) << '\n'
              << "OpenGL renderer=" << glGetString(GL_RENDERER) << '\n'
              << "OpenGL version=" << glGetString(GL_VERSION) << '\n';

    LL::GHI::ShaderPackageDesc shaderPackage;
    LL::GHI::Status status = LL::GHI::loadShaderPackage(
        LL_GHI_R3_SHADER_PACKAGE, shaderPackage);
    int result = 0;
    if (!status)
    {
        std::cerr << "shader package: " << status.message() << '\n';
        result = 4;
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
            result = 5;
        }
        else
        {
            const auto fixture = LL::GHI::Test::runDrawFixture(
                *creation.device, shaderPackage);
            if (!fixture.passed)
            {
                std::cerr << fixture.message << '\n';
                result = 6;
            }
            else
            {
                std::cout << fixture.message << " profile=OpenGL46 depth-stencil="
                          << static_cast<int>(fixture.depthStencilFormat) << '\n';
                LL::GHI::ShaderPackageDesc fallbackPackage = shaderPackage;
                for (auto& stage : fallbackPackage.stages)
                {
                    std::erase_if(stage.artifacts, [](const auto& artifact)
                    {
                        return artifact.target ==
                            LL::GHI::ShaderPackageDesc::TargetProfile::OpenGL46;
                    });
                }
                const auto fallback = LL::GHI::Test::runDrawFixture(
                    *creation.device, fallbackPackage);
                if (!fallback.passed)
                {
                    std::cerr << "OpenGL 4.1 fallback: " << fallback.message << '\n';
                    result = 7;
                }
                else
                {
                    std::cout << fallback.message << " profile=OpenGL41 depth-stencil="
                              << static_cast<int>(fallback.depthStencilFormat) << '\n';
                }
            }
        }
        creation.device.reset();
    }

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    return result;
}
