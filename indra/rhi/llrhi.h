/**
 * @file llrhi.h
 * @brief Render Hardware Interface (RHI) -- the C-ABI contract both backends implement.
 *
 * This is the seam Firestorm's renderer is retargeted onto: the six render-abstraction classes
 * (LLRender, LLVertexBuffer, LLGLSLShader, LLRenderTarget, LLImageGL/LLCubeMap, LLGLState) stop
 * calling gl* directly and call THIS instead. Two backends implement it:
 *   - native GL  (llrhigl.cpp, C++)  -- does exactly what the classes do today. Phase 0.
 *   - native Vulkan (fs_render, Rust/wgpu) -- the engine. Phase 2.
 * Selected at launch by RenderGLBackend {native-gl, native-vulkan}. See RHI_PLAN.md.
 *
 * DESIGN: wgpu-shaped, so the Vulkan backend maps ~1:1 and the GL backend emulates. C-ABI
 * (opaque uint64 handles, POD descriptors, extern "C") so the Rust engine implements the SAME
 * contract the C++ GL backend does. Handle 0 == null/invalid everywhere.
 *
 * v0 -- this WILL grow as each class is re-backed; the core draw path + resources + the PSO key
 * are settled here first because they are the load-bearing shape (RHI_PLAN.md sec.1-3). Grep
 * "[trace:X]" for the provenance of each field.
 */
#ifndef LL_RHI_H
#define LL_RHI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LLRHI_ABI_VERSION 1u

/* ======================================================================================
 *  Handles (opaque; 0 = null/invalid). The backend owns the objects behind these.
 * ==================================================================================== */
typedef uint64_t LLRhiDevice;
typedef uint64_t LLRhiBuffer;
typedef uint64_t LLRhiTexture;
typedef uint64_t LLRhiSampler;
typedef uint64_t LLRhiShaderModule;
typedef uint64_t LLRhiPipeline;          /* the PSO -- see LLRhiPipelineDesc */
typedef uint64_t LLRhiBindGroupLayout;
typedef uint64_t LLRhiBindGroup;
typedef uint64_t LLRhiRenderTarget;      /* a set of attachments (the FBO analogue) */

/* ======================================================================================
 *  Enums
 * ==================================================================================== */

/* Texture / attachment formats. The set the deferred pass graph actually uses
 * [trace:LLRenderTarget]. RGBA16_UNORM (the normal buffer) has no core wgpu format -- flagged
 * in RHI_PLAN.md sec.9 as a backend gap to resolve. */
typedef enum {
    LLRHI_FMT_UNDEFINED = 0,
    LLRHI_FMT_RGBA8_UNORM,
    LLRHI_FMT_RGBA8_SRGB,
    LLRHI_FMT_BGRA8_UNORM,
    LLRHI_FMT_BGRA8_SRGB,        /* the swapchain format the viewer negotiates today */
    LLRHI_FMT_R8_UNORM,
    LLRHI_FMT_RG16F,
    LLRHI_FMT_R16F,
    LLRHI_FMT_RGBA16F,           /* HDR scene / lighting */
    LLRHI_FMT_RGBA16_UNORM,      /* G-buffer normal (wgpu gap) */
    LLRHI_FMT_RGB10A2_UNORM,     /* G-buffer normal, non-HDR path */
    LLRHI_FMT_RGBA32F,           /* WBOIT accum */
    LLRHI_FMT_DEPTH24,           /* the deferred depth (depth-only, no stencil in this branch) */
    LLRHI_FMT_DEPTH32F,
    LLRHI_FMT_COUNT
} LLRhiFormat;

/* Vertex attribute formats [trace:LLVertexBuffer sTypeSize/setupVertexBuffer]. The closed set
 * the type-mask uses; attribute @location == TYPE_* ordinal (stable, no reflection needed). */
typedef enum {
    LLRHI_VF_FLOAT32X2 = 0,   /* TEXCOORD* */
    LLRHI_VF_FLOAT32X3,       /* VERTEX/NORMAL (bound as x3 from 16B blocks) */
    LLRHI_VF_FLOAT32X4,       /* TANGENT/WEIGHT4/CLOTHWEIGHT */
    LLRHI_VF_FLOAT32,         /* WEIGHT */
    LLRHI_VF_UNORM8X4,        /* COLOR/EMISSIVE (normalized) */
    LLRHI_VF_UINT16X4,        /* JOINT (integer) */
    LLRHI_VF_UINT32           /* TEXTURE_INDEX (aliases VERTEX.w at offset+12) */
} LLRhiVertexFormat;

/* Primitive topology [trace:LLVertexBuffer sGLMode]. NB: wgpu has no TRIANGLE_FAN/LINE_LOOP;
 * the Vulkan backend must expand them (the engine already does). GL backend maps direct. */
typedef enum {
    LLRHI_TOPO_TRIANGLES = 0,
    LLRHI_TOPO_TRIANGLE_STRIP,
    LLRHI_TOPO_TRIANGLE_FAN,   /* expand for Vulkan */
    LLRHI_TOPO_POINTS,
    LLRHI_TOPO_LINES,
    LLRHI_TOPO_LINE_STRIP,
    LLRHI_TOPO_LINE_LOOP       /* expand for Vulkan */
} LLRhiTopology;

typedef enum {
    LLRHI_CMP_NEVER = 0, LLRHI_CMP_LESS, LLRHI_CMP_EQUAL, LLRHI_CMP_LEQUAL,
    LLRHI_CMP_GREATER, LLRHI_CMP_NOTEQUAL, LLRHI_CMP_GEQUAL, LLRHI_CMP_ALWAYS
} LLRhiCompareFunc;

/* Blend factors [trace:LLRender BF_*]. */
typedef enum {
    LLRHI_BF_ZERO = 0, LLRHI_BF_ONE,
    LLRHI_BF_SRC_COLOR, LLRHI_BF_ONE_MINUS_SRC_COLOR,
    LLRHI_BF_DST_COLOR, LLRHI_BF_ONE_MINUS_DST_COLOR,
    LLRHI_BF_SRC_ALPHA, LLRHI_BF_ONE_MINUS_SRC_ALPHA,
    LLRHI_BF_DST_ALPHA, LLRHI_BF_ONE_MINUS_DST_ALPHA,
    LLRHI_BF_CONST_COLOR, LLRHI_BF_ONE_MINUS_CONST_COLOR,
    LLRHI_BF_SRC_ALPHA_SATURATE
} LLRhiBlendFactor;

typedef enum {
    LLRHI_BLEND_ADD = 0, LLRHI_BLEND_SUBTRACT, LLRHI_BLEND_REVERSE_SUBTRACT,
    LLRHI_BLEND_MIN, LLRHI_BLEND_MAX
} LLRhiBlendOp;

typedef enum { LLRHI_CULL_NONE = 0, LLRHI_CULL_FRONT, LLRHI_CULL_BACK } LLRhiCullMode;
typedef enum { LLRHI_FRONT_CCW = 0, LLRHI_FRONT_CW } LLRhiFrontFace;
typedef enum { LLRHI_POLY_FILL = 0, LLRHI_POLY_LINE, LLRHI_POLY_POINT } LLRhiPolygonMode;

typedef enum {
    LLRHI_SOP_KEEP = 0, LLRHI_SOP_ZERO, LLRHI_SOP_REPLACE, LLRHI_SOP_INCR_CLAMP,
    LLRHI_SOP_DECR_CLAMP, LLRHI_SOP_INVERT, LLRHI_SOP_INCR_WRAP, LLRHI_SOP_DECR_WRAP
} LLRhiStencilOp;

typedef enum { LLRHI_LOAD_LOAD = 0, LLRHI_LOAD_CLEAR, LLRHI_LOAD_DONTCARE } LLRhiLoadOp;
typedef enum { LLRHI_STORE_STORE = 0, LLRHI_STORE_DONTCARE } LLRhiStoreOp;

typedef enum { LLRHI_INDEX_U16 = 0, LLRHI_INDEX_U32 } LLRhiIndexFormat;

/* GL backend eats GLSL; Vulkan backend eats SPIR-V. The viewer assembles the final source
 * (with #defines/permutation) THEN calls create -- permutation stays viewer-side
 * [trace:LLGLSLShader mDefines]. */
typedef enum { LLRHI_SHADER_GLSL = 0, LLRHI_SHADER_SPIRV } LLRhiShaderFormat;
typedef enum { LLRHI_STAGE_VERTEX = 0, LLRHI_STAGE_FRAGMENT, LLRHI_STAGE_COMPUTE } LLRhiShaderStage;

typedef enum {
    LLRHI_BIND_UNIFORM_BUFFER = 0,
    LLRHI_BIND_STORAGE_BUFFER,
    LLRHI_BIND_TEXTURE,
    LLRHI_BIND_SAMPLER
} LLRhiBindingType;

/* Bitflags */
enum {
    LLRHI_BUF_VERTEX   = 1u << 0,
    LLRHI_BUF_INDEX    = 1u << 1,
    LLRHI_BUF_UNIFORM  = 1u << 2,
    LLRHI_BUF_STORAGE  = 1u << 3,
    LLRHI_BUF_COPY_DST = 1u << 4
};
enum {
    LLRHI_TEX_SAMPLED           = 1u << 0,
    LLRHI_TEX_RENDER_ATTACHMENT = 1u << 1,
    LLRHI_TEX_COPY_DST          = 1u << 2,
    LLRHI_TEX_COPY_SRC          = 1u << 3,
    LLRHI_TEX_STORAGE           = 1u << 4
};
enum { /* color_write_mask bits */
    LLRHI_WRITE_R = 1u << 0, LLRHI_WRITE_G = 1u << 1,
    LLRHI_WRITE_B = 1u << 2, LLRHI_WRITE_A = 1u << 3,
    LLRHI_WRITE_ALL = 0xF
};

typedef enum {
    LLRHI_ADDR_REPEAT = 0, LLRHI_ADDR_CLAMP, LLRHI_ADDR_MIRROR
} LLRhiAddressMode;
typedef enum { LLRHI_FILTER_NEAREST = 0, LLRHI_FILTER_LINEAR } LLRhiFilterMode;
typedef enum { LLRHI_TEXDIM_2D = 0, LLRHI_TEXDIM_CUBE, LLRHI_TEXDIM_2D_ARRAY, LLRHI_TEXDIM_CUBE_ARRAY } LLRhiTexDim;

/* ======================================================================================
 *  Descriptors (POD)
 * ==================================================================================== */

typedef struct {
    uint32_t     width, height;
    uint32_t     depth_or_layers;   /* 1 for 2D; 6 for cube; N for arrays */
    uint32_t     mip_levels;
    LLRhiTexDim  dim;
    LLRhiFormat  format;
    uint32_t     usage;             /* LLRHI_TEX_* bits */
    uint32_t     sample_count;      /* 1 unless MSAA */
} LLRhiTextureDesc;

typedef struct {
    LLRhiAddressMode address_u, address_v, address_w;
    LLRhiFilterMode  min_filter, mag_filter, mip_filter;
    float            max_anisotropy;   /* 1.0 = off */
    uint32_t         compare_enable;   /* shadow samplers */
    LLRhiCompareFunc compare;
} LLRhiSamplerDesc;

/* One vertex attribute. SoA layout [trace:LLVertexBuffer calcOffsets]: each attribute is a
 * contiguous, 16B-aligned block; usually one buffer_slot per attribute all sourced from the
 * same LLRhiBuffer at different offsets. location == TYPE_* ordinal. */
typedef struct {
    uint32_t          location;
    LLRhiVertexFormat format;
    uint32_t          offset;        /* byte offset within its buffer_slot */
    uint32_t          buffer_slot;   /* which bound vertex buffer feeds it */
} LLRhiVertexAttr;

typedef struct {
    const LLRhiVertexAttr* attrs;
    uint32_t               attr_count;
    const uint32_t*        strides;       /* array_stride per buffer_slot (== sTypeSize[type]) */
    uint32_t               buffer_count;  /* number of distinct buffer_slots */
} LLRhiVertexLayout;

/* THE PSO KEY (baked half) [trace:LLGLState + LLRender]. The enable/depth/compare half comes
 * from the llgl.h RAII objects; the parameter half (blend factors, cull face, offset magnitude,
 * stencil ops, color mask, polygon mode) from LLRender. Neither source alone is a complete key
 * -- the caller MUST fuse both at the draw. Dynamic state (viewport/scissor/stencil-ref/blend-
 * constant) is NOT here; it is set per-pass (see commands). */
typedef struct {
    /* depth */
    uint32_t         depth_test;            /* bool */
    uint32_t         depth_write;           /* bool (forced 0 if depth_test==0, GL-spec) */
    LLRhiCompareFunc depth_compare;
    float            depth_bias_constant;   /* from setPolygonOffset units */
    float            depth_bias_slope;      /* from setPolygonOffset factor */
    /* stencil (vestigial in this branch, but part of the key) */
    uint32_t         stencil_enable;        /* bool */
    LLRhiCompareFunc stencil_compare;
    LLRhiStencilOp   stencil_fail_op, stencil_zfail_op, stencil_pass_op;
    uint32_t         stencil_read_mask, stencil_write_mask;
    /* raster */
    LLRhiCullMode    cull_mode;
    LLRhiFrontFace   front_face;
    LLRhiPolygonMode polygon_mode;
    LLRhiTopology    topology;
    uint32_t         sample_count;
    uint32_t         alpha_to_coverage;     /* bool */
} LLRhiRenderState;

/* Per-color-target blend + format (the other half of the PSO key, per attachment). */
typedef struct {
    LLRhiFormat      format;
    uint32_t         blend_enable;          /* bool */
    LLRhiBlendFactor col_src, col_dst;  LLRhiBlendOp col_op;
    LLRhiBlendFactor a_src,   a_dst;    LLRhiBlendOp a_op;
    uint32_t         write_mask;            /* LLRHI_WRITE_* bits */
} LLRhiColorTarget;

typedef struct {
    LLRhiBindingType type;
    uint32_t         binding;        /* binding slot within the group */
    uint32_t         has_dynamic_offset;   /* uniform buffers only */
} LLRhiBindGroupLayoutEntry;

typedef struct {
    const LLRhiBindGroupLayoutEntry* entries;
    uint32_t                         entry_count;
} LLRhiBindGroupLayoutDesc;

/* One bound resource. Only the field matching the layout entry's type is read. */
typedef struct {
    uint32_t     binding;
    LLRhiBuffer  buffer;   uint64_t buffer_offset; uint64_t buffer_size; /* 0 size == whole */
    LLRhiTexture texture;
    LLRhiSampler sampler;
} LLRhiBindGroupEntry;

/* The pipeline (PSO): shader pair + vertex layout + baked state + target formats + bind layouts.
 * GL backend stores {program, state} and applies it on set_pipeline (no real PSO in GL);
 * Vulkan backend hashes+caches a wgpu::RenderPipeline [trace:RHI_PLAN.md sec.1]. */
typedef struct {
    LLRhiShaderModule           vs, fs;
    LLRhiVertexLayout           vertex_layout;
    LLRhiRenderState            state;
    const LLRhiColorTarget*     color_targets;
    uint32_t                    color_target_count;
    LLRhiFormat                 depth_format;          /* LLRHI_FMT_UNDEFINED == no depth */
    const LLRhiBindGroupLayout* bind_group_layouts;
    uint32_t                    bind_group_layout_count;
} LLRhiPipelineDesc;

typedef struct {
    LLRhiTexture texture;
    uint32_t     mip;
    uint32_t     layer;      /* cube face / array layer */
} LLRhiAttachment;

typedef struct {
    const LLRhiAttachment* color;
    uint32_t               color_count;
    LLRhiAttachment        depth;
    uint32_t               has_depth;
} LLRhiRenderTargetDesc;

typedef struct { LLRhiLoadOp load; LLRhiStoreOp store; float clear[4]; } LLRhiColorOp;
typedef struct {
    LLRhiLoadOp  load;  LLRhiStoreOp store;
    float        clear_depth;
    uint32_t     read_only;   /* sampling depth while attached [trace:LLRenderTarget shareDepthBuffer] */
} LLRhiDepthOp;

typedef struct {
    const LLRhiColorOp* color_ops;
    uint32_t            color_op_count;
    LLRhiDepthOp        depth_op;
    uint32_t            has_depth;
} LLRhiPassDesc;

/* ======================================================================================
 *  Functions
 * ==================================================================================== */

/* -- ABI / device -- */
uint32_t    llrhi_abi_version(void);
/* Phase 0: wrap the current (already-created) GL context. Phase 2 adds llrhi_device_create_vk. */
LLRhiDevice llrhi_device_create_gl(void);
void        llrhi_device_destroy(LLRhiDevice dev);

/* -- frame / swapchain -- */
void        llrhi_frame_begin(LLRhiDevice dev);
void        llrhi_frame_present(LLRhiDevice dev);
/* The backbuffer as an attachable texture (for the final pass that targets the screen). */
LLRhiTexture llrhi_frame_backbuffer(LLRhiDevice dev);

/* -- buffers [trace:LLVertexBuffer] -- */
LLRhiBuffer llrhi_buffer_create(LLRhiDevice dev, uint64_t size, uint32_t usage /*LLRHI_BUF_*/);
void        llrhi_buffer_write(LLRhiDevice dev, LLRhiBuffer buf, uint64_t offset,
                               const void* data, uint64_t size);
void        llrhi_buffer_destroy(LLRhiDevice dev, LLRhiBuffer buf);

/* -- textures + samplers [trace:LLImageGL/LLCubeMap + LLRenderTarget] -- */
LLRhiTexture llrhi_texture_create(LLRhiDevice dev, const LLRhiTextureDesc* desc);
void         llrhi_texture_write(LLRhiDevice dev, LLRhiTexture tex, uint32_t mip, uint32_t layer,
                                 uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                 const void* data, uint64_t size);
void         llrhi_texture_destroy(LLRhiDevice dev, LLRhiTexture tex);
void         llrhi_texture_generate_mips(LLRhiDevice dev, LLRhiTexture tex);
LLRhiSampler llrhi_sampler_create(LLRhiDevice dev, const LLRhiSamplerDesc* desc);
void         llrhi_sampler_destroy(LLRhiDevice dev, LLRhiSampler samp);

/* -- shaders [trace:LLGLSLShader] -- */
LLRhiShaderModule llrhi_shader_create(LLRhiDevice dev, const void* code, size_t len,
                                      LLRhiShaderFormat fmt, LLRhiShaderStage stage);
void              llrhi_shader_destroy(LLRhiDevice dev, LLRhiShaderModule mod);

/* -- pipelines / bind-group layouts -- */
LLRhiBindGroupLayout llrhi_bind_group_layout_create(LLRhiDevice dev, const LLRhiBindGroupLayoutDesc* desc);
void                 llrhi_bind_group_layout_destroy(LLRhiDevice dev, LLRhiBindGroupLayout layout);
LLRhiPipeline        llrhi_pipeline_create(LLRhiDevice dev, const LLRhiPipelineDesc* desc);
void                 llrhi_pipeline_destroy(LLRhiDevice dev, LLRhiPipeline pipe);

/* -- bind groups -- */
LLRhiBindGroup llrhi_bind_group_create(LLRhiDevice dev, LLRhiBindGroupLayout layout,
                                       const LLRhiBindGroupEntry* entries, uint32_t entry_count);
void           llrhi_bind_group_destroy(LLRhiDevice dev, LLRhiBindGroup bg);

/* -- render targets [trace:LLRenderTarget] -- */
LLRhiRenderTarget llrhi_render_target_create(LLRhiDevice dev, const LLRhiRenderTargetDesc* desc);
void              llrhi_render_target_destroy(LLRhiDevice dev, LLRhiRenderTarget rt);

/* -- pass + commands. Nested passes are illegal (linearize) [trace:LLRenderTarget]. Commands
 *    are valid only between pass_begin/pass_end. -- */
void llrhi_pass_begin(LLRhiDevice dev, LLRhiRenderTarget rt, const LLRhiPassDesc* desc);
void llrhi_pass_end(LLRhiDevice dev);

void llrhi_set_pipeline(LLRhiDevice dev, LLRhiPipeline pipe);
void llrhi_set_bind_group(LLRhiDevice dev, uint32_t slot, LLRhiBindGroup bg,
                          const uint32_t* dynamic_offsets, uint32_t dyn_count);
void llrhi_set_vertex_buffer(LLRhiDevice dev, uint32_t slot, LLRhiBuffer buf, uint64_t offset);
void llrhi_set_index_buffer(LLRhiDevice dev, LLRhiBuffer buf, LLRhiIndexFormat fmt, uint64_t offset);

/* dynamic state (NOT in the PSO key) */
void llrhi_set_viewport(LLRhiDevice dev, float x, float y, float w, float h,
                        float min_depth, float max_depth);
void llrhi_set_scissor(LLRhiDevice dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void llrhi_set_blend_constant(LLRhiDevice dev, const float rgba[4]);
void llrhi_set_stencil_ref(LLRhiDevice dev, uint32_t reference);

void llrhi_draw(LLRhiDevice dev, uint32_t vertex_count, uint32_t first_vertex);
void llrhi_draw_indexed(LLRhiDevice dev, uint32_t index_count, uint32_t first_index,
                        int32_t base_vertex);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LL_RHI_H */
