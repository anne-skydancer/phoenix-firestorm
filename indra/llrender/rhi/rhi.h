/**
 * @file rhi.h
 * Render Hardware Interface — the C ABI seam between the viewer's render abstraction
 * (LLShader/LLVertexBuffer/LLRenderTarget/LLTexUnit/LLRender/LLGLState) and a backend.
 *
 * Two backends implement this: GL (C++, wraps today's calls — parity by construction) and
 * VK (Rust/wgpu cdylib). The viewer selects one at startup (one backend per run); all render
 * primitives flow through the active RhiApi vtable. NO raw GL above this line once P0.3 lands.
 *
 * Design (see doc/vulkan-port/P0_RHI_ABI_DESIGN + V2_ARCHITECTURE.md §1):
 *  - Pure C ABI. Plain types only, so C++ (abstraction + GL backend) and Rust (VK backend, via
 *    cbindgen-matched decls) both speak it.
 *  - Opaque integer handles (uint32, 0 = invalid). Each backend maps handle->its object (GL name /
 *    wgpu slotmap). No pointers cross the FFI for resources.
 *  - Dispatch via a vtable struct (RhiApi) of function pointers; the backend fills one.
 *  - Resource + dynamic-command model (GL-flavored so the abstraction's call pattern is unchanged).
 *    Fixed-function STATE is set dynamically (R8); the VK backend folds current state into a PSO
 *    cache at draw time (arch §2) — GL sets it directly. Parity is the spec; the oracle measures it.
 */
#ifndef LL_RHI_H
#define LL_RHI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- opaque handles (0 = invalid) ---- */
typedef uint32_t RhiBuffer;
typedef uint32_t RhiVAO;      /* vertex array object (GL name; VK folds into pipeline vertex state) */
typedef uint32_t RhiTexture;
typedef uint32_t RhiTarget;   /* render target (FBO / swapchain when 0) */
typedef uint32_t RhiShader;   /* program (GL) / pipeline-by-identity (VK) */
typedef uint32_t RhiQuery;

/* ---- enums (explicit values = stable ABI) ---- */
typedef enum {                        /* R3/R4 formats — the ones the G-buffer + textures use */
    RHI_FMT_RGBA8 = 0, RHI_FMT_SRGBA8, RHI_FMT_RGBA16F, RHI_FMT_RGB10_A2, RHI_FMT_RGB16F,
    RHI_FMT_R8, RHI_FMT_R16F, RHI_FMT_RG16F, RHI_FMT_R32F, RHI_FMT_RGBA32F,
    RHI_FMT_DEPTH24, RHI_FMT_DEPTH32F, RHI_FMT_DEPTH24_STENCIL8,
    RHI_FMT_BC1, RHI_FMT_BC3, RHI_FMT_BC5, RHI_FMT_BC7,  /* compressed uploads */
    RHI_FMT_RGB8,                                        /* GL_RGB8 -- 3-byte readback (snapshots) */
    RHI_FMT_R32UI                                        /* GL_R32UI -- PPLL head-pointer image (uint image load/store) */
} RhiFormat;

typedef enum { RHI_BUF_VERTEX = 0, RHI_BUF_INDEX, RHI_BUF_UNIFORM, RHI_BUF_STREAM } RhiBufferUsage;
typedef enum { RHI_IDX_U16 = 0, RHI_IDX_U32 } RhiIndexType;
/* index-parity with LLRender::eGeomModes (TRIANGLES,TRISTRIP,TRIFAN,POINTS,LINES,LINESTRIP,LINELOOP) */
typedef enum { RHI_PRIM_TRIANGLES = 0, RHI_PRIM_TRISTRIP, RHI_PRIM_TRIFAN,
               RHI_PRIM_POINTS, RHI_PRIM_LINES, RHI_PRIM_LINESTRIP, RHI_PRIM_LINELOOP } RhiPrimitive;
typedef enum { RHI_BUFTGT_VERTEX = 0, RHI_BUFTGT_INDEX, RHI_BUFTGT_UNIFORM } RhiBufferTarget; /* GL_ARRAY / GL_ELEMENT_ARRAY / GL_UNIFORM_BUFFER */
typedef enum { RHI_BUFHINT_STATIC = 0, RHI_BUFHINT_DYNAMIC, RHI_BUFHINT_STREAM } RhiBufferHint; /* glBufferData usage */
typedef enum {                        /* R2 · LLVertexBuffer attribute (type, normalized, integer) combos */
    RHI_VF_FLOAT = 0,   /* GL_FLOAT, not normalized  */
    RHI_VF_U8_NORM,     /* GL_UNSIGNED_BYTE, normalized (color/emissive) */
    RHI_VF_FLOAT_NORM,  /* GL_FLOAT, normalized (clothweight) */
    RHI_VF_U16_INT,     /* GL_UNSIGNED_SHORT, integer -> glVertexAttribIPointer (joint) */
    RHI_VF_U32_INT      /* GL_UNSIGNED_INT, integer -> glVertexAttribIPointer (texture_index) */
} RhiVertexFormat;

typedef enum {                        /* vertex attribute component type (llvertexbuffer 14-slot enum) */
    RHI_ATTR_F32 = 0, RHI_ATTR_U8_NORM, RHI_ATTR_U8, RHI_ATTR_I16_NORM, RHI_ATTR_U16, RHI_ATTR_U32
} RhiAttribType;

typedef enum {                        /* R8 depth/blend compare — GREATER/GEQUAL carry reverse-Z */
    RHI_CMP_NEVER = 0, RHI_CMP_LESS, RHI_CMP_EQUAL, RHI_CMP_LEQUAL,
    RHI_CMP_GREATER, RHI_CMP_NOTEQUAL, RHI_CMP_GEQUAL, RHI_CMP_ALWAYS
} RhiCompare;

typedef enum {
    RHI_BF_ZERO = 0, RHI_BF_ONE, RHI_BF_SRC_ALPHA, RHI_BF_ONE_MINUS_SRC_ALPHA,
    RHI_BF_SRC_COLOR, RHI_BF_ONE_MINUS_SRC_COLOR, RHI_BF_DST_ALPHA, RHI_BF_ONE_MINUS_DST_ALPHA,
    RHI_BF_DST_COLOR, RHI_BF_ONE_MINUS_DST_COLOR
} RhiBlendFactor;
typedef enum { RHI_BO_ADD = 0, RHI_BO_SUB, RHI_BO_REV_SUB, RHI_BO_MIN, RHI_BO_MAX } RhiBlendOp;

typedef enum { RHI_CULL_NONE = 0, RHI_CULL_BACK, RHI_CULL_FRONT } RhiCull;
typedef enum { RHI_PM_FILL = 0, RHI_PM_LINE, RHI_PM_POINT } RhiPolygonMode;
typedef enum { RHI_CF_BACK = 0, RHI_CF_FRONT, RHI_CF_FRONT_AND_BACK } RhiCullFace;   /* glCullFace target */
typedef enum {                        /* matches LLRender eStencilOp order (gl_op[]) */
    RHI_SO_KEEP = 0, RHI_SO_ZERO, RHI_SO_REPLACE, RHI_SO_INCR,
    RHI_SO_DECR, RHI_SO_INVERT, RHI_SO_INCR_WRAP, RHI_SO_DECR_WRAP
} RhiStencilOp;
/* Generic fixed-function capabilities toggled by LLGLState (glEnable/glDisable). Abstract
 * values (NOT the GL enums) so the VK backend maps them to PSO/dynamic-state fields. */
typedef enum {
    RHI_CAP_BLEND = 0, RHI_CAP_CULL_FACE, RHI_CAP_DEPTH_TEST, RHI_CAP_STENCIL_TEST,
    RHI_CAP_SCISSOR_TEST, RHI_CAP_POLYGON_OFFSET_FILL, RHI_CAP_POLYGON_OFFSET_LINE,
    RHI_CAP_DEPTH_CLAMP, RHI_CAP_MULTISAMPLE, RHI_CAP_PROGRAM_POINT_SIZE,
    RHI_CAP_CLIP_DISTANCE0, RHI_CAP_ALPHA_TEST, RHI_CAP_LINE_SMOOTH,
    RHI_CAP_TEXTURE_CUBE_MAP_SEAMLESS,   /* GL global enable; VK samples cubemaps seamlessly by default (no-op) */
    RHI_CAP_UNKNOWN
} RhiCapability;
/* R4 attachment points (LLRenderTarget attaches color/depth incrementally). */
typedef enum {
    RHI_ATTACH_COLOR0 = 0, RHI_ATTACH_COLOR1, RHI_ATTACH_COLOR2, RHI_ATTACH_COLOR3,
    RHI_ATTACH_DEPTH
} RhiAttachment;
typedef enum { RHI_FILTER_NEAREST = 0, RHI_FILTER_LINEAR, RHI_FILTER_TRILINEAR } RhiFilter;
typedef enum { RHI_WRAP_REPEAT = 0, RHI_WRAP_CLAMP, RHI_WRAP_MIRROR } RhiWrap;
typedef enum {                        /* R3 · LLTexUnit eTextureType, index-parity with sGLTextureType */
    RHI_TEX_2D = 0, RHI_TEX_RECT, RHI_TEX_CUBE, RHI_TEX_CUBE_ARRAY, RHI_TEX_2D_MS, RHI_TEX_3D
} RhiTexType;
typedef enum {                        /* the 5 GL min-filter modes LLTexUnit selects */
    RHI_MINF_NEAREST = 0, RHI_MINF_LINEAR,
    RHI_MINF_NEAREST_MIP_NEAREST, RHI_MINF_LINEAR_MIP_NEAREST, RHI_MINF_LINEAR_MIP_LINEAR
} RhiMinFilter;
typedef enum { RHI_MAGF_NEAREST = 0, RHI_MAGF_LINEAR } RhiMagFilter;
typedef enum { RHI_STAGE_VERTEX = 1, RHI_STAGE_FRAGMENT = 2, RHI_STAGE_GEOMETRY = 4 } RhiStage;
typedef enum { RHI_CLEAR_COLOR = 1, RHI_CLEAR_DEPTH = 2, RHI_CLEAR_STENCIL = 4 } RhiClearFlags;
typedef enum { RHI_FB_DRAW = 0, RHI_FB_READ, RHI_FB_BOTH } RhiFbBind;  /* glBindFramebuffer point (read/draw separation) */
typedef enum {                        /* R10 query targets (glBeginQuery target); result read as u64 */
    RHI_QUERY_ANY_SAMPLES = 0,        /* GL_ANY_SAMPLES_PASSED -- boolean occlusion */
    RHI_QUERY_SAMPLES,                /* GL_SAMPLES_PASSED -- exact sample count */
    RHI_QUERY_TIME_ELAPSED,           /* GL_TIME_ELAPSED -- GPU timer (ns) */
    RHI_QUERY_PRIMITIVES,             /* GL_PRIMITIVES_GENERATED */
    RHI_QUERY_XFB_PRIMITIVES          /* GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN */
} RhiQueryType;
/* R11 · storage / image / OIT (PPLL). The alpha per-pixel-linked-list needs an SSBO node pool,
 * an atomic counter (next-node allocator), a uint head-pointer image, and barriers between the
 * append pass and the resolve. GL maps these 1:1; VK has them natively (storage + pipeline barriers). */
typedef enum { RHI_BB_UNIFORM = 0, RHI_BB_STORAGE, RHI_BB_ATOMIC_COUNTER } RhiBufferBindTarget; /* glBindBufferBase target */
typedef enum { RHI_IMG_READ = 0, RHI_IMG_WRITE, RHI_IMG_READ_WRITE } RhiImageAccess;            /* glBindImageTexture access */
typedef enum {                                     /* glMemoryBarrier bits — OR together */
    RHI_BARRIER_STORAGE = 1,                       /* GL_SHADER_STORAGE_BARRIER_BIT   — SSBO writes visible */
    RHI_BARRIER_IMAGE   = 2,                        /* GL_SHADER_IMAGE_ACCESS_BARRIER_BIT — image store visible */
    RHI_BARRIER_ATOMIC  = 4,                        /* GL_ATOMIC_COUNTER_BARRIER_BIT   — counter visible */
    RHI_BARRIER_TEXTURE = 8,                        /* GL_TEXTURE_FETCH_BARRIER_BIT — resolve samples head image */
    RHI_BARRIER_ALL     = 16                        /* GL_ALL_BARRIER_BITS (coarse) */
} RhiBarrierFlags;
typedef enum {                        /* R6 — (base type, component count); *v array form, count = array len */
    RHI_UNIFORM_F32 = 0, RHI_UNIFORM_VEC2, RHI_UNIFORM_VEC3, RHI_UNIFORM_VEC4,
    RHI_UNIFORM_I32, RHI_UNIFORM_IVEC2, RHI_UNIFORM_IVEC3, RHI_UNIFORM_IVEC4,
    RHI_UNIFORM_U32, RHI_UNIFORM_UVEC2, RHI_UNIFORM_UVEC3, RHI_UNIFORM_UVEC4,
    RHI_UNIFORM_MAT2, RHI_UNIFORM_MAT3, RHI_UNIFORM_MAT4, RHI_UNIFORM_MAT3X4
} RhiUniformType;

/* ---- descriptors ---- */
typedef struct { uint32_t slot; RhiAttribType type; uint32_t count; uint32_t offset; } RhiVertexAttrib;
typedef struct { uint32_t stride; uint32_t attrib_count; const RhiVertexAttrib* attribs; } RhiVertexLayout;

typedef struct {
    uint32_t width, height, depth, levels;
    RhiFormat format;
    RhiFilter filter; RhiWrap wrap;
    int is_cubemap; int is_array;
    int immutable;    /* allocate immutable storage (glTexStorage); REQUIRED to bind as an image (load/store) under Zink/VK */
} RhiTextureDesc;

typedef struct {                      /* R5 — shader by identity; GL uses glsl_*, VK loads spv by (id,mask) */
    uint32_t    identity;             /* stable shader id (== gDeferredSoftenProgram, etc.) */
    uint32_t    feature_mask;         /* #define variant bits */
    uint32_t    stages;               /* RhiStage bitmask */
    const char* glsl_vertex;          /* GL backend: assembled source; VK backend: ignored */
    const char* glsl_fragment;
    const char* glsl_geometry;
    /* contract (arch §4.2) layered in during the shader-corpus phase: uniform/attrib/sampler decl */
} RhiShaderDesc;

typedef struct {
    char  renderer[128], vendor[64], version[64];
    uint32_t device_type;             /* 0 other/1 integrated/2 discrete/3 virtual/4 cpu */
    uint32_t max_texture_2d, max_samples, max_uniform_block_size;
} RhiCaps;

/* ================= the dispatch vtable ================= */
typedef struct RhiApi {

    /* R1 · device / frame / caps */
    void (*begin_frame)(void);
    void (*end_frame)(void);                                   /* submit + PRESENT (replaces swapBuffers) */
    void (*get_caps)(RhiCaps* out);
    void (*set_viewport)(int x, int y, int w, int h);
    uint32_t (*get_error)(void);                               /* 0 = ok (RHI validation channel) */
    void (*set_pixel_store)(int pack_alignment, int unpack_alignment); /* glPixelStorei PACK/UNPACK_ALIGNMENT; VK no-op */

    /* R2 · buffers + vertex layout */
    RhiBuffer (*buffer_create)(RhiBufferUsage usage, size_t size, const void* initial /*or NULL*/);
    void (*buffer_update)(RhiBuffer b, size_t offset, size_t size, const void* data);
    void*(*buffer_map)(RhiBuffer b, size_t offset, size_t size);   /* NULL if unsupported */
    void (*buffer_unmap)(RhiBuffer b);
    void (*buffer_destroy)(RhiBuffer b);
    void (*bind_vertex_buffer)(RhiBuffer b, const RhiVertexLayout* layout);
    void (*bind_index_buffer)(RhiBuffer b, RhiIndexType type);
    void (*bind_uniform_block)(uint32_t binding, RhiBuffer b);  /* glBindBufferBase — UBO */
    /* R2 · LLVertexBuffer granular buffer/VAO/attrib (its exact GL; VK folds VAO+attribs into pipeline state) */
    RhiBuffer (*buffer_gen)(void);                                            /* glGenBuffers(1) */
    void (*buffer_gen_n)(uint32_t count, RhiBuffer* out);                     /* glGenBuffers(count) — name pool */
    void (*buffer_del_n)(uint32_t count, const RhiBuffer* in);                /* glDeleteBuffers(count) — deferred free */
    void (*buffer_bind)(RhiBufferTarget target, RhiBuffer b);
    void (*buffer_data)(RhiBufferTarget target, size_t size, const void* data, RhiBufferHint hint);
    void (*buffer_subdata)(RhiBufferTarget target, size_t offset, size_t size, const void* data);
    void (*buffer_del)(RhiBuffer b);                                          /* glDeleteBuffers(1) */
    RhiVAO (*vao_gen)(void);
    void (*vao_bind)(RhiVAO v);
    void (*vao_del)(RhiVAO v);
    void (*vertex_attrib)(uint32_t index, int size, RhiVertexFormat fmt, uint32_t stride, size_t offset);
    void (*enable_vertex_attrib)(uint32_t index);
    void (*disable_vertex_attrib)(uint32_t index);

    /* R3 · textures */
    RhiTexture (*texture_create)(const RhiTextureDesc* desc);
    void (*texture_upload)(RhiTexture t, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           RhiFormat data_fmt, const void* data, size_t size); /* compressed if data_fmt is BC* */
    void (*texture_gen_mips)(RhiTexture t);
    void (*texture_copy)(RhiTexture dst, RhiTexture src, uint32_t w, uint32_t h);  /* scaleDown / blit-to-self */
    void (*copy_tex_sub_image_3d)(RhiTexType type, uint32_t level, uint32_t xoff, uint32_t yoff, uint32_t zoff,
                                  uint32_t srcx, uint32_t srcy, uint32_t w, uint32_t h);  /* fbo -> cubemap-array layer (probes) */
    void (*copy_tex_sub_image_2d)(RhiTexType type, uint32_t level, uint32_t xoff, uint32_t yoff,
                                  uint32_t srcx, uint32_t srcy, uint32_t w, uint32_t h);  /* read fbo -> bound 2D tex (scene monitor) */
    void (*gen_mipmap)(RhiTexType type);                       /* glGenerateMipmap on the tex bound to `type` at active unit */
    void (*tex_image_2d)(RhiTexType type, uint32_t level, RhiFormat internal,
                         uint32_t w, uint32_t h, RhiFormat data_fmt, const void* data); /* glTexImage2D alloc+upload to bound tex */
    void (*texture_destroy)(RhiTexture t);
    /* texture-unit state (LLTexUnit): select a unit, bind by type to it, sampler params */
    void (*active_texture)(uint32_t unit);
    void (*bind_texture)(RhiTexType type, RhiTexture name);    /* to current active unit; 0 = unbind */
    void (*set_tex_wrap)(RhiTexType type, RhiWrap mode, int set_r);
    void (*set_tex_filter)(RhiTexType type, RhiMinFilter min, RhiMagFilter mag);
    void (*set_tex_anisotropy)(RhiTexType type, float max_aniso);
    void (*set_tex_compare)(RhiTexType type, int enable, RhiCompare func);  /* depth-compare sampling (shadow maps) */
    void (*set_tex_wrap_axis)(RhiTexType type, uint32_t axis, RhiWrap mode); /* one axis: 0=S 1=T 2=R (glTF per-axis wrap) */
    void (*set_tex_mag_filter)(RhiTexType type, RhiMagFilter mag);          /* mag only -- never touches min (glTF sampler) */
    uint32_t (*get_active_texture)(void);                     /* debug: current unit index */

    /* R4 · render targets — INCREMENTAL to match LLRenderTarget (create empty FBO, attach
     * color/depth over multiple calls). VK backend accumulates attachments + defers the VK
     * framebuffer/render-pass creation to first bind. */
    RhiTarget (*target_create)(void);                          /* empty FBO */
    void (*target_attach)(RhiTarget t, RhiAttachment point, RhiTexture tex, uint32_t level); /* tex=0 detaches */
    void (*target_draw_buffers)(RhiTarget t, uint32_t color_count);  /* 0 = GL_NONE draw+read */
    void (*draw_buffers)(uint32_t color_count);                      /* glDrawBuffers on the CURRENT fbo */
    uint32_t (*target_get_status)(void);                             /* glCheckFramebufferStatus (raw) */
    void (*target_bind)(RhiTarget t);                          /* 0 = swapchain/default */
    void (*bind_framebuffer)(RhiFbBind which, RhiTarget t);    /* glBindFramebuffer read/draw/both, no draw-buffer side effects */
    void (*set_clear_color)(float r, float g, float b, float a); /* glClearColor (clear-state) */
    void (*set_clear_depth)(float d);                          /* glClearDepth */
    void (*set_clear_stencil)(int s);                          /* glClearStencil */
    void (*target_clear)(uint32_t flags);                      /* glClear current target w/ current clear-state */
    void (*target_blit)(RhiTarget dst, RhiTarget src, uint32_t w, uint32_t h, RhiFilter f);
    void (*target_read_pixels)(RhiTarget t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                               RhiFormat out_fmt, void* out);   /* snapshot / engine-mode capture */
    void (*read_pixels)(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        RhiFormat fmt, void* out);              /* glReadPixels on the CURRENT read fbo (pick) */
    void (*target_destroy)(RhiTarget t);

    /* R5 · shaders (GL compiles GLSL; VK loads SPIR-V by identity) */
    RhiShader (*shader_create)(const RhiShaderDesc* desc);
    void (*shader_bind)(RhiShader s);
    int  (*shader_uniform_id)(RhiShader s, const char* name);  /* name -> stable id (contract slot) */
    void (*shader_destroy)(RhiShader s);

    /* R6 · uniforms (route to GL location / VK UBO offset via the contract) */
    void (*set_uniform)(RhiShader s, int uniform_id, RhiUniformType type, uint32_t count,
                        const void* data, int transpose);           /* transpose: matrices only */
    void (*set_vertex_attrib4f)(uint32_t index, const float* v4);   /* glVertexAttrib4fv — constant attribute */

    /* R7 · draw */
    void (*draw_indexed)(RhiPrimitive prim, uint32_t index_count, uint32_t index_offset,
                         uint32_t base_vertex, uint32_t instances);
    void (*draw_range_elements)(RhiPrimitive prim, uint32_t start, uint32_t end, uint32_t count,
                                RhiIndexType idx, size_t offset_bytes);   /* glDrawRangeElements */
    void (*draw_arrays)(RhiPrimitive prim, uint32_t first, uint32_t count, uint32_t instances);

    /* R8 · dynamic state — GRANULAR, 1:1 with GL/LLGLState (LLGLEnable toggles enable; the
     * func/equation are set separately). GL backend applies directly; VK backend accumulates
     * these into current-state and resolves a PSO at draw. */
    void (*set_blend_enabled)(int enable);
    void (*set_blend_func)(RhiBlendFactor srcRGB, RhiBlendFactor dstRGB,
                           RhiBlendFactor srcA, RhiBlendFactor dstA);
    void (*set_blend_equation)(RhiBlendOp rgb, RhiBlendOp a);
    void (*set_depth_test)(int enable);
    void (*set_depth_write)(int enable);
    void (*set_depth_func)(RhiCompare cmp);                    /* GREATER = reverse-Z */
    void (*set_capability)(RhiCapability cap, int enable);     /* generic glEnable/glDisable (LLGLState) */
    void (*set_cull)(RhiCull mode);
    void (*set_color_mask)(int r, int g, int b, int a);
    void (*set_scissor)(int enable, int x, int y, int w, int h);
    void (*set_stencil)(int enable, RhiCompare cmp, uint32_t ref, uint32_t mask, uint32_t write_mask);
    void (*set_depth_clamp)(int enable);                       /* GL_DEPTH_CLAMP */
    void (*set_reverse_z)(int enable);                         /* glClipControl [0,1] + clear-depth convention */
    void (*set_polygon_offset)(float factor, float units);
    void (*set_line_width)(float w);
    void (*set_point_size)(float size);
    void (*set_polygon_mode)(RhiPolygonMode mode);
    void (*set_cull_face)(RhiCullFace face);                    /* glCullFace target (not enable/disable) */
    void (*set_stencil_func)(RhiCompare func, int ref, uint32_t mask);
    void (*set_stencil_op)(RhiStencilOp sfail, RhiStencilOp dpfail, RhiStencilOp dppass);
    void (*set_stencil_mask)(uint32_t mask);
    int  (*is_enabled)(RhiCapability cap);                      /* glIsEnabled (e.g. LINE_SMOOTH clamp) */

    /* R10 · queries (occlusion / timer / primitives). Target is chosen at begin (some sites pick it
     * dynamically), and glEndQuery takes the TARGET not the handle -- so type flows through begin/end.
     * Availability is split from result because sites run their own timeout/poll logic. */
    RhiQuery (*query_create)(void);                            /* glGenQueries(1) */
    void (*query_gen_n)(uint32_t count, RhiQuery* out);        /* glGenQueries(count) -- name pool */
    void (*query_begin)(RhiQueryType type, RhiQuery q);        /* glBeginQuery(target, q) */
    void (*query_end)(RhiQueryType type);                      /* glEndQuery(target) */
    int  (*query_available)(RhiQuery q);                       /* glGetQueryObjectuiv(RESULT_AVAILABLE) */
    void (*query_result)(RhiQuery q, uint64_t* out);           /* glGetQueryObjectui64v(RESULT) */
    void (*query_destroy)(RhiQuery q);

    /* R9 · sync — mostly backend-owned (wgpu manages it). Explicit only where GL stalls for readback. */
    void (*flush)(void);
    void (*finish)(void);
    void (*debug_force_driver_crash)(void);   /* deliberate GPU-driver crash for crash-reporter testing */

    /* R11 · storage buffers / images / barriers — the alpha OIT (PPLL) path only. GL wraps
     * glBindBufferBase (SSBO + atomic counter), glBindImageTexture, glMemoryBarrier,
     * glClearTexImage; VK binds storage descriptors + emits pipeline barriers natively. */
    void (*bind_buffer_base)(RhiBufferBindTarget target, uint32_t binding, RhiBuffer b); /* SSBO / atomic-counter binding point */
    void (*bind_image_texture)(uint32_t unit, RhiTexture tex, uint32_t level, int layered,
                               uint32_t layer, RhiImageAccess access, RhiFormat fmt);    /* head-pointer image bind */
    void (*memory_barrier)(uint32_t barrier_flags);                                      /* RhiBarrierFlags OR-mask */
    void (*clear_texture_u32)(RhiTexture t, uint32_t value);                             /* clear head image to 0xFFFFFFFF each frame */
    void (*buffer_read)(RhiBuffer b, size_t offset, size_t size, void* out);             /* glGetBufferSubData -- GPU->CPU readback (stats/diagnostics; syncs) */

} RhiApi;

/* ---- backend selection (one per run) ----
 * rhi_create_gl:  C++ GL backend, in-process (parity baseline).
 * rhi_create_vulkan: exported by the Rust/wgpu cdylib.
 * `native_window` = HWND on Windows. Returns NULL on failure (caller keeps GL). */
RhiApi* rhi_create_gl(void* native_window);
RhiApi* rhi_create_vulkan(void* native_window);
void    rhi_destroy(RhiApi* api);

/* The active backend, set once at GL/engine init (LLGLManager::initGL). All render code calls
 * through this during P0.3d routing. NULL until bootstrapped. */
extern RhiApi* gRHI;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LL_RHI_H */
