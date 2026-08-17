#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghiinteractionfixture.h"

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#ifndef LL_GHI_R8_INTERACTION_SHADER_PACKAGE
#error LL_GHI_R8_INTERACTION_SHADER_PACKAGE must name the R8 interaction package
#endif

namespace
{
LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{ return DefWindowProc(window, message, wparam, lparam); }

void report(const LL::GHI::Test::InteractionFixtureResult& result,
            const char* profile, int iterations, double maximumNanoseconds)
{
    std::cout << result.message << " backend=OpenGL profile=" << profile
              << " iterations=" << iterations
              << " snapshot=" << result.snapshotSha256
              << " selection=" << result.selectionSha256
              << " pick-depth=" << result.pickDepthSha256
              << " selected-pixels=" << result.selectedPixelCount
              << " max-gpu-ns=" << maximumNanoseconds << '\n';
}
}

int main(int argc, char** argv)
{
    int iterations = 1;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument(argv[i]);
        if (argument == "--iterations" && i + 1 < argc)
            iterations = std::max(1, std::atoi(argv[++i]));
        else
        {
            std::cerr << "usage: llrender_opengl_interaction_harness "
                         "[--iterations N]\n";
            return 2;
        }
    }
    HINSTANCE instance = GetModuleHandle(nullptr);
    const wchar_t* className = L"VulkanstormR8InteractionOpenGLHarness";
    WNDCLASSW windowClass{};
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 3;
    HWND window = CreateWindowExW(0, className, L"R8 OpenGL interaction",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 128, 128,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 3;
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

    LL::GHI::ShaderPackageDesc package;
    auto status = LL::GHI::loadShaderPackage(
        LL_GHI_R8_INTERACTION_SHADER_PACKAGE, package);
    int exitCode = 0;
    if (!status) { std::cerr << status.message() << '\n'; exitCode = 5; }
    else
    {
        LL::GHI::DeviceCreateInfo info;
        info.backend = LL::GHI::Backend::OpenGL;
        info.framesInFlight = 2;
        auto creation = LL::GHI::createDevice(info);
        if (!creation.status)
        {
            std::cerr << creation.status.message() << '\n';
            exitCode = 6;
        }
        else
        {
            LL::GHI::Test::InteractionFixtureResult last;
            double maximumNanoseconds = 0.0;
            for (int iteration = 0; iteration < iterations; ++iteration)
            {
                last = LL::GHI::Test::runInteractionFixture(*creation.device, package);
                if (!last.passed)
                {
                    std::cerr << last.message << '\n';
                    exitCode = 7;
                    break;
                }
                maximumNanoseconds = std::max(maximumNanoseconds, last.gpuNanoseconds);
            }
            if (!exitCode) report(last, "OpenGL44", iterations, maximumNanoseconds);

            auto fallback = package;
            for (auto& stage : fallback.stages)
                std::erase_if(stage.artifacts, [](const auto& artifact)
                {
                    return artifact.target ==
                        LL::GHI::ShaderPackageDesc::TargetProfile::OpenGL44;
                });
            if (!exitCode)
            {
                auto peer = LL::GHI::Test::runInteractionFixture(
                    *creation.device, fallback);
                if (!peer.passed ||
                    peer.snapshotSha256 != last.snapshotSha256 ||
                    peer.selectionSha256 != last.selectionSha256 ||
                    peer.pickDepthSha256 != last.pickDepthSha256)
                {
                    std::cerr << "OpenGL 4.1 fallback: " << peer.message << '\n';
                    exitCode = 8;
                }
                else report(peer, "OpenGL41", 1, peer.gpuNanoseconds);
            }
            creation.device.reset();
        }
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    return exitCode;
}
