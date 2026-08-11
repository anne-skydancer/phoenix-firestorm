/**
 * @file llrendertarget.cpp
 * @brief LLRenderTarget implementation
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llrendertarget.h"
#include "llrender.h"
#include "llgl.h"
#include "rhi/rhi.h"        // <FSVulkan P0.3d> route FBO ops through the RHI seam

LLRenderTarget* LLRenderTarget::sBoundTarget = NULL;
U32 LLRenderTarget::sBytesAllocated = 0;

// <FSVulkan P0.3d> FBO ops via the RHI. gRHI-null fallback to raw GL is essential -- LLRenderTarget
// is used at init before gRHI bootstraps (see the P0.3d "route 100%" invariant + null-window caveat).
// The GL backend's target_attach/draw_buffers act on the currently-bound target, so the caller does
// rt_bind first (mirroring LLRenderTarget's own bind mFBO -> attach -> bind sCurFBO).
namespace
{
    GLenum rt_gl_attach(RhiAttachment p)
    {
        switch (p)
        {
            case RHI_ATTACH_COLOR0: return GL_COLOR_ATTACHMENT0;
            case RHI_ATTACH_COLOR1: return GL_COLOR_ATTACHMENT1;
            case RHI_ATTACH_COLOR2: return GL_COLOR_ATTACHMENT2;
            case RHI_ATTACH_COLOR3: return GL_COLOR_ATTACHMENT3;
            case RHI_ATTACH_DEPTH:  return GL_DEPTH_ATTACHMENT;
        }
        return GL_COLOR_ATTACHMENT0;
    }
    RhiTarget rt_create()      { if (gRHI) return gRHI->target_create(); GLuint f = 0; glGenFramebuffers(1, &f); return (RhiTarget)f; }
    void rt_bind(RhiTarget t)  { if (gRHI) gRHI->target_bind(t); else glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)t); }
    void rt_attach(RhiTarget t, RhiAttachment p, GLenum textype, U32 tex)
    {
        if (gRHI) gRHI->target_attach(t, p, tex, 0);
        else glFramebufferTexture2D(GL_FRAMEBUFFER, rt_gl_attach(p), textype, tex, 0);
    }
    void rt_draw_buffers(RhiTarget t, U32 count)
    {
        if (gRHI) { gRHI->target_draw_buffers(t, count); return; }
        if (count == 0) { glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE); return; }
        GLenum b[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
        glDrawBuffers((GLsizei)(count < 4 ? count : 4), b);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }
    void rt_destroy(RhiTarget t) { if (gRHI) gRHI->target_destroy(t); else { GLuint f = (GLuint)t; glDeleteFramebuffers(1, &f); } }
    void rt_viewport(S32 x, S32 y, S32 w, S32 h) { if (gRHI) gRHI->set_viewport(x, y, w, h); else glViewport(x, y, w, h); }
    void rt_clear(U32 flags)
    {
        if (gRHI) { gRHI->target_clear(flags); return; }
        GLbitfield m = 0;
        if (flags & RHI_CLEAR_COLOR)   m |= GL_COLOR_BUFFER_BIT;
        if (flags & RHI_CLEAR_DEPTH)   m |= GL_DEPTH_BUFFER_BIT;
        if (flags & RHI_CLEAR_STENCIL) m |= GL_STENCIL_BUFFER_BIT;
        glClear(m);
    }
    U32 rt_get_error() { return gRHI ? gRHI->get_error() : (U32)glGetError(); }
    void rt_gen_mips_2d(U32 tex) { if (gRHI) gRHI->texture_gen_mips(tex); else glGenerateMipmap(GL_TEXTURE_2D); }
}

void check_framebuffer_status()
{
    if (gDebugGL)
    {
        GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        switch (status)
        {
        case GL_FRAMEBUFFER_COMPLETE:
            break;
        default:
            LL_WARNS() << "check_framebuffer_status failed -- " << std::hex << status << LL_ENDL;
            ll_fail("check_framebuffer_status failed");
            break;
        }
    }
}

bool LLRenderTarget::sUseFBO = false;
U32 LLRenderTarget::sCurFBO = 0;


extern S32 gGLViewport[4];

U32 LLRenderTarget::sCurResX = 0;
U32 LLRenderTarget::sCurResY = 0;

LLRenderTarget::LLRenderTarget() :
    mResX(0),
    mResY(0),
    mFBO(0),
    mDepth(0),
    mUseDepth(false),
    mUsage(LLTexUnit::TT_TEXTURE)
{
}

LLRenderTarget::~LLRenderTarget()
{
    release();
}

void LLRenderTarget::resize(U32 resx, U32 resy)
{
    //for accounting, get the number of pixels added/subtracted
    S32 pix_diff = (resx*resy)-(mResX*mResY);

    mResX = resx;
    mResY = resy;

    llassert(mInternalFormat.size() == mTex.size());

    for (U32 i = 0; i < mTex.size(); ++i)
    { //resize color attachments
        gGL.getTexUnit(0)->bindManual(mUsage, mTex[i]);
        LLImageGL::setManualImage(LLTexUnit::getInternalType(mUsage), 0, mInternalFormat[i], mResX, mResY, GL_RGBA, GL_UNSIGNED_BYTE, NULL, false);
        sBytesAllocated += pix_diff*4;
    }

    if (mDepth)
    {
        gGL.getTexUnit(0)->bindManual(mUsage, mDepth);
        U32 internal_type = LLTexUnit::getInternalType(mUsage);
        LLImageGL::setManualImage(internal_type, 0, GL_DEPTH_COMPONENT24, mResX, mResY, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL, false);

        sBytesAllocated += pix_diff*4;
    }
}


bool LLRenderTarget::allocate(U32 resx, U32 resy, U32 color_fmt, bool depth, LLTexUnit::eTextureType usage, LLTexUnit::eTextureMipGeneration generateMipMaps)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(usage == LLTexUnit::TT_TEXTURE);
    llassert(!isBoundInStack());

    if(mResX == resx && mResY == resy && mUsage == usage && depth == mUseDepth && mGenerateMipMaps == generateMipMaps)
    {
        return true;
    }
    resx = llmin(resx, (U32) gGLManager.mGLMaxTextureSize);
    resy = llmin(resy, (U32) gGLManager.mGLMaxTextureSize);

    release();

    mResX = resx;
    mResY = resy;

    mUsage = usage;
    mUseDepth = depth;

    mGenerateMipMaps = generateMipMaps;

    if (mGenerateMipMaps != LLTexUnit::TMG_NONE) {
        // Calculate the number of mip levels based upon resolution that we should have.
        mMipLevels = 1 + (U32)floor(log10((float)llmax(mResX, mResY)) / log10(2.0));
    }

    if (depth)
    {
        if (!allocateDepth())
        {
            LL_WARNS() << "Failed to allocate depth buffer for render target." << LL_ENDL;
            return false;
        }
    }

    mFBO = rt_create();

    if (mDepth)
    {
        rt_bind(mFBO);
        rt_attach(mFBO, RHI_ATTACH_DEPTH, LLTexUnit::getInternalType(mUsage), mDepth);
        rt_bind(sCurFBO);
    }

    return addColorAttachment(color_fmt);
}

void LLRenderTarget::setColorAttachment(LLImageGL* img, LLGLuint use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    llassert(img != nullptr); // img must not be null
    llassert(sUseFBO); // FBO support must be enabled
    llassert(mDepth == 0); // depth buffers not supported with this mode
    llassert(mTex.empty()); // mTex must be empty with this mode (binding target should be done via LLImageGL)
    llassert(!isBoundInStack());

    if (mFBO == 0)
    {
        mFBO = rt_create();
    }

    mResX = img->getWidth();
    mResY = img->getHeight();
    mUsage = img->getTarget();

    if (use_name == 0)
    {
        use_name = img->getTexName();
    }

    mTex.push_back(use_name);

    rt_bind(mFBO);
    rt_attach(mFBO, RHI_ATTACH_COLOR0, LLTexUnit::getInternalType(mUsage), use_name);
        stop_glerror();

    check_framebuffer_status();

    rt_bind(sCurFBO);
}

void LLRenderTarget::releaseColorAttachment()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    llassert(!isBoundInStack());
    llassert(mTex.size() == 1); //cannot use releaseColorAttachment with LLRenderTarget managed color targets
    llassert(mFBO != 0);  // mFBO must be valid

    rt_bind(mFBO);
    rt_attach(mFBO, RHI_ATTACH_COLOR0, LLTexUnit::getInternalType(mUsage), 0);
    rt_bind(sCurFBO);

    mTex.clear();
}

bool LLRenderTarget::addColorAttachment(U32 color_fmt)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(!isBoundInStack());

    if (color_fmt == 0)
    {
        return true;
    }

    U32 offset = static_cast<U32>(mTex.size());

    if( offset >= 4 )
    {
        LL_WARNS() << "Too many color attachments" << LL_ENDL;
        llassert( offset < 4 );
        return false;
    }
    if( offset > 0 && (mFBO == 0) )
    {
        llassert(  mFBO != 0 );
        return false;
    }

    U32 tex;
    LLImageGL::generateTextures(1, &tex);
    gGL.getTexUnit(0)->bindManual(mUsage, tex);

    stop_glerror();


    {
        clear_glerror();
        LLImageGL::setManualImage(LLTexUnit::getInternalType(mUsage), 0, color_fmt, mResX, mResY, GL_RGBA, GL_UNSIGNED_BYTE, NULL, false);
        if (rt_get_error() != GL_NO_ERROR)
        {
            LL_WARNS() << "Could not allocate color buffer for render target." << LL_ENDL;
            return false;
        }
    }

    sBytesAllocated += mResX*mResY*4;

    stop_glerror();


    if (offset == 0)
    { //use bilinear filtering on single texture render targets that aren't multisampled
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        stop_glerror();
    }
    else
    { //don't filter data attachments
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
        stop_glerror();
    }

    if (mUsage != LLTexUnit::TT_RECT_TEXTURE)
    {
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_MIRROR);
        stop_glerror();
    }
    else
    {
        // ATI doesn't support mirrored repeat for rectangular textures.
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        stop_glerror();
    }

    if (mFBO)
    {
        rt_bind(mFBO);
        rt_attach(mFBO, (RhiAttachment)(RHI_ATTACH_COLOR0 + offset), LLTexUnit::getInternalType(mUsage), tex);

        check_framebuffer_status();

        rt_bind(sCurFBO);
    }

    mTex.push_back(tex);
    mInternalFormat.push_back(color_fmt);

    if (gDebugGL)
    { //bind and unbind to validate target
        bindTarget();
        flush();
    }


    return true;
}

bool LLRenderTarget::allocateDepth()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LLImageGL::generateTextures(1, &mDepth);
    gGL.getTexUnit(0)->bindManual(mUsage, mDepth);

    U32 internal_type = LLTexUnit::getInternalType(mUsage);
    stop_glerror();
    clear_glerror();
    LLImageGL::setManualImage(internal_type, 0, GL_DEPTH_COMPONENT24, mResX, mResY, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL, false);
    gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);

    sBytesAllocated += mResX*mResY*4;

    if (rt_get_error() != GL_NO_ERROR)
    {
        LL_WARNS() << "Unable to allocate depth buffer for render target." << LL_ENDL;
        return false;
    }

    return true;
}

void LLRenderTarget::shareDepthBuffer(LLRenderTarget& target)
{
    llassert(!isBoundInStack());

    if (!mFBO || !target.mFBO)
    {
        LL_ERRS() << "Cannot share depth buffer between non FBO render targets." << LL_ENDL;
    }

    if (target.mDepth)
    {
        LL_ERRS() << "Attempting to override existing depth buffer.  Detach existing buffer first." << LL_ENDL;
    }

    if (target.mUseDepth)
    {
        LL_ERRS() << "Attempting to override existing shared depth buffer. Detach existing buffer first." << LL_ENDL;
    }

    if (mDepth)
    {
        rt_bind(target.mFBO);
        rt_attach(target.mFBO, RHI_ATTACH_DEPTH, LLTexUnit::getInternalType(mUsage), mDepth);
        check_framebuffer_status();
        rt_bind(sCurFBO);

        target.mUseDepth = true;
    }
}

void LLRenderTarget::release()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(!isBoundInStack());

    if (mDepth)
    {
        LLImageGL::deleteTextures(1, &mDepth);

        mDepth = 0;

        sBytesAllocated -= mResX*mResY*4;
    }
    // else if (mFBO)
    if (mFBO)
    {
        rt_bind(mFBO);

        if (mUseDepth)
        { //detach shared depth buffer
            rt_attach(mFBO, RHI_ATTACH_DEPTH, LLTexUnit::getInternalType(mUsage), 0);
            mUseDepth = false;
        }

        rt_bind(sCurFBO);
    }

    // Detach any extra color buffers (e.g. SRGB spec buffers)
    //
    if (mFBO && (mTex.size() > 1))
    {
        rt_bind(mFBO);
        size_t z;
        for (z = mTex.size() - 1; z >= 1; z--)
        {
            sBytesAllocated -= mResX*mResY*4;
            rt_attach(mFBO, (RhiAttachment)(RHI_ATTACH_COLOR0 + z), LLTexUnit::getInternalType(mUsage), 0);
            LLImageGL::deleteTextures(1, &mTex[z]);
        }
        rt_bind(sCurFBO);
    }

    if (mFBO)
    {
        if (mFBO == sCurFBO)
        {
            sCurFBO = 0;
            rt_bind(0);
        }

        rt_destroy(mFBO);
        mFBO = 0;
    }

    if (mTex.size() > 0)
    {
        sBytesAllocated -= mResX*mResY*4;
        LLImageGL::deleteTextures(1, &mTex[0]);
    }

    mTex.clear();
    mInternalFormat.clear();

    mResX = mResY = 0;
}

void LLRenderTarget::bindTarget()
{
    LL_PROFILE_GPU_ZONE("bindTarget");
    llassert(mFBO);
    llassert(!isBoundInStack());

    rt_bind(mFBO);
    sCurFBO = mFBO;

    //setup multiple render targets (empty => GL_NONE draw+read)
    rt_draw_buffers(mFBO, mTex.empty() ? 0 : static_cast<U32>(mTex.size()));
    check_framebuffer_status();

    rt_viewport(0, 0, mResX, mResY);
    sCurResX = mResX;
    sCurResY = mResY;

    mPreviousRT = sBoundTarget;
    sBoundTarget = this;
}

void LLRenderTarget::clear(U32 mask_in)
{
    LL_PROFILE_GPU_ZONE("clear");
    llassert(mFBO);
    U32 mask = GL_COLOR_BUFFER_BIT;
    if (mUseDepth)
    {
        mask |= GL_DEPTH_BUFFER_BIT;

    }
    U32 gl_mask = mask & mask_in;
    U32 flags = 0;
    if (gl_mask & GL_COLOR_BUFFER_BIT)   flags |= RHI_CLEAR_COLOR;
    if (gl_mask & GL_DEPTH_BUFFER_BIT)   flags |= RHI_CLEAR_DEPTH;
    if (gl_mask & GL_STENCIL_BUFFER_BIT) flags |= RHI_CLEAR_STENCIL;
    if (mFBO)
    {
        check_framebuffer_status();
        stop_glerror();
        rt_clear(flags);
        stop_glerror();
    }
    else
    {
        LLGLEnable scissor(GL_SCISSOR_TEST);
        if (gRHI) gRHI->set_scissor(1, 0, 0, mResX, mResY); else glScissor(0, 0, mResX, mResY);
        stop_glerror();
        rt_clear(flags);
    }
}

U32 LLRenderTarget::getTexture(U32 attachment) const
{
    if (attachment >= mTex.size())
    {
        LL_WARNS() << "Invalid attachment index " << attachment << " for size " << mTex.size() << LL_ENDL;
        llassert(false);
        return 0;
    }
    return mTex[attachment];
}

U32 LLRenderTarget::getNumTextures() const
{
    return static_cast<U32>(mTex.size());
}

void LLRenderTarget::bindTexture(U32 index, S32 channel, LLTexUnit::eTextureFilterOptions filter_options)
{
    gGL.getTexUnit(channel)->bindManual(mUsage, getTexture(index), filter_options == LLTexUnit::TFO_TRILINEAR || filter_options == LLTexUnit::TFO_ANISOTROPIC);
    gGL.getTexUnit(channel)->setTextureFilteringOption(filter_options);
}

void LLRenderTarget::flush()
{
    LL_PROFILE_GPU_ZONE("rt flush");
    gGL.flush();
    llassert(mFBO);
    llassert(sCurFBO == mFBO);
    llassert(sBoundTarget == this);

    if (mGenerateMipMaps == LLTexUnit::TMG_AUTO)
    {
        LL_PROFILE_GPU_ZONE("rt generate mipmaps");
        bindTexture(0, 0, LLTexUnit::TFO_TRILINEAR);
        rt_gen_mips_2d(getTexture(0));
    }

    if (mPreviousRT)
    {
        // a bit hacky -- pop the RT stack back two frames and push
        // the previous frame back on to play nice with the GL state machine
        sBoundTarget = mPreviousRT->mPreviousRT;
        mPreviousRT->bindTarget();
    }
    else
    {
        sBoundTarget = nullptr;
        rt_bind(0);                 // gl_target_bind(0) also restores GL_BACK draw/read
        sCurFBO = 0;
        rt_viewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
        sCurResX = gGLViewport[2];
        sCurResY = gGLViewport[3];
    }
}

bool LLRenderTarget::isComplete() const
{
    return !mTex.empty() || mDepth;
}

void LLRenderTarget::getViewport(S32* viewport)
{
    viewport[0] = 0;
    viewport[1] = 0;
    viewport[2] = mResX;
    viewport[3] = mResY;
}

bool LLRenderTarget::isBoundInStack() const
{
    LLRenderTarget* cur = sBoundTarget;
    while (cur && cur != this)
    {
        cur = cur->mPreviousRT;
    }

    return cur == this;
}

void LLRenderTarget::swapFBORefs(LLRenderTarget& other)
{
    // Must be initialized
    llassert(mFBO);
    llassert(other.mFBO);

    // Must be unbound
    // *NOTE: mPreviousRT can be non-null even if this target is unbound - presumably for debugging purposes?
    llassert(sCurFBO != mFBO);
    llassert(sCurFBO != other.mFBO);
    llassert(!isBoundInStack());
    llassert(!other.isBoundInStack());

    // Must be same type
    llassert(sUseFBO == other.sUseFBO);
    llassert(mResX == other.mResX);
    llassert(mResY == other.mResY);
    llassert(mInternalFormat == other.mInternalFormat);
    llassert(mTex.size() == other.mTex.size());
    llassert(mDepth == other.mDepth);
    llassert(mUseDepth == other.mUseDepth);
    llassert(mGenerateMipMaps == other.mGenerateMipMaps);
    llassert(mMipLevels == other.mMipLevels);
    llassert(mUsage == other.mUsage);

    std::swap(mFBO, other.mFBO);
    std::swap(mTex, other.mTex);
}
