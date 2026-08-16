/**
 * @file llghiopengldevice.cpp
 * @brief OpenGL 4.1 implementation of the R2 GHI resource contract.
 *
 * Native OpenGL names and entry points remain private to this translation unit.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/core/llghidevicebackend.h"
#include "ghi/core/llghihandlepool.h"
#include "llglheaders.h"

#if LL_WINDOWS
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LL::GHI
{
namespace
{

#if LL_WINDOWS
PFNGLTEXIMAGE3DPROC sTexImage3D = nullptr;
PFNGLTEXSUBIMAGE3DPROC sTexSubImage3D = nullptr;

PROC getOpenGLProcedure(const char* name)
{
    PROC procedure = wglGetProcAddress(name);
    if (procedure == nullptr || procedure == reinterpret_cast<PROC>(1) ||
        procedure == reinterpret_cast<PROC>(2) || procedure == reinterpret_cast<PROC>(3) ||
        procedure == reinterpret_cast<PROC>(-1))
    {
        procedure = reinterpret_cast<PROC>(GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name));
    }
    return procedure;
}

bool loadResourceEntryPoints()
{
#define LL_GHI_LOAD_GL(name) \
    do { if (!(name)) (name) = reinterpret_cast<decltype(name)>(getOpenGLProcedure(#name)); } while (false)
    LL_GHI_LOAD_GL(glGenQueries);
    LL_GHI_LOAD_GL(glDeleteQueries);
    LL_GHI_LOAD_GL(glGetQueryObjectuiv);
    LL_GHI_LOAD_GL(glBindBuffer);
    LL_GHI_LOAD_GL(glDeleteBuffers);
    LL_GHI_LOAD_GL(glGenBuffers);
    LL_GHI_LOAD_GL(glBufferData);
    LL_GHI_LOAD_GL(glBufferSubData);
    LL_GHI_LOAD_GL(glGetBufferSubData);
    LL_GHI_LOAD_GL(glGenerateMipmap);
    LL_GHI_LOAD_GL(glCopyBufferSubData);
    LL_GHI_LOAD_GL(glFenceSync);
    LL_GHI_LOAD_GL(glDeleteSync);
    LL_GHI_LOAD_GL(glClientWaitSync);
    LL_GHI_LOAD_GL(glGenSamplers);
    LL_GHI_LOAD_GL(glDeleteSamplers);
    LL_GHI_LOAD_GL(glSamplerParameteri);
    LL_GHI_LOAD_GL(glSamplerParameterf);
    LL_GHI_LOAD_GL(glQueryCounter);
    LL_GHI_LOAD_GL(glGetQueryObjectui64v);
    if (!sTexImage3D) sTexImage3D = reinterpret_cast<PFNGLTEXIMAGE3DPROC>(getOpenGLProcedure("glTexImage3D"));
    if (!sTexSubImage3D) sTexSubImage3D = reinterpret_cast<PFNGLTEXSUBIMAGE3DPROC>(getOpenGLProcedure("glTexSubImage3D"));
#undef LL_GHI_LOAD_GL
    return glGenQueries && glDeleteQueries && glGetQueryObjectuiv &&
           glBindBuffer && glDeleteBuffers && glGenBuffers && glBufferData &&
           glBufferSubData && glGetBufferSubData && glGenerateMipmap &&
           glCopyBufferSubData && glFenceSync && glDeleteSync && glClientWaitSync &&
           glGenSamplers && glDeleteSamplers && glSamplerParameteri &&
           glSamplerParameterf && glQueryCounter && glGetQueryObjectui64v &&
           sTexImage3D && sTexSubImage3D;
}
#endif

Status invalidArgument(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

Status invalidState(const char* message)
{
    return Status::failure(StatusCode::InvalidState, message);
}

Status invalidHandle(const char* message)
{
    return Status::failure(StatusCode::InvalidHandle, message);
}

Status unsupported(const char* message)
{
    return Status::failure(StatusCode::Unsupported, message);
}

Status backendError(const char* message)
{
    return Status::failure(StatusCode::BackendError, message);
}

template<typename Tag>
std::uint64_t handleKey(Handle<Tag> handle)
{
    return (static_cast<std::uint64_t>(handle.generation()) << 32) | handle.index();
}

bool rangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t total)
{
    return offset <= total && size <= total - offset;
}

struct GLFormat
{
    GLenum internal = 0;
    GLenum external = 0;
    GLenum type = 0;
    std::uint32_t bytes = 0;
    ImageAspect aspect = ImageAspect::Color;
};

GLFormat translateFormat(Format format)
{
    switch (format)
    {
    case Format::R8UNorm: return {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, ImageAspect::Color};
    case Format::RG8UNorm: return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, ImageAspect::Color};
    case Format::RGBA8UNorm: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, ImageAspect::Color};
    case Format::RGBA8SRGB: return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, ImageAspect::Color};
    case Format::BGRA8UNorm: return {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 4, ImageAspect::Color};
    case Format::BGRA8SRGB: return {GL_SRGB8_ALPHA8, GL_BGRA, GL_UNSIGNED_BYTE, 4, ImageAspect::Color};
    case Format::R16Float: return {GL_R16F, GL_RED, GL_HALF_FLOAT, 2, ImageAspect::Color};
    case Format::RG16Float: return {GL_RG16F, GL_RG, GL_HALF_FLOAT, 4, ImageAspect::Color};
    case Format::RGBA16Float: return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, ImageAspect::Color};
    case Format::R32Float: return {GL_R32F, GL_RED, GL_FLOAT, 4, ImageAspect::Color};
    case Format::RG32Float: return {GL_RG32F, GL_RG, GL_FLOAT, 8, ImageAspect::Color};
    case Format::RGBA32Float: return {GL_RGBA32F, GL_RGBA, GL_FLOAT, 16, ImageAspect::Color};
    case Format::R32UInt: return {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 4, ImageAspect::Color};
    case Format::Depth16UNorm: return {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, ImageAspect::Depth};
    case Format::Depth24Stencil8: return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, ImageAspect::DepthStencil};
    case Format::Depth32Float: return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4, ImageAspect::Depth};
    case Format::Depth32FloatStencil8: return {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 8, ImageAspect::DepthStencil};
    case Format::Undefined: break;
    }
    return {};
}

bool aspectCompatible(ImageAspect requested, ImageAspect actual)
{
    if (requested == actual) return true;
    return actual == ImageAspect::DepthStencil &&
           (requested == ImageAspect::Depth || requested == ImageAspect::Stencil);
}

GLenum textureBinding(GLenum target)
{
    switch (target)
    {
    case GL_TEXTURE_2D: return GL_TEXTURE_BINDING_2D;
    case GL_TEXTURE_3D: return GL_TEXTURE_BINDING_3D;
    case GL_TEXTURE_2D_ARRAY: return GL_TEXTURE_BINDING_2D_ARRAY;
    default: return 0;
    }
}

class ScopedBufferBinding
{
public:
    ScopedBufferBinding(GLenum target, GLenum binding, GLuint name) : mTarget(target)
    {
        glGetIntegerv(binding, &mPrevious);
        glBindBuffer(target, name);
    }
    ~ScopedBufferBinding() { glBindBuffer(mTarget, static_cast<GLuint>(mPrevious)); }
private:
    GLenum mTarget;
    GLint mPrevious = 0;
};

class ScopedTextureBinding
{
public:
    ScopedTextureBinding(GLenum target, GLuint name) : mTarget(target)
    {
        glGetIntegerv(textureBinding(target), &mPrevious);
        glBindTexture(target, name);
    }
    ~ScopedTextureBinding() { glBindTexture(mTarget, static_cast<GLuint>(mPrevious)); }
private:
    GLenum mTarget;
    GLint mPrevious = 0;
};

class ScopedPixelStore
{
public:
    explicit ScopedPixelStore(bool pack) :
        mAlignment(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT),
        mRowLength(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH),
        mImageHeight(pack ? GL_PACK_IMAGE_HEIGHT : GL_UNPACK_IMAGE_HEIGHT)
    {
        glGetIntegerv(mAlignment, &mPreviousAlignment);
        glGetIntegerv(mRowLength, &mPreviousRowLength);
        glGetIntegerv(mImageHeight, &mPreviousImageHeight);
        glPixelStorei(mAlignment, 1);
        glPixelStorei(mRowLength, 0);
        glPixelStorei(mImageHeight, 0);
    }
    ~ScopedPixelStore()
    {
        glPixelStorei(mAlignment, mPreviousAlignment);
        glPixelStorei(mRowLength, mPreviousRowLength);
        glPixelStorei(mImageHeight, mPreviousImageHeight);
    }
private:
    GLenum mAlignment;
    GLenum mRowLength;
    GLenum mImageHeight;
    GLint mPreviousAlignment = 4;
    GLint mPreviousRowLength = 0;
    GLint mPreviousImageHeight = 0;
};

class OpenGLDevice;

class OpenGLCommandContext final : public CommandContext
{
public:
    explicit OpenGLCommandContext(OpenGLDevice& device) : mDevice(device) {}

    Status beginFrame() override;
    Status endFrame() override;
    Status copyBuffer(BufferHandle, BufferHandle, std::span<const BufferCopyRegion>) override;
    Status copyBufferToImage(BufferHandle, ImageHandle, std::span<const BufferImageCopyRegion>) override;
    Status copyImageToBuffer(ImageHandle, BufferHandle, std::span<const BufferImageCopyRegion>) override;
    Status generateMipmaps(ImageHandle, const ImageSubresourceRange&) override;
    Status resetQueryPool(QueryPoolHandle, std::uint32_t, std::uint32_t) override;
    Status writeTimestamp(QueryPoolHandle, std::uint32_t) override;

    Status beginRendering(const RenderingInfo&) override { return unsupported("OpenGL rendering begins in R3"); }
    Status endRendering() override { return unsupported("OpenGL rendering begins in R3"); }
    Status bindPipeline(PipelineHandle) override { return unsupported("OpenGL pipelines begin in R3"); }
    Status bindBindingSet(std::uint8_t, BindingSetHandle, std::span<const std::uint32_t>) override { return unsupported("OpenGL binding sets begin in R3"); }
    Status setViewport(const Viewport&) override { return unsupported("OpenGL dynamic state begins in R3"); }
    Status setScissor(const ScissorRect&) override { return unsupported("OpenGL dynamic state begins in R3"); }
    Status bindVertexBuffer(std::uint32_t, BufferHandle, std::uint64_t) override { return unsupported("OpenGL vertex binding begins in R3"); }
    Status bindIndexBuffer(BufferHandle, std::uint64_t, IndexType) override { return unsupported("OpenGL index binding begins in R3"); }
    Status draw(const DrawArguments&) override { return unsupported("OpenGL drawing begins in R3"); }
    Status drawIndexed(const DrawIndexedArguments&) override { return unsupported("OpenGL drawing begins in R3"); }

    bool frameActive() const { return mFrameActive; }
    void setFrameActive(bool value) { mFrameActive = value; }

private:
    Status requireTransfer() const;
    OpenGLDevice& mDevice;
    bool mFrameActive = false;
};

class OpenGLDevice final : public Device
{
public:
    explicit OpenGLDevice(const DeviceCreateInfo& info);
    ~OpenGLDevice() override;

    Backend backend() const override { return Backend::OpenGL; }
    const RendererCapabilities& capabilities() const override { return mCapabilities; }
    CommandContext& commandContext() override { return mCommands; }

    BufferHandle createBuffer(const BufferDesc&, Status&) override;
    ImageHandle createImage(const ImageDesc&, Status&) override;
    ImageViewHandle createImageView(const ImageViewDesc&, Status&) override;
    SamplerHandle createSampler(const SamplerDesc&, Status&) override;
    QueryPoolHandle createQueryPool(const QueryPoolDesc&, Status&) override;
    ShaderPackageHandle createShaderPackage(const ShaderPackageDesc&, Status& status) override
    {
        status = unsupported("OpenGL shader packages begin in R3"); return {};
    }
    BindingSetHandle createBindingSet(const BindingSetDesc&, Status& status) override
    {
        status = unsupported("OpenGL binding sets begin in R3"); return {};
    }
    PipelineHandle createPipeline(const PipelineDesc&, Status& status) override
    {
        status = unsupported("OpenGL pipelines begin in R3"); return {};
    }

    Status destroy(BufferHandle) override;
    Status destroy(ImageHandle) override;
    Status destroy(ImageViewHandle) override;
    Status destroy(SamplerHandle) override;
    Status destroy(QueryPoolHandle) override;
    Status destroy(ShaderPackageHandle) override { return unsupported("OpenGL shader packages begin in R3"); }
    Status destroy(BindingSetHandle) override { return unsupported("OpenGL binding sets begin in R3"); }
    Status destroy(PipelineHandle) override { return unsupported("OpenGL pipelines begin in R3"); }

    Status writeBuffer(BufferHandle, std::uint64_t, std::span<const std::byte>) override;
    Status readBuffer(BufferHandle, std::uint64_t, std::span<std::byte>) override;
    Status getQueryResults(QueryPoolHandle, std::uint32_t, std::span<std::uint64_t>, QueryReadMode) override;
    Status waitIdle() override;

    Status beginFrame();
    Status endFrame();
    Status copyBuffer(BufferHandle, BufferHandle, std::span<const BufferCopyRegion>);
    Status copyBufferToImage(BufferHandle, ImageHandle, std::span<const BufferImageCopyRegion>);
    Status copyImageToBuffer(ImageHandle, BufferHandle, std::span<const BufferImageCopyRegion>);
    Status generateMipmaps(ImageHandle, const ImageSubresourceRange&);
    Status resetQueryPool(QueryPoolHandle, std::uint32_t, std::uint32_t);
    Status writeTimestamp(QueryPoolHandle, std::uint32_t);

private:
    struct BufferRecord { BufferDesc desc; GLuint name = 0; std::uint64_t readySerial = 0; };
    struct ImageRecord { ImageDesc desc; GLuint name = 0; GLenum target = 0; GLFormat format; };
    struct ViewRecord { ImageViewDesc desc; };
    struct SamplerRecord { SamplerDesc desc; GLuint name = 0; };
    struct QueryRecord { QueryPoolDesc desc; std::vector<GLuint> names; std::vector<bool> written; };
    enum class NativeKind { Buffer, Texture, Sampler, Query };
    struct Retirement { NativeKind kind; GLuint name; std::uint64_t releaseAfter; };
    struct Fence { std::uint64_t serial; GLsync sync; };

    Status canMutate() const;
    void retireNative(NativeKind, GLuint);
    void pollFences(bool wait);
    void drainRetirements(bool force);
    bool serialComplete(std::uint64_t serial);

    RendererCapabilities mCapabilities;
    std::uint32_t mFramesInFlight = 1;
    std::uint64_t mSubmittedSerial = 0;
    std::uint64_t mCompletedSerial = 0;
    HandlePool<BufferTag> mBufferPool;
    HandlePool<ImageTag> mImagePool;
    HandlePool<ImageViewTag> mViewPool;
    HandlePool<SamplerTag> mSamplerPool;
    HandlePool<QueryPoolTag> mQueryPool;
    std::unordered_map<std::uint64_t, BufferRecord> mBuffers;
    std::unordered_map<std::uint64_t, ImageRecord> mImages;
    std::unordered_map<std::uint64_t, ViewRecord> mViews;
    std::unordered_map<std::uint64_t, SamplerRecord> mSamplers;
    std::unordered_map<std::uint64_t, QueryRecord> mQueries;
    std::vector<Retirement> mRetirements;
    std::vector<Fence> mFences;
    OpenGLCommandContext mCommands;
};

OpenGLDevice::OpenGLDevice(const DeviceCreateInfo& info) :
    mFramesInFlight(info.framesInFlight), mCommands(*this)
{
    GLint value = 0;
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &value);
    mCapabilities.maxColorAttachments = std::max(1, value);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &value);
    mCapabilities.maxSampledImagesPerStage = std::max(1, value);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
    mCapabilities.maxTexture2DSize = std::max(1, value);
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &value);
    mCapabilities.maxUniformBufferSize = std::max(0, value);
    glGetIntegerv(GL_MAX_VARYING_COMPONENTS, &value);
    mCapabilities.maxVaryingVectors = std::max(0, value / 4);
    glGetIntegerv(GL_MAX_SAMPLES, &value);
    mCapabilities.maxSamples = std::max(1, value);
    mCapabilities.maxFramesInFlight = info.framesInFlight;
    mCapabilities.maxBufferSize = static_cast<std::uint64_t>(std::numeric_limits<GLsizeiptr>::max());
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &value);
    mCapabilities.uniformBufferOffsetAlignment = std::max(1, value);
    mCapabilities.preferredDepthStencilFormat = Format::Depth24Stencil8;
    mCapabilities.timestampQueries = glQueryCounter && glGetQueryObjectui64v;
    mCapabilities.timestampPeriodNanoseconds = mCapabilities.timestampQueries ? 1.0 : 0.0;
    mCapabilities.occlusionQueries = true;
    mCapabilities.depthClamp = true;
    mCapabilities.baselineGraphicsPipeline = true;
    int major = 0;
    int minor = 0;
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (version) std::sscanf(version, "%d.%d", &major, &minor);
    if (major > 4 || (major == 4 && minor >= 3))
    {
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &value);
        mCapabilities.storageBufferOffsetAlignment = std::max(1, value);
    }
    mCapabilities.advancedGraphicsPipeline = major > 4 || (major == 4 && minor >= 6);
}

OpenGLDevice::~OpenGLDevice()
{
#if LL_WINDOWS
    if (!wglGetCurrentContext()) return;
#endif
    if (mCommands.frameActive())
    {
        // A partially recorded frame is not submitted, but immediate OpenGL
        // resource commands still need to finish before native deletion.
        mCommands.setFrameActive(false);
    }
    waitIdle();
    for (const auto& [unused, buffer] : mBuffers) glDeleteBuffers(1, &buffer.name);
    for (const auto& [unused, image] : mImages) glDeleteTextures(1, &image.name);
    for (const auto& [unused, sampler] : mSamplers) glDeleteSamplers(1, &sampler.name);
    for (const auto& [unused, query] : mQueries)
        glDeleteQueries(static_cast<GLsizei>(query.names.size()), query.names.data());
}

Status OpenGLDevice::canMutate() const
{
    return mCommands.frameActive()
        ? invalidState("resources may not be created or destroyed during an active frame")
        : Status::success();
}

BufferHandle OpenGLDevice::createBuffer(const BufferDesc& desc, Status& status)
{
    status = canMutate();
    if (!status) return {};
    if (desc.size == 0 || desc.usage == ResourceUsage::None || desc.size > mCapabilities.maxBufferSize)
    {
        status = invalidArgument("buffer size and usage must be nonzero and supported");
        return {};
    }
    GLuint name = 0;
    glGenBuffers(1, &name);
    if (!name) { status = backendError("glGenBuffers failed"); return {}; }
    {
        ScopedBufferBinding binding(GL_COPY_WRITE_BUFFER, GL_COPY_WRITE_BUFFER_BINDING, name);
        const GLenum hint = desc.memory == MemoryClass::DeviceLocal ? GL_STATIC_DRAW : GL_STREAM_DRAW;
        glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(desc.size), nullptr, hint);
    }
    if (glGetError() != GL_NO_ERROR)
    {
        glDeleteBuffers(1, &name);
        status = backendError("OpenGL buffer allocation failed");
        return {};
    }
    BufferHandle handle = mBufferPool.allocate();
    mBuffers.emplace(handleKey(handle), BufferRecord{desc, name, 0});
    status = Status::success();
    return handle;
}

ImageHandle OpenGLDevice::createImage(const ImageDesc& desc, Status& status)
{
    status = canMutate();
    if (!status) return {};
    const GLFormat format = translateFormat(desc.format);
    std::uint32_t maxDimension = std::max({desc.extent.width, desc.extent.height, desc.extent.depth});
    std::uint16_t maxMips = 1;
    while (maxDimension > 1) { maxDimension >>= 1; ++maxMips; }
    if (!format.internal || !desc.extent.width || !desc.extent.height || !desc.extent.depth ||
        desc.usage == ResourceUsage::None || !desc.mipLevels || !desc.arrayLayers || desc.samples != 1 ||
        desc.mipLevels > maxMips || desc.extent.width > mCapabilities.maxTexture2DSize ||
        desc.extent.height > mCapabilities.maxTexture2DSize ||
        (desc.extent.depth > 1 && desc.arrayLayers > 1))
    {
        status = invalidArgument("invalid or unsupported OpenGL image descriptor");
        return {};
    }

    const GLenum target = desc.arrayLayers > 1 ? GL_TEXTURE_2D_ARRAY :
                          desc.extent.depth > 1 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
    GLuint name = 0;
    glGenTextures(1, &name);
    if (!name) { status = backendError("glGenTextures failed"); return {}; }
    {
        ScopedTextureBinding binding(target, name);
        glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, desc.mipLevels - 1);
        for (std::uint16_t mip = 0; mip < desc.mipLevels; ++mip)
        {
            const GLsizei width = std::max(1u, desc.extent.width >> mip);
            const GLsizei height = std::max(1u, desc.extent.height >> mip);
            if (target == GL_TEXTURE_2D)
            {
                glTexImage2D(target, mip, format.internal, width, height, 0,
                             format.external, format.type, nullptr);
            }
            else
            {
                const GLsizei depth = target == GL_TEXTURE_2D_ARRAY
                    ? desc.arrayLayers : std::max(1u, desc.extent.depth >> mip);
                sTexImage3D(target, mip, format.internal, width, height, depth, 0,
                            format.external, format.type, nullptr);
            }
        }
    }
    if (glGetError() != GL_NO_ERROR)
    {
        glDeleteTextures(1, &name);
        status = backendError("OpenGL image allocation failed");
        return {};
    }
    ImageHandle handle = mImagePool.allocate();
    mImages.emplace(handleKey(handle), ImageRecord{desc, name, target, format});
    status = Status::success();
    return handle;
}

ImageViewHandle OpenGLDevice::createImageView(const ImageViewDesc& desc, Status& status)
{
    status = canMutate();
    if (!status) return {};
    auto image = mImages.find(handleKey(desc.image));
    const auto& range = desc.subresources;
    if (!mImagePool.isLive(desc.image) || image == mImages.end())
    {
        status = invalidHandle("image view references a stale or invalid image"); return {};
    }
    if (desc.format != image->second.desc.format ||
        !aspectCompatible(range.aspect, image->second.format.aspect) ||
        !range.mipLevelCount || !range.arrayLayerCount ||
        range.baseMipLevel >= image->second.desc.mipLevels ||
        range.mipLevelCount > image->second.desc.mipLevels - range.baseMipLevel ||
        range.baseArrayLayer >= image->second.desc.arrayLayers ||
        range.arrayLayerCount > image->second.desc.arrayLayers - range.baseArrayLayer)
    {
        status = invalidArgument("invalid or incompatible image view descriptor"); return {};
    }
    ImageViewHandle handle = mViewPool.allocate();
    mViews.emplace(handleKey(handle), ViewRecord{desc});
    status = Status::success();
    return handle;
}

SamplerHandle OpenGLDevice::createSampler(const SamplerDesc& desc, Status& status)
{
    status = canMutate();
    if (!status) return {};
    if (desc.maxAnisotropy < 1.f || !glGenSamplers)
    {
        status = desc.maxAnisotropy < 1.f ? invalidArgument("sampler anisotropy must be at least one")
                                         : unsupported("OpenGL sampler objects are unavailable");
        return {};
    }
    GLuint name = 0;
    glGenSamplers(1, &name);
    if (!name) { status = backendError("OpenGL sampler allocation failed"); return {}; }
    const auto address = [](AddressMode mode)
    {
        switch (mode)
        {
        case AddressMode::Repeat: return GL_REPEAT;
        case AddressMode::MirroredRepeat: return GL_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge: return GL_CLAMP_TO_EDGE;
        case AddressMode::ClampToBorder: return GL_CLAMP_TO_BORDER;
        }
        return GL_REPEAT;
    };
    const GLint minFilter = desc.minFilter == Filter::Nearest
        ? (desc.mipFilter == Filter::Nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR)
        : (desc.mipFilter == Filter::Nearest ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(name, GL_TEXTURE_MIN_FILTER, minFilter);
    glSamplerParameteri(name, GL_TEXTURE_MAG_FILTER, desc.magFilter == Filter::Nearest ? GL_NEAREST : GL_LINEAR);
    glSamplerParameteri(name, GL_TEXTURE_WRAP_S, address(desc.addressU));
    glSamplerParameteri(name, GL_TEXTURE_WRAP_T, address(desc.addressV));
    glSamplerParameteri(name, GL_TEXTURE_WRAP_R, address(desc.addressW));
    if (desc.maxAnisotropy > 1.f)
    {
        GLfloat maximum = 1.f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximum);
        if (glGetError() == GL_NO_ERROR)
            glSamplerParameterf(name, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                std::min(desc.maxAnisotropy, maximum));
    }
    SamplerHandle handle = mSamplerPool.allocate();
    mSamplers.emplace(handleKey(handle), SamplerRecord{desc, name});
    status = Status::success();
    return handle;
}

QueryPoolHandle OpenGLDevice::createQueryPool(const QueryPoolDesc& desc, Status& status)
{
    status = canMutate();
    if (!status) return {};
    if (!desc.count) { status = invalidArgument("query pool count must be nonzero"); return {}; }
    if (!mCapabilities.timestampQueries) { status = unsupported("timestamp queries are unavailable"); return {}; }
    QueryRecord record{desc, std::vector<GLuint>(desc.count), std::vector<bool>(desc.count, false)};
    glGenQueries(desc.count, record.names.data());
    if (std::any_of(record.names.begin(), record.names.end(), [](GLuint name) { return name == 0; }))
    {
        glDeleteQueries(desc.count, record.names.data());
        status = backendError("OpenGL query allocation failed"); return {};
    }
    QueryPoolHandle handle = mQueryPool.allocate();
    mQueries.emplace(handleKey(handle), std::move(record));
    status = Status::success();
    return handle;
}

void OpenGLDevice::retireNative(NativeKind kind, GLuint name)
{
    if (name) mRetirements.push_back({kind, name, mSubmittedSerial + mFramesInFlight});
}

Status OpenGLDevice::destroy(BufferHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    auto found = mBuffers.find(handleKey(handle));
    if (!mBufferPool.release(handle) || found == mBuffers.end()) return invalidHandle("invalid buffer handle");
    retireNative(NativeKind::Buffer, found->second.name); mBuffers.erase(found); return Status::success();
}

Status OpenGLDevice::destroy(ImageHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    for (const auto& [unused, view] : mViews) if (view.desc.image == handle) return invalidState("image must outlive its image views");
    auto found = mImages.find(handleKey(handle));
    if (!mImagePool.release(handle) || found == mImages.end()) return invalidHandle("invalid image handle");
    retireNative(NativeKind::Texture, found->second.name); mImages.erase(found); return Status::success();
}

Status OpenGLDevice::destroy(ImageViewHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    auto found = mViews.find(handleKey(handle));
    if (!mViewPool.release(handle) || found == mViews.end()) return invalidHandle("invalid image-view handle");
    mViews.erase(found); return Status::success();
}

Status OpenGLDevice::destroy(SamplerHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    auto found = mSamplers.find(handleKey(handle));
    if (!mSamplerPool.release(handle) || found == mSamplers.end()) return invalidHandle("invalid sampler handle");
    retireNative(NativeKind::Sampler, found->second.name); mSamplers.erase(found); return Status::success();
}

Status OpenGLDevice::destroy(QueryPoolHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    auto found = mQueries.find(handleKey(handle));
    if (!mQueryPool.release(handle) || found == mQueries.end()) return invalidHandle("invalid query-pool handle");
    for (GLuint name : found->second.names) retireNative(NativeKind::Query, name);
    mQueries.erase(found); return Status::success();
}

Status OpenGLDevice::writeBuffer(BufferHandle handle, std::uint64_t offset, std::span<const std::byte> data)
{
    if (mCommands.frameActive()) return invalidState("host writes are not allowed during an active frame");
    auto found = mBuffers.find(handleKey(handle));
    if (!mBufferPool.isLive(handle) || found == mBuffers.end()) return invalidHandle("invalid buffer handle");
    if (found->second.desc.memory != MemoryClass::Upload) return invalidArgument("writeBuffer requires an upload buffer");
    if (!rangeFits(offset, data.size(), found->second.desc.size)) return invalidArgument("writeBuffer range exceeds the buffer");
    ScopedBufferBinding binding(GL_COPY_WRITE_BUFFER, GL_COPY_WRITE_BUFFER_BINDING, found->second.name);
    glBufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(data.size()), data.data());
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL buffer upload failed");
}

bool OpenGLDevice::serialComplete(std::uint64_t serial)
{
    if (!serial || serial <= mCompletedSerial) return true;
    pollFences(false);
    return serial <= mCompletedSerial;
}

Status OpenGLDevice::readBuffer(BufferHandle handle, std::uint64_t offset, std::span<std::byte> data)
{
    if (mCommands.frameActive()) return invalidState("host reads are not allowed during an active frame");
    auto found = mBuffers.find(handleKey(handle));
    if (!mBufferPool.isLive(handle) || found == mBuffers.end()) return invalidHandle("invalid buffer handle");
    if (found->second.desc.memory != MemoryClass::Readback) return invalidArgument("readBuffer requires a readback buffer");
    if (!rangeFits(offset, data.size(), found->second.desc.size)) return invalidArgument("readBuffer range exceeds the buffer");
    if (!serialComplete(found->second.readySerial)) return Status::failure(StatusCode::NotReady, "readback buffer is not ready");
    ScopedBufferBinding binding(GL_COPY_READ_BUFFER, GL_COPY_READ_BUFFER_BINDING, found->second.name);
    glGetBufferSubData(GL_COPY_READ_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(data.size()), data.data());
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL buffer readback failed");
}

Status OpenGLDevice::getQueryResults(QueryPoolHandle pool, std::uint32_t first, std::span<std::uint64_t> results, QueryReadMode mode)
{
    if (mCommands.frameActive()) return invalidState("query results may not be read during an active frame");
    auto found = mQueries.find(handleKey(pool));
    if (!mQueryPool.isLive(pool) || found == mQueries.end()) return invalidHandle("invalid query-pool handle");
    if (results.empty() || first >= found->second.desc.count || results.size() > found->second.desc.count - first)
        return invalidArgument("query result range is empty or out of bounds");
    if (mode == QueryReadMode::AvailableOnly)
    {
        for (std::size_t i = 0; i < results.size(); ++i)
        {
            const std::size_t index = first + i;
            if (!found->second.written[index]) return Status::failure(StatusCode::NotReady, "query has not been written");
            GLuint available = GL_FALSE;
            glGetQueryObjectuiv(found->second.names[index], GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) return Status::failure(StatusCode::NotReady, "query result is not ready");
        }
    }
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        const std::size_t index = first + i;
        if (!found->second.written[index]) return Status::failure(StatusCode::NotReady, "query has not been written");
        glGetQueryObjectui64v(found->second.names[index], GL_QUERY_RESULT, &results[i]);
    }
    return Status::success();
}

Status OpenGLDevice::beginFrame()
{
    if (mCommands.frameActive()) return invalidState("a frame is already active");
    pollFences(false); drainRetirements(false); mCommands.setFrameActive(true); return Status::success();
}

Status OpenGLDevice::endFrame()
{
    if (!mCommands.frameActive()) return invalidState("no frame is active");
    ++mSubmittedSerial;
    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!sync)
    {
        mCommands.setFrameActive(false);
        return backendError("OpenGL frame fence creation failed");
    }
    glFlush();
    mFences.push_back({mSubmittedSerial, sync});
    mCommands.setFrameActive(false);
    return Status::success();
}

Status OpenGLDevice::copyBuffer(BufferHandle source, BufferHandle destination, std::span<const BufferCopyRegion> regions)
{
    auto src = mBuffers.find(handleKey(source)); auto dst = mBuffers.find(handleKey(destination));
    if (!mBufferPool.isLive(source) || !mBufferPool.isLive(destination) || src == mBuffers.end() || dst == mBuffers.end()) return invalidHandle("copyBuffer received an invalid buffer");
    if (!hasUsage(src->second.desc.usage, ResourceUsage::TransferSource) || !hasUsage(dst->second.desc.usage, ResourceUsage::TransferDestination)) return invalidArgument("copyBuffer usage flags are incompatible");
    if (regions.empty()) return invalidArgument("copyBuffer requires at least one region");
    ScopedBufferBinding read(GL_COPY_READ_BUFFER, GL_COPY_READ_BUFFER_BINDING, src->second.name);
    ScopedBufferBinding write(GL_COPY_WRITE_BUFFER, GL_COPY_WRITE_BUFFER_BINDING, dst->second.name);
    for (const auto& region : regions)
    {
        if (!region.size || ((region.sourceOffset | region.destinationOffset | region.size) & 3u) != 0 ||
            !rangeFits(region.sourceOffset, region.size, src->second.desc.size) ||
            !rangeFits(region.destinationOffset, region.size, dst->second.desc.size))
            return invalidArgument("copyBuffer region is unaligned or out of bounds");
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, region.sourceOffset, region.destinationOffset, region.size);
    }
    if (dst->second.desc.memory == MemoryClass::Readback) dst->second.readySerial = mSubmittedSerial + 1;
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL buffer copy failed");
}

Status OpenGLDevice::copyBufferToImage(BufferHandle source, ImageHandle destination, std::span<const BufferImageCopyRegion> regions)
{
    auto src = mBuffers.find(handleKey(source)); auto dst = mImages.find(handleKey(destination));
    if (!mBufferPool.isLive(source) || !mImagePool.isLive(destination) || src == mBuffers.end() || dst == mImages.end()) return invalidHandle("buffer-to-image copy received an invalid resource");
    if (!hasUsage(src->second.desc.usage, ResourceUsage::TransferSource) || !hasUsage(dst->second.desc.usage, ResourceUsage::TransferDestination)) return invalidArgument("buffer-to-image usage flags are incompatible");
    if (regions.empty()) return invalidArgument("buffer-to-image copy requires at least one region");
    ScopedBufferBinding unpack(GL_PIXEL_UNPACK_BUFFER, GL_PIXEL_UNPACK_BUFFER_BINDING, src->second.name);
    ScopedTextureBinding texture(dst->second.target, dst->second.name);
    ScopedPixelStore pixelStore(false);
    for (const auto& region : regions)
    {
        const auto& sub = region.imageSubresource;
        if ((region.bufferOffset & 3u) != 0 || sub.aspect != ImageAspect::Color ||
            !aspectCompatible(sub.aspect, dst->second.format.aspect) ||
            sub.mipLevel >= dst->second.desc.mipLevels || !sub.arrayLayerCount ||
            sub.baseArrayLayer + sub.arrayLayerCount > dst->second.desc.arrayLayers ||
            region.imageOffset.x < 0 || region.imageOffset.y < 0 || region.imageOffset.z < 0)
            return invalidArgument("unsupported or invalid buffer-to-image region");
        const std::uint32_t mipWidth = std::max(1u, dst->second.desc.extent.width >> sub.mipLevel);
        const std::uint32_t mipHeight = std::max(1u, dst->second.desc.extent.height >> sub.mipLevel);
        const std::uint32_t mipDepth = std::max(1u, dst->second.desc.extent.depth >> sub.mipLevel);
        if (static_cast<std::uint32_t>(region.imageOffset.x) + region.imageExtent.width > mipWidth ||
            static_cast<std::uint32_t>(region.imageOffset.y) + region.imageExtent.height > mipHeight ||
            (dst->second.target == GL_TEXTURE_3D &&
             static_cast<std::uint32_t>(region.imageOffset.z) + region.imageExtent.depth > mipDepth))
            return invalidArgument("buffer-to-image region is out of bounds");
        const std::uint32_t row = region.bufferRowLength ? region.bufferRowLength : region.imageExtent.width;
        const std::uint32_t rows = region.bufferImageHeight ? region.bufferImageHeight : region.imageExtent.height;
        const std::uint32_t slices = dst->second.target == GL_TEXTURE_3D
            ? region.imageExtent.depth : sub.arrayLayerCount;
        const std::uint64_t bytes = static_cast<std::uint64_t>(row) * rows * slices * dst->second.format.bytes;
        if (!rangeFits(region.bufferOffset, bytes, src->second.desc.size)) return invalidArgument("buffer-to-image source range is out of bounds");
        glPixelStorei(GL_UNPACK_ROW_LENGTH, region.bufferRowLength);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, region.bufferImageHeight);
        const void* offset = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(region.bufferOffset));
        if (dst->second.target == GL_TEXTURE_2D)
            glTexSubImage2D(dst->second.target, sub.mipLevel, region.imageOffset.x, region.imageOffset.y,
                region.imageExtent.width, region.imageExtent.height, dst->second.format.external, dst->second.format.type, offset);
        else
            sTexSubImage3D(dst->second.target, sub.mipLevel, region.imageOffset.x, region.imageOffset.y,
                dst->second.target == GL_TEXTURE_2D_ARRAY ? sub.baseArrayLayer : region.imageOffset.z,
                region.imageExtent.width, region.imageExtent.height,
                dst->second.target == GL_TEXTURE_2D_ARRAY ? sub.arrayLayerCount : region.imageExtent.depth,
                dst->second.format.external, dst->second.format.type, offset);
    }
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL buffer-to-image copy failed");
}

Status OpenGLDevice::copyImageToBuffer(ImageHandle source, BufferHandle destination, std::span<const BufferImageCopyRegion> regions)
{
    auto src = mImages.find(handleKey(source)); auto dst = mBuffers.find(handleKey(destination));
    if (!mImagePool.isLive(source) || !mBufferPool.isLive(destination) || src == mImages.end() || dst == mBuffers.end()) return invalidHandle("image-to-buffer copy received an invalid resource");
    if (!hasUsage(src->second.desc.usage, ResourceUsage::TransferSource) || !hasUsage(dst->second.desc.usage, ResourceUsage::TransferDestination)) return invalidArgument("image-to-buffer usage flags are incompatible");
    if (regions.empty()) return invalidArgument("image-to-buffer copy requires at least one region");
    ScopedBufferBinding pack(GL_PIXEL_PACK_BUFFER, GL_PIXEL_PACK_BUFFER_BINDING, dst->second.name);
    ScopedTextureBinding texture(src->second.target, src->second.name);
    ScopedPixelStore pixelStore(true);
    for (const auto& region : regions)
    {
        const auto& sub = region.imageSubresource;
        const std::uint32_t width = std::max(1u, src->second.desc.extent.width >> sub.mipLevel);
        const std::uint32_t height = std::max(1u, src->second.desc.extent.height >> sub.mipLevel);
        const std::uint32_t layers = src->second.target == GL_TEXTURE_2D_ARRAY ? src->second.desc.arrayLayers : 1;
        const std::uint32_t depth = src->second.target == GL_TEXTURE_3D
            ? std::max(1u, src->second.desc.extent.depth >> sub.mipLevel) : 1;
        if ((region.bufferOffset & 3u) != 0 || sub.aspect != ImageAspect::Color ||
            sub.mipLevel >= src->second.desc.mipLevels ||
            region.imageOffset != Offset3D{} || region.imageExtent.width != width ||
            region.imageExtent.height != height || region.imageExtent.depth != depth ||
            sub.baseArrayLayer != 0 || sub.arrayLayerCount != layers ||
            region.bufferRowLength || region.bufferImageHeight)
            return unsupported("OpenGL 4.1 image readback supports tightly packed complete mip levels");
        const std::uint64_t bytes = static_cast<std::uint64_t>(width) * height * layers * depth * src->second.format.bytes;
        if (!rangeFits(region.bufferOffset, bytes, dst->second.desc.size)) return invalidArgument("image-to-buffer destination range is out of bounds");
        glGetTexImage(src->second.target, sub.mipLevel, src->second.format.external, src->second.format.type,
                      reinterpret_cast<void*>(static_cast<std::uintptr_t>(region.bufferOffset)));
    }
    if (dst->second.desc.memory == MemoryClass::Readback) dst->second.readySerial = mSubmittedSerial + 1;
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL image-to-buffer copy failed");
}

Status OpenGLDevice::generateMipmaps(ImageHandle image, const ImageSubresourceRange& range)
{
    auto found = mImages.find(handleKey(image));
    if (!mImagePool.isLive(image) || found == mImages.end()) return invalidHandle("invalid image handle");
    if (!hasUsage(found->second.desc.usage, ResourceUsage::TransferSource) || !hasUsage(found->second.desc.usage, ResourceUsage::TransferDestination)) return invalidArgument("mipmap generation requires transfer source and destination usage");
    if (range.aspect != ImageAspect::Color || range.baseMipLevel != 0 ||
        range.mipLevelCount != found->second.desc.mipLevels || range.baseArrayLayer != 0 ||
        range.arrayLayerCount != found->second.desc.arrayLayers)
        return unsupported("OpenGL 4.1 generates only a complete color mip chain");
    ScopedTextureBinding texture(found->second.target, found->second.name);
    glGenerateMipmap(found->second.target);
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL mip generation failed");
}

Status OpenGLDevice::resetQueryPool(QueryPoolHandle pool, std::uint32_t first, std::uint32_t count)
{
    auto found = mQueries.find(handleKey(pool));
    if (!mQueryPool.isLive(pool) || found == mQueries.end()) return invalidHandle("invalid query-pool handle");
    if (!count || first >= found->second.desc.count || count > found->second.desc.count - first) return invalidArgument("query reset range is empty or out of bounds");
    std::fill(found->second.written.begin() + first, found->second.written.begin() + first + count, false);
    return Status::success();
}

Status OpenGLDevice::writeTimestamp(QueryPoolHandle pool, std::uint32_t query)
{
    auto found = mQueries.find(handleKey(pool));
    if (!mQueryPool.isLive(pool) || found == mQueries.end()) return invalidHandle("invalid query-pool handle");
    if (query >= found->second.desc.count) return invalidArgument("timestamp query is out of bounds");
    glQueryCounter(found->second.names[query], GL_TIMESTAMP);
    found->second.written[query] = true;
    return Status::success();
}

void OpenGLDevice::pollFences(bool wait)
{
    while (!mFences.empty())
    {
        const GLenum result = glClientWaitSync(mFences.front().sync,
            wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, wait ? GL_TIMEOUT_IGNORED : 0);
        if (result == GL_TIMEOUT_EXPIRED) break;
        if (result == GL_WAIT_FAILED) break;
        mCompletedSerial = std::max(mCompletedSerial, mFences.front().serial);
        glDeleteSync(mFences.front().sync);
        mFences.erase(mFences.begin());
    }
}

void OpenGLDevice::drainRetirements(bool force)
{
    auto end = std::remove_if(mRetirements.begin(), mRetirements.end(), [&](const Retirement& item)
    {
        if (!force && item.releaseAfter > mCompletedSerial) return false;
        switch (item.kind)
        {
        case NativeKind::Buffer: glDeleteBuffers(1, &item.name); break;
        case NativeKind::Texture: glDeleteTextures(1, &item.name); break;
        case NativeKind::Sampler: glDeleteSamplers(1, &item.name); break;
        case NativeKind::Query: glDeleteQueries(1, &item.name); break;
        }
        return true;
    });
    mRetirements.erase(end, mRetirements.end());
}

Status OpenGLDevice::waitIdle()
{
#if LL_WINDOWS
    if (!wglGetCurrentContext()) return invalidState("waitIdle requires the owning OpenGL context to be current");
#endif
    if (mCommands.frameActive()) return invalidState("waitIdle is not allowed during an active frame");
    glFinish();
    pollFences(true);
    mCompletedSerial = std::max(mCompletedSerial, mSubmittedSerial + mFramesInFlight);
    drainRetirements(true);
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL waitIdle failed");
}

Status OpenGLCommandContext::requireTransfer() const
{
    return mFrameActive ? Status::success() : invalidState("transfer commands require an active frame");
}

Status OpenGLCommandContext::beginFrame() { return mDevice.beginFrame(); }
Status OpenGLCommandContext::endFrame() { return mDevice.endFrame(); }
Status OpenGLCommandContext::copyBuffer(BufferHandle a, BufferHandle b, std::span<const BufferCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyBuffer(a,b,r) : s; }
Status OpenGLCommandContext::copyBufferToImage(BufferHandle a, ImageHandle b, std::span<const BufferImageCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyBufferToImage(a,b,r) : s; }
Status OpenGLCommandContext::copyImageToBuffer(ImageHandle a, BufferHandle b, std::span<const BufferImageCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyImageToBuffer(a,b,r) : s; }
Status OpenGLCommandContext::generateMipmaps(ImageHandle a, const ImageSubresourceRange& r) { Status s=requireTransfer(); return s ? mDevice.generateMipmaps(a,r) : s; }
Status OpenGLCommandContext::resetQueryPool(QueryPoolHandle a, std::uint32_t b, std::uint32_t c) { Status s=requireTransfer(); return s ? mDevice.resetQueryPool(a,b,c) : s; }
Status OpenGLCommandContext::writeTimestamp(QueryPoolHandle a, std::uint32_t b) { Status s=requireTransfer(); return s ? mDevice.writeTimestamp(a,b) : s; }

} // namespace

DeviceCreationResult createOpenGLDevice(const DeviceCreateInfo& info)
{
    if (info.backend != Backend::OpenGL)
        return {nullptr, invalidArgument("OpenGL factory received a different backend")};
#if LL_WINDOWS
    if (!wglGetCurrentContext())
        return {nullptr, invalidState("OpenGL GHI device requires a current context")};
#endif
    int major = 0;
    int minor = 0;
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (!version || std::sscanf(version, "%d.%d", &major, &minor) != 2 ||
        major < 4 || (major == 4 && minor < 1))
        return {nullptr, unsupported("OpenGL GHI device requires OpenGL 4.1 or newer")};
#if LL_WINDOWS
    if (!loadResourceEntryPoints())
#else
    if (!glFenceSync || !glClientWaitSync || !glCopyBufferSubData || !glGenerateMipmap)
#endif
        return {nullptr, unsupported("required OpenGL 4.1 resource entry points are unavailable")};
    return {std::make_unique<OpenGLDevice>(info), Status::success()};
}

} // namespace LL::GHI
