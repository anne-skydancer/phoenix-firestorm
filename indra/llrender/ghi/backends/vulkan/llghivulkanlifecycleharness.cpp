/**
 * @file llghivulkanlifecycleharness.cpp
 * @brief R00 native Vulkan presentation lifecycle harness for R1.
 */

#include "llghivulkanpresentation.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE)
    {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(window, message, wparam, lparam);
}

bool pumpMessages()
{
    MSG message{};
    while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            return false;
        }
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return true;
}

bool presentFrames(
    LL::GHI::PresentationSurface& surface,
    int count,
    float phase)
{
    for (int frame = 0; frame < count && pumpMessages(); ++frame)
    {
        const float t = static_cast<float>(frame) / static_cast<float>(std::max(1, count));
        const LL::GHI::ClearColor color{
            0.08f + 0.35f * t,
            0.10f + 0.20f * phase,
            0.20f + 0.30f * (1.f - t),
            1.f
        };
        LL::GHI::Status status = surface.presentClear(color);
        if (!status)
        {
            std::cerr << status.message() << '\n';
            return false;
        }
        Sleep(4);
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    int frameCount = 90;
    bool validation = false;
    bool expectCreateFailure = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--validation")
        {
            validation = true;
        }
        else if (argument == "--expect-create-failure")
        {
            expectCreateFailure = true;
        }
        else if (argument == "--frames" && i + 1 < argc)
        {
            frameCount = std::max(1, std::atoi(argv[++i]));
        }
    }

    const HINSTANCE instance = GetModuleHandle(nullptr);
    const wchar_t* className = L"VulkanstormR1LifecycleHarness";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        std::cerr << "RegisterClassW failed: " << GetLastError() << '\n';
        return 2;
    }

    HWND window = CreateWindowExW(
        0,
        className,
        L"Vulkanstorm R1 native Vulkan lifecycle",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        960,
        540,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!window)
    {
        std::cerr << "CreateWindowExW failed: " << GetLastError() << '\n';
        return 2;
    }
    ShowWindow(window, SW_SHOW);

    RECT client{};
    GetClientRect(window, &client);
    LL::GHI::PresentationCreateInfo createInfo;
    createInfo.backend = LL::GHI::Backend::Vulkan;
    createInfo.nativeWindow = window;
    createInfo.nativeInstance = instance;
    createInfo.width = static_cast<std::uint32_t>(client.right - client.left);
    createInfo.height = static_cast<std::uint32_t>(client.bottom - client.top);
    createInfo.framesInFlight = 2;
    createInfo.enableValidation = validation;
    createInfo.enableVsync = true;

    auto creation = LL::GHI::createPresentationSurface(createInfo);
    if (!creation.status)
    {
        std::cerr << creation.status.message() << '\n';
        DestroyWindow(window);
        if (expectCreateFailure)
        {
            std::cout << "R00 explicit Vulkan initialization failure PASS" << '\n';
            return 0;
        }
        return 3;
    }
    if (expectCreateFailure)
    {
        std::cerr << "Vulkan initialization unexpectedly succeeded" << '\n';
        creation.surface->shutdown();
        DestroyWindow(window);
        return 6;
    }

    const auto& snapshot = creation.surface->rendererSnapshot();
    std::cout << LL::GHI::formatRendererSummary(snapshot.identity) << '\n'
              << "stable-device-id=" << snapshot.identity.stableDeviceId << '\n'
              << "driver=" << snapshot.identity.driverName << ' '
              << snapshot.identity.driverVersion << '\n';

    bool passed = presentFrames(*creation.surface, frameCount, 0.f);
    if (passed)
    {
        SetWindowPos(window, nullptr, 0, 0, 800, 640, SWP_NOMOVE | SWP_NOZORDER);
        GetClientRect(window, &client);
        creation.surface->resize(
            static_cast<std::uint32_t>(client.right - client.left),
            static_cast<std::uint32_t>(client.bottom - client.top));
        passed = presentFrames(*creation.surface, frameCount, 0.5f);
    }
    if (passed)
    {
        creation.surface->setSuspended(true);
        ShowWindow(window, SW_MINIMIZE);
        for (int i = 0; i < 12 && pumpMessages(); ++i) Sleep(10);
        ShowWindow(window, SW_RESTORE);
        GetClientRect(window, &client);
        creation.surface->resize(
            static_cast<std::uint32_t>(client.right - client.left),
            static_cast<std::uint32_t>(client.bottom - client.top));
        creation.surface->setSuspended(false);
        passed = presentFrames(*creation.surface, frameCount, 1.f);
    }

    LL::GHI::Status shutdown = creation.surface->shutdown();
    creation.surface.reset();
    DestroyWindow(window);
    if (!shutdown)
    {
        std::cerr << shutdown.message() << '\n';
        return 4;
    }
    if (!passed)
    {
        return 5;
    }
    std::cout << "R00 Vulkan lifecycle PASS" << '\n';
    return 0;
}
