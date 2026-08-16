/**
 * @file llghiopengldevice.cpp
 * @brief OpenGL 4.1 implementation of the R3 GHI draw contract.
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
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
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
    LL_GHI_LOAD_GL(glActiveTexture);
    LL_GHI_LOAD_GL(glAttachShader);
    LL_GHI_LOAD_GL(glBindAttribLocation);
    LL_GHI_LOAD_GL(glBindBufferRange);
    LL_GHI_LOAD_GL(glBindFramebuffer);
    LL_GHI_LOAD_GL(glBindSampler);
    LL_GHI_LOAD_GL(glBindVertexArray);
    LL_GHI_LOAD_GL(glBlendEquationSeparate);
    LL_GHI_LOAD_GL(glBlendFuncSeparate);
    LL_GHI_LOAD_GL(glCheckFramebufferStatus);
    LL_GHI_LOAD_GL(glClearBufferfi);
    LL_GHI_LOAD_GL(glClearBufferfv);
    LL_GHI_LOAD_GL(glColorMaski);
    LL_GHI_LOAD_GL(glCompileShader);
    LL_GHI_LOAD_GL(glCreateProgram);
    LL_GHI_LOAD_GL(glCreateShader);
    LL_GHI_LOAD_GL(glDeleteFramebuffers);
    LL_GHI_LOAD_GL(glDeleteProgram);
    LL_GHI_LOAD_GL(glDeleteShader);
    LL_GHI_LOAD_GL(glDeleteVertexArrays);
    LL_GHI_LOAD_GL(glDrawArraysInstanced);
    LL_GHI_LOAD_GL(glDrawBuffers);
    LL_GHI_LOAD_GL(glDrawElementsInstancedBaseVertex);
    LL_GHI_LOAD_GL(glEnableVertexAttribArray);
    LL_GHI_LOAD_GL(glFramebufferTexture2D);
    LL_GHI_LOAD_GL(glGenFramebuffers);
    LL_GHI_LOAD_GL(glGenVertexArrays);
    LL_GHI_LOAD_GL(glGetProgramInfoLog);
    LL_GHI_LOAD_GL(glGetProgramiv);
    LL_GHI_LOAD_GL(glGetShaderInfoLog);
    LL_GHI_LOAD_GL(glGetShaderiv);
    LL_GHI_LOAD_GL(glGetUniformBlockIndex);
    LL_GHI_LOAD_GL(glGetUniformLocation);
    LL_GHI_LOAD_GL(glLinkProgram);
    LL_GHI_LOAD_GL(glShaderSource);
    LL_GHI_LOAD_GL(glStencilFuncSeparate);
    LL_GHI_LOAD_GL(glStencilMaskSeparate);
    LL_GHI_LOAD_GL(glStencilOpSeparate);
    LL_GHI_LOAD_GL(glUniform1i);
    LL_GHI_LOAD_GL(glUniformBlockBinding);
    LL_GHI_LOAD_GL(glUseProgram);
    LL_GHI_LOAD_GL(glVertexAttribDivisor);
    LL_GHI_LOAD_GL(glVertexAttribIPointer);
    LL_GHI_LOAD_GL(glVertexAttribPointer);
    if (!sTexImage3D) sTexImage3D = reinterpret_cast<PFNGLTEXIMAGE3DPROC>(getOpenGLProcedure("glTexImage3D"));
    if (!sTexSubImage3D) sTexSubImage3D = reinterpret_cast<PFNGLTEXSUBIMAGE3DPROC>(getOpenGLProcedure("glTexSubImage3D"));
#undef LL_GHI_LOAD_GL
    return glGenQueries && glDeleteQueries && glGetQueryObjectuiv &&
           glBindBuffer && glDeleteBuffers && glGenBuffers && glBufferData &&
           glBufferSubData && glGetBufferSubData && glGenerateMipmap &&
           glCopyBufferSubData && glFenceSync && glDeleteSync && glClientWaitSync &&
           glGenSamplers && glDeleteSamplers && glSamplerParameteri &&
           glSamplerParameterf && glQueryCounter && glGetQueryObjectui64v &&
           sTexImage3D && sTexSubImage3D && glActiveTexture && glAttachShader &&
           glBindAttribLocation && glBindBufferRange && glBindFramebuffer &&
           glBindSampler && glBindVertexArray && glCheckFramebufferStatus &&
           glClearBufferfi && glClearBufferfv && glCompileShader &&
           glCreateProgram && glCreateShader && glDeleteFramebuffers &&
           glDeleteProgram && glDeleteShader && glDeleteVertexArrays &&
           glDrawArraysInstanced && glDrawElementsInstancedBaseVertex &&
           glEnableVertexAttribArray && glFramebufferTexture2D &&
           glGenFramebuffers && glGenVertexArrays && glGetProgramInfoLog &&
           glGetProgramiv && glGetShaderInfoLog && glGetShaderiv &&
           glGetUniformBlockIndex && glGetUniformLocation && glLinkProgram &&
           glShaderSource && glUniform1i && glUniformBlockBinding && glUseProgram &&
           glVertexAttribDivisor && glVertexAttribIPointer && glVertexAttribPointer;
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

GLenum translateTopology(PrimitiveTopology topology)
{
    switch (topology)
    {
    case PrimitiveTopology::Points: return GL_POINTS;
    case PrimitiveTopology::Lines: return GL_LINES;
    case PrimitiveTopology::LineStrip: return GL_LINE_STRIP;
    case PrimitiveTopology::Triangles: return GL_TRIANGLES;
    case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
    }
    return 0;
}

GLenum translateCompare(CompareOp compare)
{
    switch (compare)
    {
    case CompareOp::Never: return GL_NEVER;
    case CompareOp::Less: return GL_LESS;
    case CompareOp::Equal: return GL_EQUAL;
    case CompareOp::LessEqual: return GL_LEQUAL;
    case CompareOp::Greater: return GL_GREATER;
    case CompareOp::NotEqual: return GL_NOTEQUAL;
    case CompareOp::GreaterEqual: return GL_GEQUAL;
    case CompareOp::Always: return GL_ALWAYS;
    }
    return GL_ALWAYS;
}

GLenum translateStencilOp(StencilOp op)
{
    switch (op)
    {
    case StencilOp::Keep: return GL_KEEP;
    case StencilOp::Zero: return GL_ZERO;
    case StencilOp::Replace: return GL_REPLACE;
    case StencilOp::IncrementClamp: return GL_INCR;
    case StencilOp::DecrementClamp: return GL_DECR;
    case StencilOp::Invert: return GL_INVERT;
    case StencilOp::IncrementWrap: return GL_INCR_WRAP;
    case StencilOp::DecrementWrap: return GL_DECR_WRAP;
    }
    return GL_KEEP;
}

GLenum translateBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
    case BlendFactor::Zero: return GL_ZERO;
    case BlendFactor::One: return GL_ONE;
    case BlendFactor::SourceColor: return GL_SRC_COLOR;
    case BlendFactor::OneMinusSourceColor: return GL_ONE_MINUS_SRC_COLOR;
    case BlendFactor::DestinationColor: return GL_DST_COLOR;
    case BlendFactor::OneMinusDestinationColor: return GL_ONE_MINUS_DST_COLOR;
    case BlendFactor::SourceAlpha: return GL_SRC_ALPHA;
    case BlendFactor::OneMinusSourceAlpha: return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestinationAlpha: return GL_DST_ALPHA;
    case BlendFactor::OneMinusDestinationAlpha: return GL_ONE_MINUS_DST_ALPHA;
    }
    return GL_ONE;
}

GLenum translateBlendOp(BlendOp op)
{
    switch (op)
    {
    case BlendOp::Add: return GL_FUNC_ADD;
    case BlendOp::Subtract: return GL_FUNC_SUBTRACT;
    case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
    case BlendOp::Minimum: return GL_MIN;
    case BlendOp::Maximum: return GL_MAX;
    }
    return GL_FUNC_ADD;
}

struct GLVertexFormat
{
    GLint components = 0;
    GLenum type = 0;
    GLboolean normalized = GL_FALSE;
    bool integer = false;
    std::uint32_t bytes = 0;
};

GLVertexFormat translateVertexFormat(VertexFormat format)
{
    switch (format)
    {
    case VertexFormat::Float32: return {1, GL_FLOAT, GL_FALSE, false, 4};
    case VertexFormat::Float32x2: return {2, GL_FLOAT, GL_FALSE, false, 8};
    case VertexFormat::Float32x3: return {3, GL_FLOAT, GL_FALSE, false, 12};
    case VertexFormat::Float32x4: return {4, GL_FLOAT, GL_FALSE, false, 16};
    case VertexFormat::UNorm8x4: return {4, GL_UNSIGNED_BYTE, GL_TRUE, false, 4};
    case VertexFormat::SNorm8x4: return {4, GL_BYTE, GL_TRUE, false, 4};
    case VertexFormat::UInt16x2: return {2, GL_UNSIGNED_SHORT, GL_FALSE, true, 4};
    case VertexFormat::UInt16x4: return {4, GL_UNSIGNED_SHORT, GL_FALSE, true, 8};
    case VertexFormat::UInt32: return {1, GL_UNSIGNED_INT, GL_FALSE, true, 4};
    }
    return {};
}

std::uint32_t bindingKey(std::uint8_t group, std::uint16_t binding,
                         std::uint16_t arrayElement = 0)
{
    return (static_cast<std::uint32_t>(group) << 24) |
           (static_cast<std::uint32_t>(binding) << 8) | arrayElement;
}

const ShaderPackageDesc::CodeArtifact* findOpenGLArtifact(
    const ShaderPackageDesc::StageArtifact& stage, bool prefer46)
{
    const auto find = [&](ShaderPackageDesc::TargetProfile target)
    {
        auto it = std::find_if(stage.artifacts.begin(), stage.artifacts.end(),
            [=](const auto& artifact) { return artifact.target == target; });
        return it == stage.artifacts.end() ? nullptr : &*it;
    };
    if (prefer46)
    {
        if (const auto* artifact = find(ShaderPackageDesc::TargetProfile::OpenGL46))
            return artifact;
    }
    return find(ShaderPackageDesc::TargetProfile::OpenGL41);
}

GLuint compileShader(GLenum type, const std::string& source, std::string& diagnostic)
{
    GLuint shader = glCreateShader(type);
    if (!shader) { diagnostic = "glCreateShader failed"; return 0; }
    const GLchar* text = source.c_str();
    const GLint length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    diagnostic.resize(std::max(1, logLength));
    GLsizei written = 0;
    glGetShaderInfoLog(shader, static_cast<GLsizei>(diagnostic.size()), &written,
                       diagnostic.data());
    diagnostic.resize(std::max(0, written));
    glDeleteShader(shader);
    return 0;
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

    Status beginRendering(const RenderingInfo&) override;
    Status endRendering() override;
    Status bindPipeline(PipelineHandle) override;
    Status bindBindingSet(std::uint8_t, BindingSetHandle, std::span<const std::uint32_t>) override;
    Status setViewport(const Viewport&) override;
    Status setScissor(const ScissorRect&) override;
    Status bindVertexBuffer(std::uint32_t, BufferHandle, std::uint64_t) override;
    Status bindIndexBuffer(BufferHandle, std::uint64_t, IndexType) override;
    Status draw(const DrawArguments&) override;
    Status drawIndexed(const DrawIndexedArguments&) override;

    bool frameActive() const { return mFrameActive; }
    void setFrameActive(bool value) { mFrameActive = value; }
    bool renderingActive() const { return mRenderingActive; }
    void resetDrawState();

private:
    friend class OpenGLDevice;
    Status requireTransfer() const;
    OpenGLDevice& mDevice;
    bool mFrameActive = false;
    bool mRenderingActive = false;
    bool mViewportSet = false;
    bool mScissorSet = false;
    std::uint32_t mRenderWidth = 0;
    std::uint32_t mRenderHeight = 0;
    std::vector<Format> mRenderColorFormats;
    std::optional<Format> mRenderDepthFormat;
    PipelineHandle mPipeline;
    std::map<std::uint8_t, BindingSetHandle> mBindingSets;
    std::map<std::uint32_t, std::pair<BufferHandle, std::uint64_t>> mVertexBuffers;
    BufferHandle mIndexBuffer;
    std::uint64_t mIndexOffset = 0;
    IndexType mIndexType = IndexType::UInt16;
};

class OpenGLDevice final : public Device
{
public:
    explicit OpenGLDevice(const DeviceCreateInfo& info);
    ~OpenGLDevice() override;

    Backend backend() const override { return Backend::OpenGL; }
    const RendererCapabilities& capabilities() const override { return mCapabilities; }
    PipelineCacheDomain pipelineCacheDomain() const override
    {
        const auto text = [](GLenum name)
        {
            const GLubyte* value = glGetString(name);
            return value ? std::string(reinterpret_cast<const char*>(value)) : std::string{};
        };
        return {text(GL_VENDOR) + "|" + text(GL_RENDERER),
                text(GL_VERSION) + "|" + text(GL_SHADING_LANGUAGE_VERSION)};
    }
    CommandContext& commandContext() override { return mCommands; }

    BufferHandle createBuffer(const BufferDesc&, Status&) override;
    ImageHandle createImage(const ImageDesc&, Status&) override;
    ImageViewHandle createImageView(const ImageViewDesc&, Status&) override;
    SamplerHandle createSampler(const SamplerDesc&, Status&) override;
    QueryPoolHandle createQueryPool(const QueryPoolDesc&, Status&) override;
    ShaderPackageHandle createShaderPackage(const ShaderPackageDesc&, Status&) override;
    BindingSetHandle createBindingSet(const BindingSetDesc&, Status&) override;
    PipelineHandle createPipeline(const PipelineDesc&, Status&) override;

    Status destroy(BufferHandle) override;
    Status destroy(ImageHandle) override;
    Status destroy(ImageViewHandle) override;
    Status destroy(SamplerHandle) override;
    Status destroy(QueryPoolHandle) override;
    Status destroy(ShaderPackageHandle) override;
    Status destroy(BindingSetHandle) override;
    Status destroy(PipelineHandle) override;

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
    Status beginRendering(const RenderingInfo&);
    Status endRendering();
    Status bindPipeline(PipelineHandle);
    Status bindBindingSet(std::uint8_t, BindingSetHandle, std::span<const std::uint32_t>);
    Status setViewport(const Viewport&);
    Status setScissor(const ScissorRect&);
    Status bindVertexBuffer(std::uint32_t, BufferHandle, std::uint64_t);
    Status bindIndexBuffer(BufferHandle, std::uint64_t, IndexType);
    Status draw(const DrawArguments&);
    Status drawIndexed(const DrawIndexedArguments&);

private:
    struct BufferRecord { BufferDesc desc; GLuint name = 0; std::uint64_t readySerial = 0; };
    struct ImageRecord { ImageDesc desc; GLuint name = 0; GLenum target = 0; GLFormat format; };
    struct ViewRecord { ImageViewDesc desc; };
    struct SamplerRecord { SamplerDesc desc; GLuint name = 0; };
    struct QueryRecord { QueryPoolDesc desc; std::vector<GLuint> names; std::vector<bool> written; };
    struct ShaderRecord
    {
        ShaderPackageDesc desc;
        GLuint program = 0;
        std::unordered_map<std::uint32_t, GLuint> bufferSlots;
        std::unordered_map<std::uint32_t, GLuint> textureUnits;
    };
    struct BindingSetRecord { BindingSetDesc desc; };
    struct PipelineRecord { PipelineDesc desc; GLuint vertexArray = 0; };
    enum class NativeKind { Buffer, Texture, Sampler, Query, Program, VertexArray };
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
    HandlePool<ShaderPackageTag> mShaderPool;
    HandlePool<BindingSetTag> mBindingSetPool;
    HandlePool<PipelineTag> mPipelinePool;
    std::unordered_map<std::uint64_t, BufferRecord> mBuffers;
    std::unordered_map<std::uint64_t, ImageRecord> mImages;
    std::unordered_map<std::uint64_t, ViewRecord> mViews;
    std::unordered_map<std::uint64_t, SamplerRecord> mSamplers;
    std::unordered_map<std::uint64_t, QueryRecord> mQueries;
    std::unordered_map<std::uint64_t, ShaderRecord> mShaders;
    std::unordered_map<std::uint64_t, BindingSetRecord> mBindingSets;
    std::unordered_map<std::uint64_t, PipelineRecord> mPipelines;
    std::vector<Retirement> mRetirements;
    std::vector<Fence> mFences;
    GLuint mFramebuffer = 0;
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
    glGenFramebuffers(1, &mFramebuffer);
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
    for (const auto& [unused, shader] : mShaders) glDeleteProgram(shader.program);
    for (const auto& [unused, pipeline] : mPipelines)
        glDeleteVertexArrays(1, &pipeline.vertexArray);
    if (mFramebuffer) glDeleteFramebuffers(1, &mFramebuffer);
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

ShaderPackageHandle OpenGLDevice::createShaderPackage(
    const ShaderPackageDesc& desc, Status& status)
{
    status = canMutate();
    if (!status) return {};
    if (desc.schemaVersion != ShaderPackageDesc::CURRENT_SCHEMA_VERSION ||
        desc.pushConstantBytes != 0)
    {
        status = unsupported("OpenGL requires the current shader schema and no push constants");
        return {};
    }

    GLuint program = glCreateProgram();
    if (!program) { status = backendError("glCreateProgram failed"); return {}; }
    std::vector<GLuint> compiled;
    bool hasVertex = false;
    bool hasFragment = false;
    const bool prefer46 = mCapabilities.advancedGraphicsPipeline;
    for (const auto& stage : desc.stages)
    {
        GLenum type = 0;
        if (stage.stage == ShaderPackageDesc::Stage::Vertex)
        {
            type = GL_VERTEX_SHADER; hasVertex = true;
        }
        else if (stage.stage == ShaderPackageDesc::Stage::Fragment)
        {
            type = GL_FRAGMENT_SHADER; hasFragment = true;
        }
        else
        {
            status = unsupported("R3d OpenGL supports graphics shader packages only");
            break;
        }
        const auto* artifact = findOpenGLArtifact(stage, prefer46);
        if (!artifact || artifact->source.empty())
        {
            status = unsupported("shader package has no compatible OpenGL GLSL artifact");
            break;
        }
        std::string diagnostic;
        GLuint shader = compileShader(type, artifact->source, diagnostic);
        if (!shader)
        {
            status = Status::failure(StatusCode::BackendError,
                "OpenGL shader compilation failed: " + diagnostic);
            break;
        }
        glAttachShader(program, shader);
        compiled.push_back(shader);
    }
    if (status && (!hasVertex || !hasFragment))
        status = invalidArgument("OpenGL graphics package requires vertex and fragment stages");
    if (status)
    {
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE)
        {
            GLint length = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            std::string diagnostic(std::max(1, length), '\0');
            GLsizei written = 0;
            glGetProgramInfoLog(program, static_cast<GLsizei>(diagnostic.size()),
                                &written, diagnostic.data());
            diagnostic.resize(std::max(0, written));
            status = Status::failure(StatusCode::BackendError,
                "OpenGL program link failed: " + diagnostic);
        }
    }
    for (GLuint shader : compiled) glDeleteShader(shader);
    if (!status)
    {
        glDeleteProgram(program);
        return {};
    }

    ShaderRecord record;
    record.desc = desc;
    record.program = program;
    GLuint nextBufferSlot = 0;
    GLuint nextTextureUnit = 0;
    GLint maxBufferSlots = 0;
    glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &maxBufferSlots);
    GLint previousProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glUseProgram(program);
    std::vector<const ShaderPackageDesc::Binding*> orderedBindings;
    orderedBindings.reserve(desc.bindings.size());
    for (const auto& binding : desc.bindings) orderedBindings.push_back(&binding);
    std::sort(orderedBindings.begin(), orderedBindings.end(), [](const auto* lhs, const auto* rhs)
    {
        return std::tie(lhs->group, lhs->binding) < std::tie(rhs->group, rhs->binding);
    });
    for (const auto* bindingPointer : orderedBindings)
    {
        const auto& binding = *bindingPointer;
        if (binding.arrayCount != 1 || binding.name.empty())
        {
            status = unsupported("R3d OpenGL requires named, non-array shader bindings");
            break;
        }
        const std::uint32_t key = bindingKey(binding.group, binding.binding);
        if (record.bufferSlots.contains(key) || record.textureUnits.contains(key))
        {
            status = invalidArgument("shader package contains duplicate OpenGL bindings");
            break;
        }
        if (binding.type == ShaderPackageDesc::BindingType::UniformBuffer)
        {
            const GLuint index = glGetUniformBlockIndex(program, binding.name.c_str());
            if (index == GL_INVALID_INDEX || nextBufferSlot >= static_cast<GLuint>(maxBufferSlots))
            {
                status = backendError("reflected OpenGL uniform block was not linked");
                break;
            }
            glUniformBlockBinding(program, index, nextBufferSlot);
            record.bufferSlots.emplace(key, nextBufferSlot++);
        }
        else if (binding.type == ShaderPackageDesc::BindingType::CombinedImageSampler)
        {
            const GLint location = glGetUniformLocation(program, binding.name.c_str());
            if (location < 0 || nextTextureUnit >= mCapabilities.maxSampledImagesPerStage)
            {
                status = backendError("reflected OpenGL sampler was not linked");
                break;
            }
            glUniform1i(location, static_cast<GLint>(nextTextureUnit));
            record.textureUnits.emplace(key, nextTextureUnit++);
        }
        else
        {
            status = unsupported("R3d OpenGL supports uniform buffers and combined samplers");
            break;
        }
    }
    glUseProgram(static_cast<GLuint>(previousProgram));
    if (!status || glGetError() != GL_NO_ERROR)
    {
        glDeleteProgram(program);
        if (status) status = backendError("OpenGL shader binding reflection failed");
        return {};
    }
    ShaderPackageHandle handle = mShaderPool.allocate();
    mShaders.emplace(handleKey(handle), std::move(record));
    status = Status::success();
    return handle;
}

BindingSetHandle OpenGLDevice::createBindingSet(
    const BindingSetDesc& desc, Status& status)
{
    status = canMutate();
    if (!status) return {};
    auto shader = mShaders.find(handleKey(desc.shader));
    if (!mShaderPool.isLive(desc.shader) || shader == mShaders.end())
    {
        status = invalidHandle("binding set references an invalid shader package");
        return {};
    }
    std::size_t expected = 0;
    for (const auto& reflected : shader->second.desc.bindings)
        if (reflected.group == desc.group) ++expected;
    if (expected != desc.resources.size())
    {
        status = invalidArgument("binding set does not exactly cover its reflected group");
        return {};
    }
    for (const auto& resource : desc.resources)
    {
        const auto reflected = std::find_if(shader->second.desc.bindings.begin(),
            shader->second.desc.bindings.end(), [&](const auto& binding)
            {
                return binding.group == desc.group && binding.binding == resource.binding &&
                       binding.type == resource.type;
            });
        if (reflected == shader->second.desc.bindings.end() || resource.arrayElement != 0)
        {
            status = invalidArgument("binding set resource does not match shader reflection");
            return {};
        }
        if (resource.type == ShaderPackageDesc::BindingType::UniformBuffer)
        {
            auto buffer = mBuffers.find(handleKey(resource.buffer));
            if (!mBufferPool.isLive(resource.buffer) || buffer == mBuffers.end() ||
                !hasUsage(buffer->second.desc.usage, ResourceUsage::Uniform) ||
                !rangeFits(resource.bufferOffset,
                    resource.bufferRange ? resource.bufferRange :
                        buffer->second.desc.size - resource.bufferOffset,
                    buffer->second.desc.size))
            {
                status = invalidArgument("uniform binding references an incompatible buffer range");
                return {};
            }
        }
        else
        {
            auto view = mViews.find(handleKey(resource.imageView));
            auto sampler = mSamplers.find(handleKey(resource.sampler));
            if (!mViewPool.isLive(resource.imageView) || view == mViews.end() ||
                !mSamplerPool.isLive(resource.sampler) || sampler == mSamplers.end())
            {
                status = invalidHandle("combined sampler references an invalid image view or sampler");
                return {};
            }
            const auto image = mImages.find(handleKey(view->second.desc.image));
            if (image == mImages.end() ||
                !hasUsage(image->second.desc.usage, ResourceUsage::Sampled))
            {
                status = invalidArgument("combined sampler image lacks sampled usage");
                return {};
            }
        }
    }
    BindingSetHandle handle = mBindingSetPool.allocate();
    mBindingSets.emplace(handleKey(handle), BindingSetRecord{desc});
    status = Status::success();
    return handle;
}

PipelineHandle OpenGLDevice::createPipeline(const PipelineDesc& desc, Status& status)
{
    status = canMutate();
    if (!status) return {};
    if (!mShaderPool.isLive(desc.shader) || !mShaders.contains(handleKey(desc.shader)))
    {
        status = invalidHandle("pipeline references an invalid shader package"); return {};
    }
    if (!translateTopology(desc.topology) || desc.samples != 1 ||
        desc.colorFormats.empty() || desc.colorFormats.size() != desc.blendStates.size() ||
        !desc.specializationConstants.empty())
    {
        status = unsupported("R3d OpenGL requires single-sample graphics state without specialization constants");
        return {};
    }
    if (desc.colorFormats.size() > mCapabilities.maxColorAttachments)
    {
        status = invalidArgument("pipeline exceeds OpenGL color attachment limits"); return {};
    }
    for (Format format : desc.colorFormats)
        if (translateFormat(format).aspect != ImageAspect::Color)
        { status = invalidArgument("pipeline color format is not color-renderable"); return {}; }
    GLuint vertexArray = 0;
    glGenVertexArrays(1, &vertexArray);
    if (!vertexArray)
    {
        status = backendError("OpenGL vertex-array allocation failed"); return {};
    }
    PipelineHandle handle = mPipelinePool.allocate();
    mPipelines.emplace(handleKey(handle), PipelineRecord{desc, vertexArray});
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

Status OpenGLDevice::destroy(ShaderPackageHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    for (const auto& [unused, set] : mBindingSets)
        if (set.desc.shader == handle) return invalidState("shader package must outlive its binding sets");
    for (const auto& [unused, pipeline] : mPipelines)
        if (pipeline.desc.shader == handle) return invalidState("shader package must outlive its pipelines");
    auto found = mShaders.find(handleKey(handle));
    if (!mShaderPool.release(handle) || found == mShaders.end())
        return invalidHandle("invalid shader-package handle");
    retireNative(NativeKind::Program, found->second.program);
    mShaders.erase(found);
    return Status::success();
}

Status OpenGLDevice::destroy(BindingSetHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    auto found = mBindingSets.find(handleKey(handle));
    if (!mBindingSetPool.release(handle) || found == mBindingSets.end())
        return invalidHandle("invalid binding-set handle");
    mBindingSets.erase(found);
    return Status::success();
}

Status OpenGLDevice::destroy(PipelineHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    auto found = mPipelines.find(handleKey(handle));
    if (!mPipelinePool.release(handle) || found == mPipelines.end())
        return invalidHandle("invalid pipeline handle");
    retireNative(NativeKind::VertexArray, found->second.vertexArray);
    mPipelines.erase(found);
    return Status::success();
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
    pollFences(false);
    drainRetirements(false);
    mCommands.resetDrawState();
    mCommands.setFrameActive(true);
    return Status::success();
}

Status OpenGLDevice::endFrame()
{
    if (!mCommands.frameActive()) return invalidState("no frame is active");
    if (mCommands.renderingActive()) return invalidState("endFrame requires endRendering first");
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

Status OpenGLDevice::beginRendering(const RenderingInfo& info)
{
    if (!mCommands.frameActive()) return invalidState("beginRendering requires an active frame");
    if (mCommands.renderingActive()) return invalidState("a rendering scope is already active");
    if (!mFramebuffer || !info.width || !info.height || info.colors.empty() ||
        info.colors.size() > mCapabilities.maxColorAttachments)
        return invalidArgument("invalid OpenGL rendering scope");

    glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffer);
    std::vector<GLenum> drawBuffers;
    drawBuffers.reserve(info.colors.size());
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(~GLuint{0});
    const bool srgbTarget = std::any_of(info.colors.begin(), info.colors.end(),
        [](const AttachmentDesc& attachment)
        {
            return attachment.format == Format::RGBA8SRGB ||
                   attachment.format == Format::BGRA8SRGB;
        });
    if (srgbTarget) glEnable(GL_FRAMEBUFFER_SRGB);
    else glDisable(GL_FRAMEBUFFER_SRGB);
    for (std::size_t i = 0; i < info.colors.size(); ++i)
    {
        const auto& attachment = info.colors[i];
        auto view = mViews.find(handleKey(attachment.view));
        if (!mViewPool.isLive(attachment.view) || view == mViews.end())
            return invalidHandle("rendering references an invalid color view");
        auto image = mImages.find(handleKey(view->second.desc.image));
        if (image == mImages.end() || image->second.target != GL_TEXTURE_2D ||
            attachment.format != image->second.desc.format ||
            !hasUsage(image->second.desc.usage, ResourceUsage::ColorAttachment) ||
            image->second.desc.extent.width < info.width ||
            image->second.desc.extent.height < info.height)
            return invalidArgument("rendering color attachment is incompatible");
        const GLenum slot = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
        glFramebufferTexture2D(GL_FRAMEBUFFER, slot, GL_TEXTURE_2D,
                               image->second.name, view->second.desc.subresources.baseMipLevel);
        drawBuffers.push_back(slot);
    }
    glDrawBuffers(static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    if (info.depthStencil)
    {
        const auto& attachment = *info.depthStencil;
        auto view = mViews.find(handleKey(attachment.view));
        if (!mViewPool.isLive(attachment.view) || view == mViews.end())
            return invalidHandle("rendering references an invalid depth/stencil view");
        auto image = mImages.find(handleKey(view->second.desc.image));
        if (image == mImages.end() || image->second.target != GL_TEXTURE_2D ||
            attachment.format != image->second.desc.format ||
            !hasUsage(image->second.desc.usage, ResourceUsage::DepthStencilAttachment) ||
            image->second.desc.extent.width < info.width ||
            image->second.desc.extent.height < info.height)
            return invalidArgument("rendering depth/stencil attachment is incompatible");
        const GLenum slot = image->second.format.aspect == ImageAspect::DepthStencil
            ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        glFramebufferTexture2D(GL_FRAMEBUFFER, slot, GL_TEXTURE_2D,
                               image->second.name, view->second.desc.subresources.baseMipLevel);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return backendError("OpenGL rendering framebuffer is incomplete");
    }
    for (std::size_t i = 0; i < info.colors.size(); ++i)
        if (info.colors[i].load == LoadOp::Clear)
            glClearBufferfv(GL_COLOR, static_cast<GLint>(i), info.colors[i].clear.color.data());
    if (info.depthStencil && info.depthStencil->load == LoadOp::Clear)
    {
        if (translateFormat(info.depthStencil->format).aspect == ImageAspect::DepthStencil)
            glClearBufferfi(GL_DEPTH_STENCIL, 0, info.depthStencil->clear.depth,
                            static_cast<GLint>(info.depthStencil->clear.stencil));
        else
            glClearBufferfv(GL_DEPTH, 0, &info.depthStencil->clear.depth);
    }
    if (glGetError() != GL_NO_ERROR)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return backendError("OpenGL rendering-scope setup failed");
    }
    mCommands.resetDrawState();
    mCommands.mRenderingActive = true;
    mCommands.mRenderWidth = info.width;
    mCommands.mRenderHeight = info.height;
    for (const auto& attachment : info.colors)
        mCommands.mRenderColorFormats.push_back(attachment.format);
    if (info.depthStencil) mCommands.mRenderDepthFormat = info.depthStencil->format;
    return Status::success();
}

Status OpenGLDevice::endRendering()
{
    if (!mCommands.renderingActive()) return invalidState("no rendering scope is active");
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    mCommands.mRenderingActive = false;
    mCommands.resetDrawState();
    return glGetError() == GL_NO_ERROR ? Status::success()
                                       : backendError("OpenGL rendering-scope teardown failed");
}

Status OpenGLDevice::bindPipeline(PipelineHandle handle)
{
    if (!mCommands.renderingActive()) return invalidState("bindPipeline requires a rendering scope");
    auto pipeline = mPipelines.find(handleKey(handle));
    if (!mPipelinePool.isLive(handle) || pipeline == mPipelines.end())
        return invalidHandle("invalid pipeline handle");
    const auto shader = mShaders.find(handleKey(pipeline->second.desc.shader));
    if (shader == mShaders.end()) return invalidHandle("pipeline shader package is stale");
    const auto& desc = pipeline->second.desc;
    if (desc.colorFormats != mCommands.mRenderColorFormats ||
        desc.depthStencilFormat != mCommands.mRenderDepthFormat)
        return invalidArgument("pipeline attachment formats do not match the rendering scope");
    glUseProgram(shader->second.program);
    glBindVertexArray(pipeline->second.vertexArray);
    if (desc.cullMode == CullMode::None) glDisable(GL_CULL_FACE);
    else
    {
        glEnable(GL_CULL_FACE);
        glCullFace(desc.cullMode == CullMode::Front ? GL_FRONT : GL_BACK);
    }
    glFrontFace(desc.frontFaceCounterClockwise ? GL_CCW : GL_CW);
    if (desc.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(desc.depthWrite ? GL_TRUE : GL_FALSE);
    glDepthFunc(translateCompare(desc.depthCompare));
    if (desc.depthClamp) glEnable(GL_DEPTH_CLAMP); else glDisable(GL_DEPTH_CLAMP);
    if (desc.stencilTest)
    {
        glEnable(GL_STENCIL_TEST);
        const auto applyStencil = [](GLenum face, const StencilFaceState& value)
        {
            glStencilFuncSeparate(face, translateCompare(value.compare),
                                  static_cast<GLint>(value.reference), value.compareMask);
            glStencilMaskSeparate(face, value.writeMask);
            glStencilOpSeparate(face, translateStencilOp(value.fail),
                                translateStencilOp(value.depthFail),
                                translateStencilOp(value.pass));
        };
        applyStencil(GL_FRONT, desc.frontStencil);
        applyStencil(GL_BACK, desc.backStencil);
    }
    else glDisable(GL_STENCIL_TEST);
    const BlendState& blend = desc.blendStates.front();
    if (blend.enabled)
    {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(translateBlendFactor(blend.sourceColor),
            translateBlendFactor(blend.destinationColor),
            translateBlendFactor(blend.sourceAlpha),
            translateBlendFactor(blend.destinationAlpha));
        glBlendEquationSeparate(translateBlendOp(blend.colorOp), translateBlendOp(blend.alphaOp));
    }
    else glDisable(GL_BLEND);
    glColorMask((blend.colorWriteMask & 1) != 0, (blend.colorWriteMask & 2) != 0,
                (blend.colorWriteMask & 4) != 0, (blend.colorWriteMask & 8) != 0);
    mCommands.mPipeline = handle;
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL pipeline bind failed");
}

Status OpenGLDevice::bindBindingSet(
    std::uint8_t group, BindingSetHandle handle, std::span<const std::uint32_t> dynamicOffsets)
{
    if (!mCommands.renderingActive() || !mCommands.mPipeline)
        return invalidState("bindBindingSet requires a bound pipeline");
    auto set = mBindingSets.find(handleKey(handle));
    if (!mBindingSetPool.isLive(handle) || set == mBindingSets.end())
        return invalidHandle("invalid binding-set handle");
    auto pipeline = mPipelines.find(handleKey(mCommands.mPipeline));
    auto shader = mShaders.find(handleKey(pipeline->second.desc.shader));
    if (set->second.desc.shader != pipeline->second.desc.shader || set->second.desc.group != group)
        return invalidArgument("binding set is incompatible with the bound pipeline or group");
    std::size_t dynamicIndex = 0;
    for (const auto& resource : set->second.desc.resources)
    {
        const auto reflected = std::find_if(shader->second.desc.bindings.begin(),
            shader->second.desc.bindings.end(), [&](const auto& binding)
            { return binding.group == group && binding.binding == resource.binding; });
        if (reflected == shader->second.desc.bindings.end())
            return invalidArgument("binding set no longer matches shader reflection");
        const std::uint64_t dynamicOffset = reflected->dynamicOffset
            ? (dynamicIndex < dynamicOffsets.size() ? dynamicOffsets[dynamicIndex++] : 0) : 0;
        const std::uint32_t key = bindingKey(group, resource.binding, resource.arrayElement);
        if (resource.type == ShaderPackageDesc::BindingType::UniformBuffer)
        {
            auto buffer = mBuffers.find(handleKey(resource.buffer));
            if (buffer == mBuffers.end()) return invalidHandle("uniform buffer is stale");
            const std::uint64_t offset = resource.bufferOffset + dynamicOffset;
            const std::uint64_t range = resource.bufferRange ? resource.bufferRange :
                buffer->second.desc.size - resource.bufferOffset;
            if (!rangeFits(offset, range, buffer->second.desc.size) ||
                offset % mCapabilities.uniformBufferOffsetAlignment != 0)
                return invalidArgument("dynamic uniform-buffer range is invalid or unaligned");
            glBindBufferRange(GL_UNIFORM_BUFFER, shader->second.bufferSlots.at(key),
                              buffer->second.name, static_cast<GLintptr>(offset),
                              static_cast<GLsizeiptr>(range));
        }
        else
        {
            auto view = mViews.find(handleKey(resource.imageView));
            auto sampler = mSamplers.find(handleKey(resource.sampler));
            if (view == mViews.end() || sampler == mSamplers.end())
                return invalidHandle("sampled image or sampler is stale");
            auto image = mImages.find(handleKey(view->second.desc.image));
            const GLuint unit = shader->second.textureUnits.at(key);
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(image->second.target, image->second.name);
            glBindSampler(unit, sampler->second.name);
        }
    }
    if (dynamicIndex != dynamicOffsets.size())
        return invalidArgument("dynamic offset count does not match shader reflection");
    mCommands.mBindingSets[group] = handle;
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL binding-set bind failed");
}

Status OpenGLDevice::setViewport(const Viewport& viewport)
{
    if (!mCommands.renderingActive()) return invalidState("setViewport requires a rendering scope");
    if (viewport.x < 0.f || viewport.y < 0.f ||
        viewport.width <= 0.f || viewport.height <= 0.f ||
        viewport.x + viewport.width > mCommands.mRenderWidth ||
        viewport.y + viewport.height > mCommands.mRenderHeight ||
        viewport.minDepth < 0.f || viewport.maxDepth > 1.f || viewport.minDepth > viewport.maxDepth)
        return invalidArgument("invalid OpenGL viewport");
    glViewport(static_cast<GLint>(viewport.x), static_cast<GLint>(viewport.y),
               static_cast<GLsizei>(viewport.width), static_cast<GLsizei>(viewport.height));
    glDepthRange(viewport.minDepth, viewport.maxDepth);
    mCommands.mViewportSet = true;
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL viewport update failed");
}

Status OpenGLDevice::setScissor(const ScissorRect& scissor)
{
    if (!mCommands.renderingActive()) return invalidState("setScissor requires a rendering scope");
    if (scissor.x < 0 || scissor.y < 0 || !scissor.width || !scissor.height ||
        static_cast<std::uint64_t>(scissor.x) + scissor.width > mCommands.mRenderWidth ||
        static_cast<std::uint64_t>(scissor.y) + scissor.height > mCommands.mRenderHeight)
        return invalidArgument("invalid OpenGL scissor rectangle");
    glEnable(GL_SCISSOR_TEST);
    glScissor(scissor.x, scissor.y, scissor.width, scissor.height);
    mCommands.mScissorSet = true;
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL scissor update failed");
}

Status OpenGLDevice::bindVertexBuffer(std::uint32_t slot, BufferHandle handle, std::uint64_t offset)
{
    auto buffer = mBuffers.find(handleKey(handle));
    if (!mCommands.renderingActive() || !mCommands.mPipeline)
        return invalidState("bindVertexBuffer requires a bound pipeline");
    if (!mBufferPool.isLive(handle) || buffer == mBuffers.end())
        return invalidHandle("invalid vertex-buffer handle");
    if (!hasUsage(buffer->second.desc.usage, ResourceUsage::Vertex) || offset >= buffer->second.desc.size)
        return invalidArgument("vertex buffer usage or offset is invalid");
    mCommands.mVertexBuffers[slot] = {handle, offset};
    return Status::success();
}

Status OpenGLDevice::bindIndexBuffer(BufferHandle handle, std::uint64_t offset, IndexType type)
{
    auto buffer = mBuffers.find(handleKey(handle));
    if (!mCommands.renderingActive() || !mCommands.mPipeline)
        return invalidState("bindIndexBuffer requires a bound pipeline");
    if (!mBufferPool.isLive(handle) || buffer == mBuffers.end())
        return invalidHandle("invalid index-buffer handle");
    const std::uint64_t alignment = type == IndexType::UInt16 ? 2 : 4;
    if (!hasUsage(buffer->second.desc.usage, ResourceUsage::Index) ||
        offset >= buffer->second.desc.size || offset % alignment != 0)
        return invalidArgument("index buffer usage or offset is invalid");
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer->second.name);
    mCommands.mIndexBuffer = handle;
    mCommands.mIndexOffset = offset;
    mCommands.mIndexType = type;
    return Status::success();
}

Status OpenGLDevice::draw(const DrawArguments& arguments)
{
    if (!mCommands.renderingActive() || !mCommands.mPipeline)
        return invalidState("draw requires a bound pipeline");
    if (!mCommands.mViewportSet || !mCommands.mScissorSet)
        return invalidState("draw requires explicit viewport and scissor state");
    if (!arguments.vertexCount || !arguments.instanceCount || arguments.firstInstance != 0)
        return unsupported("R3d OpenGL draw requires nonzero counts and firstInstance zero");
    auto pipeline = mPipelines.find(handleKey(mCommands.mPipeline));
    auto shader = mShaders.find(handleKey(pipeline->second.desc.shader));
    for (const auto& binding : shader->second.desc.bindings)
        if (!mCommands.mBindingSets.contains(binding.group))
            return invalidState("draw is missing a reflected binding group");
    for (const auto& attribute : pipeline->second.desc.vertexAttributes)
    {
        const auto layout = std::find_if(pipeline->second.desc.vertexBuffers.begin(),
            pipeline->second.desc.vertexBuffers.end(), [&](const auto& value)
            { return value.slot == attribute.bufferSlot; });
        auto bound = mCommands.mVertexBuffers.find(attribute.bufferSlot);
        if (layout == pipeline->second.desc.vertexBuffers.end() || bound == mCommands.mVertexBuffers.end())
            return invalidState("draw is missing a required vertex buffer");
        auto buffer = mBuffers.find(handleKey(bound->second.first));
        const auto format = translateVertexFormat(attribute.format);
        glBindBuffer(GL_ARRAY_BUFFER, buffer->second.name);
        const auto pointer = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(
            bound->second.second + attribute.offset));
        glEnableVertexAttribArray(attribute.location);
        if (format.integer)
            glVertexAttribIPointer(attribute.location, format.components, format.type,
                                   layout->stride, pointer);
        else
            glVertexAttribPointer(attribute.location, format.components, format.type,
                                  format.normalized, layout->stride, pointer);
        glVertexAttribDivisor(attribute.location,
            layout->inputRate == VertexInputRate::PerInstance ? 1 : 0);
    }
    glDrawArraysInstanced(translateTopology(pipeline->second.desc.topology),
                          arguments.firstVertex, arguments.vertexCount,
                          arguments.instanceCount);
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL draw failed");
}

Status OpenGLDevice::drawIndexed(const DrawIndexedArguments& arguments)
{
    if (!mCommands.renderingActive() || !mCommands.mPipeline)
        return invalidState("drawIndexed requires a bound pipeline");
    if (!mCommands.mViewportSet || !mCommands.mScissorSet)
        return invalidState("drawIndexed requires explicit viewport and scissor state");
    if (!mCommands.mIndexBuffer) return invalidState("drawIndexed requires an index buffer");
    if (!arguments.indexCount || !arguments.instanceCount || arguments.firstInstance != 0)
        return unsupported("R3d OpenGL indexed draw requires nonzero counts and firstInstance zero");
    auto pipeline = mPipelines.find(handleKey(mCommands.mPipeline));
    auto shader = mShaders.find(handleKey(pipeline->second.desc.shader));
    for (const auto& binding : shader->second.desc.bindings)
        if (!mCommands.mBindingSets.contains(binding.group))
            return invalidState("drawIndexed is missing a reflected binding group");
    auto indexBuffer = mBuffers.find(handleKey(mCommands.mIndexBuffer));
    const std::uint64_t indexBytes = mCommands.mIndexType == IndexType::UInt16 ? 2 : 4;
    const std::uint64_t firstByte = mCommands.mIndexOffset +
        static_cast<std::uint64_t>(arguments.firstIndex) * indexBytes;
    const std::uint64_t drawBytes = static_cast<std::uint64_t>(arguments.indexCount) * indexBytes;
    if (indexBuffer == mBuffers.end() || !rangeFits(firstByte, drawBytes, indexBuffer->second.desc.size))
        return invalidArgument("drawIndexed index range exceeds the bound buffer");
    for (const auto& attribute : pipeline->second.desc.vertexAttributes)
    {
        const auto layout = std::find_if(pipeline->second.desc.vertexBuffers.begin(),
            pipeline->second.desc.vertexBuffers.end(), [&](const auto& value)
            { return value.slot == attribute.bufferSlot; });
        auto bound = mCommands.mVertexBuffers.find(attribute.bufferSlot);
        if (layout == pipeline->second.desc.vertexBuffers.end() || bound == mCommands.mVertexBuffers.end())
            return invalidState("drawIndexed is missing a required vertex buffer");
        auto buffer = mBuffers.find(handleKey(bound->second.first));
        const auto format = translateVertexFormat(attribute.format);
        glBindBuffer(GL_ARRAY_BUFFER, buffer->second.name);
        const auto vertexPointer = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(
            bound->second.second + attribute.offset));
        glEnableVertexAttribArray(attribute.location);
        if (format.integer)
            glVertexAttribIPointer(attribute.location, format.components, format.type,
                                   layout->stride, vertexPointer);
        else
            glVertexAttribPointer(attribute.location, format.components, format.type,
                                  format.normalized, layout->stride, vertexPointer);
        glVertexAttribDivisor(attribute.location,
            layout->inputRate == VertexInputRate::PerInstance ? 1 : 0);
    }
    const GLenum type = mCommands.mIndexType == IndexType::UInt16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    const auto pointer = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(
        firstByte));
    glDrawElementsInstancedBaseVertex(translateTopology(pipeline->second.desc.topology),
        arguments.indexCount, type, pointer, arguments.instanceCount, arguments.vertexOffset);
    return glGetError() == GL_NO_ERROR ? Status::success() : backendError("OpenGL indexed draw failed");
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
        case NativeKind::Program: glDeleteProgram(item.name); break;
        case NativeKind::VertexArray: glDeleteVertexArrays(1, &item.name); break;
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

void OpenGLCommandContext::resetDrawState()
{
    mViewportSet = false;
    mScissorSet = false;
    mRenderWidth = 0;
    mRenderHeight = 0;
    mRenderColorFormats.clear();
    mRenderDepthFormat.reset();
    mPipeline = {};
    mBindingSets.clear();
    mVertexBuffers.clear();
    mIndexBuffer = {};
    mIndexOffset = 0;
    mIndexType = IndexType::UInt16;
}

Status OpenGLCommandContext::beginFrame() { return mDevice.beginFrame(); }
Status OpenGLCommandContext::endFrame() { return mDevice.endFrame(); }
Status OpenGLCommandContext::copyBuffer(BufferHandle a, BufferHandle b, std::span<const BufferCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyBuffer(a,b,r) : s; }
Status OpenGLCommandContext::copyBufferToImage(BufferHandle a, ImageHandle b, std::span<const BufferImageCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyBufferToImage(a,b,r) : s; }
Status OpenGLCommandContext::copyImageToBuffer(ImageHandle a, BufferHandle b, std::span<const BufferImageCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyImageToBuffer(a,b,r) : s; }
Status OpenGLCommandContext::generateMipmaps(ImageHandle a, const ImageSubresourceRange& r) { Status s=requireTransfer(); return s ? mDevice.generateMipmaps(a,r) : s; }
Status OpenGLCommandContext::resetQueryPool(QueryPoolHandle a, std::uint32_t b, std::uint32_t c) { Status s=requireTransfer(); return s ? mDevice.resetQueryPool(a,b,c) : s; }
Status OpenGLCommandContext::writeTimestamp(QueryPoolHandle a, std::uint32_t b) { Status s=requireTransfer(); return s ? mDevice.writeTimestamp(a,b) : s; }
Status OpenGLCommandContext::beginRendering(const RenderingInfo& a) { return mDevice.beginRendering(a); }
Status OpenGLCommandContext::endRendering() { return mDevice.endRendering(); }
Status OpenGLCommandContext::bindPipeline(PipelineHandle a) { return mDevice.bindPipeline(a); }
Status OpenGLCommandContext::bindBindingSet(std::uint8_t a, BindingSetHandle b, std::span<const std::uint32_t> c) { return mDevice.bindBindingSet(a,b,c); }
Status OpenGLCommandContext::setViewport(const Viewport& a) { return mDevice.setViewport(a); }
Status OpenGLCommandContext::setScissor(const ScissorRect& a) { return mDevice.setScissor(a); }
Status OpenGLCommandContext::bindVertexBuffer(std::uint32_t a, BufferHandle b, std::uint64_t c) { return mDevice.bindVertexBuffer(a,b,c); }
Status OpenGLCommandContext::bindIndexBuffer(BufferHandle a, std::uint64_t b, IndexType c) { return mDevice.bindIndexBuffer(a,b,c); }
Status OpenGLCommandContext::draw(const DrawArguments& a) { return mDevice.draw(a); }
Status OpenGLCommandContext::drawIndexed(const DrawIndexedArguments& a) { return mDevice.drawIndexed(a); }

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
