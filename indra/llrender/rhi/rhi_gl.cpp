/**
 * @file rhi_gl.cpp
 * GL backend for the RHI (rhi.h). Implements the RhiApi vtable against real OpenGL — the
 * parity baseline. Handles ARE GL names (buffer/texture/fbo/program/query), so mapping is
 * trivial and 1:1 with today's calls. Nothing routes through this yet (P0.3d wires the
 * chokepoints); it is additive and compile-only for now.
 *
 * NOTE (transition): while LLGLState still tracks fixed-function state, the R8 setters here
 * call GL directly. That is fine because this backend is dormant until a chokepoint routes
 * through it; once state routing lands, LLGLState becomes the caller of these (or is retired).
 */
#include "linden_common.h"
#include "rhi.h"
#include "llglheaders.h"

namespace {

// ---- enum mapping (RHI -> GL) ----
GLenum gl_compare(RhiCompare c)
{
    switch (c)
    {
        case RHI_CMP_NEVER:    return GL_NEVER;
        case RHI_CMP_LESS:     return GL_LESS;
        case RHI_CMP_EQUAL:    return GL_EQUAL;
        case RHI_CMP_LEQUAL:   return GL_LEQUAL;
        case RHI_CMP_GREATER:  return GL_GREATER;
        case RHI_CMP_NOTEQUAL: return GL_NOTEQUAL;
        case RHI_CMP_GEQUAL:   return GL_GEQUAL;
        case RHI_CMP_ALWAYS:   return GL_ALWAYS;
    }
    return GL_LEQUAL;
}

GLenum gl_blend(RhiBlendFactor f)
{
    switch (f)
    {
        case RHI_BF_ZERO:                 return GL_ZERO;
        case RHI_BF_ONE:                  return GL_ONE;
        case RHI_BF_SRC_ALPHA:            return GL_SRC_ALPHA;
        case RHI_BF_ONE_MINUS_SRC_ALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
        case RHI_BF_SRC_COLOR:            return GL_SRC_COLOR;
        case RHI_BF_ONE_MINUS_SRC_COLOR:  return GL_ONE_MINUS_SRC_COLOR;
        case RHI_BF_DST_ALPHA:            return GL_DST_ALPHA;
        case RHI_BF_ONE_MINUS_DST_ALPHA:  return GL_ONE_MINUS_DST_ALPHA;
        case RHI_BF_DST_COLOR:            return GL_DST_COLOR;
        case RHI_BF_ONE_MINUS_DST_COLOR:  return GL_ONE_MINUS_DST_COLOR;
    }
    return GL_ONE;
}

GLenum gl_blendop(RhiBlendOp o)
{
    switch (o)
    {
        case RHI_BO_ADD:     return GL_FUNC_ADD;
        case RHI_BO_SUB:     return GL_FUNC_SUBTRACT;
        case RHI_BO_REV_SUB: return GL_FUNC_REVERSE_SUBTRACT;
        case RHI_BO_MIN:     return GL_MIN;
        case RHI_BO_MAX:     return GL_MAX;
    }
    return GL_FUNC_ADD;
}

GLenum gl_prim(RhiPrimitive p)
{
    switch (p)
    {
        case RHI_PRIM_TRIANGLES: return GL_TRIANGLES;
        case RHI_PRIM_TRISTRIP:  return GL_TRIANGLE_STRIP;
        case RHI_PRIM_TRIFAN:    return GL_TRIANGLE_FAN;
        case RHI_PRIM_POINTS:    return GL_POINTS;
        case RHI_PRIM_LINES:     return GL_LINES;
        case RHI_PRIM_LINESTRIP: return GL_LINE_STRIP;
        case RHI_PRIM_LINELOOP:  return GL_LINE_LOOP;
    }
    return GL_TRIANGLES;
}

GLenum gl_attr(RhiAttribType t, GLboolean* norm)
{
    *norm = GL_FALSE;
    switch (t)
    {
        case RHI_ATTR_F32:     return GL_FLOAT;
        case RHI_ATTR_U8_NORM: *norm = GL_TRUE; return GL_UNSIGNED_BYTE;
        case RHI_ATTR_U8:      return GL_UNSIGNED_BYTE;
        case RHI_ATTR_I16_NORM:*norm = GL_TRUE; return GL_SHORT;
        case RHI_ATTR_U16:     return GL_UNSIGNED_SHORT;
        case RHI_ATTR_U32:     return GL_UNSIGNED_INT;
    }
    return GL_FLOAT;
}

// internal/format/type triple for a texture format
void gl_format(RhiFormat f, GLenum* internal, GLenum* fmt, GLenum* type)
{
    switch (f)
    {
        case RHI_FMT_RGBA8:            *internal = GL_RGBA8;              *fmt = GL_RGBA; *type = GL_UNSIGNED_BYTE; return;
        case RHI_FMT_SRGBA8:           *internal = GL_SRGB8_ALPHA8;      *fmt = GL_RGBA; *type = GL_UNSIGNED_BYTE; return;
        case RHI_FMT_RGBA16F:          *internal = GL_RGBA16F;           *fmt = GL_RGBA; *type = GL_HALF_FLOAT;    return;
        case RHI_FMT_RGB10_A2:         *internal = GL_RGB10_A2;          *fmt = GL_RGBA; *type = GL_UNSIGNED_INT_2_10_10_10_REV; return;
        case RHI_FMT_RGB16F:           *internal = GL_RGB16F;            *fmt = GL_RGB;  *type = GL_HALF_FLOAT;    return;
        case RHI_FMT_R8:               *internal = GL_R8;                *fmt = GL_RED;  *type = GL_UNSIGNED_BYTE; return;
        case RHI_FMT_R16F:             *internal = GL_R16F;              *fmt = GL_RED;  *type = GL_HALF_FLOAT;    return;
        case RHI_FMT_RG16F:            *internal = GL_RG16F;             *fmt = GL_RG;   *type = GL_HALF_FLOAT;    return;
        case RHI_FMT_R32F:             *internal = GL_R32F;              *fmt = GL_RED;  *type = GL_FLOAT;         return;
        case RHI_FMT_RGBA32F:          *internal = GL_RGBA32F;           *fmt = GL_RGBA; *type = GL_FLOAT;         return;
        case RHI_FMT_RGB8:             *internal = GL_RGB8;              *fmt = GL_RGB;  *type = GL_UNSIGNED_BYTE; return;
        case RHI_FMT_R32UI:            *internal = GL_R32UI;             *fmt = GL_RED_INTEGER; *type = GL_UNSIGNED_INT; return;
        case RHI_FMT_DEPTH24:          *internal = GL_DEPTH_COMPONENT24; *fmt = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_INT; return;
        case RHI_FMT_DEPTH32F:         *internal = GL_DEPTH_COMPONENT32F;*fmt = GL_DEPTH_COMPONENT; *type = GL_FLOAT;        return;
        case RHI_FMT_DEPTH24_STENCIL8: *internal = GL_DEPTH24_STENCIL8;  *fmt = GL_DEPTH_STENCIL;   *type = GL_UNSIGNED_INT_24_8; return;
        default:                       *internal = GL_RGBA8;             *fmt = GL_RGBA; *type = GL_UNSIGNED_BYTE; return;
    }
}

GLuint g_scratch_vao = 0;   // GL core requires a bound VAO for vertex specification + draw

// ================= R1 device / frame / caps =================
void gl_begin_frame(void) {}                          // GL presents via swapBuffers (llwindow), not here
void gl_end_frame(void) {}
void gl_set_viewport(int x, int y, int w, int h) { glViewport(x, y, w, h); }
uint32_t gl_get_error(void) { return (uint32_t)glGetError(); }
void gl_set_pixel_store(int pack, int unpack) { glPixelStorei(GL_PACK_ALIGNMENT, pack); glPixelStorei(GL_UNPACK_ALIGNMENT, unpack); }
void gl_get_caps(RhiCaps* out)
{
    if (!out) return;
    const char* r = (const char*)glGetString(GL_RENDERER);
    const char* v = (const char*)glGetString(GL_VENDOR);
    const char* ver = (const char*)glGetString(GL_VERSION);
    strncpy(out->renderer, r ? r : "", sizeof(out->renderer) - 1);
    strncpy(out->vendor,   v ? v : "", sizeof(out->vendor) - 1);
    strncpy(out->version,  ver ? ver : "", sizeof(out->version) - 1);
    out->device_type = 2; // discrete (GL can't tell reliably)
    GLint mt = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mt);           out->max_texture_2d = (uint32_t)mt;
    GLint ms = 0; glGetIntegerv(GL_MAX_SAMPLES, &ms);                out->max_samples = (uint32_t)ms;
    GLint ub = 0; glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &ub);     out->max_uniform_block_size = (uint32_t)ub;
}

// ================= R2 buffers =================
GLenum buf_target(RhiBufferUsage u)
{
    switch (u)
    {
        case RHI_BUF_VERTEX:  return GL_ARRAY_BUFFER;
        case RHI_BUF_INDEX:   return GL_ELEMENT_ARRAY_BUFFER;
        case RHI_BUF_UNIFORM: return GL_UNIFORM_BUFFER;
        case RHI_BUF_STREAM:  return GL_ARRAY_BUFFER;
    }
    return GL_ARRAY_BUFFER;
}
RhiBuffer gl_buffer_create(RhiBufferUsage usage, size_t size, const void* initial)
{
    GLuint b = 0; glGenBuffers(1, &b);
    GLenum tgt = buf_target(usage);
    glBindBuffer(tgt, b);
    glBufferData(tgt, (GLsizeiptr)size, initial, usage == RHI_BUF_STREAM ? GL_STREAM_DRAW : GL_STATIC_DRAW);
    glBindBuffer(tgt, 0);
    return (RhiBuffer)b;
}
void gl_buffer_update(RhiBuffer b, size_t offset, size_t size, const void* data)
{
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)b);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
void* gl_buffer_map(RhiBuffer b, size_t offset, size_t size)
{
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)b);
    return glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size,
                            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
}
void gl_buffer_unmap(RhiBuffer b)
{
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)b);
    glUnmapBuffer(GL_ARRAY_BUFFER);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
void gl_buffer_destroy(RhiBuffer b) { GLuint n = (GLuint)b; glDeleteBuffers(1, &n); }
void gl_bind_vertex_buffer(RhiBuffer b, const RhiVertexLayout* layout)
{
    glBindVertexArray(g_scratch_vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)b);
    if (layout)
    {
        for (uint32_t i = 0; i < layout->attrib_count; ++i)
        {
            const RhiVertexAttrib& a = layout->attribs[i];
            GLboolean norm = GL_FALSE;
            GLenum ty = gl_attr(a.type, &norm);
            glEnableVertexAttribArray(a.slot);
            glVertexAttribPointer(a.slot, (GLint)a.count, ty, norm, (GLsizei)layout->stride,
                                  (const void*)(size_t)a.offset);
        }
    }
}
void gl_bind_index_buffer(RhiBuffer b, RhiIndexType) { glBindVertexArray(g_scratch_vao); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)b); }

// ---- R2 granular (LLVertexBuffer): buffers, VAOs, per-attribute pointers ----
static GLenum gl_buftgt(RhiBufferTarget t)
{
    if (t == RHI_BUFTGT_INDEX)   return GL_ELEMENT_ARRAY_BUFFER;
    if (t == RHI_BUFTGT_UNIFORM) return GL_UNIFORM_BUFFER;
    return GL_ARRAY_BUFFER;
}
RhiBuffer gl_buffer_gen(void) { GLuint b = 0; glGenBuffers(1, &b); return (RhiBuffer)b; }
void gl_buffer_gen_n(uint32_t count, RhiBuffer* out) { glGenBuffers((GLsizei)count, (GLuint*)out); }
void gl_buffer_del_n(uint32_t count, const RhiBuffer* in) { glDeleteBuffers((GLsizei)count, (const GLuint*)in); }
void gl_buffer_bind(RhiBufferTarget t, RhiBuffer b) { glBindBuffer(gl_buftgt(t), (GLuint)b); }
void gl_buffer_data(RhiBufferTarget t, size_t size, const void* data, RhiBufferHint hint)
{
    GLenum usage = GL_STATIC_DRAW;
    if (hint == RHI_BUFHINT_DYNAMIC)     usage = GL_DYNAMIC_DRAW;
    else if (hint == RHI_BUFHINT_STREAM) usage = GL_STREAM_DRAW;
    glBufferData(gl_buftgt(t), (GLsizeiptr)size, data, usage);
}
void gl_buffer_subdata(RhiBufferTarget t, size_t offset, size_t size, const void* data)
{ glBufferSubData(gl_buftgt(t), (GLintptr)offset, (GLsizeiptr)size, data); }
void gl_buffer_del(RhiBuffer b) { GLuint n = (GLuint)b; glDeleteBuffers(1, &n); }
RhiVAO gl_vao_gen(void) { GLuint v = 0; glGenVertexArrays(1, &v); return (RhiVAO)v; }
void gl_vao_bind(RhiVAO v) { glBindVertexArray((GLuint)v); }
void gl_vao_del(RhiVAO v) { GLuint n = (GLuint)v; glDeleteVertexArrays(1, &n); }
void gl_vertex_attrib(uint32_t index, int size, RhiVertexFormat fmt, uint32_t stride, size_t offset)
{
    const void* ptr = (const void*)offset;
    switch (fmt)
    {
        case RHI_VF_FLOAT:      glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, (GLsizei)stride, ptr); break;
        case RHI_VF_U8_NORM:    glVertexAttribPointer(index, size, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, ptr); break;
        case RHI_VF_FLOAT_NORM: glVertexAttribPointer(index, size, GL_FLOAT, GL_TRUE, (GLsizei)stride, ptr); break;
        case RHI_VF_U16_INT:    glVertexAttribIPointer(index, size, GL_UNSIGNED_SHORT, (GLsizei)stride, ptr); break;
        case RHI_VF_U32_INT:    glVertexAttribIPointer(index, size, GL_UNSIGNED_INT, (GLsizei)stride, ptr); break;
    }
}
void gl_enable_vertex_attrib(uint32_t index) { glEnableVertexAttribArray(index); }
void gl_disable_vertex_attrib(uint32_t index) { glDisableVertexAttribArray(index); }
void gl_draw_range_elements(RhiPrimitive prim, uint32_t start, uint32_t end, uint32_t count, RhiIndexType idx, size_t offset_bytes)
{
    glDrawRangeElements(gl_prim(prim), start, end, (GLsizei)count,
                        idx == RHI_IDX_U32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void*)offset_bytes);
}
void gl_bind_uniform_block(uint32_t binding, RhiBuffer b) { glBindBufferBase(GL_UNIFORM_BUFFER, binding, (GLuint)b); }

// ================= R3 textures =================
RhiTexture gl_texture_create(const RhiTextureDesc* d)
{
    if (!d) return 0;
    GLuint t = 0; glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    GLenum internal, fmt, type; gl_format(d->format, &internal, &fmt, &type);
    if (d->immutable)
    {   // immutable storage -- required to bind this texture as an image (load/store) under Zink/Vulkan
        glTexStorage2D(GL_TEXTURE_2D, (GLsizei)(d->levels ? d->levels : 1), internal,
                       (GLsizei)d->width, (GLsizei)d->height);
    }
    else
    {
        glTexImage2D(GL_TEXTURE_2D, 0, internal, (GLsizei)d->width, (GLsizei)d->height, 0, fmt, type, nullptr);
    }
    GLenum wrap = d->wrap == RHI_WRAP_CLAMP ? GL_CLAMP_TO_EDGE : (d->wrap == RHI_WRAP_MIRROR ? GL_MIRRORED_REPEAT : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    GLenum mag = d->filter == RHI_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mag);
    return (RhiTexture)t;
}
void gl_texture_upload(RhiTexture t, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       RhiFormat data_fmt, const void* data, size_t)
{
    glBindTexture(GL_TEXTURE_2D, (GLuint)t);
    GLenum internal, fmt, type; gl_format(data_fmt, &internal, &fmt, &type);
    glTexSubImage2D(GL_TEXTURE_2D, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, fmt, type, data);
}
void gl_texture_gen_mips(RhiTexture t) { glBindTexture(GL_TEXTURE_2D, (GLuint)t); glGenerateMipmap(GL_TEXTURE_2D); }
void gl_texture_copy(RhiTexture dst, RhiTexture, uint32_t w, uint32_t h)
{
    // scaleDown / blit-to-self via copy from the read framebuffer (caller sets up src as read target)
    glBindTexture(GL_TEXTURE_2D, (GLuint)dst);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)w, (GLsizei)h);
}
void gl_texture_destroy(RhiTexture t) { GLuint n = (GLuint)t; glDeleteTextures(1, &n); }

// ---- texture-unit state (LLTexUnit) — k_gl_textype mirrors sGLTextureType in llrender.cpp ----
static const GLenum k_gl_textype[] = {
    GL_TEXTURE_2D, GL_TEXTURE_RECTANGLE, GL_TEXTURE_CUBE_MAP,
    GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_3D
};
void gl_active_texture(uint32_t unit) { glActiveTexture(GL_TEXTURE0 + unit); }
void gl_bind_texture(RhiTexType type, RhiTexture name) { glBindTexture(k_gl_textype[type], (GLuint)name); }
void gl_set_tex_wrap(RhiTexType type, RhiWrap mode, int set_r)
{
    GLenum tgt = k_gl_textype[type];
    GLint m = mode == RHI_WRAP_MIRROR ? GL_MIRRORED_REPEAT : (mode == RHI_WRAP_CLAMP ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(tgt, GL_TEXTURE_WRAP_S, m);
    glTexParameteri(tgt, GL_TEXTURE_WRAP_T, m);
    if (set_r) glTexParameteri(tgt, GL_TEXTURE_WRAP_R, m);
}
void gl_set_tex_filter(RhiTexType type, RhiMinFilter mn, RhiMagFilter mg)
{
    static const GLint k_min[] = { GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST,
                                   GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR };
    GLenum tgt = k_gl_textype[type];
    glTexParameteri(tgt, GL_TEXTURE_MIN_FILTER, k_min[mn]);
    glTexParameteri(tgt, GL_TEXTURE_MAG_FILTER, mg == RHI_MAGF_NEAREST ? GL_NEAREST : GL_LINEAR);
}
void gl_set_tex_anisotropy(RhiTexType type, float a) { glTexParameterf(k_gl_textype[type], GL_TEXTURE_MAX_ANISOTROPY, a); }
void gl_set_tex_compare(RhiTexType type, int enable, RhiCompare func)
{
    GLenum tgt = k_gl_textype[type];
    glTexParameteri(tgt, GL_TEXTURE_COMPARE_MODE, enable ? GL_COMPARE_R_TO_TEXTURE : GL_NONE);
    if (enable) glTexParameteri(tgt, GL_TEXTURE_COMPARE_FUNC, gl_compare(func));
}
void gl_set_tex_wrap_axis(RhiTexType type, uint32_t axis, RhiWrap mode)
{
    GLint m = mode == RHI_WRAP_MIRROR ? GL_MIRRORED_REPEAT : (mode == RHI_WRAP_CLAMP ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    GLenum pname = axis == 0 ? GL_TEXTURE_WRAP_S : (axis == 1 ? GL_TEXTURE_WRAP_T : GL_TEXTURE_WRAP_R);
    glTexParameteri(k_gl_textype[type], pname, m);
}
void gl_set_tex_mag_filter(RhiTexType type, RhiMagFilter mag)
{
    glTexParameteri(k_gl_textype[type], GL_TEXTURE_MAG_FILTER, mag == RHI_MAGF_NEAREST ? GL_NEAREST : GL_LINEAR);
}
uint32_t gl_get_active_texture(void) { GLint a = GL_TEXTURE0; glGetIntegerv(GL_ACTIVE_TEXTURE, &a); return (uint32_t)(a - GL_TEXTURE0); }

// ================= R4 render targets =================
RhiTarget gl_target_create(void) { GLuint fbo = 0; glGenFramebuffers(1, &fbo); return (RhiTarget)fbo; }
GLenum gl_attach(RhiAttachment p)
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
// Operate on the currently-bound target: the caller does target_bind first (mirrors LLRenderTarget's
// bind mFBO -> attach -> bind sCurFBO). The t param is for the VK backend's per-target accumulation.
void gl_target_attach(RhiTarget /*t*/, RhiAttachment point, RhiTexture tex, uint32_t level)
{
    glFramebufferTexture2D(GL_FRAMEBUFFER, gl_attach(point), GL_TEXTURE_2D, (GLuint)tex, (GLint)level);
}
void gl_target_draw_buffers(RhiTarget /*t*/, uint32_t color_count)
{
    if (color_count == 0) { glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE); return; }
    GLenum bufs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers((GLsizei)(color_count < 4 ? color_count : 4), bufs);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
}
void gl_draw_buffers(uint32_t color_count)
{
    GLenum bufs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers((GLsizei)(color_count < 4 ? color_count : 4), bufs);
}
uint32_t gl_target_get_status(void) { return (uint32_t)glCheckFramebufferStatus(GL_FRAMEBUFFER); }
void gl_target_bind(RhiTarget t)
{
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)t);
    if (t == 0) { glDrawBuffer(GL_BACK); glReadBuffer(GL_BACK); }  // default framebuffer draws to BACK
}
void gl_bind_framebuffer(RhiFbBind which, RhiTarget t)  // plain read/draw/both bind, no draw-buffer side effects
{
    GLenum p = which == RHI_FB_READ ? GL_READ_FRAMEBUFFER : (which == RHI_FB_DRAW ? GL_DRAW_FRAMEBUFFER : GL_FRAMEBUFFER);
    glBindFramebuffer(p, (GLuint)t);
}
void gl_set_clear_color(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
void gl_set_clear_depth(float d) { glClearDepth(d); }
void gl_set_clear_stencil(int s) { glClearStencil(s); }
void gl_target_clear(uint32_t flags)
{
    GLbitfield mask = 0;
    if (flags & RHI_CLEAR_COLOR)   mask |= GL_COLOR_BUFFER_BIT;
    if (flags & RHI_CLEAR_DEPTH)   mask |= GL_DEPTH_BUFFER_BIT;
    if (flags & RHI_CLEAR_STENCIL) mask |= GL_STENCIL_BUFFER_BIT;
    glClear(mask);
}
void gl_target_blit(RhiTarget dst, RhiTarget src, uint32_t w, uint32_t h, RhiFilter f)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dst);
    glBlitFramebuffer(0, 0, (GLint)w, (GLint)h, 0, 0, (GLint)w, (GLint)h, GL_COLOR_BUFFER_BIT,
                      f == RHI_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR);
}
void gl_target_read_pixels(RhiTarget t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, RhiFormat out_fmt, void* out)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)t);
    GLenum internal, fmt, type; gl_format(out_fmt, &internal, &fmt, &type);
    glReadPixels((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, fmt, type, out);
}
void gl_read_pixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h, RhiFormat rfmt, void* out)
{
    GLenum internal, fmt, type; gl_format(rfmt, &internal, &fmt, &type);
    glReadPixels((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, fmt, type, out);
}
void gl_copy_tex_sub_image_3d(RhiTexType type, uint32_t level, uint32_t xoff, uint32_t yoff, uint32_t zoff,
                              uint32_t srcx, uint32_t srcy, uint32_t w, uint32_t h)
{
    glCopyTexSubImage3D(k_gl_textype[type], (GLint)level, (GLint)xoff, (GLint)yoff, (GLint)zoff,
                        (GLint)srcx, (GLint)srcy, (GLsizei)w, (GLsizei)h);
}
void gl_copy_tex_sub_image_2d(RhiTexType type, uint32_t level, uint32_t xoff, uint32_t yoff,
                              uint32_t srcx, uint32_t srcy, uint32_t w, uint32_t h)
{
    glCopyTexSubImage2D(k_gl_textype[type], (GLint)level, (GLint)xoff, (GLint)yoff,
                        (GLint)srcx, (GLint)srcy, (GLsizei)w, (GLsizei)h);
}
void gl_gen_mipmap(RhiTexType type) { glGenerateMipmap(k_gl_textype[type]); }  // mips for the tex bound to `type`
void gl_tex_image_2d(RhiTexType type, uint32_t level, RhiFormat internal_fmt,
                     uint32_t w, uint32_t h, RhiFormat data_fmt, const void* data)
{
    GLenum in_i, in_f, in_t;  gl_format(internal_fmt, &in_i, &in_f, &in_t);  // sized internal format
    GLenum cl_i, cl_f, cl_t;  gl_format(data_fmt,     &cl_i, &cl_f, &cl_t);  // (format,type) of `data`
    glTexImage2D(k_gl_textype[type], (GLint)level, (GLint)in_i, (GLsizei)w, (GLsizei)h, 0, cl_f, cl_t, data);
}
void gl_target_destroy(RhiTarget t) { GLuint n = (GLuint)t; glDeleteFramebuffers(1, &n); }

// ================= R5 shaders (GL compiles GLSL) =================
GLuint compile_stage(GLenum stage, const char* src)
{
    if (!src) return 0;
    GLuint s = glCreateShader(stage);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}
RhiShader gl_shader_create(const RhiShaderDesc* d)
{
    if (!d) return 0;
    GLuint prog = glCreateProgram();
    GLuint vs = compile_stage(GL_VERTEX_SHADER, d->glsl_vertex);
    GLuint fs = compile_stage(GL_FRAGMENT_SHADER, d->glsl_fragment);
    GLuint gs = d->glsl_geometry ? compile_stage(GL_GEOMETRY_SHADER, d->glsl_geometry) : 0;
    if (vs) glAttachShader(prog, vs);
    if (fs) glAttachShader(prog, fs);
    if (gs) glAttachShader(prog, gs);
    glLinkProgram(prog);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (gs) glDeleteShader(gs);
    return (RhiShader)prog;
}
void gl_shader_bind(RhiShader s) { glUseProgram((GLuint)s); }
int  gl_shader_uniform_id(RhiShader s, const char* name) { return name ? glGetUniformLocation((GLuint)s, name) : -1; }
void gl_shader_destroy(RhiShader s) { glDeleteProgram((GLuint)s); }

// ================= R6 uniforms (by GL location) =================
void gl_set_uniform(RhiShader, int loc, RhiUniformType type, uint32_t count, const void* data, int transpose)
{
    if (loc < 0 || !data) return;
    const GLfloat* f = (const GLfloat*)data;
    const GLint*   i = (const GLint*)data;
    const GLuint*  u = (const GLuint*)data;
    GLboolean t = transpose ? GL_TRUE : GL_FALSE;
    switch (type)
    {
        case RHI_UNIFORM_F32:    glUniform1fv(loc, count, f); break;
        case RHI_UNIFORM_VEC2:   glUniform2fv(loc, count, f); break;
        case RHI_UNIFORM_VEC3:   glUniform3fv(loc, count, f); break;
        case RHI_UNIFORM_VEC4:   glUniform4fv(loc, count, f); break;
        case RHI_UNIFORM_I32:    glUniform1iv(loc, count, i); break;
        case RHI_UNIFORM_IVEC2:  glUniform2iv(loc, count, i); break;
        case RHI_UNIFORM_IVEC3:  glUniform3iv(loc, count, i); break;
        case RHI_UNIFORM_IVEC4:  glUniform4iv(loc, count, i); break;
        case RHI_UNIFORM_U32:    glUniform1uiv(loc, count, u); break;
        case RHI_UNIFORM_UVEC2:  glUniform2uiv(loc, count, u); break;
        case RHI_UNIFORM_UVEC3:  glUniform3uiv(loc, count, u); break;
        case RHI_UNIFORM_UVEC4:  glUniform4uiv(loc, count, u); break;
        case RHI_UNIFORM_MAT2:   glUniformMatrix2fv(loc, count, t, f); break;
        case RHI_UNIFORM_MAT3:   glUniformMatrix3fv(loc, count, t, f); break;
        case RHI_UNIFORM_MAT4:   glUniformMatrix4fv(loc, count, t, f); break;
        case RHI_UNIFORM_MAT3X4: glUniformMatrix3x4fv(loc, count, t, f); break;
    }
}
void gl_set_vertex_attrib4f(uint32_t index, const float* v) { glVertexAttrib4fv(index, v); }

// ================= R7 draw =================
void gl_draw_indexed(RhiPrimitive prim, uint32_t index_count, uint32_t index_offset, uint32_t base_vertex, uint32_t instances)
{
    const void* off = (const void*)(size_t)(index_offset * sizeof(GLushort));
    if (instances > 1)
        glDrawElementsInstanced(gl_prim(prim), (GLsizei)index_count, GL_UNSIGNED_SHORT, off, (GLsizei)instances);
    else if (base_vertex)
        glDrawElementsBaseVertex(gl_prim(prim), (GLsizei)index_count, GL_UNSIGNED_SHORT, off, (GLint)base_vertex);
    else
        glDrawElements(gl_prim(prim), (GLsizei)index_count, GL_UNSIGNED_SHORT, off);
}
void gl_draw_arrays(RhiPrimitive prim, uint32_t first, uint32_t count, uint32_t instances)
{
    if (instances > 1) glDrawArraysInstanced(gl_prim(prim), (GLint)first, (GLsizei)count, (GLsizei)instances);
    else               glDrawArrays(gl_prim(prim), (GLint)first, (GLsizei)count);
}

// ================= R8 dynamic state =================
void gl_set_blend_enabled(int enable) { if (enable) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
void gl_set_blend_func(RhiBlendFactor sRGB, RhiBlendFactor dRGB, RhiBlendFactor sA, RhiBlendFactor dA)
{ glBlendFuncSeparate(gl_blend(sRGB), gl_blend(dRGB), gl_blend(sA), gl_blend(dA)); }
void gl_set_blend_equation(RhiBlendOp rgb, RhiBlendOp a) { glBlendEquationSeparate(gl_blendop(rgb), gl_blendop(a)); }
void gl_set_depth_test(int enable) { if (enable) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); }
void gl_set_depth_write(int enable) { glDepthMask(enable ? GL_TRUE : GL_FALSE); }
void gl_set_depth_func(RhiCompare cmp) { glDepthFunc(gl_compare(cmp)); }
GLenum gl_cap(RhiCapability c)
{
    switch (c)
    {
        case RHI_CAP_BLEND:               return GL_BLEND;
        case RHI_CAP_CULL_FACE:           return GL_CULL_FACE;
        case RHI_CAP_DEPTH_TEST:          return GL_DEPTH_TEST;
        case RHI_CAP_STENCIL_TEST:        return GL_STENCIL_TEST;
        case RHI_CAP_SCISSOR_TEST:        return GL_SCISSOR_TEST;
        case RHI_CAP_POLYGON_OFFSET_FILL: return GL_POLYGON_OFFSET_FILL;
        case RHI_CAP_POLYGON_OFFSET_LINE: return GL_POLYGON_OFFSET_LINE;
        case RHI_CAP_DEPTH_CLAMP:         return GL_DEPTH_CLAMP;
        case RHI_CAP_MULTISAMPLE:         return GL_MULTISAMPLE;
        case RHI_CAP_PROGRAM_POINT_SIZE:  return GL_PROGRAM_POINT_SIZE;
        case RHI_CAP_CLIP_DISTANCE0:      return GL_CLIP_DISTANCE0;   /* == GL_CLIP_PLANE0 (0x3000) */
        case RHI_CAP_ALPHA_TEST:          return GL_ALPHA_TEST;
        case RHI_CAP_LINE_SMOOTH:         return GL_LINE_SMOOTH;
        case RHI_CAP_TEXTURE_CUBE_MAP_SEAMLESS: return GL_TEXTURE_CUBE_MAP_SEAMLESS;
        default:                          return 0;
    }
}
void gl_set_capability(RhiCapability c, int enable)
{
    GLenum g = gl_cap(c);
    if (!g) return;
    if (enable) glEnable(g); else glDisable(g);
}
void gl_set_cull(RhiCull mode)
{
    if (mode == RHI_CULL_NONE) { glDisable(GL_CULL_FACE); return; }
    glEnable(GL_CULL_FACE);
    glCullFace(mode == RHI_CULL_FRONT ? GL_FRONT : GL_BACK);
}
void gl_set_color_mask(int r, int g, int b, int a) { glColorMask(r, g, b, a); }
void gl_set_scissor(int enable, int x, int y, int w, int h)
{
    if (enable) { glEnable(GL_SCISSOR_TEST); glScissor(x, y, w, h); } else glDisable(GL_SCISSOR_TEST);
}
void gl_set_stencil(int enable, RhiCompare cmp, uint32_t ref, uint32_t mask, uint32_t write_mask)
{
    if (enable) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glStencilFunc(gl_compare(cmp), (GLint)ref, mask);
    glStencilMask(write_mask);
}
void gl_set_depth_clamp(int enable) { if (enable) glEnable(GL_DEPTH_CLAMP); else glDisable(GL_DEPTH_CLAMP); }
void gl_set_reverse_z(int enable) { glClipControl(GL_LOWER_LEFT, enable ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE); }
void gl_set_polygon_offset(float factor, float units) { glPolygonOffset(factor, units); }
void gl_set_line_width(float w) { glLineWidth(w); }
void gl_set_point_size(float s) { glPointSize(s); }
void gl_set_polygon_mode(RhiPolygonMode m)
{
    glPolygonMode(GL_FRONT_AND_BACK, m == RHI_PM_LINE ? GL_LINE : (m == RHI_PM_POINT ? GL_POINT : GL_FILL));
}
void gl_set_cull_face(RhiCullFace f)
{
    glCullFace(f == RHI_CF_FRONT ? GL_FRONT : (f == RHI_CF_FRONT_AND_BACK ? GL_FRONT_AND_BACK : GL_BACK));
}
void gl_set_stencil_func(RhiCompare func, int ref, uint32_t mask) { glStencilFunc(gl_compare(func), ref, mask); }
void gl_set_stencil_op(RhiStencilOp sf, RhiStencilOp df, RhiStencilOp dp)
{
    static const GLenum k[] = { GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
    glStencilOp(k[sf], k[df], k[dp]);
}
void gl_set_stencil_mask(uint32_t mask) { glStencilMask(mask); }
int gl_is_enabled(RhiCapability cap) { GLenum g = gl_cap(cap); return g && glIsEnabled(g) ? 1 : 0; }

// ================= R9 sync =================
void gl_flush(void) { glFlush(); }
void gl_finish(void) { glFinish(); }
void gl_debug_force_driver_crash(void) { glDeleteTextures(1, NULL); }  // intentionally invalid: crash the GL driver

// ================= R10 queries =================
static GLenum gl_query_target(RhiQueryType t)
{
    switch (t)
    {
        case RHI_QUERY_ANY_SAMPLES:    return GL_ANY_SAMPLES_PASSED;
        case RHI_QUERY_SAMPLES:        return GL_SAMPLES_PASSED;
        case RHI_QUERY_TIME_ELAPSED:   return GL_TIME_ELAPSED;
        case RHI_QUERY_PRIMITIVES:     return GL_PRIMITIVES_GENERATED;
        case RHI_QUERY_XFB_PRIMITIVES: return GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN;
    }
    return GL_ANY_SAMPLES_PASSED;
}
RhiQuery gl_query_create(void) { GLuint q = 0; glGenQueries(1, &q); return (RhiQuery)q; }
void gl_query_gen_n(uint32_t count, RhiQuery* out) { glGenQueries((GLsizei)count, (GLuint*)out); }
void gl_query_begin(RhiQueryType t, RhiQuery q) { glBeginQuery(gl_query_target(t), (GLuint)q); }
void gl_query_end(RhiQueryType t) { glEndQuery(gl_query_target(t)); }
int  gl_query_available(RhiQuery q) { GLuint a = 0; glGetQueryObjectuiv((GLuint)q, GL_QUERY_RESULT_AVAILABLE, &a); return (int)a; }
void gl_query_result(RhiQuery q, uint64_t* out) { GLuint64 r = 0; glGetQueryObjectui64v((GLuint)q, GL_QUERY_RESULT, &r); if (out) *out = (uint64_t)r; }
void gl_query_destroy(RhiQuery q) { GLuint n = (GLuint)q; glDeleteQueries(1, &n); }

// ================= R11 storage / images / barriers (PPLL OIT) =================
static GLenum gl_bb_target(RhiBufferBindTarget t)
{
    switch (t)
    {
        case RHI_BB_UNIFORM:        return GL_UNIFORM_BUFFER;
        case RHI_BB_STORAGE:        return GL_SHADER_STORAGE_BUFFER;
        case RHI_BB_ATOMIC_COUNTER: return GL_ATOMIC_COUNTER_BUFFER;
    }
    return GL_SHADER_STORAGE_BUFFER;
}
void gl_bind_buffer_base(RhiBufferBindTarget target, uint32_t binding, RhiBuffer b)
{
    glBindBufferBase(gl_bb_target(target), binding, (GLuint)b);
}
void gl_bind_image_texture(uint32_t unit, RhiTexture tex, uint32_t level, int layered,
                           uint32_t layer, RhiImageAccess access, RhiFormat fmt)
{
    GLenum gl_access = GL_READ_WRITE;
    if (access == RHI_IMG_READ)       gl_access = GL_READ_ONLY;
    else if (access == RHI_IMG_WRITE) gl_access = GL_WRITE_ONLY;
    GLenum internal, ignore_f, ignore_t; gl_format(fmt, &internal, &ignore_f, &ignore_t);
    glBindImageTexture(unit, (GLuint)tex, (GLint)level, layered ? GL_TRUE : GL_FALSE,
                       (GLint)layer, gl_access, internal);
}
void gl_memory_barrier(uint32_t barrier_flags)
{
    if (barrier_flags & RHI_BARRIER_ALL) { glMemoryBarrier(GL_ALL_BARRIER_BITS); return; }
    GLbitfield bits = 0;
    if (barrier_flags & RHI_BARRIER_STORAGE) bits |= GL_SHADER_STORAGE_BARRIER_BIT;
    if (barrier_flags & RHI_BARRIER_IMAGE)   bits |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
    if (barrier_flags & RHI_BARRIER_ATOMIC)  bits |= GL_ATOMIC_COUNTER_BARRIER_BIT;
    if (barrier_flags & RHI_BARRIER_TEXTURE) bits |= GL_TEXTURE_FETCH_BARRIER_BIT;
    if (bits) glMemoryBarrier(bits);
}
void gl_clear_texture_u32(RhiTexture t, uint32_t value)
{
    glClearTexImage((GLuint)t, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
}
void gl_buffer_read(RhiBuffer b, size_t offset, size_t size, void* out)
{
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)b);
    glGetBufferSubData(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size, out);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

RhiApi g_gl_api;

} // namespace

// The active backend pointer (declared in rhi.h). Bootstrapped by LLGLManager::initGL.
RhiApi* gRHI = nullptr;

extern "C" RhiApi* rhi_create_gl(void* /*native_window*/)
{
    if (g_scratch_vao == 0) glGenVertexArrays(1, &g_scratch_vao);

    g_gl_api.begin_frame        = gl_begin_frame;
    g_gl_api.end_frame          = gl_end_frame;
    g_gl_api.get_caps           = gl_get_caps;
    g_gl_api.set_viewport       = gl_set_viewport;
    g_gl_api.get_error          = gl_get_error;
    g_gl_api.set_pixel_store    = gl_set_pixel_store;

    g_gl_api.buffer_create      = gl_buffer_create;
    g_gl_api.buffer_update      = gl_buffer_update;
    g_gl_api.buffer_map         = gl_buffer_map;
    g_gl_api.buffer_unmap       = gl_buffer_unmap;
    g_gl_api.buffer_destroy     = gl_buffer_destroy;
    g_gl_api.bind_vertex_buffer = gl_bind_vertex_buffer;
    g_gl_api.bind_index_buffer  = gl_bind_index_buffer;
    g_gl_api.buffer_gen         = gl_buffer_gen;
    g_gl_api.buffer_gen_n       = gl_buffer_gen_n;
    g_gl_api.buffer_del_n       = gl_buffer_del_n;
    g_gl_api.buffer_bind        = gl_buffer_bind;
    g_gl_api.buffer_data        = gl_buffer_data;
    g_gl_api.buffer_subdata     = gl_buffer_subdata;
    g_gl_api.buffer_del         = gl_buffer_del;
    g_gl_api.vao_gen            = gl_vao_gen;
    g_gl_api.vao_bind           = gl_vao_bind;
    g_gl_api.vao_del            = gl_vao_del;
    g_gl_api.vertex_attrib      = gl_vertex_attrib;
    g_gl_api.enable_vertex_attrib  = gl_enable_vertex_attrib;
    g_gl_api.disable_vertex_attrib = gl_disable_vertex_attrib;
    g_gl_api.bind_uniform_block = gl_bind_uniform_block;

    g_gl_api.texture_create     = gl_texture_create;
    g_gl_api.texture_upload     = gl_texture_upload;
    g_gl_api.texture_gen_mips   = gl_texture_gen_mips;
    g_gl_api.texture_copy       = gl_texture_copy;
    g_gl_api.copy_tex_sub_image_3d = gl_copy_tex_sub_image_3d;
    g_gl_api.copy_tex_sub_image_2d = gl_copy_tex_sub_image_2d;
    g_gl_api.gen_mipmap         = gl_gen_mipmap;
    g_gl_api.tex_image_2d       = gl_tex_image_2d;
    g_gl_api.texture_destroy    = gl_texture_destroy;
    g_gl_api.active_texture     = gl_active_texture;
    g_gl_api.bind_texture       = gl_bind_texture;
    g_gl_api.set_tex_wrap       = gl_set_tex_wrap;
    g_gl_api.set_tex_filter     = gl_set_tex_filter;
    g_gl_api.set_tex_anisotropy = gl_set_tex_anisotropy;
    g_gl_api.set_tex_compare    = gl_set_tex_compare;
    g_gl_api.set_tex_wrap_axis  = gl_set_tex_wrap_axis;
    g_gl_api.set_tex_mag_filter = gl_set_tex_mag_filter;
    g_gl_api.get_active_texture = gl_get_active_texture;

    g_gl_api.target_create      = gl_target_create;
    g_gl_api.target_attach      = gl_target_attach;
    g_gl_api.target_draw_buffers= gl_target_draw_buffers;
    g_gl_api.draw_buffers       = gl_draw_buffers;
    g_gl_api.target_get_status  = gl_target_get_status;
    g_gl_api.target_bind        = gl_target_bind;
    g_gl_api.bind_framebuffer   = gl_bind_framebuffer;
    g_gl_api.set_clear_color    = gl_set_clear_color;
    g_gl_api.set_clear_depth    = gl_set_clear_depth;
    g_gl_api.set_clear_stencil  = gl_set_clear_stencil;
    g_gl_api.target_clear       = gl_target_clear;
    g_gl_api.target_blit        = gl_target_blit;
    g_gl_api.target_read_pixels = gl_target_read_pixels;
    g_gl_api.read_pixels        = gl_read_pixels;
    g_gl_api.target_destroy     = gl_target_destroy;

    g_gl_api.shader_create      = gl_shader_create;
    g_gl_api.shader_bind        = gl_shader_bind;
    g_gl_api.shader_uniform_id  = gl_shader_uniform_id;
    g_gl_api.shader_destroy     = gl_shader_destroy;
    g_gl_api.set_uniform        = gl_set_uniform;
    g_gl_api.set_vertex_attrib4f = gl_set_vertex_attrib4f;

    g_gl_api.draw_indexed       = gl_draw_indexed;
    g_gl_api.draw_arrays        = gl_draw_arrays;
    g_gl_api.draw_range_elements = gl_draw_range_elements;

    g_gl_api.set_blend_enabled  = gl_set_blend_enabled;
    g_gl_api.set_blend_func     = gl_set_blend_func;
    g_gl_api.set_blend_equation = gl_set_blend_equation;
    g_gl_api.set_depth_test     = gl_set_depth_test;
    g_gl_api.set_depth_write    = gl_set_depth_write;
    g_gl_api.set_depth_func     = gl_set_depth_func;
    g_gl_api.set_capability     = gl_set_capability;
    g_gl_api.set_cull           = gl_set_cull;
    g_gl_api.set_color_mask     = gl_set_color_mask;
    g_gl_api.set_scissor        = gl_set_scissor;
    g_gl_api.set_stencil        = gl_set_stencil;
    g_gl_api.set_depth_clamp    = gl_set_depth_clamp;
    g_gl_api.set_reverse_z      = gl_set_reverse_z;
    g_gl_api.set_polygon_offset = gl_set_polygon_offset;
    g_gl_api.set_line_width     = gl_set_line_width;
    g_gl_api.set_point_size     = gl_set_point_size;
    g_gl_api.set_polygon_mode   = gl_set_polygon_mode;
    g_gl_api.set_cull_face      = gl_set_cull_face;
    g_gl_api.set_stencil_func   = gl_set_stencil_func;
    g_gl_api.set_stencil_op     = gl_set_stencil_op;
    g_gl_api.set_stencil_mask   = gl_set_stencil_mask;
    g_gl_api.is_enabled         = gl_is_enabled;

    g_gl_api.query_create       = gl_query_create;
    g_gl_api.query_gen_n        = gl_query_gen_n;
    g_gl_api.query_begin        = gl_query_begin;
    g_gl_api.query_end          = gl_query_end;
    g_gl_api.query_available    = gl_query_available;
    g_gl_api.query_result       = gl_query_result;
    g_gl_api.query_destroy      = gl_query_destroy;

    g_gl_api.flush              = gl_flush;
    g_gl_api.finish             = gl_finish;
    g_gl_api.debug_force_driver_crash = gl_debug_force_driver_crash;

    g_gl_api.bind_buffer_base   = gl_bind_buffer_base;
    g_gl_api.bind_image_texture = gl_bind_image_texture;
    g_gl_api.memory_barrier     = gl_memory_barrier;
    g_gl_api.clear_texture_u32  = gl_clear_texture_u32;
    g_gl_api.buffer_read        = gl_buffer_read;

    return &g_gl_api;
}

extern "C" void rhi_destroy(RhiApi* /*api*/)
{
    if (g_scratch_vao) { glDeleteVertexArrays(1, &g_scratch_vao); g_scratch_vao = 0; }
}
