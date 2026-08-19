/**
 * @file llghiproductionframeharness.cpp
 * @brief Isolated native-peer replay of a captured production frame.
 */

#include "ghi/core/llghishaderpackage.h"
#include "ghi/include/llghidevice.h"
#include "ghi/include/llghiproductionframepacket.h"
#include "ghi/include/llghiproductionenvironmentexecutor.h"
#include "ghi/include/llghiproductionframetargets.h"
#include "ghi/include/llghiproductiongbufferexecutor.h"
#include "ghi/include/llghiproductionlightingexecutor.h"
#include "ghi/include/llghiproductiontextureresidency.h"

#if defined(LL_GHI_PRODUCTION_OPENGL)
#if !defined(_WIN32)
#error The isolated OpenGL production-frame harness currently requires Windows
#endif
#include <windows.h>
#endif

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(LL_GHI_PRODUCTION_OPENGL) && !defined(LL_GHI_PRODUCTION_VULKAN)
#error A production-frame harness backend must be selected
#endif

namespace
{
using namespace LL::GHI;

class IsolatedOpenGLContext
{
public:
    bool create()
    {
#if defined(LL_GHI_PRODUCTION_OPENGL)
        mInstance = GetModuleHandle(nullptr);
        mClassName = L"VulkanstormProductionFrameOpenGLHarness";
        WNDCLASSW windowClass{};
        windowClass.style = CS_OWNDC;
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = mInstance;
        windowClass.lpszClassName = mClassName;
        if (!RegisterClassW(&windowClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        mWindow = CreateWindowExW(
            0, mClassName, L"Vulkanstorm production-frame OpenGL peer",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 64, 64,
            nullptr, nullptr, mInstance, nullptr);
        if (!mWindow) return false;
        mDc = GetDC(mWindow);
        PIXELFORMATDESCRIPTOR descriptor{};
        descriptor.nSize = sizeof(descriptor);
        descriptor.nVersion = 1;
        descriptor.dwFlags =
            PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        descriptor.iPixelType = PFD_TYPE_RGBA;
        descriptor.cColorBits = 32;
        descriptor.cDepthBits = 24;
        descriptor.cStencilBits = 8;
        descriptor.iLayerType = PFD_MAIN_PLANE;
        const int format = ChoosePixelFormat(mDc, &descriptor);
        if (!format || !SetPixelFormat(mDc, format, &descriptor)) return false;
        HGLRC bootstrap = wglCreateContext(mDc);
        if (!bootstrap || !wglMakeCurrent(mDc, bootstrap)) return false;
        using CreateContextAttributes = HGLRC (WINAPI*)(HDC, HGLRC, const int*);
        const auto createContextAttributes =
            reinterpret_cast<CreateContextAttributes>(
                wglGetProcAddress("wglCreateContextAttribsARB"));
        if (!createContextAttributes)
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(bootstrap);
            return false;
        }
        constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
        constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
        constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
        constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
        constexpr int attributes[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
            WGL_CONTEXT_MINOR_VERSION_ARB, 1,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0};
        mContext = createContextAttributes(mDc, nullptr, attributes);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(bootstrap);
        if (!mContext || !wglMakeCurrent(mDc, mContext)) return false;
#endif
        return true;
    }

    ~IsolatedOpenGLContext()
    {
#if defined(LL_GHI_PRODUCTION_OPENGL)
        if (mContext)
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(mContext);
        }
        if (mWindow && mDc) ReleaseDC(mWindow, mDc);
        if (mWindow) DestroyWindow(mWindow);
#endif
    }

private:
#if defined(LL_GHI_PRODUCTION_OPENGL)
    HINSTANCE mInstance = nullptr;
    const wchar_t* mClassName = nullptr;
    HWND mWindow = nullptr;
    HDC mDc = nullptr;
    HGLRC mContext = nullptr;
#endif
};

bool parseAdapterIndex(const char* value, std::uint32_t& adapterIndex)
{
    const char* end = value + std::char_traits<char>::length(value);
    const auto result = std::from_chars(value, end, adapterIndex);
    return result.ec == std::errc{} && result.ptr == end;
}

bool readPacket(const char* path, ProductionFramePacket& packet,
                std::string& message)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        message = "could not open production-frame packet";
        return false;
    }
    const std::streamoff length = stream.tellg();
    if (length <= 0)
    {
        message = "production-frame packet is empty";
        return false;
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> encoded(static_cast<std::size_t>(length));
    stream.read(reinterpret_cast<char*>(encoded.data()), length);
    if (!stream)
    {
        message = "could not read production-frame packet";
        return false;
    }
    const Status status = decodeProductionFramePacket(encoded, packet);
    if (!status)
    {
        message = status.message();
        return false;
    }
    return true;
}

bool readEnvironmentPacket(const char* path, EnvironmentScenePacket& packet,
                           std::string& message)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) { message = "could not open environment packet"; return false; }
    const std::streamoff length = stream.tellg();
    if (length <= 0) { message = "environment packet is empty"; return false; }
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> encoded(static_cast<std::size_t>(length));
    stream.read(reinterpret_cast<char*>(encoded.data()), length);
    if (!stream) { message = "could not read environment packet"; return false; }
    const Status status = decodeEnvironmentScenePacket(encoded, packet);
    if (!status) { message = status.message(); return false; }
    return true;
}

bool loadPackage(const char* path, ShaderPackageDesc& package)
{
    const Status status = loadShaderPackage(path, package);
    if (status) return true;
    std::cerr << path << ": " << status.message() << '\n';
    return false;
}

template<typename Poll>
Status pollUntilReady(Poll&& poll)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(30);
    for (;;)
    {
        Status status = poll();
        if (status || status.code() != StatusCode::NotReady) return status;
        if (std::chrono::steady_clock::now() >= deadline)
            return Status::failure(StatusCode::NotReady,
                                   "production-frame replay timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

template<typename Values>
void printValues(const char* name, const Values& values)
{
    std::cout << ' ' << name << '=';
    for (std::size_t index = 0; index < values.size(); ++index)
        std::cout << (index ? "," : "") << values[index];
}
}

int main(int argc, char** argv)
{
    using namespace LL::GHI;
    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0]
                  << " <production-frame.llghif> [--adapter <index>]"
                     " [--validation] [--environment <packet.llghie>]\n";
        return 2;
    }

    std::uint32_t adapterIndex = 0;
    bool validation = false;
    const char* environmentPath = nullptr;
    for (int argument = 2; argument < argc; ++argument)
    {
        const std::string option = argv[argument];
        if (option == "--adapter" && argument + 1 < argc)
        {
            if (!parseAdapterIndex(argv[++argument], adapterIndex))
            {
                std::cerr << "adapter index must be an unsigned integer\n";
                return 2;
            }
        }
        else if (option == "--validation") validation = true;
        else if (option == "--environment" && argument + 1 < argc)
            environmentPath = argv[++argument];
        else
        {
            std::cerr << "unknown or incomplete option: " << option << '\n';
            return 2;
        }
    }

    ProductionFramePacket frame;
    std::string message;
    if (!readPacket(argv[1], frame, message))
    {
        std::cerr << message << '\n';
        return 3;
    }
    EnvironmentScenePacket environmentPacket;
    if (environmentPath &&
        !readEnvironmentPacket(environmentPath, environmentPacket, message))
    {
        std::cerr << message << '\n';
        return 3;
    }

    IsolatedOpenGLContext context;
    if (!context.create())
    {
        std::cerr << "could not create the isolated OpenGL context\n";
        return 4;
    }

    DeviceCreateInfo createInfo;
#if defined(LL_GHI_PRODUCTION_OPENGL)
    createInfo.backend = Backend::OpenGL;
    constexpr const char* BACKEND_NAME = "OpenGL";
#else
    createInfo.backend = Backend::Vulkan;
    constexpr const char* BACKEND_NAME = "Vulkan";
#endif
    createInfo.framesInFlight = 2;
    createInfo.adapterIndex = adapterIndex;
    createInfo.enableValidation = validation;
    DeviceCreationResult creation = createDevice(createInfo);
    if (!creation.status || !creation.device)
    {
        std::cerr << creation.status.message() << '\n';
        return 5;
    }

    ShaderPackageDesc opaquePackage, materialPackage, terrainPackage;
    ShaderPackageDesc lightingPackage, projectorPackage, shadowPackage;
    ShaderPackageDesc environmentPackage;
    if (!loadPackage(LL_GHI_PRODUCTION_OPAQUE_PACKAGE, opaquePackage) ||
        !loadPackage(LL_GHI_PRODUCTION_MATERIAL_PACKAGE, materialPackage) ||
        !loadPackage(LL_GHI_PRODUCTION_TERRAIN_PACKAGE, terrainPackage) ||
        !loadPackage(LL_GHI_PRODUCTION_LIGHTING_PACKAGE, lightingPackage) ||
        !loadPackage(LL_GHI_PRODUCTION_PROJECTOR_PACKAGE, projectorPackage) ||
        !loadPackage(LL_GHI_PRODUCTION_SHADOW_PACKAGE, shadowPackage) ||
        (environmentPath && !loadPackage(
            std::filesystem::path(LL_GHI_PRODUCTION_SHADOW_PACKAGE)
                .replace_filename("p0_environment.llghisp").string().c_str(),
            environmentPackage)))
        return 6;

    ProductionTextureResidency residency(*creation.device);
    ProductionFrameTargets targets(*creation.device);
    ProductionGBufferExecutor gbuffer(
        *creation.device, std::move(opaquePackage),
        std::move(materialPackage), std::move(terrainPackage));
    ProductionLightingExecutor lighting(
        *creation.device, std::move(lightingPackage),
        std::move(projectorPackage), std::move(shadowPackage));
    std::unique_ptr<ProductionEnvironmentExecutor> environment;
    if (environmentPath)
        environment = std::make_unique<ProductionEnvironmentExecutor>(
            *creation.device, std::move(environmentPackage));

    ProductionTextureResidencyResult residencyResult;
    Status status = residency.update(
        frame, ProductionTextureResidencyLimits{}, residencyResult);
    ProductionFrameTargetResult targetResult;
    if (status)
        status = targets.ensure(
            frame, ProductionFrameTargetLimits{}, targetResult);
    if (status)
        status = gbuffer.submit(
            frame, targets.targets(), residency, ProductionGBufferLimits{});
    if (status && environment)
        status = environment->submit(environmentPacket, targets.targets(),
                                     ProductionEnvironmentLimits{});
    if (status)
        status = lighting.submit(
            frame, targets.targets(), residency, ProductionLightingLimits{});
    if (!status)
    {
        std::cerr << status.message() << '\n';
        return 7;
    }

    ProductionGBufferResult gbufferResult;
    status = pollUntilReady([&]() { return gbuffer.poll(gbufferResult); });
    ProductionLightingResult lightingResult;
    ProductionEnvironmentResult environmentResult;
    if (status && environment)
        status = pollUntilReady([&]() { return environment->poll(environmentResult); });
    if (status)
        status = pollUntilReady([&]() { return lighting.poll(lightingResult); });
    if (!status)
    {
        std::cerr << status.message() << '\n';
        return 8;
    }

    std::cout << "P0e1 production-frame replay PASS backend=" << BACKEND_NAME
              << " frame=" << frame.frameId
              << " assembly-epoch=" << frame.assemblyEpoch
              << " packet-sha256=" << gbufferResult.frameSha256
              << " extent=" << targetResult.width << 'x' << targetResult.height
              << " draws=" << gbufferResult.opaqueDraws << ','
              << gbufferResult.materialDraws << ','
              << gbufferResult.legacyMaterialDraws << ','
              << gbufferResult.riggedMaterialDraws << ','
              << gbufferResult.terrainDraws << ','
              << lightingResult.shadowCasterDraws << ','
              << lightingResult.projectorLights
              << " residency=" << residencyResult.residentEntries << ','
              << residencyResult.residentBytes;
    std::cout << " lights=" << lightingResult.directionalLights << ','
              << lightingResult.pointLights << ','
              << lightingResult.projectorLights << ','
              << lightingResult.projectorTextures << ','
              << lightingResult.shadowMaps << ','
              << lightingResult.directionalShadowMaps << ','
              << lightingResult.projectorShadowMaps
              << " shadow-draws=" << lightingResult.shadowCasterDraws << ','
              << lightingResult.shadowRiggedDraws << ','
              << lightingResult.shadowMaskedDraws << ','
              << lightingResult.deferredShadowDraws;
    printValues("gbuffer-coverage", gbufferResult.nonClearPixels);
    printValues("gbuffer-sha256", gbufferResult.colorSha256);
    if (environment)
    {
        std::cout << " environment-draws=" << environmentResult.atmosphereDraws
                  << ',' << environmentResult.sunDraws << ','
                  << environmentResult.moonDraws << ','
                  << environmentResult.starDraws << ','
                  << environmentResult.cloudDraws
                  << " environment-sha256=" << environmentResult.packetSha256;
        printValues("environment-coverage", environmentResult.nonClearPixels);
        printValues("environment-color-sha256", environmentResult.colorSha256);
    }
    std::cout << " lighting-coverage=" << lightingResult.litNonClearPixels
              << " lighting-sha256=" << lightingResult.lightingSha256;
    printValues("shadow-active", lightingResult.shadowActive);
    printValues("shadow-coverage", lightingResult.shadowNonClearPixels);
    std::cout << '\n';

    Status shutdown = Status::success();
    const auto retainFirstFailure = [&](Status candidate)
    {
        if (shutdown && !candidate) shutdown = std::move(candidate);
    };
    retainFirstFailure(lighting.shutdown());
    if (environment) retainFirstFailure(environment->shutdown());
    retainFirstFailure(gbuffer.shutdown());
    retainFirstFailure(targets.shutdown());
    retainFirstFailure(residency.shutdown());
    retainFirstFailure(creation.device->waitIdle());
    creation.device.reset();
    if (!shutdown)
    {
        std::cerr << shutdown.message() << '\n';
        return 9;
    }
    return 0;
}
