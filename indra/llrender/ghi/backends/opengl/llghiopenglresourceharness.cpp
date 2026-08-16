/**
 * @file llghiopenglresourceharness.cpp
 * @brief OpenGL execution harness for the shared R2 resource fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/include/llghi.h"
#include "tests/ghi/llghiresourcefixture.h"

#include <windows.h>
#include <GL/gl.h>

#include <iostream>

namespace
{

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProc(window, message, wparam, lparam);
}

} // namespace

int main()
{
    std::cerr << "R2 OpenGL harness: window" << std::endl;
    const HINSTANCE instance = GetModuleHandle(nullptr);
    const wchar_t* className = L"VulkanstormR2OpenGLResourceHarness";
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
    HWND window = CreateWindowExW(0, className, L"R2 OpenGL resources", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, nullptr, nullptr, instance, nullptr);
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
    LL::GHI::DeviceCreateInfo createInfo;
    createInfo.backend = LL::GHI::Backend::OpenGL;
    createInfo.framesInFlight = 2;
    std::cerr << "R2 OpenGL harness: create device" << std::endl;
    auto creation = LL::GHI::createDevice(createInfo);
    int result = 0;
    if (!creation.status)
    {
        std::cerr << creation.status.message() << '\n';
        result = 5;
    }
    else
    {
        std::cerr << "R2 OpenGL harness: run fixture" << std::endl;
        const auto fixture = LL::GHI::Test::runResourceFixture(*creation.device);
        if (!fixture.passed)
        {
            std::cerr << fixture.message << '\n';
            result = 6;
        }
        else
        {
            std::cout << fixture.message << '\n';
        }
    }
    std::cerr << "R2 OpenGL harness: shutdown" << std::endl;
    creation.device.reset();
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    return result;
}
