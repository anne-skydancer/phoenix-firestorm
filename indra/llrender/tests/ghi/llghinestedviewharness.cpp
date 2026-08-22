/**
 * @file llghinestedviewharness.cpp
 * @brief Native-peer replay of one authenticated nested-view packet.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/include/llghi.h"
#include "ghi/include/llghinestedviewscenepacket.h"
#include "tests/ghi/llghinestedviewfixture.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#if defined(LL_GHI_NESTED_OPENGL)
#include <windows.h>
#include <GL/gl.h>
#endif

namespace
{
using namespace LL::GHI;

Status loadPacket(const char* path, NestedViewScenePacket& packet)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return Status::failure(StatusCode::InvalidArgument,
                               "unable to open nested-view packet");
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    std::vector<std::byte> encoded(raw.size());
    std::transform(raw.begin(), raw.end(), encoded.begin(), [](char value)
    {
        return static_cast<std::byte>(static_cast<unsigned char>(value));
    });
    return decodeNestedViewScenePacket(encoded, packet);
}

void report(const LL::GHI::Test::NestedViewFixtureResult& result,
            const char* backend, bool validation)
{
    std::cout << result.message
              << " backend=" << backend
              << " validation=" << (validation ? "on" : "off")
              << " packet_sha256=" << result.packetSha256
              << " image_sha256=" << result.imageSha256
              << " passes=" << result.passCount
              << " coverage=" << result.shadedPixelCount
              << " cube_snapshot=" << result.views[
                    static_cast<std::size_t>(RenderViewClass::CubeSnapshot)]
              << " impostor=" << result.views[
                    static_cast<std::size_t>(RenderViewClass::Impostor)]
              << " dynamic_texture=" << result.views[
                    static_cast<std::size_t>(RenderViewClass::DynamicTexture)]
              << " preview=" << result.views[
                    static_cast<std::size_t>(RenderViewClass::Preview)]
              << " pre_water_alpha=" << result.views[
                    static_cast<std::size_t>(RenderViewClass::PreWaterAlpha)]
              << " media_surface=" << result.views[
                    static_cast<std::size_t>(RenderViewClass::MediaSurface)]
              << '\n';
}

#if defined(LL_GHI_NESTED_OPENGL)
LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProc(window, message, wparam, lparam);
}
#endif
}

int main(int argc, char** argv)
{
    const char* packetPath = nullptr;
    bool validation = false;
    for (int index = 1; index < argc; ++index)
    {
        if (std::string(argv[index]) == "--validation") validation = true;
        else if (!packetPath) packetPath = argv[index];
        else
        {
            std::cerr << "usage: nested-view-harness [--validation] <packet>\n";
            return 2;
        }
    }
    if (!packetPath)
    {
        std::cerr << "usage: nested-view-harness [--validation] <packet>\n";
        return 2;
    }

    NestedViewScenePacket packet;
    Status status = loadPacket(packetPath, packet);
    if (!status)
    {
        std::cerr << status.message() << '\n';
        return 3;
    }

#if defined(LL_GHI_NESTED_OPENGL)
    HINSTANCE instance = GetModuleHandle(nullptr);
    const wchar_t* className = L"VulkanstormP0e4NestedOpenGLHarness";
    WNDCLASSW windowClass{};
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 4;
    HWND window = CreateWindowExW(0, className, L"P0e4 OpenGL nested-view peer",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 128, 128,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 4;
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
    HGLRC context = nullptr;
    const int format = ChoosePixelFormat(dc, &descriptor);
    if (!format || !SetPixelFormat(dc, format, &descriptor) ||
        !(context = wglCreateContext(dc)) || !wglMakeCurrent(dc, context))
        return 4;
    const Backend backend = Backend::OpenGL;
    const char* backendName = "OpenGL";
#elif defined(LL_GHI_NESTED_VULKAN)
    const Backend backend = Backend::Vulkan;
    const char* backendName = "Vulkan";
#else
#error A nested-view harness backend must be selected
#endif

    DeviceCreateInfo info;
    info.backend = backend;
    info.framesInFlight = 2;
    info.enableValidation = validation;
    auto creation = createDevice(info);
    int exitCode = 0;
    if (!creation.status)
    {
        std::cerr << creation.status.message() << '\n';
        exitCode = 5;
    }
    else
    {
        const auto result = LL::GHI::Test::runNestedViewFixture(*creation.device, packet);
        if (!result.passed)
        {
            std::cerr << result.message << '\n';
            exitCode = 6;
        }
        else
        {
            report(result, backendName, validation);
        }
        creation.device.reset();
    }

#if defined(LL_GHI_NESTED_OPENGL)
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
#endif
    return exitCode;
}