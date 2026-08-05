//! Live draw-stream renderer (PHASE3_PLAN.md P3c) — the proven replay renderer
//! (vkharness/src/replay.rs, gate-1 passed on a real sim frame) reshaped for live
//! per-frame submission from the viewer's LLVertexBuffer taps.
//!
//! Contract per frame: fsr_begin_frame() -> N x fsr_submit(desc, vtx, idx) -> fsr_end_frame().
//! Vertex bytes are the viewer's SoA blocks (calcOffsets: per-attribute contiguous,
//! 16-byte aligned, TYPE_VERTEX = vec4 stride 16 w/ .w = texture index) — bound at three
//! slots (pos/uv/color) with per-slot offsets, exactly as the replay proved.

use std::collections::HashMap;

use glam::Mat4;
use wgpu::util::DeviceExt;

// LLVertexBuffer type sizes (llvertexbuffer.cpp:714) — SoA block strides
const TYPE_SIZES: [u32; 14] = [16, 16, 8, 8, 8, 8, 4, 4, 16, 4, 16, 16, 8, 16];
const TYPE_VERTEX: usize = 0;
const TYPE_NORMAL: usize = 1;

/// #3: G-buffer attachment formats. RT0 = albedo.rgb + gbufferFlag.a; RT1 = eye-space
/// normal. 16-bit normal is required for the encode; albedo is 8-bit like stock.
const GBUF_ALBEDO_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8Unorm;
const GBUF_NORMAL_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;
// RT2: legacy = (specular color.rgb, exponent/glossiness.a); PBR = ORM (occlusion, roughness, metallic).
// 8-bit like stock GL_RGBA8 (pipeline.cpp:383 addDeferredAttachments). Materials stratum (M1).
const GBUF_SPEC_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8Unorm;
// RT3: PBR emissive.rgb (linear, additive) + legacy fullbright.a. GL_RGB16F in stock; +alpha for the
// legacy fullbright factor (stock keeps that in RT0.a, but ours holds the flag there). Materials M2b.
const GBUF_EMISSIVE_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;
// P3 reflection-probe constants (mirror llreflectionmapmanager.h). The ReflectionProbes std140 block is
// byte-identical to resolve.frag; RP_UBO_SIZE is its size for MAX_REFMAP_COUNT=256.
const PROBE_MAX_COUNT: usize = 256;         // LL_MAX_REFLECTION_PROBE_COUNT / MAX_REFMAP_COUNT
const PROBE_IRRADIANCE_RES: u32 = 16;       // LL_IRRADIANCE_MAP_RESOLUTION
const RP_UBO_SIZE: u64 = 49248;             // std140 size of the ReflectionProbes block (see resolve_pass.rs RP_SIZE)
const CAP_MAX_WATER: u64 = 16;              // max water draws captured per probe face (ring slots)
const PROBE_CUBE_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float; // radiance/irradiance (D4: Rgba16Float for now)
// S3 convolution constants. CAP_SUPER = stock's probe-capture supersample (mRenderTarget = mProbeResolution*4);
// the gaussian pre-blur (resScale = 1/(probeRes*2)) filters this before the 4:1 reflection-mip reduction to
// mip 0. GEN_SLOT/GAUSS_SLOT are 256-byte dynamic-offset UBO strides for the per-face/mip gen + gaussian params.
const CAP_SUPER: u32 = 4;
const GEN_SLOT: u64 = 256;
const GAUSS_SLOT: u64 = 256;

/// mips in a probe's scratch/radiance chain: round(log2(res)) (stock: (S32)(log2(res)+0.5f)). 128 -> 7 (128..2).
fn probe_mip_count(res: u32) -> u32 {
    (res as f32).log2().round() as u32
}
// std140 offsets into the ReflectionProbes block (element 0 of each array); mirror resolve_pass.rs RP_*.
const RP_OFF_REFSPHERE: usize = 16448;
const RP_OFF_REFPARAMS: usize = 20544;
const RP_OFF_REFINDEX: usize = 24656;
const RP_OFF_REFMAPCOUNT: usize = 49232;

#[inline]
fn f16_bytes(x: f32) -> [u8; 2] {
    let bits = x.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exp = ((bits >> 23) & 0xff) as i32 - 127 + 15;
    let mant = (bits >> 13) & 0x3ff;
    let h = if exp <= 0 { sign } else if exp >= 31 { sign | 0x7c00 } else { sign | ((exp as u16) << 10) | mant as u16 };
    h.to_le_bytes()
}
fn put_vec4(b: &mut [u8], off: usize, v: [f32; 4]) {
    for (i, f) in v.iter().enumerate() { b[off + i * 4..off + i * 4 + 4].copy_from_slice(&f.to_le_bytes()); }
}
fn put_ivec4(b: &mut [u8], off: usize, v: [i32; 4]) {
    for (i, f) in v.iter().enumerate() { b[off + i * 4..off + i * 4 + 4].copy_from_slice(&f.to_le_bytes()); }
}

/// ProbeParams std140 bytes: mat4 pp_inv_proj(64) + mat4 pp_env_mat(64) + vec4 pp_misc(16) =
/// (max_probe_lod, cube_snapshot, z-convention[0=OGL 2*d-1, 1=Vulkan reverse-Z], 0).
fn probe_params_bytes(inv_proj: &glam::Mat4, env_mat: &glam::Mat4, max_lod: f32, cube_snapshot: f32, zconv: f32) -> Vec<u8> {
    let mut v: Vec<u8> = Vec::with_capacity(144);
    for f in inv_proj.to_cols_array() { v.extend_from_slice(&f.to_le_bytes()); }
    for f in env_mat.to_cols_array() { v.extend_from_slice(&f.to_le_bytes()); }
    for f in [max_lod, cube_snapshot, zconv, 0.0] { v.extend_from_slice(&f.to_le_bytes()); }
    v
}

/// A cube-map ARRAY (`cubes` cubes = 6*cubes layers, single mip) uniformly filled with `color`. Usage
/// includes RENDER_ATTACHMENT (S2 face capture) + COPY_DST (S3 mip copies) + TEXTURE_BINDING (sampling).
fn cube_array_filled(device: &wgpu::Device, queue: &wgpu::Queue, res: u32, cubes: u32, color: [f32; 3], label: &str) -> (wgpu::Texture, wgpu::TextureView) {
    let layers = cubes * 6;
    let texel = [f16_bytes(color[0]), f16_bytes(color[1]), f16_bytes(color[2]), f16_bytes(1.0)].concat();
    let mut data = Vec::with_capacity((res * res * layers) as usize * 8);
    for _ in 0..(res * res * layers) { data.extend_from_slice(&texel); }
    let t = device.create_texture_with_data(
        queue,
        &wgpu::TextureDescriptor {
            label: Some(label),
            size: wgpu::Extent3d { width: res, height: res, depth_or_array_layers: layers },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
            format: PROBE_CUBE_FORMAT,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::RENDER_ATTACHMENT
                | wgpu::TextureUsages::COPY_DST | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        },
        wgpu::util::TextureDataOrder::LayerMajor,
        &data,
    );
    let view = t.create_view(&wgpu::TextureViewDescriptor { dimension: Some(wgpu::TextureViewDimension::CubeArray), ..Default::default() });
    (t, view)
}

/// A cube-map ARRAY with a full `mips`-level chain, every subresource seeded with `color`. The radiance
/// output cube: the resolve samples it via textureLod up to max_probe_lod, so no mip may be uninitialized
/// before the first capture+convolve. RENDER_ATTACHMENT so the radiance prefilter renders into each mip/face.
fn cube_array_seeded_mipped(device: &wgpu::Device, queue: &wgpu::Queue, res: u32, cubes: u32, mips: u32, color: [f32; 3], label: &str) -> (wgpu::Texture, wgpu::TextureView) {
    let layers = cubes * 6;
    let texel = [f16_bytes(color[0]), f16_bytes(color[1]), f16_bytes(color[2]), f16_bytes(1.0)].concat();
    let t = device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width: res, height: res, depth_or_array_layers: layers },
        mip_level_count: mips, sample_count: 1, dimension: wgpu::TextureDimension::D2,
        format: PROBE_CUBE_FORMAT,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::RENDER_ATTACHMENT
            | wgpu::TextureUsages::COPY_DST | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    for m in 0..mips {
        let mw = (res >> m).max(1);
        let mh = mw;
        let mut data = Vec::with_capacity((mw * mh * layers) as usize * 8);
        for _ in 0..(mw * mh * layers) { data.extend_from_slice(&texel); }
        queue.write_texture(
            wgpu::ImageCopyTexture { texture: &t, mip_level: m, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            &data,
            wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(mw * 8), rows_per_image: Some(mh) },
            wgpu::Extent3d { width: mw, height: mh, depth_or_array_layers: layers },
        );
    }
    let view = t.create_view(&wgpu::TextureViewDescriptor { dimension: Some(wgpu::TextureViewDimension::CubeArray), ..Default::default() });
    (t, view)
}

// Split-sum env-BRDF (genbrdflutF.glsl): 1024-sample GGX importance-sampled (A,B) for one (NoV, roughness).
fn brdf_hammersley(i: u32, n: u32) -> (f32, f32) {
    let mut bits = (i << 16) | (i >> 16);
    bits = ((bits & 0x5555_5555) << 1) | ((bits & 0xAAAA_AAAA) >> 1);
    bits = ((bits & 0x3333_3333) << 2) | ((bits & 0xCCCC_CCCC) >> 2);
    bits = ((bits & 0x0F0F_0F0F) << 4) | ((bits & 0xF0F0_F0F0) >> 4);
    bits = ((bits & 0x00FF_00FF) << 8) | ((bits & 0xFF00_FF00) >> 8);
    (i as f32 / n as f32, bits as f32 * 2.3283064365386963e-10)
}
fn brdf_pair(nov: f32, roughness: f32) -> (f32, f32) {
    let v = [(1.0 - nov * nov).max(0.0).sqrt(), 0.0, nov];
    let k = (roughness * roughness) / 2.0;
    let (mut a, mut b) = (0.0f32, 0.0f32);
    let num = 1024u32;
    let cross = |x: [f32; 3], y: [f32; 3]| [x[1]*y[2]-x[2]*y[1], x[2]*y[0]-x[0]*y[2], x[0]*y[1]-x[1]*y[0]];
    let norm = |x: [f32; 3]| { let l = (x[0]*x[0]+x[1]*x[1]+x[2]*x[2]).sqrt(); [x[0]/l, x[1]/l, x[2]/l] };
    for i in 0..num {
        let (xi0, xi1) = brdf_hammersley(i, num);
        // normal = (0,0,1); random(normal.xz)=random(0,1) -> a constant phase offset, matched to the shader.
        let rnd = { let dt = 0.0f32 * 12.9898 + 1.0 * 78.233; let sn = dt % 3.14; let f = (sn.sin() * 43758.5453).fract(); if f < 0.0 { f + 1.0 } else { f } };
        let phi = 2.0 * std::f32::consts::PI * xi0 + rnd * 0.1;
        let alpha = roughness * roughness;
        let cos_t = ((1.0 - xi1) / (1.0 + (alpha * alpha - 1.0) * xi1)).sqrt();
        let sin_t = (1.0 - cos_t * cos_t).sqrt();
        let h_ts = [sin_t * phi.cos(), sin_t * phi.sin(), cos_t];
        let up = [1.0f32, 0.0, 0.0];
        let n3 = [0.0f32, 0.0, 1.0];
        let tx = norm(cross(up, n3));
        let ty = cross(n3, tx);
        let h = norm([tx[0]*h_ts[0]+ty[0]*h_ts[1]+n3[0]*h_ts[2],
                      tx[1]*h_ts[0]+ty[1]*h_ts[1]+n3[1]*h_ts[2],
                      tx[2]*h_ts[0]+ty[2]*h_ts[1]+n3[2]*h_ts[2]]);
        let vdoth = v[0]*h[0] + v[1]*h[1] + v[2]*h[2];
        let l = [2.0*vdoth*h[0]-v[0], 2.0*vdoth*h[1]-v[1], 2.0*vdoth*h[2]-v[2]];
        let dot_nl = l[2].max(0.0);
        let dot_nv = v[2].max(0.0);
        let dot_vh = vdoth.max(0.0);
        let dot_nh = h[2].max(0.0);
        if dot_nl > 0.0 {
            let g = (dot_nl / (dot_nl * (1.0 - k) + k)) * (dot_nv / (dot_nv * (1.0 - k) + k));
            let g_vis = (g * dot_vh) / (dot_nh * dot_nv);
            let fc = (1.0 - dot_vh).powf(5.0);
            a += (1.0 - fc) * g_vis;
            b += fc * g_vis;
        }
    }
    (a / num as f32, b / num as f32)
}
fn brdf_lut_data(res: u32) -> Vec<u8> {
    let mut data = Vec::with_capacity((res * res) as usize * 8);
    for y in 0..res {
        for x in 0..res {
            let nov = x as f32 / (res - 1) as f32;
            let rough = 1.0 - y as f32 / (res - 1) as f32;
            let (a, b) = brdf_pair(nov, rough);
            data.extend_from_slice(&[f16_bytes(a), f16_bytes(b), f16_bytes(0.0), f16_bytes(1.0)].concat());
        }
    }
    data
}

const TYPE_TEXCOORD0: usize = 2;
const TYPE_COLOR: usize = 6;
const TYPE_TEXCOORD1: usize = 3;
const TYPE_WEIGHT4: usize = 10;
/// LL_MAX_JOINTS_PER_MESH_OBJECT joints x 3 vec4 rows, 256-aligned ring stride.
const PALETTE_FLOATS: usize = 110 * 12;
const PALETTE_BYTES: usize = PALETTE_FLOATS * 4;   // 5280
const PALETTE_STRIDE: usize = 5376;                // 256-aligned

pub const CLASS_TERRAIN: u32 = 1;
pub const CLASS_WATER: u32 = 2;
pub const CLASS_SKY_DOME: u32 = 3;
pub const CLASS_SKY_SUN: u32 = 4;
pub const CLASS_SKY_MOON: u32 = 5;
pub const CLASS_SKY_STARS: u32 = 6;

/// Increment 1a: the linear-HDR scene-color format. World/sky/water/terrain render into
/// an Rgba16Float target (no ROP encode) instead of straight to the sRGB swapchain, so a
/// later resolve/tonemap can operate in linear HDR. This is the format the deferred
/// G-buffer resolve (Phase 4) writes its lit output into.
const SCENE_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

/// The per-draw descriptor the viewer fills (POD, `#[repr(C)]`, mirrored in C++).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DrawDesc {
    pub mode: u32,       // LLRender: TRIANGLES=0, TRIANGLE_STRIP=1, ...
    pub count: u32,      // indices (indexed) or vertices (arrays)
    pub offset: u32,     // index offset / first vertex
    pub indexed: u32,    // bool
    pub typemask: u32,   // LLVertexBuffer type mask
    pub num_verts: u32,  // vertices in the buffer (drives calcOffsets)
    pub vtx_bytes: u32,  // size of the vertex blob
    pub idx_bytes: u32,  // size of the index blob
    pub tex0: u32,       // bound diffuse texture id (unit 0)
    pub depth_test: u32,
    pub depth_write: u32,
    pub blend: u32,
    pub mvp: [f32; 16],  // projection * modelview at draw time
    pub offsets: [u32; 16], // the viewer's own mOffsets[] (authoritative layout)
    pub color: [f32; 4],    // the bound shader's diffuse colour for THIS draw
    pub tex: [u32; 4],      // texture units 0..3 (indexed textures, chosen by position.w)
    pub draw_class: u32,    // 0 = generic, 1 = terrain
    pub tex_ex: [u32; 8],   // terrain: detail0-3 + alpha_ramp
    pub aux: [f32; 48],     // terrain planes / water payload / sky EEP env
    pub texmat: [f32; 16],  // texture_matrix0 (identity in the common case)
    pub khr: [f32; 8],      // KHR base-color transform [sx,sy,rot,_, ox,oy,_,_]
    pub indexed_ch: u32,    // 0 = position.w is NOT a texture index for this draw
    pub min_alpha: f32,     // MASK cutoff (-1 = disabled)
    pub skin_id: u32,       // nonzero = rigged draw; palette registered via fsr_set_matrix_palette
    pub cull: u32,          // GL_CULL_FACE at draw time
    pub blend_add: u32,     // (SRC_ALPHA, ONE): stars/glow additive
    // ---- BLOCKER #2 (deferred foundation): geometry + material plumbing ----
    // Appended to the struct (ABI-matched with C++ FsrDrawDesc). The current viewer must be
    // rebuilt in lockstep with any engine that READS these -- old-viewer + new-engine would
    // read past the smaller struct. Append-only keeps old-engine + new-viewer safe.
    pub modelview: [f32; 16],     // #2a: GL modelview (view*model) -> normal matrix + eye-space pos
    pub material_model: u32,      // #2b: 0=simple/generic, 1=legacy-material, 2=PBR
    pub pbr: [f32; 4],            // #2b: metallic, roughness, emissive_scale, env_intensity
    pub emissive_color: [f32; 4], // #2b: emissive rgb + _
    pub orm_tex: u32,             // #2b: occlusion-roughness-metallic texture id
    pub emissive_tex: u32,        // #2b: emissive texture id
    pub flags2: u32,              // #4: bit0 = UI pass (bypass tonemap); else reserved
}

pub fn calc_offsets(typemask: u32, num_verts: u32) -> [u32; 14] {
    let mut offsets = [u32::MAX; 14];
    let mut offset = 0u32;
    for i in 0..13 {
        if typemask & (1 << i) != 0 && TYPE_SIZES[i] > 0 {
            offsets[i] = offset;
            offset += TYPE_SIZES[i] * num_verts;
            offset = (offset + 0xF) & !0xF;
        }
    }
    offsets
}

/// GL clip [-1,1] -> wgpu [0,1] depth remap (proven in the replay).
pub fn depth_fix() -> Mat4 {
    // REVERSE-Z: GL clip z [-w,w] -> wgpu [w,0] (near=1, far=0). Combined with float32
    // depth this gives near-uniform precision across the frustum instead of piling it
    // at the far plane -- the fix for sub-mm decal/onion-layer separations dithering at
    // distance. Requires clear=0.0 + GreaterEqual compare everywhere (done below).
    Mat4::from_cols_array(&[
        1.0, 0.0, 0.0, 0.0, //
        0.0, 1.0, 0.0, 0.0, //
        0.0, 0.0, -0.5, 0.0, //
        0.0, 0.0, 0.5, 1.0,
    ])
}

/// One submitted draw, resources already uploaded.
struct Queued {
    bind_key: [u32; 4],
    ubo_off: u32,
    pal_off: u32,
    skinned: bool,
    draw_class: u32,
    terrain_key: [u32; 5],
    vptr: usize,
    iptr: Option<usize>,
    icount: u32,
    voffsets: [u32; 14],
    pipe_key: (bool, bool, bool, bool, bool, bool), // depth_test, depth_write, blend, lines, skinned, cull
    blend_add: bool,
    first: u32,
    vcount: u32,
    is_gb: bool, // #3: opaque generic WITH a normal -> deferred G-buffer fill, not forward
}

/// Phase A.3: one native-VK UI draw -- the honest snapshot of a single LLRender::flush.
struct UiDraw {
    mvp: [f32; 16],     // the viewer's OWN projection * modelview (mMatrix), not glGet
    tex_id: u32,        // getTexUnit(0)->mCurrTexture; 0 or unknown -> 1x1 white
    first_vertex: u32,  // into ui_verts (post mode-expansion)
    vertex_count: u32,
    line: bool,         // LINES/LINE_STRIP -> line pipeline; else triangle pipeline
}

pub struct LiveRenderer {
    bgl: wgpu::BindGroupLayout,
    layout: wgpu::PipelineLayout,
    vs: wgpu::ShaderModule,
    fs: wgpu::ShaderModule,
    pipelines: HashMap<(bool, bool, bool, bool, bool, bool), wgpu::RenderPipeline>,
    pipelines_add: HashMap<(bool, bool, bool, bool, bool, bool), wgpu::RenderPipeline>,
    terrain_bgl: wgpu::BindGroupLayout,
    terrain_pipeline: Option<wgpu::RenderPipeline>,
    terrain_binds: HashMap<[u32; 5], wgpu::BindGroup>,
    // Ground Phase 1 (typed, forward): mesh built engine-side from the fed heightmap, rendered into
    // scene_hdr after the sky (depth-tested) with in-shader N.L. Rebuilt only on TerrainBlock.gen change.
    terrain_typed_pipeline: Option<wgpu::RenderPipeline>,  // P1 forward terrain_typed.{vert,frag} (dormant; P2 uses the G-buffer path)
    terrain_gb_pipeline: Option<wgpu::RenderPipeline>,     // P2/P3 terrain_gb.{vert,frag} -> [albedo+flag, world-normal] + depth
    terrain_gb_bgl: Option<wgpu::BindGroupLayout>,         // P3 splat: UBO(0) + 4 detail(1-4) + ramp(5) + wrap(6) + clamp(7)
    terrain_gb_bind: Option<wgpu::BindGroup>,              // rebuilt per-frame (picks up detail textures as they decode)
    // PBR-2: the PBR base-color splat -- a parallel pipeline reusing terrain_gb.vert; the fragment writes
    // LINEAR base color + HAS_PBR. Selected over the legacy splat when the region is a PBR region.
    pbr_gb_pipeline: Option<wgpu::RenderPipeline>,
    pbr_gb_bgl: Option<wgpu::BindGroupLayout>,
    pbr_gb_bind: Option<wgpu::BindGroup>,                  // rebuilt per-frame
    terrain_is_pbr: bool,                                  // cached from the TerrainBlock -> draw selects PBR vs legacy
    terrain_sampler_wrap: Option<wgpu::Sampler>,           // detail textures (TAM_WRAP)
    terrain_sampler_clamp: Option<wgpu::Sampler>,          // alpha ramp (TAM_CLAMP)
    terrain_typed_ubo: wgpu::Buffer,                       // view_proj + sun/sunlight/ambient + hdr (128B)
    pbr_factors_ubo: wgpu::Buffer,                         // PBR-2: 4x vec4 GLTF baseColor factor (64B), default white
    terrain_typed_bind: Option<wgpu::BindGroup>,
    terrain_vbuf: Option<wgpu::Buffer>,                    // pos(3)+normal(3) per vertex
    terrain_ibuf: Option<wgpu::Buffer>,                    // u32 indices
    terrain_index_count: u32,
    terrain_built_gen: u64,                                // TerrainBlock.gen the current buffers were built for
    skywater_bgl: wgpu::BindGroupLayout,
    sky_pipeline: Option<wgpu::RenderPipeline>,
    water_pipeline: Option<wgpu::RenderPipeline>,
    skywater_binds: HashMap<[u32; 2], wgpu::BindGroup>,
    sampler_clamp: wgpu::Sampler,
    sampler: wgpu::Sampler,
    white: wgpu::TextureView,
    textures: HashMap<u32, (wgpu::Texture, wgpu::TextureView)>,
    queued: Vec<Queued>,
    /// GPU copies of the viewer's vertex/index buffers, keyed by its CPU-shadow pointer.
    /// The viewer tells us when one changes (flush_vbo -> fsr_buffer_dirty), so we upload
    /// each buffer ONCE instead of re-uploading megabytes every frame (the CPU hog).
    geo: HashMap<usize, (wgpu::Buffer, u64)>,
    /// STRIP/FAN/LINE_STRIP expansions are cached under synthetic keys derived from the
    /// source pointer; without tracking, invalidate() (which only knows the real ptr)
    /// never reclaimed them and they leaked for the whole session. Map base ptr ->
    /// derived keys so bufferDirty reclaims both.
    geo_derived: HashMap<usize, Vec<usize>>,
    /// Reusable per-slot uniform buffers + bind groups (80 bytes each, thousands/frame).
    // One ring UBO for the whole frame: per-draw blocks at 256-byte stride, staged on
    // CPU and uploaded with a SINGLE write_buffer in flush. The old design (one tiny
    // buffer + write_buffer per draw, bind groups keyed by (slot, texture)) allocated
    // hundreds of bind groups per frame because the viewer distance-sorts draws -- the
    // slot<->texture pairing churns every frame. Measured: ~14fps at 2,700 draws.
    ubo_ring: Option<wgpu::Buffer>,
    ubo_cap: u64,
    ubo_stage: Vec<u8>,
    binds: HashMap<[u32; 4], wgpu::BindGroup>, // keyed by the batch's 4 texture ids
    pending_uploads: std::collections::VecDeque<(u32, u32, u32, Vec<u8>)>,
    frame_upload_bytes: usize,
    /// HARD SAFETY CEILING: total texture bytes the engine may hold. Oversubscribing
    /// VRAM forced WDDM paging and wedged the ENTIRE system (force-restart level).
    /// Beyond budget, least-recently-used textures are evicted; they re-stream on
    /// demand. Never again.
    tex_bytes: usize,
    tex_budget: usize,
    tex_last_used: HashMap<u32, u64>,
    frame_no: u64,
    /// Rigged skinning: palettes registered per skin id (viewer world-space mGLMp),
    /// staged into a second dynamic-offset ring each frame they are drawn.
    palettes: HashMap<u32, Vec<f32>>,
    palette_ring: Option<wgpu::Buffer>,
    palette_cap: u64,
    palette_stage: Vec<u8>,
    skin_slots: HashMap<u32, u32>, // skin id -> ring offset (this frame)
    slot: usize,
    // Neutral attribute fallbacks: when a draw's typemask lacks TEXCOORD0/COLOR, binding
    // the POSITION block in their place reinterprets float positions as uv/RGBA8 -- which
    // painted the login text red/orange. Bind these instead: uv (0,0), colour opaque white.
    uv_zero: wgpu::Buffer,
    color_white: wgpu::Buffer,
    depth: Option<(wgpu::TextureView, u32, u32, u32)>, // view, w, h, samples
    msaa: u32, // 1/2/4/8 -- from the viewer's RenderFSAASamples
    msaa_color: Option<(wgpu::TextureView, u32, u32, u32)>, // MS resolve source
    /// Increment 1a: linear-HDR scene target. World/sky/water/terrain render here
    /// (Rgba16Float, no ROP encode); the post pass then composites to the swapchain.
    /// The deferred-lighting stage (Phase 4) will write its lit output into this target.
    scene_hdr: Option<(wgpu::TextureView, u32, u32)>,
    scene_hdr_tex: Option<wgpu::Texture>, // DEBUG: same texture, kept for headless readback (COPY_SRC)
    post_bgl: wgpu::BindGroupLayout,
    post_pipeline: Option<wgpu::RenderPipeline>,
    post_bind: Option<wgpu::BindGroup>,
    // Metered auto-exposure (v1): a measure pass averages scene_hdr luminance into a 1x1 target;
    // the tonemap samples it to normalize the HDR before the clamp (replaces the fixed exp_scale).
    exp_lum: Option<wgpu::TextureView>,          // 1x1 R16Float avg scene luminance (temporally smoothed)
    exp_lum_tex: Option<wgpu::Texture>,          // same texture, kept for the diagnostic readback
    exp_primed: bool,                            // false until exp_lum holds a real avg (first measure frame)
    exp_bgl: wgpu::BindGroupLayout,              // measure pass bindings: scene_hdr texture + sampler
    exp_pipeline: Option<wgpu::RenderPipeline>,  // post.vert + exposure_measure.frag
    exp_bind: Option<wgpu::BindGroup>,           // rebuilt when scene_hdr changes
    exp_sampler: wgpu::Sampler,
    // ---- #3/#4: deferred G-buffer + resolve + tonemap ----
    normal_up: wgpu::Buffer, // (0,0,1) fallback for G-buffer draws lacking a NORMAL block
    gbuf_albedo: Option<(wgpu::TextureView, u32, u32)>, // RT0 albedo.rgb + flag.a
    gbuf_normal: Option<(wgpu::TextureView, u32, u32)>, // RT1 eye-space normal
    gbuf_spec: Option<(wgpu::TextureView, u32, u32)>,   // RT2 legacy spec color+exponent / PBR ORM
    gbuf_emissive: Option<(wgpu::TextureView, u32, u32)>, // RT3 PBR emissive.rgb / legacy fullbright.a
    gb_pipeline: Option<wgpu::RenderPipeline>,          // live_gb.vert + live_gb.frag
    resolve_bgl: wgpu::BindGroupLayout,
    resolve_pipeline: Option<wgpu::RenderPipeline>,     // fullscreen: G-buffer -> lit scene_hdr
    resolve_bind: Option<wgpu::BindGroup>,
    env_ubo: wgpu::Buffer,                              // sun_dir + sunlight + ambient (std140)
    frame_env: [f32; 12],                               // CPU copy pushed to env_ubo each flush
    // ---- P3: reflection-probe IBL (default sky probe). resolve.frag bindings 5-11. ----
    probe_radiance: Option<wgpu::Texture>,              // radiance cube ARRAY (count+2 cubes, mipped) -- binding 7
    probe_radiance_view: Option<wgpu::TextureView>,
    probe_irradiance: Option<wgpu::Texture>,            // irradiance cube ARRAY (count cubes, no mips) -- binding 8
    probe_irradiance_view: Option<wgpu::TextureView>,
    probe_ubo: wgpu::Buffer,                            // ReflectionProbes std140 (binding 6), RP_UBO_SIZE bytes
    probe_params_ubo: wgpu::Buffer,                     // ProbeParams std140 (binding 10): inv_proj/env_mat/misc
    brdf_lut_view: Option<wgpu::TextureView>,           // split-sum env-BRDF LUT (binding 11)
    probe_sampler: wgpu::Sampler,                       // filtering, for both cube arrays + brdfLut
    probe_depth_view: Option<wgpu::TextureView>,        // sampleable depth (binding 5); rebuilt with ensure_depth
    probe_count: u32,                                   // active probes (slice 1 = 1 default probe)
    probe_res: u32,                                     // radiance cube face resolution (RenderReflectionProbeResolution)
    // S2: probe capture. A per-face sky UBO (same 240-byte layout as sky_fs_ubo, camera swapped per face)
    // + a bind group for the sky-fs pipeline. The full sky environment is rendered into the 6 cube faces.
    probe_cap_ubo: wgpu::Buffer,
    probe_cap_bind: Option<wgpu::BindGroup>,
    probe_captured: bool,                               // sky captured into the radiance cube at least once
    // S2-remainder: a face-res half-res cloud target + its composite bind, for capturing clouds per face.
    probe_cap_cloud: Option<(wgpu::TextureView, u32, u32)>,
    probe_cap_cloud_comp_bind: Option<wgpu::BindGroup>,
    // S2-terrain: a probe_res mini-frame scene target (the deferred terrain resolve can't write the cube it
    // samples, so the mini-frame renders here, then is copied to the cube face) + a face-res G-buffer/depth
    // + a face resolve bind (binding 0 = probe_cap_ubo, textures = the face G-buffer). terrain_ubo_cache
    // holds the main terrain UBO so the per-face view_proj swap can be restored.
    probe_cap_scene: Option<wgpu::Texture>,
    probe_cap_gbuf: Option<[wgpu::TextureView; 4]>,
    probe_cap_depth: Option<wgpu::TextureView>,
    probe_cap_resolve_bind: Option<wgpu::BindGroup>,
    terrain_ubo_cache: std::cell::Cell<[f32; 32]>,
    // S2-water: a small ring of face water UBOs ([face_mvp | aux]) + per-sw_key capture binds (binding 0 =
    // the ring w/ dynamic offset, 1/2 = the draw's bump maps). Water is a self-contained forward pass.
    probe_cap_water_ubo: wgpu::Buffer,
    probe_cap_water_binds: HashMap<[u32; 2], wgpu::BindGroup>,
    // S3: probe convolution. probe_scratch = a 1-cube array with a full mip chain holding the gaussian-pre-
    // blurred + downsampled captured environment (stock's sourceIdx scratch layer; here its own texture so
    // the radiance prefilter can sample it while writing probe_radiance -- wgpu forbids read+write of one
    // texture in a pass). probe_scratch2d = the per-face 2D reflection-mip targets (probeRes>>i) + gauss_tmp
    // the supersample-res gaussian ping-pong; both rebuilt per face and copied into the cube.
    probe_scratch: Option<wgpu::Texture>,
    probe_scratch_view: Option<wgpu::TextureView>,
    probe_convolved: bool,
    probe_gauss_tmp: Option<wgpu::TextureView>,
    probe_scratch2d: Vec<(wgpu::Texture, wgpu::TextureView, u32)>, // (mip target, its view, res) i=0..mips-1
    gauss_pipeline: Option<wgpu::RenderPipeline>,
    gauss_bgl: Option<wgpu::BindGroupLayout>,
    gauss_params_ubo: wgpu::Buffer,
    refmip_pipeline: Option<wgpu::RenderPipeline>,
    refmip_bgl: Option<wgpu::BindGroupLayout>,
    gen_bgl: Option<wgpu::BindGroupLayout>,
    radiance_gen_pipeline: Option<wgpu::RenderPipeline>,
    irradiance_gen_pipeline: Option<wgpu::RenderPipeline>,
    gen_vbuf: Option<wgpu::Buffer>,
    gen_params_ubo: wgpu::Buffer,
    // S1: Post-params UBO consumed by post_tonemap.frag (std140, 32 bytes).
    post_ubo: wgpu::Buffer,
    post_exposure: f32,    // RenderExposure clamp[0.5,4]
    post_exp_scale: f32,   // auto-exposure meter; 1.0 = fixed (D2)
    post_exp_min: f32,     // stock exposureF dynamic-exposure lower bound (1/hdr_scale, or 1 legacy)
    post_exp_max: f32,     // upper bound (hdr_scale, or 1 legacy); a bright day pins exposure here
    post_tonemap_mix: f32, // 0 = legacy passthrough, else RenderTonemapMix
    post_gamma: f32,       // sky gamma -> legacyGamma soft-clip
    post_tonemap_type: i32,// 0 = PBRNeutral, 1 = ACES Hill
    post_legacy_gamma: i32,// 1 = apply legacyGamma (legacy skies)
    tonemap_pipeline: Option<wgpu::RenderPipeline>,     // post.vert + post_tonemap.frag
    // Phase A.1: fullscreen procedural sky, parallel-read from the typed camera + EepSkyBlock
    // (no tap, no dome mesh). post.vert + sky_fullscreen.frag into scene_hdr as the background.
    sky_fs_bgl: wgpu::BindGroupLayout,
    sky_fs_pipeline: Option<wgpu::RenderPipeline>,
    sky_fs_bind: Option<wgpu::BindGroup>,
    sky_fs_ubo: wgpu::Buffer,        // inv_view_proj(16) + a0..a10(44) = 60 floats (std140, 240B)
    sky_fs_data: [f32; 60],
    sky_fs_enabled: bool,
    // Half-res volumetric cloud pass (perf + grain): cloud.frag raymarches into cloud_target at 1/2
    // res, then cloud_composite.frag bilinearly upscales + alpha-blends it over scene_hdr.
    cloud_target: Option<(wgpu::TextureView, u32, u32)>,  // half-res RGBA16F: rgb=radiance, a=coverage
    cloud_pipeline: Option<wgpu::RenderPipeline>,         // post.vert + cloud.frag (reuses sky_fs_bgl/bind)
    cloud_comp_bgl: wgpu::BindGroupLayout,                // composite: cloud texture + linear sampler
    cloud_comp_pipeline: Option<wgpu::RenderPipeline>,    // post.vert + cloud_composite.frag -> scene_hdr (blend)
    cloud_comp_bind: Option<wgpu::BindGroup>,             // rebuilt when cloud_target changes
    cloud_sampler: wgpu::Sampler,                         // linear, for the bilinear upscale
    clouds_enabled: bool,                                 // gate the half-res cloud pass + composite (debug/perf)
    // Phase A.3: native-VK UI. Honest feed from LLRender::flush (its own matrices + verts, no
    // glGet). One ortho pass over the tonemapped swapchain, painter's order, alpha-blended.
    ui_bgl: Option<wgpu::BindGroupLayout>,
    ui_tri_pipeline: Option<wgpu::RenderPipeline>,  // TRIANGLES/STRIP/FAN (expanded to list)
    ui_line_pipeline: Option<wgpu::RenderPipeline>, // LINES/LINE_STRIP (expanded to list)
    ui_sampler: Option<wgpu::Sampler>,
    ui_verts: Vec<u8>,                 // interleaved {vec3 pos, vec2 uv, u8x4 color} = 24B/vtx
    ui_draws: Vec<UiDraw>,             // per-flush draw records, in submission (painter's) order
    ui_vbuf: Option<(wgpu::Buffer, u64)>, // (buffer, capacity bytes) -- grown as needed
    ui_ubo: Option<(wgpu::Buffer, u64)>,  // per-draw mvp ring, 256-aligned slots
    format: wgpu::TextureFormat,
    pub submitted: u64,
    pub drawn: u64,
    pub upload_drops: u64,
    pub sky_draws: u64,
    pub water_draws: u64,
    sky_dumped: bool,
    /// Current diffuse colour (viewer's "color" uniform, captured by the null-GL stub).
    /// UI text/background colour lives here, not in vertex colours.
    pub cur_color: [f32; 4],
}

impl LiveRenderer {
    pub fn new(device: &wgpu::Device, queue: &wgpu::Queue, format: wgpu::TextureFormat) -> LiveRenderer {
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("live-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    // BOTH stages: the vertex shader reads mvp, the fragment shader reads
                    // the diffuse colour from the same block (vertex-only failed validation
                    // -> every pipeline dropped -> 0 draws rendered).
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: true, // ring UBO: one buffer, per-draw offsets
                        min_binding_size: None,
                    },
                    count: None,
                },
                // Four textures: batches select per-vertex via position.w's integer bit
                // pattern (llface.cpp:2109); a flipped HUD button's texture lands on a
                // channel != 0 that a unit-0-only layout can never show.
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                // Joint palette ring (skinned pipelines read it; others ignore it --
                // extra BGL entries beyond what a shader declares are valid).
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::VERTEX,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: true,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let mut tex_entry = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        let skywater_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("skywater-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: true,
                        min_binding_size: None,
                    },
                    count: None,
                },
                tex_entry(1),
                tex_entry(2),
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let terrain_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("terrain-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: true,
                        min_binding_size: None,
                    },
                    count: None,
                },
                tex_entry(1),
                tex_entry(2),
                tex_entry(3),
                tex_entry(4),
                tex_entry(5),
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 7,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("live-pl"),
            bind_group_layouts: &[&bgl],
            push_constant_ranges: &[],
        });
        // SPIR-V precompiled by glslc (no runtime compiler in the DLL); regenerate from
        // shaders/live.{vert,frag} via: glslc --target-env=vulkan1.2 live.vert -o live.vert.spv
        let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/live.vert.spv"));
        let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/live.frag.spv"));
        let sampler_clamp = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("live-samp-clamp"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("live-samp"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });
        let white_tex = device.create_texture_with_data(
            queue,
            &wgpu::TextureDescriptor {
                label: Some("white"),
                size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8UnormSrgb,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            },
            wgpu::util::TextureDataOrder::LayerMajor,
            &[255u8, 255, 255, 255],
        );
        const FALLBACK_VERTS: usize = 1 << 16;
        let uv_zero = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("uv-zero"),
            contents: &vec![0u8; FALLBACK_VERTS * 8],
            usage: wgpu::BufferUsages::VERTEX,
        });
        let color_white = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("color-white"),
            contents: &vec![0xFFu8; FALLBACK_VERTS * 4],
            usage: wgpu::BufferUsages::VERTEX,
        });
        let post_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("post-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                // S1: Post-params UBO (exposure/exp_scale/tonemap_mix/gamma/tonemap_type/legacy_gamma).
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                    count: None,
                },
                // Metered auto-exposure: 1x1 average scene luminance (texelFetch, no sampler).
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
            ],
        });
        // Metered auto-exposure: the measure pass samples scene_hdr (filterable) through a sampler.
        let exp_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("exp-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let exp_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("exp-sampler"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        // #3: (0,0,1) fallback normal for G-buffer draws whose SoA lacks a NORMAL block.
        let normal_up = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("normal-up"),
            contents: bytemuck::cast_slice(&vec![[0.0f32, 0.0, 1.0, 0.0]; FALLBACK_VERTS]),
            usage: wgpu::BufferUsages::VERTEX,
        });
        // #4: resolve bind group = Env UBO (sun/ambient) + the two G-buffer textures
        // (sampled via texelFetch, so no sampler entry).
        let resolve_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("resolve-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3, // RT2 spec/ORM (materials M1)
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 4, // RT3 emissive/fullbright (materials M2b)
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                // P3 probe bindings 5-11.
                wgpu::BindGroupLayoutEntry { // 5: g_depth (R32Float; view-pos reconstruction). Sampled as
                    // Float to match resolve.frag's `texture2D g_depth` (wgpu rejects a Depth texture on a
                    // Float sampled image under SPIR-V passthrough). normalize(view_pos) is depth-independent
                    // (ray direction), so a stand-in suffices for the sky probe; real depth-export = parallax.
                    binding: 5, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: false }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry { // 6: ReflectionProbes UBO
                    binding: 6, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry { // 7: radiance cube array
                    binding: 7, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::CubeArray, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry { // 8: irradiance cube array
                    binding: 8, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::CubeArray, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry { // 9: probe sampler (filtering)
                    binding: 9, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry { // 10: ProbeParams UBO
                    binding: 10, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry { // 11: brdfLut
                    binding: 11, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
            ],
        });
        let env_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("resolve-env"),
            size: 64, // 4 x vec4 (sun_dir, sunlight, ambient, bg)
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let post_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("post-params"),
            size: 32, // 6 scalars + pad (std140)
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // Phase A.1: fullscreen sky -- one UBO binding (inv_view_proj + WL params + camera).
        let sky_fs_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("sky-fs-bgl"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                count: None,
            }],
        });
        // Half-res cloud composite bindings: cloud texture (filterable) + linear sampler.
        let cloud_comp_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("cloud-comp-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let cloud_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("cloud-sampler"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            ..Default::default()
        });
        let sky_fs_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("sky-fs-ubo"),
            size: 240, // mat4(64) + 11 x vec4(176)
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // P3: reflection-probe UBOs + sampler (persistent). Cube arrays, brdfLut, and the sampleable depth
        // view are created lazily (ensure_probes / ensure_depth).
        let probe_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("reflection-probes-ubo"),
            size: RP_UBO_SIZE,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let probe_params_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("probe-params-ubo"),
            size: 144, // mat4 pp_inv_proj(64) + mat4 pp_env_mat(64) + vec4 pp_misc(16)
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let probe_cap_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("probe-capture-sky-ubo"),
            size: 240, // same layout as sky_fs_ubo (mat4 inv_view_proj + 11 vec4 WL params)
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let probe_cap_water_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("probe-capture-water-ubo"),
            size: 256 * CAP_MAX_WATER, // [face_mvp | 12 aux vec4] = 256 B per water draw
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // S3 convolution UBOs. gaussian: 2 dynamic-offset slots (H = dir(1,0), V = dir(0,1)). gen: one slot
        // per (face,mip) draw (radiance 6*mips + irradiance 6 = 6*(mips+1) <= 48 for res 128), dynamic offset.
        let gauss_params_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("probe-gauss-params-ubo"),
            size: GAUSS_SLOT * 2,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let gen_params_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("probe-gen-params-ubo"),
            size: GEN_SLOT * 48,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let probe_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("probe-sampler"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::FilterMode::Linear,
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            ..Default::default()
        });
        LiveRenderer {
            bgl,
            layout,
            vs,
            fs,
            pipelines: HashMap::new(),
            pipelines_add: HashMap::new(),
            terrain_bgl,
            terrain_pipeline: None,
            terrain_binds: HashMap::new(),
            terrain_typed_pipeline: None,
            terrain_gb_pipeline: None,
            terrain_gb_bgl: None,
            terrain_gb_bind: None,
            pbr_gb_pipeline: None,
            pbr_gb_bgl: None,
            pbr_gb_bind: None,
            terrain_is_pbr: false,
            terrain_sampler_wrap: None,
            terrain_sampler_clamp: None,
            terrain_typed_ubo: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("terrain-typed-ubo"),
                size: 128, // view_proj(64) + sun_dir/sunlight/ambient/misc (64)
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }),
            pbr_factors_ubo: {
                use wgpu::util::DeviceExt;
                device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some("pbr-terrain-factors-ubo"),
                    contents: bytemuck::cast_slice(&[1.0f32; 16]), // 4x white baseColor factor
                    usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                })
            },
            terrain_typed_bind: None,
            terrain_vbuf: None,
            terrain_ibuf: None,
            terrain_index_count: 0,
            terrain_built_gen: 0,
            skywater_bgl,
            sky_pipeline: None,
            water_pipeline: None,
            skywater_binds: HashMap::new(),
            sampler_clamp,
            sampler,
            white: white_tex.create_view(&wgpu::TextureViewDescriptor::default()),
            textures: HashMap::new(),
            queued: Vec::new(),
            geo: HashMap::new(),
            geo_derived: HashMap::new(),
            ubo_ring: None,
            ubo_cap: 0,
            ubo_stage: Vec::new(),
            binds: HashMap::new(),
            pending_uploads: std::collections::VecDeque::new(),
            frame_upload_bytes: 0,
            tex_bytes: 0,
            tex_budget: std::env::var("FS_ENGINE_TEX_BUDGET_MB")
                .ok()
                .and_then(|v| v.parse::<usize>().ok())
                .unwrap_or(4096)
                * 1024 * 1024,
            tex_last_used: HashMap::new(),
            frame_no: 0,
            palettes: HashMap::new(),
            palette_ring: None,
            palette_cap: 0,
            palette_stage: Vec::new(),
            skin_slots: HashMap::new(),
            slot: 0,
            uv_zero,
            color_white,
            depth: None,
            msaa: 1,
            msaa_color: None,
            scene_hdr: None,
            scene_hdr_tex: None,
            post_bgl,
            post_pipeline: None,
            post_bind: None,
            exp_lum: None,
            exp_lum_tex: None,
            exp_primed: false,
            exp_bgl,
            exp_pipeline: None,
            exp_bind: None,
            exp_sampler,
            normal_up,
            gbuf_albedo: None,
            gbuf_normal: None,
            gbuf_spec: None,
            gbuf_emissive: None,
            gb_pipeline: None,
            resolve_bgl,
            resolve_pipeline: None,
            resolve_bind: None,
            env_ubo,
            frame_env: [
                0.3, 0.5, 0.8, 0.0,    // sun_dir (eye-space default; the viewer overrides)
                1.0, 1.0, 1.0, 0.0,    // sunlight (linear)
                0.25, 0.25, 0.30, 0.0, // ambient (linear)
            ],
            probe_radiance: None,
            probe_radiance_view: None,
            probe_irradiance: None,
            probe_irradiance_view: None,
            probe_ubo,
            probe_params_ubo,
            brdf_lut_view: None,
            probe_sampler,
            probe_depth_view: None,
            probe_count: 1,
            probe_res: 128,
            probe_cap_ubo,
            probe_cap_bind: None,
            probe_captured: false,
            probe_cap_cloud: None,
            probe_cap_cloud_comp_bind: None,
            probe_cap_scene: None,
            probe_cap_gbuf: None,
            probe_cap_depth: None,
            probe_cap_resolve_bind: None,
            terrain_ubo_cache: std::cell::Cell::new([0.0f32; 32]),
            probe_cap_water_ubo,
            probe_cap_water_binds: HashMap::new(),
            probe_scratch: None,
            probe_scratch_view: None,
            probe_convolved: false,
            probe_gauss_tmp: None,
            probe_scratch2d: Vec::new(),
            gauss_pipeline: None,
            gauss_bgl: None,
            gauss_params_ubo,
            refmip_pipeline: None,
            refmip_bgl: None,
            gen_bgl: None,
            radiance_gen_pipeline: None,
            irradiance_gen_pipeline: None,
            gen_vbuf: None,
            gen_params_ubo,
            post_ubo,
            // S1 defaults: non-legacy baseline (settings.xml RenderExposure=1, TonemapType=1
            // ACES, TonemapMix=0.7). S3 overrides these per sky regime (legacy => mix=0,
            // legacy_gamma=1, sky-driven gamma).
            post_exposure: 1.0,
            post_exp_scale: 1.0,
            post_exp_min: 1.0,
            post_exp_max: 1.0,
            post_tonemap_mix: 0.7,
            post_gamma: 1.0,
            post_tonemap_type: 1,
            post_legacy_gamma: 0,
            tonemap_pipeline: None,
            sky_fs_bgl,
            sky_fs_pipeline: None,
            sky_fs_bind: None,
            sky_fs_ubo,
            sky_fs_data: [0.0; 60],
            sky_fs_enabled: false,
            cloud_target: None,
            cloud_pipeline: None,
            cloud_comp_bgl,
            cloud_comp_pipeline: None,
            cloud_comp_bind: None,
            cloud_sampler,
            clouds_enabled: true,
            ui_bgl: None,
            ui_tri_pipeline: None,
            ui_line_pipeline: None,
            ui_sampler: None,
            ui_verts: Vec::new(),
            ui_draws: Vec::new(),
            ui_vbuf: None,
            ui_ubo: None,
            format,
            submitted: 0,
            drawn: 0,
            upload_drops: 0,
            sky_draws: 0,
            water_draws: 0,
            sky_dumped: false,
            cur_color: [1.0, 1.0, 1.0, 1.0],
        }
    }

    /// DEFLATE: bounded upload budget. Unthrottled uploads during avatar-rez bursts
    /// queued hundreds of MB of GPU copies with no backpressure -- the driver then
    /// spends MINUTES draining the debt (the 2-minute post-crash desktop freeze).
    /// Over-budget uploads defer to following frames.
    pub fn drain_pending_uploads(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) {
        let mut budget: usize = 32 * 1024 * 1024;
        while budget > 0 {
            let Some((id, w, h, data)) = self.pending_uploads.pop_front() else { break };
            budget = budget.saturating_sub(data.len());
            self.upload_texture_now(device, queue, id, w, h, &data);
        }
        self.frame_upload_bytes = 0;
    }

    pub fn upload_texture(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, id: u32, w: u32, h: u32, rgba: &[u8]) {
        let bytes = (w as usize) * (h as usize) * 4;
        if self.frame_upload_bytes + bytes > 32 * 1024 * 1024 {
            // over budget this frame: defer (latest content wins per id). The deferred
            // queue is itself HARD-CAPPED at 256MB of host RAM -- an uncapped version
            // moved the GPU debt into system memory (measured: 1.5GB free of 32GB,
            // machine paging to death). Beyond cap the oldest entries drop; the viewer
            // re-uploads on its own schedule.
            self.pending_uploads.retain(|(pid, _, _, _)| *pid != id);
            self.pending_uploads.push_back((id, w, h, rgba[..bytes.min(rgba.len())].to_vec()));
            let mut total: usize = self.pending_uploads.iter().map(|(_, _, _, d)| d.len()).sum();
            while total > 256 * 1024 * 1024 {
                if let Some((_, _, _, dropped)) = self.pending_uploads.pop_front() {
                    total -= dropped.len();
                    self.upload_drops += 1;
                } else {
                    break;
                }
            }
            return;
        }
        self.frame_upload_bytes += bytes;
        self.upload_texture_now(device, queue, id, w, h, rgba);
    }

    fn upload_texture_now(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, id: u32, w: u32, h: u32, rgba: &[u8]) {
        if id == 0 || w == 0 || h == 0 || rgba.len() < (w * h * 4) as usize {
            return;
        }
        if let Some((old, _)) = self.textures.get(&id) {
            let sz = old.size();
            self.tex_bytes = self.tex_bytes.saturating_sub((sz.width * sz.height * 4) as usize);
        }
        self.tex_bytes += (w * h * 4) as usize;
        self.tex_last_used.insert(id, self.frame_no);
        let t = device.create_texture_with_data(
            queue,
            &wgpu::TextureDescriptor {
                label: Some("live-tex"),
                size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8UnormSrgb,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            },
            wgpu::util::TextureDataOrder::LayerMajor,
            &rgba[..(w * h * 4) as usize],
        );
        let v = t.create_view(&wgpu::TextureViewDescriptor::default());
        // F1 (gap G16): discard-level changes re-spec textures via glTexImage2D at a new
        // size (llimagegl.cpp:1503) -- bind groups referencing the OLD view would pin it
        // (or the white fallback) forever, leaving textures stuck stale/white. Evict every
        // bind group that references this id so the next draw rebinds the new view.
        self.binds.retain(|k, _| !k.contains(&id));
        self.terrain_binds.retain(|k, _| !k.contains(&id));
        self.textures.insert(id, (t, v));
    }

    /// Patch a sub-rect of an existing texture IN PLACE (glTexSubImage2D). Font atlases
    /// are built glyph-by-glyph this way; re-uploading a whole (re-zeroed) atlas per glyph
    /// erased every glyph already written -- measured as blank login fields.
    pub fn upload_subtexture(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, id: u32, x: u32, y: u32, w: u32, h: u32, full_w: u32, full_h: u32, rgba: &[u8]) -> bool {
        // Create-on-first-content: NULL re-specs no longer upload zeros (each cost
        // 700-1600ms; ~90s cumulative freeze). The first real sub-rect creates the
        // texture at the registered full size.
        if !self.textures.contains_key(&id) {
            let (full_w, full_h) = if full_w == 0 || full_h == 0 {
                // dims registry miss (exhaustion regression guard): partial content
                // beats permanent white -- size to the covering pow2
                ((x + w).next_power_of_two().max(64), (y + h).next_power_of_two().max(64))
            } else {
                (full_w, full_h)
            };
            if x + w > full_w || y + h > full_h {
                return false;
            }
            self.tex_bytes += (full_w * full_h * 4) as usize;
            self.tex_last_used.insert(id, self.frame_no);
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("live-tex-lazy"),
                size: wgpu::Extent3d { width: full_w, height: full_h, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8UnormSrgb,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            });
            let v = t.create_view(&wgpu::TextureViewDescriptor::default());
            self.binds.retain(|k, _| !k.contains(&id));
            self.terrain_binds.retain(|k, _| !k.contains(&id));
            self.textures.insert(id, (t, v));
        }
        let Some((tex, _)) = self.textures.get(&id) else { return false };
        if w == 0 || h == 0 || rgba.len() < (w * h * 4) as usize {
            return false;
        }
        let size = tex.size();
        if x + w > size.width || y + h > size.height {
            return false;
        }
        queue.write_texture(
            wgpu::ImageCopyTexture {
                texture: tex,
                mip_level: 0,
                origin: wgpu::Origin3d { x, y, z: 0 },
                aspect: wgpu::TextureAspect::All,
            },
            &rgba[..(w * h * 4) as usize],
            wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(w * 4), rows_per_image: Some(h) },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        true
    }

    pub fn delete_texture(&mut self, id: u32) {
        if let Some((t, _)) = self.textures.get(&id) {
            let sz = t.size();
            self.tex_bytes = self.tex_bytes.saturating_sub((sz.width * sz.height * 4) as usize);
        }
        self.tex_last_used.remove(&id);
        if self.textures.remove(&id).is_some() {
            self.binds.retain(|k, _| !k.contains(&id));
            self.terrain_binds.retain(|k, _| !k.contains(&id));
        }
    }

    pub fn stats(&self) -> (usize, usize, usize) {
        (self.textures.len(), self.geo.len(), self.palettes.len())
    }

    pub fn mem_stats(&self) -> (usize, u64, usize) {
        // engine texture bytes, cumulative upload drops, deferred-queue depth
        (self.tex_bytes, self.upload_drops, self.pending_uploads.len())
    }

    pub fn set_palette(&mut self, skin_id: u32, joint_count: u32, floats: &[f32]) {
        let n = (joint_count as usize * 12).min(PALETTE_FLOATS).min(floats.len());
        let mut v = vec![0f32; PALETTE_FLOATS];
        v[..n].copy_from_slice(&floats[..n]);
        self.palettes.insert(skin_id, v);
    }

    pub fn set_msaa(&mut self, samples: u32) {
        // SAFETY: the MSAA render path black-screened the moment AA was enabled. Keep
        // the plumbing but force single-sample until it is fixed AND verified. Opt back
        // in for testing via FS_ENGINE_MSAA=1.
        let msaa_enabled = std::env::var("FS_ENGINE_MSAA").map(|v| v == "1").unwrap_or(false);
        let s = if !msaa_enabled { 1 }
            else if samples >= 4 { 4 } else if samples >= 2 { 2 } else { 1 }; // cap 4x (8x not universal)
        if s == self.msaa {
            return;
        }
        self.msaa = s;
        // every pipeline baked the old sample count; force rebuild
        self.pipelines.clear();
        self.pipelines_add.clear();
        self.terrain_pipeline = None;
        self.sky_pipeline = None;
        self.water_pipeline = None;
        self.depth = None;
        self.msaa_color = None;
    }

    // P1 diag getters (temporary): armed sky pass, effective sample count, tonemap exposure/mix.
    pub fn sky_fs_enabled(&self) -> bool { self.sky_fs_enabled }
    pub fn msaa_samples(&self) -> u32 { self.msaa }
    pub fn post_diag(&self) -> (f32, f32, f32) { (self.post_exposure, self.post_exp_scale, self.post_tonemap_mix) }

    /// Flash diagnostic: read back the 1x1 measured avg scene luminance (what the tonemap uses to
    /// derive auto_exp). NaN => the scene shader produced NaN (bad camera/UBO under motion); a
    /// near-zero value => the meter read dark and auto_exp clamps high -> over-exposure. -1 if n/a.
    pub fn read_exp_lum(&self, device: &wgpu::Device, queue: &wgpu::Queue) -> f32 {
        let Some(tex) = self.exp_lum_tex.as_ref() else { return -1.0 };
        let buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("exp-read"), size: 256,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ, mapped_at_creation: false,
        });
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("exp-read") });
        enc.copy_texture_to_buffer(
            wgpu::ImageCopyTexture { texture: tex, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            wgpu::ImageCopyBuffer { buffer: &buf, layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(256), rows_per_image: Some(1) } },
            wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
        );
        queue.submit([enc.finish()]);
        buf.slice(..).map_async(wgpu::MapMode::Read, |_| {});
        device.poll(wgpu::Maintain::Wait);
        let v = {
            let data = buf.slice(..).get_mapped_range();
            let bits = u16::from_le_bytes([data[0], data[1]]);
            let sign = ((bits >> 15) & 1) as u32;
            let exp = ((bits >> 10) & 0x1f) as i32;
            let mant = (bits & 0x3ff) as u32;
            let f = if exp == 0 { (mant as f32) * (2.0f32).powi(-24) }
                else if exp == 0x1f { if mant == 0 { f32::INFINITY } else { f32::NAN } }
                else { (1.0 + (mant as f32) / 1024.0) * (2.0f32).powi(exp - 15) };
            if sign == 1 { -f } else { f }
        };
        buf.unmap();
        v
    }

    fn ensure_msaa_color(&mut self, device: &wgpu::Device, w: u32, h: u32) {
        if self.msaa <= 1 {
            self.msaa_color = None;
            return;
        }
        let need = match &self.msaa_color {
            Some((_, cw, ch, cs)) => *cw != w || *ch != h || *cs != self.msaa,
            None => true,
        };
        if need {
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("live-msaa-color"),
                size: wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: self.msaa,
                dimension: wgpu::TextureDimension::D2,
                format: SCENE_FORMAT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                view_formats: &[],
            });
            self.msaa_color = Some((t.create_view(&wgpu::TextureViewDescriptor::default()), w, h, self.msaa));
        }
    }

    pub fn queued_len(&self) -> usize {
        self.queued.len()
    }

    /// Phase A.3: is there anything to present? Under native-vulkan the tapped world is empty
    /// (queued==0) but the parallel sky and the native UI must still present every frame -- the
    /// old "queued==0 -> skip present" guard would blank the whole viewer otherwise.
    pub fn has_content(&self) -> bool {
        !self.queued.is_empty() || self.sky_fs_enabled || !self.ui_draws.is_empty()
    }

    pub fn begin(&mut self) {
        self.frame_no += 1;
        // The blind LRU that lived here DELETED on-screen textures the viewer believed
        // were resident, with no channel to re-request them -> permanent white. Removed.
        // Texture residency is now governed by the VIEWER's discard machinery (fed a
        // truthful, deflated VRAM budget) + real glDeleteTextures. delete_texture()
        // remains, driven only by the viewer's own frees.
        self.queued.clear();
        self.slot = 0;
        self.ubo_stage.clear();
        self.palette_stage.clear();
        self.skin_slots.clear();
    }

    /// The viewer wrote to this CPU-shadow buffer: drop our GPU copy so it re-uploads once.
    pub fn invalidate(&mut self, ptr: usize) {
        self.geo.remove(&ptr);
        if let Some(derived) = self.geo_derived.remove(&ptr) {
            for k in derived {
                self.geo.remove(&k);
            }
        }
    }

    fn ensure_pipeline_add(&mut self, device: &wgpu::Device, key: (bool, bool, bool, bool, bool, bool)) {
        let ms = self.msaa;
        if self.pipelines_add.contains_key(&key) {
            return;
        }
        // additive: SRC_ALPHA * src + 1 * dst (stars, ADD_WITH_ALPHA)
        self.ensure_pipeline(device, key);
        if let Some(base) = self.pipelines.get(&key) {
            let _ = base; // template only; build the additive variant below
        }
        let add_blend = wgpu::BlendState {
            color: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::SrcAlpha,
                dst_factor: wgpu::BlendFactor::One,
                operation: wgpu::BlendOperation::Add,
            },
            alpha: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::One,
                dst_factor: wgpu::BlendFactor::One,
                operation: wgpu::BlendOperation::Add,
            },
        };
        let vs_add;
        let base_layouts = [
            wgpu::VertexBufferLayout {
                array_stride: 16,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 0, format: wgpu::VertexFormat::Float32x4 }],
            },
            wgpu::VertexBufferLayout {
                array_stride: 8,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 1, format: wgpu::VertexFormat::Float32x2 }],
            },
            wgpu::VertexBufferLayout {
                array_stride: 4,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 2, format: wgpu::VertexFormat::Unorm8x4 }],
            },
        ];
        let vmodule: &wgpu::ShaderModule = if key.4 {
            vs_add = device.create_shader_module(wgpu::include_spirv!("../shaders/skin.vert.spv"));
            &vs_add
        } else {
            &self.vs
        };
        let p = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("live-add"),
            layout: Some(&self.layout),
            vertex: wgpu::VertexState { module: vmodule, entry_point: "main", buffers: &base_layouts },
            fragment: Some(wgpu::FragmentState {
                module: &self.fs,
                entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: SCENE_FORMAT,
                    blend: Some(add_blend),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            primitive: wgpu::PrimitiveState {
                topology: if key.3 { wgpu::PrimitiveTopology::LineList } else { wgpu::PrimitiveTopology::TriangleList },
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: wgpu::TextureFormat::Depth32Float,
                depth_write_enabled: key.1,
                depth_compare: if key.0 { wgpu::CompareFunction::GreaterEqual } else { wgpu::CompareFunction::Always },
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState { count: ms, ..Default::default() },
            multiview: None,
        });
        self.pipelines_add.insert(key, p);
    }

    fn ensure_skywater_pipelines(&mut self, device: &wgpu::Device) {
        let ms = self.msaa;
        if self.sky_pipeline.is_some() {
            return;
        }
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("skywater-pl"),
            bind_group_layouts: &[&self.skywater_bgl],
            push_constant_ranges: &[],
        });
        let make = |device: &wgpu::Device, vs: wgpu::ShaderModule, fsm: wgpu::ShaderModule, slots: u32, depth_write: bool, format: wgpu::TextureFormat, layout: &wgpu::PipelineLayout| {
            let l0 = wgpu::VertexBufferLayout {
                array_stride: 16,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 0, format: wgpu::VertexFormat::Float32x4 }],
            };
            let l1 = wgpu::VertexBufferLayout {
                array_stride: 8,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 1, format: wgpu::VertexFormat::Float32x2 }],
            };
            let both = [l0.clone(), l1];
            let one = [l0];
            let buffers: &[wgpu::VertexBufferLayout] = if slots == 2 { &both } else { &one };
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("skywater"),
                layout: Some(layout),
                vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers },
                fragment: Some(wgpu::FragmentState {
                    module: &fsm,
                    entry_point: "main",
                    targets: &[Some(wgpu::ColorTargetState {
                        format,
                        blend: Some(wgpu::BlendState::REPLACE),
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: wgpu::TextureFormat::Depth32Float,
                    depth_write_enabled: depth_write,
                    depth_compare: wgpu::CompareFunction::GreaterEqual,
                    stencil: wgpu::StencilState::default(),
                    bias: wgpu::DepthBiasState::default(),
                }),
                multisample: wgpu::MultisampleState { count: ms, ..Default::default() },
                multiview: None,
            })
        };
        let sky_vs = device.create_shader_module(wgpu::include_spirv!("../shaders/sky.vert.spv"));
        let sky_fs = device.create_shader_module(wgpu::include_spirv!("../shaders/sky.frag.spv"));
        self.sky_pipeline = Some(make(device, sky_vs, sky_fs, 2, false, SCENE_FORMAT, &layout));
        let water_vs = device.create_shader_module(wgpu::include_spirv!("../shaders/water.vert.spv"));
        let water_fs = device.create_shader_module(wgpu::include_spirv!("../shaders/water.frag.spv"));
        self.water_pipeline = Some(make(device, water_vs, water_fs, 1, true, SCENE_FORMAT, &layout));
    }

    fn ensure_terrain_pipeline(&mut self, device: &wgpu::Device) {
        let ms = self.msaa;
        if self.terrain_pipeline.is_some() {
            return;
        }
        let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/terrain.vert.spv"));
        let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/terrain.frag.spv"));
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("terrain-pl"),
            bind_group_layouts: &[&self.terrain_bgl],
            push_constant_ranges: &[],
        });
        let vlayouts = [
            wgpu::VertexBufferLayout {
                array_stride: 16,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 0, format: wgpu::VertexFormat::Float32x4 }],
            },
            wgpu::VertexBufferLayout {
                array_stride: 8,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 1, format: wgpu::VertexFormat::Float32x2 }],
            },
        ];
        let p = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("terrain"),
            layout: Some(&layout),
            vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &vlayouts },
            fragment: Some(wgpu::FragmentState {
                module: &fs,
                entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: SCENE_FORMAT,
                    blend: Some(wgpu::BlendState::REPLACE), // terrain draws with blending OFF
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: wgpu::TextureFormat::Depth32Float,
                depth_write_enabled: true,
                depth_compare: wgpu::CompareFunction::GreaterEqual,
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState { count: ms, ..Default::default() },
            multiview: None,
        });
        self.terrain_pipeline = Some(p);
    }

    /// Ground Phase 1 (typed forward): (re)build the terrain vertex/index buffers from the fed
    /// heightmap on `gen` change, and ensure the terrain pipeline + UBO bind group. `terrain` is
    /// `e.scene.terrain`. pos = origin + grid*metres + height (world/agent, SL Z-up); per-vertex
    /// normal = normalize(-dz/dx, -dz/dy, 1) via central differences (edge-clamped).
    pub fn ensure_terrain(&mut self, device: &wgpu::Device, terrain: Option<&crate::scene::TerrainBlock>) {
        use wgpu::util::DeviceExt;
        if let Some(t) = terrain {
            if t.dim >= 2 && (t.gen != self.terrain_built_gen || self.terrain_vbuf.is_none()) {
                let dim = t.dim as usize;
                let mpg = t.meters_per_grid.max(1e-4);
                let (ox, oy, oz) = (t.origin[0], t.origin[1], t.origin[2]);
                let hh = &t.heights;
                let h = |i: usize, j: usize| -> f32 { hh[i * dim + j] };
                // P3: composition band value (stock generateHeights). grids_per_region_edge = dim-1
                // (257 -> 256). Per-vertex texcoord1 = (comp, noise) drives the splat.
                let tbl = crate::terrain_noise::tables();
                let dim_c = (dim - 1).max(1);
                let datap = crate::terrain_noise::build_composition(
                    dim_c, hh, dim, mpg, t.origin_global, t.start_height, t.height_range, tbl);
                // Vertex = pos(3) + normal(3) + texcoord1(2) = stride 32.
                let mut verts: Vec<f32> = Vec::with_capacity(dim * dim * 8);
                for i in 0..dim {
                    for j in 0..dim {
                        let x = ox + (j as f32) * mpg;
                        let y = oy + (i as f32) * mpg;
                        let z = oz + h(i, j);
                        let (jm, jp) = (j.saturating_sub(1), (j + 1).min(dim - 1));
                        let (im, ip) = (i.saturating_sub(1), (i + 1).min(dim - 1));
                        let dzdx = (h(i, jp) - h(i, jm)) / (((jp - jm) as f32) * mpg).max(1e-4);
                        let dzdy = (h(ip, j) - h(im, j)) / (((ip - im) as f32) * mpg).max(1e-4);
                        let (nx, ny, nz) = (-dzdx, -dzdy, 1.0f32);
                        let inv = 1.0 / (nx * nx + ny * ny + nz * nz).sqrt();
                        // grid (x=j, y=i) -> vertex_texcoord1(gx=j, gy=i).
                        let tc1 = crate::terrain_noise::vertex_texcoord1(j, i, dim_c, &datap, t.origin_global, tbl);
                        verts.extend_from_slice(&[x, y, z, nx * inv, ny * inv, nz * inv, tc1[0], tc1[1]]);
                    }
                }
                let mut idx: Vec<u32> = Vec::with_capacity((dim - 1) * (dim - 1) * 6);
                for i in 0..dim - 1 {
                    for j in 0..dim - 1 {
                        let a = (i * dim + j) as u32;
                        let b = a + 1;
                        let c = a + dim as u32;
                        let d = c + 1;
                        idx.extend_from_slice(&[a, c, b, b, c, d]);
                    }
                }
                self.terrain_index_count = idx.len() as u32;
                self.terrain_vbuf = Some(device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some("terrain-vbuf"), contents: bytemuck::cast_slice(&verts), usage: wgpu::BufferUsages::VERTEX,
                }));
                self.terrain_ibuf = Some(device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some("terrain-ibuf"), contents: bytemuck::cast_slice(&idx), usage: wgpu::BufferUsages::INDEX,
                }));
                self.terrain_built_gen = t.gen;
            }
        }
        if self.terrain_typed_pipeline.is_none() {
            let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/terrain_typed.vert.spv"));
            let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/terrain_typed.frag.spv"));
            // Dedicated BGL: the terrain reads the UBO in BOTH stages (vertex: view_proj -> gl_Position;
            // fragment: lighting). The sky/cloud passes are fragment-only, so their sky_fs_bgl is
            // FRAGMENT-visibility -- reusing it here fails pipeline validation (the vertex stage's UBO
            // access is not permitted by the layout) and panics the DLL on the first frame.
            let terrain_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("terrain-typed-bgl"),
                entries: &[wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                    count: None,
                }],
            });
            let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("terrain-typed-pl"), bind_group_layouts: &[&terrain_bgl], push_constant_ranges: &[],
            });
            self.terrain_typed_pipeline = Some(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("terrain-typed"),
                layout: Some(&layout),
                vertex: wgpu::VertexState {
                    module: &vs, entry_point: "main",
                    buffers: &[wgpu::VertexBufferLayout {
                        array_stride: 24, step_mode: wgpu::VertexStepMode::Vertex,
                        attributes: &[
                            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 0, shader_location: 0 },
                            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 12, shader_location: 1 },
                        ],
                    }],
                },
                fragment: Some(wgpu::FragmentState {
                    module: &fs, entry_point: "main",
                    targets: &[Some(wgpu::ColorTargetState { format: SCENE_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL })],
                }),
                primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: wgpu::TextureFormat::Depth32Float, depth_write_enabled: true,
                    depth_compare: wgpu::CompareFunction::GreaterEqual, // reverse-Z (matches engine depth)
                    stencil: wgpu::StencilState::default(), bias: wgpu::DepthBiasState::default(),
                }),
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            }));
            // Same dedicated BGL for the bind group (layout-compatible with the pipeline above).
            self.terrain_typed_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("terrain-typed-bind"), layout: &terrain_bgl,
                entries: &[wgpu::BindGroupEntry { binding: 0, resource: self.terrain_typed_ubo.as_entire_binding() }],
            }));
            // P3: the DEFERRED SPLAT variant -- mesh+texcoord1 -> G-buffer [4-detail albedo+FLAG,
            // world-normal] + depth. Own BGL: UBO(0) + 4 detail(1-4) + alpha ramp(5) + wrap(6) + clamp(7).
            let tex_entry = |binding: u32| wgpu::BindGroupLayoutEntry {
                binding, visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                count: None,
            };
            let samp_entry = |binding: u32| wgpu::BindGroupLayoutEntry {
                binding, visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering), count: None,
            };
            let gb_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("terrain-gb-bgl"),
                entries: &[
                    wgpu::BindGroupLayoutEntry { binding: 0, visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None },
                    tex_entry(1), tex_entry(2), tex_entry(3), tex_entry(4), tex_entry(5),
                    samp_entry(6), samp_entry(7),
                ],
            });
            self.terrain_sampler_wrap = Some(device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some("terrain-detail-wrap"),
                address_mode_u: wgpu::AddressMode::Repeat, address_mode_v: wgpu::AddressMode::Repeat, address_mode_w: wgpu::AddressMode::Repeat,
                mag_filter: wgpu::FilterMode::Linear, min_filter: wgpu::FilterMode::Linear, mipmap_filter: wgpu::FilterMode::Linear,
                ..Default::default()
            }));
            self.terrain_sampler_clamp = Some(device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some("terrain-ramp-clamp"),
                address_mode_u: wgpu::AddressMode::ClampToEdge, address_mode_v: wgpu::AddressMode::ClampToEdge, address_mode_w: wgpu::AddressMode::ClampToEdge,
                mag_filter: wgpu::FilterMode::Linear, min_filter: wgpu::FilterMode::Linear, mipmap_filter: wgpu::FilterMode::Nearest,
                ..Default::default()
            }));
            let gb_vs = device.create_shader_module(wgpu::include_spirv!("../shaders/terrain_gb.vert.spv"));
            let gb_fs = device.create_shader_module(wgpu::include_spirv!("../shaders/terrain_gb.frag.spv"));
            let gb_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("terrain-gb-pl"), bind_group_layouts: &[&gb_bgl], push_constant_ranges: &[],
            });
            self.terrain_gb_pipeline = Some(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("terrain-gb"),
                layout: Some(&gb_layout),
                vertex: wgpu::VertexState {
                    module: &gb_vs, entry_point: "main",
                    buffers: &[wgpu::VertexBufferLayout {
                        array_stride: 32, step_mode: wgpu::VertexStepMode::Vertex,
                        attributes: &[
                            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 0, shader_location: 0 },
                            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 12, shader_location: 1 },
                            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x2, offset: 24, shader_location: 2 },
                        ],
                    }],
                },
                fragment: Some(wgpu::FragmentState {
                    module: &gb_fs, entry_point: "main",
                    targets: &[
                        Some(wgpu::ColorTargetState { format: GBUF_ALBEDO_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                        Some(wgpu::ColorTargetState { format: GBUF_NORMAL_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                        Some(wgpu::ColorTargetState { format: GBUF_SPEC_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                        Some(wgpu::ColorTargetState { format: GBUF_EMISSIVE_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                    ],
                }),
                primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: wgpu::TextureFormat::Depth32Float, depth_write_enabled: true,
                    depth_compare: wgpu::CompareFunction::GreaterEqual, // reverse-Z
                    stencil: wgpu::StencilState::default(), bias: wgpu::DepthBiasState::default(),
                }),
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            }));
            self.terrain_gb_bgl = Some(gb_bgl);
            // PBR-2: the parallel PBR base-color pipeline. Reuses terrain_gb.vert, the same 4 textures +
            // ramp + samplers; adds binding 8 = the per-material baseColor factor UBO (FRAGMENT). The
            // fragment (pbrterrain_gb.frag) writes LINEAR base + HAS_PBR so the resolve runs the BRDF.
            let pbr_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("pbr-terrain-gb-bgl"),
                entries: &[
                    wgpu::BindGroupLayoutEntry { binding: 0, visibility: wgpu::ShaderStages::VERTEX,
                        ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None },
                    tex_entry(1), tex_entry(2), tex_entry(3), tex_entry(4), tex_entry(5),
                    samp_entry(6), samp_entry(7),
                    wgpu::BindGroupLayoutEntry { binding: 8, visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None },
                ],
            });
            let pbr_fs = device.create_shader_module(wgpu::include_spirv!("../shaders/pbrterrain_gb.frag.spv"));
            let pbr_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("pbr-terrain-gb-pl"), bind_group_layouts: &[&pbr_bgl], push_constant_ranges: &[],
            });
            self.pbr_gb_pipeline = Some(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("pbr-terrain-gb"),
                layout: Some(&pbr_layout),
                vertex: wgpu::VertexState {
                    module: &gb_vs, entry_point: "main",
                    buffers: &[wgpu::VertexBufferLayout {
                        array_stride: 32, step_mode: wgpu::VertexStepMode::Vertex,
                        attributes: &[
                            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 0, shader_location: 0 },
                            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 12, shader_location: 1 },
                            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x2, offset: 24, shader_location: 2 },
                        ],
                    }],
                },
                fragment: Some(wgpu::FragmentState {
                    module: &pbr_fs, entry_point: "main",
                    targets: &[
                        Some(wgpu::ColorTargetState { format: GBUF_ALBEDO_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                        Some(wgpu::ColorTargetState { format: GBUF_NORMAL_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                        Some(wgpu::ColorTargetState { format: GBUF_SPEC_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                        Some(wgpu::ColorTargetState { format: GBUF_EMISSIVE_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                    ],
                }),
                primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: wgpu::TextureFormat::Depth32Float, depth_write_enabled: true,
                    depth_compare: wgpu::CompareFunction::GreaterEqual, // reverse-Z
                    stencil: wgpu::StencilState::default(), bias: wgpu::DepthBiasState::default(),
                }),
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            }));
            self.pbr_gb_bgl = Some(pbr_bgl);
        }
        // Cache the PBR-region flag for the draw-time pipeline selection (before the disjoint borrow).
        self.terrain_is_pbr = terrain.map(|t| t.pbr).unwrap_or(false);
        // Rebuild the splat binds each frame -- picks up detail textures as they decode (fallback = white).
        // wgpu 0.19 handles aren't Clone, so destructure self into disjoint field refs: the immutable
        // reads (textures/bgl/samplers/ubo) coexist with writing the disjoint *_bind fields.
        let Self {
            terrain_gb_bgl, terrain_sampler_wrap, terrain_sampler_clamp,
            terrain_typed_ubo, textures, white, terrain_gb_bind,
            pbr_gb_bgl, pbr_factors_ubo, pbr_gb_bind, ..
        } = self;
        if let (Some(t), Some(bgl), Some(sw), Some(sc)) = (
            terrain, terrain_gb_bgl.as_ref(), terrain_sampler_wrap.as_ref(), terrain_sampler_clamp.as_ref(),
        ) {
            let white_ref: &wgpu::TextureView = white;
            let d0 = textures.get(&t.detail_tex_ids[0]).map(|(_, v)| v).unwrap_or(white_ref);
            let d1 = textures.get(&t.detail_tex_ids[1]).map(|(_, v)| v).unwrap_or(white_ref);
            let d2 = textures.get(&t.detail_tex_ids[2]).map(|(_, v)| v).unwrap_or(white_ref);
            let d3 = textures.get(&t.detail_tex_ids[3]).map(|(_, v)| v).unwrap_or(white_ref);
            let ramp = textures.get(&t.alpha_ramp_id).map(|(_, v)| v).unwrap_or(white_ref);
            *terrain_gb_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("terrain-gb-bind"),
                layout: bgl,
                entries: &[
                    wgpu::BindGroupEntry { binding: 0, resource: terrain_typed_ubo.as_entire_binding() },
                    wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(d0) },
                    wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(d1) },
                    wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::TextureView(d2) },
                    wgpu::BindGroupEntry { binding: 4, resource: wgpu::BindingResource::TextureView(d3) },
                    wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::TextureView(ramp) },
                    wgpu::BindGroupEntry { binding: 6, resource: wgpu::BindingResource::Sampler(sw) },
                    wgpu::BindGroupEntry { binding: 7, resource: wgpu::BindingResource::Sampler(sc) },
                ],
            }));
            // PBR-2: parallel bind -- same textures/ramp/samplers (detail_tex_ids hold the material
            // base-color ids in a PBR region), + the baseColor-factor UBO at binding 8.
            if let Some(pbgl) = pbr_gb_bgl.as_ref() {
                *pbr_gb_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("pbr-terrain-gb-bind"),
                    layout: pbgl,
                    entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: terrain_typed_ubo.as_entire_binding() },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(d0) },
                        wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(d1) },
                        wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::TextureView(d2) },
                        wgpu::BindGroupEntry { binding: 4, resource: wgpu::BindingResource::TextureView(d3) },
                        wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::TextureView(ramp) },
                        wgpu::BindGroupEntry { binding: 6, resource: wgpu::BindingResource::Sampler(sw) },
                        wgpu::BindGroupEntry { binding: 7, resource: wgpu::BindingResource::Sampler(sc) },
                        wgpu::BindGroupEntry { binding: 8, resource: pbr_factors_ubo.as_entire_binding() },
                    ],
                }));
            }
        }
    }

    /// Feed the terrain UBO (view_proj reverse-Z + world sun/sunlight/ambient + hdr_scale).
    pub fn set_terrain_ubo(&self, queue: &wgpu::Queue, ubo: &[f32; 32]) {
        self.terrain_ubo_cache.set(*ubo); // remember the main UBO so the probe capture can swap+restore view_proj
        queue.write_buffer(&self.terrain_typed_ubo, 0, bytemuck::cast_slice(ubo));
    }

    /// PBR-2/3: feed the 4 per-material GLTF baseColor factors (rgba each, 16 floats). Default = white.
    /// The PBR splat multiplies each material's linearized base color by its factor.
    pub fn set_terrain_pbr_factors(&self, queue: &wgpu::Queue, factors: &[f32; 16]) {
        queue.write_buffer(&self.pbr_factors_ubo, 0, bytemuck::cast_slice(factors));
    }

    fn ensure_palette_ring(&mut self, device: &wgpu::Device, want: u64) {
        if self.palette_cap >= want && self.palette_ring.is_some() {
            return;
        }
        let cap = want.next_power_of_two().max((PALETTE_STRIDE * 16) as u64);
        self.palette_ring = Some(device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("palette-ring"),
            size: cap,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        }));
        self.palette_cap = cap;
        self.binds.clear(); // generic bind groups reference the palette ring
    }

    fn ensure_ring(&mut self, device: &wgpu::Device, want: u64) {
        if self.ubo_cap >= want && self.ubo_ring.is_some() {
            return;
        }
        let cap = want.next_power_of_two().max(256 * 1024);
        self.ubo_ring = Some(device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("live-ubo-ring"),
            size: cap,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        }));
        self.ubo_cap = cap;
        self.binds.clear(); // existing bind groups reference the old ring buffer
        self.terrain_binds.clear();
        self.skywater_binds.clear(); // WATER FIX: these also bind the ring UBO -- omitting
        // this left a stale bind group pointing at the freed ring once draws>1024
        // (ring realloc) -> yellow/green stripes -> freeze.
    }

    /// Upload-once helper: returns a cached GPU buffer for `ptr`, creating it if needed.
    fn geo_buffer(&mut self, device: &wgpu::Device, ptr: usize, bytes: &[u8], usage: wgpu::BufferUsages) -> bool {
        if let Some((_, len)) = self.geo.get(&ptr) {
            if *len == bytes.len() as u64 {
                return true;
            }
        }
        let mut padded;
        let data = if bytes.len() % 4 == 0 {
            bytes
        } else {
            padded = bytes.to_vec();
            while padded.len() % 4 != 0 { padded.push(0); }
            &padded[..]
        };
        let b = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("geo"),
            contents: data,
            usage,
        });
        self.geo.insert(ptr, (b, bytes.len() as u64));
        true
    }

    /// Queue one draw. Vertex/index bytes are copied into GPU buffers (the viewer's CPU
    /// shadow is reused next frame, so we cannot hold the pointers).
    pub fn submit(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, d: &DrawDesc, vtx: &[u8], idx: &[u8]) {
        self.submitted += 1;
        // LLRender eGeomModes: TRIANGLES=0, STRIP=1, FAN=2, POINTS=3, LINES=4,
        // LINE_STRIP=5, LINE_LOOP=6. The edit-tool manipulator arrows draw with FAN and
        // LINES (measured missing); STRIP/FAN/LINE_STRIP expand to list forms, LINES
        // passes through to a line-list pipeline. POINTS/LINE_LOOP remain unsupported.
        let supported = matches!(d.mode, 0 | 1 | 2 | 4 | 5);
        if !supported || d.num_verts == 0 || d.typemask & 1 == 0 || vtx.is_empty() {
            return;
        }
        // Geometry uploaded ONCE per buffer and reused until the viewer invalidates it
        // (flush_vbo -> fsr_buffer_dirty). Re-uploading every frame was the CPU hog.
        let vptr = vtx.as_ptr() as usize;
        if !self.geo_buffer(device, vptr, vtx, wgpu::BufferUsages::VERTEX) {
            return;
        }
        let needs_expand = matches!(d.mode, 1 | 2 | 5);
        let (iptr, icount, first, vcount) = if d.indexed != 0 && !idx.is_empty() {
            let base_ptr = idx.as_ptr() as usize;
            if needs_expand {
                let key = base_ptr
                    ^ 0x5311_D000
                    ^ (d.offset as usize)
                    ^ ((d.count as usize) << 20)
                    ^ ((d.mode as usize) << 40);
                if let Some((_, len)) = self.geo.get(&key) {
                    // cached: skip the per-submit expansion entirely (sky dome = strips
                    // every frame; re-expanding was per-frame allocation churn)
                    (Some(key), (*len / 2) as u32, 0, 0)
                } else {
                    let src: Vec<u16> = idx.chunks_exact(2).map(|b| u16::from_le_bytes([b[0], b[1]])).collect();
                    let take: Vec<u16> = src.iter().skip(d.offset as usize).take(d.count as usize).copied().collect();
                    let list = expand_to_list(d.mode, &take);
                    if list.is_empty() {
                        return;
                    }
                    let n = list.len() as u32;
                    if !self.geo_buffer(device, key, bytemuck::cast_slice(&list), wgpu::BufferUsages::INDEX) {
                        return;
                    }
                    self.geo_derived.entry(base_ptr).or_default().push(key);
                    (Some(key), n, 0, 0)
                }
            } else {
                if !self.geo_buffer(device, base_ptr, idx, wgpu::BufferUsages::INDEX) {
                    return;
                }
                (Some(base_ptr), d.count, d.offset, 0)
            }
        } else if needs_expand {
            let key = 0xA51D_0000
                ^ (d.offset as usize)
                ^ ((d.count as usize) << 20)
                ^ ((d.mode as usize) << 40);
            if let Some((_, len)) = self.geo.get(&key) {
                (Some(key), (*len / 2) as u32, 0, 0)
            } else {
                let seq: Vec<u16> = (d.offset as u16..(d.offset + d.count) as u16).collect();
                let list = expand_to_list(d.mode, &seq);
                if list.is_empty() {
                    return;
                }
                let n = list.len() as u32;
                if !self.geo_buffer(device, key, bytemuck::cast_slice(&list), wgpu::BufferUsages::INDEX) {
                    return;
                }
                self.geo_derived.entry(vptr).or_default().push(key);
                (Some(key), n, 0, 0)
            }
        } else {
            (None, 0, d.offset, d.count)
        };

        // Stage this draw's UBO block (mvp + colour) in the CPU ring at a 256-byte slot;
        // ONE write_buffer uploads the whole frame in flush.
        const STRIDE: usize = 512;
        let slot = self.slot;
        self.slot += 1;
        let need = (slot + 1) * STRIDE;
        if self.ubo_stage.len() < need {
            self.ubo_stage.resize(need, 0);
        }
        let mvp = depth_fix() * Mat4::from_cols_array(&d.mvp);
        let skywater = matches!(d.draw_class, CLASS_WATER | CLASS_SKY_DOME);
        if d.draw_class == CLASS_SKY_DOME { self.sky_draws += 1; }
        if d.draw_class == CLASS_WATER { self.water_draws += 1; }
        if d.draw_class == CLASS_SKY_DOME {
            // #4: self-derive the deferred sun/atmospherics from the sky dome's EEP env.
            // aux[4..6]=LIGHTNORM, aux[8..10]=SUNLIGHT_COLOR, aux[16..18]=AMBIENT (packed by
            // the tap). LIGHTNORM -> eye space via the sky's modelview (sky model ~= view).
            // Space/sign confirmed at the in-world eyeball; iterate here (DLL-only) if off.
            let ln = glam::Vec3::new(d.aux[4], d.aux[5], d.aux[6]);
            let mv3 = glam::Mat3::from_mat4(Mat4::from_cols_array(&d.modelview));
            let eye_sun = (mv3 * ln).normalize_or_zero();
            self.frame_env = [
                eye_sun.x, eye_sun.y, eye_sun.z, 0.0,
                d.aux[8], d.aux[9], d.aux[10], 0.0,
                d.aux[16], d.aux[17], d.aux[18], 0.0,
            ];
        }
        if d.draw_class == CLASS_SKY_DOME && !self.sky_dumped {
            self.sky_dumped = true;
            if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open("C:/fs/fsr_perf.log") {
                use std::io::Write;
                let e = &d.aux;
                let _ = writeln!(f, "SKY_ENV camPos({:.1},{:.1},{:.1}) maxY {:.1} | lightnorm({:.2},{:.2},{:.2}) sunUp {:.1} | sunCol({:.2},{:.2},{:.2}) densMul {:.3} | blueHoriz({:.2},{:.2},{:.2}) hazeHoriz {:.2} | blueDens({:.2},{:.2},{:.2}) cloudShadow {:.2} | glow({:.2},{:.2},{:.2})",
                    e[0],e[1],e[2],e[3], e[4],e[5],e[6],e[7], e[8],e[9],e[10],e[11], e[20],e[21],e[22],e[23], e[24],e[25],e[26],e[27], e[28],e[29],e[30]);
            }
        }
        if skywater {
            // dedicated slot layout: [mvp | 12 vec4 aux] = 256 bytes exactly
            let mut block = [0f32; 64];
            block[..16].copy_from_slice(&mvp.to_cols_array());
            block[16..64].copy_from_slice(&d.aux);
            let bytes: &[u8] = bytemuck::cast_slice(&block);
            self.ubo_stage[slot * STRIDE..slot * STRIDE + bytes.len()].copy_from_slice(bytes);
        } else {
            let mut block = [0f32; 68]; // mvp + color + planes + texmat + khr + flags + normal_mat(std140 mat3)
            block[..16].copy_from_slice(&mvp.to_cols_array());
            block[16..20].copy_from_slice(&d.color);
            block[20..28].copy_from_slice(&d.aux[..8]);
            block[28..44].copy_from_slice(&d.texmat);
            block[44..52].copy_from_slice(&d.khr);
            block[52] = if d.blend != 0 { 1.0 } else { 0.0 }; // flags.x: blending enabled
            block[53] = if d.indexed_ch > 0 { 1.0 } else { 0.0 }; // flags.y: pos.w is a tex index
            block[54] = d.min_alpha; // flags.z: MASK cutoff (-1 disabled)
            // #3: normal matrix = inverse-transpose of the modelview upper-3x3, std140 mat3
            // (3 columns each padded to vec4). Read by live_gb.vert; ignored by live.frag.
            let nm = glam::Mat3::from_mat4(Mat4::from_cols_array(&d.modelview)).inverse().transpose();
            let nc = nm.to_cols_array();
            block[56] = nc[0]; block[57] = nc[1]; block[58] = nc[2];
            block[60] = nc[3]; block[61] = nc[4]; block[62] = nc[5];
            block[64] = nc[6]; block[65] = nc[7]; block[66] = nc[8];
            let bytes: &[u8] = bytemuck::cast_slice(&block);
            self.ubo_stage[slot * STRIDE..slot * STRIDE + bytes.len()].copy_from_slice(bytes);
        }
        let _ = queue;

        // Rigged: stage this skin's palette once per frame; draws whose palette has
        // not arrived are SKIPPED (unskinned bind-pose geometry would land at region
        // origin -- the invisible-statue failure, worse than absence).
        let mut pal_off = 0u32;
        let mut skinned = false;
        if d.skin_id != 0 {
            if let Some(slot_off) = self.skin_slots.get(&d.skin_id) {
                pal_off = *slot_off;
                skinned = true;
            } else if let Some(pal) = self.palettes.get(&d.skin_id) {
                let off = self.palette_stage.len();
                self.palette_stage.resize(off + PALETTE_STRIDE, 0);
                self.palette_stage[off..off + PALETTE_BYTES].copy_from_slice(bytemuck::cast_slice(pal));
                pal_off = off as u32;
                self.skin_slots.insert(d.skin_id, pal_off);
                skinned = true;
            }
            // no palette yet, or no WEIGHT4 stream: skip (bind-pose geometry at
            // region origin is worse than a frame of absence)
            if !skinned || d.typemask & (1u32 << TYPE_WEIGHT4) == 0 {
                return;
            }
        }

        let bind_key = d.tex;
        for id in bind_key.iter().chain(d.tex_ex[..5].iter()) {
            if *id != 0 {
                self.tex_last_used.insert(*id, self.frame_no);
            }
        }
        if d.draw_class != CLASS_TERRAIN && !self.binds.contains_key(&bind_key) {
            self.ensure_ring(device, need.max(STRIDE * 1024) as u64);
            self.ensure_palette_ring(device, (PALETTE_STRIDE * 16) as u64);
            let ring = self.ubo_ring.as_ref().unwrap();
            let pal_ring = self.palette_ring.as_ref().unwrap();
            let tv: Vec<&wgpu::TextureView> = bind_key
                .iter()
                .map(|id| self.textures.get(id).map(|(_, v)| v).unwrap_or(&self.white))
                .collect();
            let bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("live-bind"),
                layout: &self.bgl,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        // 272 = 68-float generic block (incl. #3 normal_mat, std140 mat3).
                        // The forward live.vert reads only the first 224; live_gb.vert reads all 272.
                        resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                            buffer: ring,
                            offset: 0,
                            size: std::num::NonZeroU64::new(272),
                        }),
                    },
                    wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(tv[0]) },
                    wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(tv[1]) },
                    wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::TextureView(tv[2]) },
                    wgpu::BindGroupEntry { binding: 4, resource: wgpu::BindingResource::TextureView(tv[3]) },
                    wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::Sampler(&self.sampler) },
                    wgpu::BindGroupEntry {
                        binding: 6,
                        resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                            buffer: pal_ring,
                            offset: 0,
                            size: std::num::NonZeroU64::new(PALETTE_BYTES as u64),
                        }),
                    },
                ],
            });
            self.binds.insert(bind_key, bg);
        }

        let lines = matches!(d.mode, 4 | 5);
        if skywater {
            let sw_key = [d.tex_ex[0], d.tex_ex[1]];
            self.ensure_skywater_pipelines(device);
            if !self.skywater_binds.contains_key(&sw_key) {
                self.ensure_ring(device, need.max(STRIDE * 1024) as u64);
                let ring = self.ubo_ring.as_ref().unwrap();
                let t0 = self.textures.get(&sw_key[0]).map(|(_, v)| v).unwrap_or(&self.white);
                let t1 = self.textures.get(&sw_key[1]).map(|(_, v)| v).unwrap_or(&self.white);
                let bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("skywater-bind"),
                    layout: &self.skywater_bgl,
                    entries: &[
                        wgpu::BindGroupEntry {
                            binding: 0,
                            resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                                buffer: ring,
                                offset: 0,
                                size: std::num::NonZeroU64::new(256),
                            }),
                        },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(t0) },
                        wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(t1) },
                        wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::Sampler(&self.sampler) },
                    ],
                });
                self.skywater_binds.insert(sw_key, bg);
            }
        }
        let pipe_key = (d.depth_test != 0, d.depth_write != 0, d.blend != 0 || d.blend_add != 0, lines, skinned, d.cull != 0);
        if d.blend_add != 0 {
            self.ensure_pipeline_add(device, pipe_key);
        }
        let mut terrain_key = [0u32; 5];
        if d.draw_class == CLASS_TERRAIN {
            terrain_key.copy_from_slice(&d.tex_ex[..5]);
            self.ensure_terrain_pipeline(device);
            if !self.terrain_binds.contains_key(&terrain_key) {
                self.ensure_ring(device, need.max(STRIDE * 1024) as u64);
                let ring = self.ubo_ring.as_ref().unwrap();
                let tv: Vec<&wgpu::TextureView> = terrain_key
                    .iter()
                    .map(|id| self.textures.get(id).map(|(_, v)| v).unwrap_or(&self.white))
                    .collect();
                let bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("terrain-bind"),
                    layout: &self.terrain_bgl,
                    entries: &[
                        wgpu::BindGroupEntry {
                            binding: 0,
                            resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                                buffer: ring,
                                offset: 0,
                                size: std::num::NonZeroU64::new(224),
                            }),
                        },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(tv[0]) },
                        wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(tv[1]) },
                        wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::TextureView(tv[2]) },
                        wgpu::BindGroupEntry { binding: 4, resource: wgpu::BindingResource::TextureView(tv[3]) },
                        wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::TextureView(tv[4]) },
                        wgpu::BindGroupEntry { binding: 6, resource: wgpu::BindingResource::Sampler(&self.sampler) },
                        wgpu::BindGroupEntry { binding: 7, resource: wgpu::BindingResource::Sampler(&self.sampler_clamp) },
                    ],
                });
                self.terrain_binds.insert(terrain_key, bg);
            }
        } else {
            self.ensure_pipeline(device, pipe_key);
        }
        let voffsets = {
            let mut o = [u32::MAX; 14];
            let has = d.offsets.iter().take(14).any(|&x| x != 0);
            for i in 0..14 {
                if d.typemask & (1 << i) != 0 {
                    o[i] = if has { d.offsets[i] } else { calc_offsets(d.typemask, d.num_verts)[i] };
                }
            }
            o
        };
        let mut tk = terrain_key;
        if skywater {
            tk[0] = d.tex_ex[0];
            tk[1] = d.tex_ex[1];
        }
        // #3: opaque generic with a NORMAL block -> deferred G-buffer fill (not forward).
        let is_gb = d.draw_class == 0
            && d.blend == 0
            && d.blend_add == 0
            && d.skin_id == 0
            && (d.typemask & (1u32 << TYPE_NORMAL)) != 0
            && matches!(d.mode, 0 | 1 | 2);
        self.queued.push(Queued { bind_key, ubo_off: (slot * STRIDE) as u32, pal_off, skinned, draw_class: d.draw_class, terrain_key: tk, vptr, iptr, icount, voffsets, pipe_key, first, vcount, blend_add: d.blend_add != 0, is_gb });
    }

    fn ensure_pipeline(&mut self, device: &wgpu::Device, key: (bool, bool, bool, bool, bool, bool)) {
        let ms = self.msaa;
        if self.pipelines.contains_key(&key) {
            return;
        }
        let base_layouts = [
            wgpu::VertexBufferLayout {
                array_stride: 16,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 0, format: wgpu::VertexFormat::Float32x4 }],
            },
            wgpu::VertexBufferLayout {
                array_stride: 8,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 1, format: wgpu::VertexFormat::Float32x2 }],
            },
            wgpu::VertexBufferLayout {
                array_stride: 4,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 2, format: wgpu::VertexFormat::Unorm8x4 }],
            },
        ];
        let weight_layout = wgpu::VertexBufferLayout {
            array_stride: 16,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 3, format: wgpu::VertexFormat::Float32x4 }],
        };
        let skinned_layouts = [
            base_layouts[0].clone(),
            base_layouts[1].clone(),
            base_layouts[2].clone(),
            weight_layout,
        ];
        let skin_vs;
        let (vmodule, vbuffers): (&wgpu::ShaderModule, &[wgpu::VertexBufferLayout]) = if key.4 {
            skin_vs = device.create_shader_module(wgpu::include_spirv!("../shaders/skin.vert.spv"));
            (&skin_vs, &skinned_layouts)
        } else {
            (&self.vs, &base_layouts)
        };
        let p = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("live"),
            layout: Some(&self.layout),
            vertex: wgpu::VertexState { module: vmodule, entry_point: "main", buffers: vbuffers },
            fragment: Some(wgpu::FragmentState {
                module: &self.fs,
                entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: SCENE_FORMAT,
                    blend: if key.2 { Some(wgpu::BlendState::ALPHA_BLENDING) } else { Some(wgpu::BlendState::REPLACE) },
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            primitive: wgpu::PrimitiveState {
                topology: if key.3 { wgpu::PrimitiveTopology::LineList } else { wgpu::PrimitiveTopology::TriangleList },
                // honor GL_CULL_FACE (GL winding conventions survive our passthrough
                // matrices); shading every avatar backface saturated the GPU
                cull_mode: if key.5 && !key.3 { Some(wgpu::Face::Back) } else { None },
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: wgpu::TextureFormat::Depth32Float,
                depth_write_enabled: key.1,
                depth_compare: if key.0 { wgpu::CompareFunction::GreaterEqual } else { wgpu::CompareFunction::Always },
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState { count: ms, ..Default::default() },
            multiview: None,
        });
        self.pipelines.insert(key, p);
    }

    fn ensure_depth(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, w: u32, h: u32) {
        let need = match &self.depth {
            Some((_, dw, dh, ds)) => *dw != w || *dh != h || *ds != self.msaa,
            None => true,
        };
        if need {
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("live-depth"),
                size: wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: self.msaa,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Depth32Float,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                view_formats: &[],
            });
            self.depth = Some((t.create_view(&wgpu::TextureViewDescriptor::default()), w, h, self.msaa));
            // P3 binding 5 stand-in: a full-size R32Float g_depth. Zero-filled; normalize(view_pos) is
            // depth-independent (ray direction) so the sky probe's reflection/fresnel/irradiance directions
            // are correct regardless. Real per-pixel depth (a depth-export RT) lands with parallax probes.
            let dw = (w.max(1) * h.max(1)) as usize;
            let zeros = vec![0u8; dw * 4];
            let dt = device.create_texture_with_data(
                queue,
                &wgpu::TextureDescriptor {
                    label: Some("probe-depth-standin"),
                    size: wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: 1 },
                    mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
                    format: wgpu::TextureFormat::R32Float, usage: wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
                },
                wgpu::util::TextureDataOrder::LayerMajor,
                &zeros,
            );
            self.probe_depth_view = Some(dt.create_view(&wgpu::TextureViewDescriptor::default()));
            self.resolve_bind = None; // rebind the resolve against the new depth stand-in (binding 5)
        }
    }

    /// Increment 1a: (re)create the linear-HDR scene target on size change. World geometry
    /// renders into this instead of the swapchain; the post pass composites it out.
    fn ensure_scene_hdr(&mut self, device: &wgpu::Device, w: u32, h: u32) {
        let need = match &self.scene_hdr {
            Some((_, sw, sh)) => *sw != w || *sh != h,
            None => true,
        };
        if need {
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("scene-hdr"),
                size: wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: SCENE_FORMAT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            });
            self.scene_hdr = Some((t.create_view(&wgpu::TextureViewDescriptor::default()), w, h));
            self.scene_hdr_tex = Some(t);
            self.post_bind = None; // stale: rebind against the new scene view
            self.exp_bind = None;  // measure pass samples scene_hdr -> rebind too
        }
    }

    /// DEBUG: the linear-HDR scene target (post-sky/cloud, pre-tonemap), for headless readback.
    pub fn scene_hdr_debug(&self) -> Option<(&wgpu::Texture, u32, u32)> {
        match (&self.scene_hdr_tex, &self.scene_hdr) {
            (Some(t), Some((_, w, h))) => Some((t, *w, *h)),
            _ => None,
        }
    }

    /// The final composite pass (fullscreen triangle) + its bind group. Reads the linear-HDR
    /// scene target and tonemaps it (PBRNeutral) to the sRGB swapchain -- the swapchain ROP
    /// applies the sRGB encode (single encode site; the shader outputs linear). This is the
    /// mandatory HDR->LDR step whose absence caused the earlier full-bright blowout: without
    /// it, HDR radiance > 1.0 hard-clamps to white across every bright pixel. The 1a identity
    /// copy (post_copy.frag) is retired; exposure is fixed at 1.0 until the post tier.
    fn ensure_post(&mut self, device: &wgpu::Device) {
        if self.post_pipeline.is_none() {
            let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/post.vert.spv"));
            let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/post_tonemap.frag.spv"));
            let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("post-pl"),
                bind_group_layouts: &[&self.post_bgl],
                push_constant_ranges: &[],
            });
            let swap_fmt = self.format;
            self.post_pipeline = Some(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("post-tonemap"),
                layout: Some(&layout),
                vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
                fragment: Some(wgpu::FragmentState {
                    module: &fs,
                    entry_point: "main",
                    targets: &[Some(wgpu::ColorTargetState {
                        format: swap_fmt,
                        blend: Some(wgpu::BlendState::REPLACE),
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            }));
        }
        if self.post_bind.is_none() {
            let bgl = &self.post_bgl;
            let bind = match (self.scene_hdr.as_ref(), self.exp_lum.as_ref()) {
                (Some((v, _, _)), Some(lum)) => Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("post-bind"),
                    layout: bgl,
                    entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(v) },
                        wgpu::BindGroupEntry { binding: 1, resource: self.post_ubo.as_entire_binding() },
                        wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(lum) },
                    ],
                })),
                _ => None,
            };
            self.post_bind = bind;
        }
    }

    /// Metered auto-exposure: ensure the 1x1 luminance target, the measure pipeline, and the bind
    /// group (rebuilt against the current scene_hdr). The measure pass fills exp_lum each frame and
    /// post_tonemap.frag samples it to derive the exposure multiplier.
    fn ensure_exp(&mut self, device: &wgpu::Device) {
        if self.exp_lum.is_none() {
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("exp-lum"),
                size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::R16Float,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            });
            self.exp_lum = Some(t.create_view(&wgpu::TextureViewDescriptor::default()));
            self.exp_lum_tex = Some(t);
        }
        if self.exp_pipeline.is_none() {
            let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/post.vert.spv"));
            let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/exposure_measure.frag.spv"));
            let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("exp-pl"),
                bind_group_layouts: &[&self.exp_bgl],
                push_constant_ranges: &[],
            });
            self.exp_pipeline = Some(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("exp-measure"),
                layout: Some(&layout),
                vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
                fragment: Some(wgpu::FragmentState {
                    module: &fs,
                    entry_point: "main",
                    targets: &[Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::R16Float,
                        // Eye-adaptation blend: out = avg*C + prev*(1-C), C = per-frame adaptation rate
                        // (set via set_blend_constant). Persists the 1x1 lum across frames (LoadOp::Load)
                        // so exposure adapts smoothly instead of tracking per-frame avg_lum swings (flicker).
                        blend: Some(wgpu::BlendState {
                            color: wgpu::BlendComponent {
                                src_factor: wgpu::BlendFactor::Constant,
                                dst_factor: wgpu::BlendFactor::OneMinusConstant,
                                operation: wgpu::BlendOperation::Add,
                            },
                            alpha: wgpu::BlendComponent::REPLACE,
                        }),
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            }));
        }
        if self.exp_bind.is_none() {
            if let Some((sv, _, _)) = &self.scene_hdr {
                self.exp_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("exp-bind"),
                    layout: &self.exp_bgl,
                    entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(sv) },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&self.exp_sampler) },
                    ],
                }));
            }
        }
    }

    /// Render every queued draw into `target` and return how many were drawn.
    pub fn flush(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        target: &wgpu::TextureView,
        w: u32,
        h: u32,
    ) -> u64 {
        self.flush_clear(device, queue, target, w, h, wgpu::Color { r: 0.09, g: 0.35, b: 0.4, a: 1.0 })
    }

    /// #4: the viewer's per-frame sun/atmospherics (eye-space sun_dir, sunlight, ambient),
    /// 12 floats (3 x vec4). Consumed by the deferred resolve. Default set in new().
    pub fn set_frame_env(&mut self, env: &[f32]) {
        let n = env.len().min(12);
        self.frame_env[..n].copy_from_slice(&env[..n]);
    }

    /// S1: the tonemap/exposure/gamma params consumed by post_tonemap.frag. Wired to the sky
    /// regime by S3 (legacy classic => tonemap_mix=0, legacy_gamma=1, sky-driven gamma;
    /// advanced => full ACES/PBRNeutral). exp_scale stays 1.0 until auto-exposure (S6).
    pub fn set_post_params(&mut self, exposure: f32, exp_scale: f32, tonemap_mix: f32,
                           gamma: f32, tonemap_type: i32, legacy_gamma: i32) {
        self.post_exposure = exposure.clamp(0.5, 4.0);
        self.post_exp_scale = exp_scale;
        self.post_tonemap_mix = tonemap_mix.clamp(0.0, 1.0);
        self.post_gamma = gamma;
        self.post_tonemap_type = tonemap_type;
        self.post_legacy_gamma = legacy_gamma;
    }

    /// S3b: consume a derived `SkyRegime` (from `SceneFrame::sky_regime()`) -- drive the tonemap
    /// post-params from the regime instead of defaults, so the global tonemap uses the SAME
    /// legacy/advanced regime the viewer used for the sky. exp_scale stays 1.0 (fixed exposure,
    /// D2) until auto-exposure (S6). `classic_mode`/scales/`sky_hdr_scale` drive the resolve (S4);
    /// the sky pass's own `sky_hdr_scale` still arrives via the tap aux (already the viewer value).
    pub fn apply_sky_regime(&mut self, r: &crate::scene::SkyRegime) {
        // Metered auto-exposure now normalizes the HDR (post_tonemap.frag derives the exposure from
        // the measured avg luminance). exp_scale is a MANUAL multiplier on top (default 1.0 = pure
        // auto); FS_ENGINE_EXPOSURE=<f32> tunes the overall level without a rebuild.
        let exp_scale = std::env::var("FS_ENGINE_EXPOSURE").ok()
            .and_then(|v| v.parse::<f32>().ok()).unwrap_or(1.0);
        self.set_post_params(r.exposure, exp_scale, r.tonemap_mix, r.gamma, r.tonemap_type as i32, r.legacy_gamma as i32);
        // Stock exposureF bounds from the sky's HDR scale (pipeline.cpp:8073-8085): probe/EEP skies get
        // [1/scale, scale] where scale = sqrt(gamma)*2; legacy (scale<=1) gets [1,1] = constant. These are
        // the FAITHFUL bounds -- the earlier `1 + (scale-1)*0.25` damping was a `// TEST` hack that hid
        // the inverted-mix blowout in post_tonemap (now fixed: bright pins at exp_min, so no blowout).
        let scale = r.sky_hdr_scale;
        if scale > 1.0 { self.post_exp_min = 1.0 / scale; self.post_exp_max = scale; }
        else { self.post_exp_min = 1.0; self.post_exp_max = 1.0; }
    }

    /// Phase A.1: the fullscreen sky UBO, packed by the caller from the typed camera +
    /// EepSkyBlock: `inv_view_proj`(16) + a0..a10(44) = 60 floats. Layout matches
    /// sky_fullscreen.frag. Enables the parallel-read sky pass this frame. Pass an empty slice
    /// (or never call it) to leave the sky off (e.g. the tap path).
    /// NOTE: a9 (sun_dir) and a10 (moon_dir) live at floats 52..60 -- copy the FULL 60 or the
    /// sun/moon discs and the star night-gate silently read zero (bug fixed 2026-08-02: was min(52)).
    pub fn set_fullscreen_sky(&mut self, ubo: &[f32]) {
        let n = ubo.len().min(60);
        self.sky_fs_data[..n].copy_from_slice(&ubo[..n]);
        self.sky_fs_enabled = n >= 52;
    }

    /// Enable/disable the half-res volumetric cloud pass + composite (default on). Used by the sky
    /// conformance diagnostics to isolate the pure haze from the cloud overlay.
    pub fn set_clouds_enabled(&mut self, on: bool) {
        self.clouds_enabled = on;
    }

    /// #3: (re)create the G-buffer attachments on size change.
    fn ensure_gbuffer(&mut self, device: &wgpu::Device, w: u32, h: u32) {
        let need = match &self.gbuf_albedo {
            Some((_, gw, gh)) => *gw != w || *gh != h,
            None => true,
        };
        if need {
            let mk = |fmt: wgpu::TextureFormat, label: &str| {
                device
                    .create_texture(&wgpu::TextureDescriptor {
                        label: Some(label),
                        size: wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: 1 },
                        mip_level_count: 1,
                        sample_count: 1,
                        dimension: wgpu::TextureDimension::D2,
                        format: fmt,
                        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
                        view_formats: &[],
                    })
                    .create_view(&wgpu::TextureViewDescriptor::default())
            };
            self.gbuf_albedo = Some((mk(GBUF_ALBEDO_FORMAT, "gbuf-albedo"), w, h));
            self.gbuf_normal = Some((mk(GBUF_NORMAL_FORMAT, "gbuf-normal"), w, h));
            self.gbuf_spec = Some((mk(GBUF_SPEC_FORMAT, "gbuf-spec"), w, h));
            self.gbuf_emissive = Some((mk(GBUF_EMISSIVE_FORMAT, "gbuf-emissive"), w, h));
            self.resolve_bind = None; // rebind against the new G-buffer views
        }
    }

    /// #3: the deferred G-buffer fill pipeline (live_gb.vert + live_gb.frag). Reuses the
    /// generic bind layout; 4 vertex buffers (pos, uv, color, normal); writes [albedo, normal].
    fn ensure_pipeline_gb(&mut self, device: &wgpu::Device) {
        if self.gb_pipeline.is_some() {
            return;
        }
        let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/live_gb.vert.spv"));
        let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/live_gb.frag.spv"));
        let vlayouts = [
            wgpu::VertexBufferLayout { array_stride: 16, step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 0, format: wgpu::VertexFormat::Float32x4 }] },
            wgpu::VertexBufferLayout { array_stride: 8, step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 1, format: wgpu::VertexFormat::Float32x2 }] },
            wgpu::VertexBufferLayout { array_stride: 4, step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 2, format: wgpu::VertexFormat::Unorm8x4 }] },
            wgpu::VertexBufferLayout { array_stride: 16, step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 3, format: wgpu::VertexFormat::Float32x3 }] },
        ];
        self.gb_pipeline = Some(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("gbuffer"),
            layout: Some(&self.layout),
            vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &vlayouts },
            fragment: Some(wgpu::FragmentState {
                module: &fs,
                entry_point: "main",
                targets: &[
                    Some(wgpu::ColorTargetState { format: GBUF_ALBEDO_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                    Some(wgpu::ColorTargetState { format: GBUF_NORMAL_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                    Some(wgpu::ColorTargetState { format: GBUF_SPEC_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                    Some(wgpu::ColorTargetState { format: GBUF_EMISSIVE_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL }),
                ],
            }),
            primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: wgpu::TextureFormat::Depth32Float,
                depth_write_enabled: true,
                depth_compare: wgpu::CompareFunction::GreaterEqual,
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        }));
    }

    /// P3: (re)create the reflection-probe resources (radiance + irradiance cube arrays, brdfLut) and seed
    /// the ReflectionProbes + ProbeParams UBOs with a single neutral DEFAULT probe. Lazy; recreated on a
    /// count/res change. Slice 1 seeds a neutral grey (no black ambient regression); S2/S3/S4 fill the
    /// arrays with the captured+convolved sky and the real per-frame packing.
    fn ensure_probes(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) {
        if self.probe_radiance.is_none() {
            let res = self.probe_res;
            let mips = probe_mip_count(res);
            let rad_cubes = self.probe_count + 2; // +2 scratch layers (realtime / spare)
            // Neutral grey sky-ambient seed so the probe path produces sensible ambient before capture.
            let seed = [0.20f32, 0.22, 0.26];
            // S3: radiance is now MIPPED (7 mips for 128) -- the resolve reads it via textureLod up to
            // max_probe_lod, and the radiance prefilter renders one mip per roughness level. Irradiance stays
            // single-mip at 16. probe_scratch (1 cube, same mip chain) holds the gaussian+downsampled capture
            // the gens sample; separate texture so the prefilter can read it while writing radiance.
            let (rad_tex, rad_view) = cube_array_seeded_mipped(device, queue, res, rad_cubes, mips, seed, "probe-radiance");
            let (irr_tex, irr_view) = cube_array_filled(device, queue, PROBE_IRRADIANCE_RES, self.probe_count, seed, "probe-irradiance");
            let scratch = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("probe-scratch"),
                size: wgpu::Extent3d { width: res, height: res, depth_or_array_layers: 6 },
                mip_level_count: mips, sample_count: 1, dimension: wgpu::TextureDimension::D2,
                format: PROBE_CUBE_FORMAT,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            });
            self.probe_scratch_view = Some(scratch.create_view(&wgpu::TextureViewDescriptor {
                dimension: Some(wgpu::TextureViewDimension::CubeArray), ..Default::default() }));
            self.probe_scratch = Some(scratch);
            self.probe_radiance = Some(rad_tex);
            self.probe_radiance_view = Some(rad_view);
            self.probe_irradiance = Some(irr_tex);
            self.probe_irradiance_view = Some(irr_view);

            // brdfLut (split-sum env BRDF), CPU-generated once.
            let brdf = device.create_texture_with_data(
                queue,
                &wgpu::TextureDescriptor {
                    label: Some("brdf-lut"),
                    size: wgpu::Extent3d { width: 64, height: 64, depth_or_array_layers: 1 },
                    mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
                    format: wgpu::TextureFormat::Rgba16Float, usage: wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
                },
                wgpu::util::TextureDataOrder::LayerMajor,
                &brdf_lut_data(64),
            );
            self.brdf_lut_view = Some(brdf.create_view(&wgpu::TextureViewDescriptor::default()));

            // Seed the ReflectionProbes UBO: one automatic default probe (layer 0), large radius (parallax
            // off), unit scales, refmapCount=1. refBucket stays 0 -> getStartIndex=1, loop empty, probe 0
            // appended. S4 replaces this with the real per-frame view-space packing.
            let mut rp = vec![0u8; RP_UBO_SIZE as usize];
            put_vec4(&mut rp, RP_OFF_REFSPHERE, [0.0, 0.0, -1.0, 4096.0]);
            put_vec4(&mut rp, RP_OFF_REFPARAMS, [1.0, 1.0, 1.0, 0.1]); // irr scale, rad scale, weight coeff, znear
            put_ivec4(&mut rp, RP_OFF_REFINDEX, [0, -1, 0, 0]);        // layer 0, no neighbors, priority 0
            rp[RP_OFF_REFMAPCOUNT..RP_OFF_REFMAPCOUNT + 4].copy_from_slice(&1i32.to_le_bytes());
            queue.write_buffer(&self.probe_ubo, 0, &rp);

            // ProbeParams: identity for slice 1 (grey cubes + parallax-off default probe don't need the
            // real projection). S4 supplies the reverse-Z inv_proj + view->world env_mat. z-conv=Vulkan.
            let max_lod = (res as f32).log2() - 1.0;
            queue.write_buffer(&self.probe_params_ubo, 0, &probe_params_bytes(
                &glam::Mat4::IDENTITY, &glam::Mat4::IDENTITY, max_lod, /*cube_snapshot=*/0.0, /*zconv_vulkan=*/1.0));

            self.resolve_bind = None; // rebind resolve against the new probe resources
            self.probe_captured = false; // fresh arrays -> re-capture the sky
            self.probe_convolved = false; // ...and re-convolve into radiance/irradiance slot 0
        }
    }

    /// Test/debug: the scratch cube (1 cube, mip chain) the capture fills -- read layer f, mip 0 to confirm a
    /// face holds the captured sky/terrain/water (not the grey seed).
    pub fn probe_scratch_debug(&self) -> Option<&wgpu::Texture> {
        self.probe_scratch.as_ref()
    }

    /// Test/debug: the convolved DEFAULT-probe outputs (radiance array, irradiance array). Slot 0 = layers
    /// 0..5 -- read to confirm convolve_probe wrote real convolved sky (not the grey seed) that the resolve reads.
    pub fn probe_outputs_debug(&self) -> Option<(&wgpu::Texture, &wgpu::Texture)> {
        match (self.probe_radiance.as_ref(), self.probe_irradiance.as_ref()) {
            (Some(r), Some(i)) => Some((r, i)),
            _ => None,
        }
    }

    /// S3: (re)create the probe-convolution pipelines -- gaussian pre-blur + reflection-mip downsample (both
    /// fullscreen fsq_uv.vert 2D passes) and the radiance-prefilter + irradiance-convolve generators (gen.vert
    /// clip-quad + the P2 GGX/cosine frags). All render into PROBE_CUBE_FORMAT. Also seeds the two static
    /// gaussian param slots (H = dir(1,0), V = dir(0,1); resScale = 1/(probeRes*2)) and the gen vertex quad.
    fn ensure_probe_convolve_pipelines(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) {
        if self.gauss_pipeline.is_some() {
            return;
        }
        let tex2d = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding, visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
            count: None,
        };
        let smp = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding, visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering), count: None,
        };
        let dyn_ubo = |binding: u32, vis: wgpu::ShaderStages| wgpu::BindGroupLayoutEntry {
            binding, visibility: vis,
            ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: true, min_binding_size: None },
            count: None,
        };

        // gaussian: texture2D + sampler + dynamic GaussParams
        let gauss_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("gauss-bgl"), entries: &[tex2d(0), smp(1), dyn_ubo(2, wgpu::ShaderStages::FRAGMENT)],
        });
        // reflection-mip: texture2D + sampler
        let refmip_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("refmip-bgl"), entries: &[tex2d(0), smp(1)],
        });
        // gen: textureCubeArray + sampler + dynamic GenParams (vertex reads modelview, fragment the rest)
        let gen_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("gen-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::CubeArray, multisampled: false },
                    count: None,
                },
                smp(1),
                dyn_ubo(2, wgpu::ShaderStages::VERTEX_FRAGMENT),
            ],
        });

        let fsq = device.create_shader_module(wgpu::include_spirv!("../shaders/fsq_uv.vert.spv"));
        let gauss_fs = device.create_shader_module(wgpu::include_spirv!("../shaders/gaussian.frag.spv"));
        let refmip_fs = device.create_shader_module(wgpu::include_spirv!("../shaders/reflection_mip.frag.spv"));
        let gen_vs = device.create_shader_module(wgpu::include_spirv!("../shaders/gen.vert.spv"));
        let rad_fs = device.create_shader_module(wgpu::include_spirv!("../shaders/radiance_gen.frag.spv"));
        let irr_fs = device.create_shader_module(wgpu::include_spirv!("../shaders/irradiance_gen.frag.spv"));

        let color_target = |blend| [Some(wgpu::ColorTargetState { format: PROBE_CUBE_FORMAT, blend, write_mask: wgpu::ColorWrites::ALL })];
        let fs_pipe = |label: &str, bgl: &wgpu::BindGroupLayout, fs: &wgpu::ShaderModule| {
            let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some(label), bind_group_layouts: &[bgl], push_constant_ranges: &[] });
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label), layout: Some(&layout),
                vertex: wgpu::VertexState { module: &fsq, entry_point: "main", buffers: &[] },
                fragment: Some(wgpu::FragmentState { module: fs, entry_point: "main", targets: &color_target(None) }),
                primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
                depth_stencil: None, multisample: wgpu::MultisampleState::default(), multiview: None,
            })
        };
        self.gauss_pipeline = Some(fs_pipe("gauss", &gauss_bgl, &gauss_fs));
        self.refmip_pipeline = Some(fs_pipe("refmip", &refmip_bgl, &refmip_fs));

        // gen pipelines: clip-space quad (4-vert tri-strip, Float32x3 position at location 0)
        let gen_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some("gen-pl"), bind_group_layouts: &[&gen_bgl], push_constant_ranges: &[] });
        let gen_vb = [wgpu::VertexBufferLayout {
            array_stride: 12, step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &[wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 0, shader_location: 0 }],
        }];
        let gen_pipe = |label: &str, fs: &wgpu::ShaderModule| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label), layout: Some(&gen_layout),
                vertex: wgpu::VertexState { module: &gen_vs, entry_point: "main", buffers: &gen_vb },
                fragment: Some(wgpu::FragmentState { module: fs, entry_point: "main", targets: &color_target(None) }),
                primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleStrip, cull_mode: None, ..Default::default() },
                depth_stencil: None, multisample: wgpu::MultisampleState::default(), multiview: None,
            })
        };
        self.radiance_gen_pipeline = Some(gen_pipe("radiance-gen", &rad_fs));
        self.irradiance_gen_pipeline = Some(gen_pipe("irradiance-gen", &irr_fs));

        // gen vertex quad (clip space, z = -1 as radianceGenV expects); drawn as a 4-vert tri-strip.
        let verts: [f32; 12] = [-1.0, -1.0, -1.0,  1.0, -1.0, -1.0,  -1.0, 1.0, -1.0,  1.0, 1.0, -1.0];
        self.gen_vbuf = Some(wgpu::util::DeviceExt::create_buffer_init(device, &wgpu::util::BufferInitDescriptor {
            label: Some("gen-vbuf"), contents: bytemuck::cast_slice(&verts), usage: wgpu::BufferUsages::VERTEX,
        }));

        // static gaussian params: resScale = 1/(probeRes*2); slot 0 = horizontal, slot 1 = vertical.
        let res_scale = 1.0f32 / (self.probe_res as f32 * 2.0);
        let mut g = vec![0u8; (GAUSS_SLOT * 2) as usize];
        put_vec4(&mut g, 0, [1.0, 0.0, res_scale, 0.0]);
        put_vec4(&mut g, GAUSS_SLOT as usize, [0.0, 1.0, res_scale, 0.0]);
        queue.write_buffer(&self.gauss_params_ubo, 0, &g);

        self.gauss_bgl = Some(gauss_bgl);
        self.refmip_bgl = Some(refmip_bgl);
        self.gen_bgl = Some(gen_bgl);
    }

    /// S2-terrain: (re)create the probe-capture mini-frame targets -- a supersample scene target, matching
    /// G-buffer (RT0-3) + depth, the face resolve bind, plus the S3 gaussian ping-pong tmp and reflection-mip
    /// chain targets (probeRes>>i). Lazy.
    fn ensure_probe_capture_targets(&mut self, device: &wgpu::Device) {
        // Capture at the supersample resolution (stock mRenderTarget = probeRes*4); the gaussian pre-blur +
        // reflection-mip reduce it to the probeRes mip 0. G-buffer + depth + scene all at cap_res.
        let cap_res = self.probe_res * CAP_SUPER;
        let mk = |fmt: wgpu::TextureFormat, usage: wgpu::TextureUsages, label: &str| {
            device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label), size: wgpu::Extent3d { width: cap_res, height: cap_res, depth_or_array_layers: 1 },
                mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format: fmt, usage, view_formats: &[],
            }).create_view(&wgpu::TextureViewDescriptor::default())
        };
        if self.probe_cap_scene.is_none() {
            // cap_scene is both a render target (sky/terrain/water/clouds, and the gaussian-V dest) and a
            // sampled source (gaussian-H, and the mip-0 reduction) -- TEXTURE_BINDING for the resamples.
            self.probe_cap_scene = Some(device.create_texture(&wgpu::TextureDescriptor {
                label: Some("probe-cap-scene"), size: wgpu::Extent3d { width: cap_res, height: cap_res, depth_or_array_layers: 1 },
                mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format: SCENE_FORMAT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
            }));
        }
        if self.probe_cap_gbuf.is_none() {
            let att = wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING;
            self.probe_cap_gbuf = Some([
                mk(GBUF_ALBEDO_FORMAT, att, "probe-cap-gb0"),
                mk(GBUF_NORMAL_FORMAT, att, "probe-cap-gb1"),
                mk(GBUF_SPEC_FORMAT, att, "probe-cap-gb2"),
                mk(GBUF_EMISSIVE_FORMAT, att, "probe-cap-gb3"),
            ]);
        }
        if self.probe_cap_depth.is_none() {
            self.probe_cap_depth = Some(mk(wgpu::TextureFormat::Depth32Float, wgpu::TextureUsages::RENDER_ATTACHMENT, "probe-cap-depth"));
        }
        // S3: gaussian ping-pong tmp (cap_res) + the reflection-mip chain targets (probeRes>>i, i=0..mips-1).
        if self.probe_gauss_tmp.is_none() {
            self.probe_gauss_tmp = Some(device.create_texture(&wgpu::TextureDescriptor {
                label: Some("probe-gauss-tmp"), size: wgpu::Extent3d { width: cap_res, height: cap_res, depth_or_array_layers: 1 },
                mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format: SCENE_FORMAT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
            }).create_view(&wgpu::TextureViewDescriptor::default()));
        }
        if self.probe_scratch2d.is_empty() {
            let mips = probe_mip_count(self.probe_res);
            for i in 0..mips {
                let r = (self.probe_res >> i).max(1);
                let t = device.create_texture(&wgpu::TextureDescriptor {
                    label: Some("probe-scratch2d"), size: wgpu::Extent3d { width: r, height: r, depth_or_array_layers: 1 },
                    mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format: PROBE_CUBE_FORMAT,
                    usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_SRC,
                    view_formats: &[],
                });
                let v = t.create_view(&wgpu::TextureViewDescriptor::default());
                self.probe_scratch2d.push((t, v, r));
            }
        }
        if self.probe_cap_resolve_bind.is_none() {
            let Self {
                resolve_bgl, probe_cap_ubo, probe_cap_gbuf, probe_depth_view, probe_ubo,
                probe_radiance_view, probe_irradiance_view, probe_sampler, probe_params_ubo,
                brdf_lut_view, probe_cap_resolve_bind, ..
            } = self;
            if let (Some(gb), Some(depth), Some(rad), Some(irr), Some(brdf)) = (
                probe_cap_gbuf.as_ref(), probe_depth_view.as_ref(), probe_radiance_view.as_ref(),
                probe_irradiance_view.as_ref(), brdf_lut_view.as_ref(),
            ) {
                *probe_cap_resolve_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("probe-cap-resolve-bind"),
                    layout: resolve_bgl,
                    entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: probe_cap_ubo.as_entire_binding() },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&gb[0]) },
                        wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(&gb[1]) },
                        wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::TextureView(&gb[2]) },
                        wgpu::BindGroupEntry { binding: 4, resource: wgpu::BindingResource::TextureView(&gb[3]) },
                        wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::TextureView(depth) },
                        wgpu::BindGroupEntry { binding: 6, resource: probe_ubo.as_entire_binding() },
                        wgpu::BindGroupEntry { binding: 7, resource: wgpu::BindingResource::TextureView(rad) },
                        wgpu::BindGroupEntry { binding: 8, resource: wgpu::BindingResource::TextureView(irr) },
                        wgpu::BindGroupEntry { binding: 9, resource: wgpu::BindingResource::Sampler(probe_sampler) },
                        wgpu::BindGroupEntry { binding: 10, resource: probe_params_ubo.as_entire_binding() },
                        wgpu::BindGroupEntry { binding: 11, resource: wgpu::BindingResource::TextureView(brdf) },
                    ],
                }));
            }
        }
    }

    /// S2/S3: capture the full sky ENVIRONMENT (sky + deferred terrain + water + volumetric clouds) into the 6
    /// faces of probe_scratch, one face per 90deg-FOV camera -- a mini deferred-frame per face (stock's
    /// display_cube_face) rendered at the CAP_SUPER supersample, then gaussian-pre-blurred and reduced through
    /// the reflection-mip chain into the scratch cube's mip levels (stock's updateProbeFace downsample). The
    /// deferred resolve samples the probe cubes, so the mini-frame renders into a scene target first. Captured
    /// once per (re)allocation; convolve_probe then GGX-prefilters/convolves the scratch into radiance +
    /// irradiance slot 0.
    fn capture_probe_environment(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) {
        if self.probe_captured || !self.sky_fs_enabled || self.probe_radiance.is_none() {
            return;
        }
        self.ensure_sky_fs(device);
        if self.probe_cap_bind.is_none() {
            self.probe_cap_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("probe-cap-bind"),
                layout: &self.sky_fs_bgl,
                entries: &[wgpu::BindGroupEntry { binding: 0, resource: self.probe_cap_ubo.as_entire_binding() }],
            }));
        }
        // Lazy half-res cloud target (half the capture res) + its composite bind (mirrors the main path).
        if self.probe_cap_cloud.is_none() {
            let half = (self.probe_res * CAP_SUPER / 2).max(1);
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("probe-cap-cloud"),
                size: wgpu::Extent3d { width: half, height: half, depth_or_array_layers: 1 },
                mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            self.probe_cap_cloud = Some((t.create_view(&wgpu::TextureViewDescriptor::default()), half, half));
        }
        if self.probe_cap_cloud_comp_bind.is_none() {
            let Self { probe_cap_cloud, cloud_comp_bgl, cloud_sampler, probe_cap_cloud_comp_bind, .. } = self;
            if let Some((cv, _, _)) = probe_cap_cloud.as_ref() {
                *probe_cap_cloud_comp_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("probe-cap-cloud-comp-bind"),
                    layout: cloud_comp_bgl,
                    entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(cv) },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(cloud_sampler) },
                    ],
                }));
            }
        }

        self.ensure_probe_capture_targets(device);
        self.ensure_probe_convolve_pipelines(device, queue); // S3: gaussian/mip/gen pipelines (before the immutable borrows below)

        // Collect the queued water draws (self-contained forward pass) + build a capture bind per sw_key
        // (binding 0 = the capture water ring w/ dynamic offset; 1/2 = the draw's bump maps).
        struct WaterCap { vptr: usize, iptr: Option<usize>, icount: u32, first: u32, vcount: u32, v_off: u32, ubo_off: u32, sw_key: [u32; 2] }
        let mut waters: Vec<WaterCap> = Vec::new();
        for q in &self.queued {
            if q.draw_class == CLASS_WATER && (waters.len() as u64) < CAP_MAX_WATER {
                waters.push(WaterCap {
                    vptr: q.vptr, iptr: q.iptr, icount: q.icount, first: q.first, vcount: q.vcount,
                    v_off: q.voffsets[TYPE_VERTEX], ubo_off: q.ubo_off, sw_key: [q.terrain_key[0], q.terrain_key[1]],
                });
            }
        }
        if !waters.is_empty() {
            self.ensure_skywater_pipelines(device);
            let keys: Vec<[u32; 2]> = waters.iter().map(|w| w.sw_key).collect();
            for k in keys {
                if !self.probe_cap_water_binds.contains_key(&k) {
                    let Self { skywater_bgl, probe_cap_water_ubo, textures, white, sampler, probe_cap_water_binds, .. } = self;
                    let t0 = textures.get(&k[0]).map(|(_, v)| v).unwrap_or(white);
                    let t1 = textures.get(&k[1]).map(|(_, v)| v).unwrap_or(white);
                    probe_cap_water_binds.insert(k, device.create_bind_group(&wgpu::BindGroupDescriptor {
                        label: Some("probe-cap-water-bind"),
                        layout: skywater_bgl,
                        entries: &[
                            wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding { buffer: probe_cap_water_ubo, offset: 0, size: std::num::NonZeroU64::new(256) }) },
                            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(t0) },
                            wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(t1) },
                            wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::Sampler(sampler) },
                        ],
                    }));
                }
            }
        }

        let (Some(sky_pipe), Some(bind), Some(cap_scene)) = (
            self.sky_fs_pipeline.as_ref(), self.probe_cap_bind.as_ref(),
            self.probe_cap_scene.as_ref(),
        ) else { return; };
        let water_pipe = self.water_pipeline.as_ref();
        let cap_depth = self.probe_cap_depth.as_ref();
        let cap_scene_view = cap_scene.create_view(&wgpu::TextureViewDescriptor::default());
        // S3 resample binds (owned; textures reused per face). H reads cap_scene, V reads gauss_tmp (dyn offsets
        // 0 / GAUSS_SLOT); the mip chain reads cap_scene for mip 0 then scratch2d[i-1] for i>0. All -> probe_sampler.
        let convolve_binds: Option<(wgpu::BindGroup, wgpu::BindGroup, Vec<wgpu::BindGroup>)> =
            if self.gauss_pipeline.is_some() && self.probe_gauss_tmp.is_some() && !self.probe_scratch2d.is_empty() {
                let gbgl = self.gauss_bgl.as_ref().unwrap();
                let rbgl = self.refmip_bgl.as_ref().unwrap();
                let gtmp = self.probe_gauss_tmp.as_ref().unwrap();
                let gauss_ubo = |off: u64| wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                    buffer: &self.gauss_params_ubo, offset: off, size: std::num::NonZeroU64::new(16) });
                let g_from_scene = device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("gauss-bind-h"), layout: gbgl, entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&cap_scene_view) },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&self.probe_sampler) },
                        wgpu::BindGroupEntry { binding: 2, resource: gauss_ubo(0) },
                    ],
                });
                let g_from_tmp = device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("gauss-bind-v"), layout: gbgl, entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(gtmp) },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&self.probe_sampler) },
                        wgpu::BindGroupEntry { binding: 2, resource: gauss_ubo(0) },
                    ],
                });
                let mut rm_binds = Vec::with_capacity(self.probe_scratch2d.len());
                for i in 0..self.probe_scratch2d.len() {
                    let src = if i == 0 { &cap_scene_view } else { &self.probe_scratch2d[i - 1].1 };
                    rm_binds.push(device.create_bind_group(&wgpu::BindGroupDescriptor {
                        label: Some("refmip-bind"), layout: rbgl, entries: &[
                            wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(src) },
                            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&self.probe_sampler) },
                        ],
                    }));
                }
                Some((g_from_scene, g_from_tmp, rm_binds))
            } else { None };
        // Clouds optional (may be disabled); capture only if the whole path is present.
        let clouds = match (self.cloud_pipeline.as_ref(), self.cloud_comp_pipeline.as_ref(),
                            self.probe_cap_cloud.as_ref(), self.probe_cap_cloud_comp_bind.as_ref()) {
            (Some(cp), Some(ccp), Some((cv, _, _)), Some(ccb)) => Some((cp, ccp, cv, ccb)),
            _ => None,
        };
        // Terrain optional (deferred: G-buffer fill -> resolve). Select legacy or PBR splat like the main pass.
        let terrain = if self.terrain_index_count > 0 {
            let use_pbr = self.terrain_is_pbr && self.pbr_gb_pipeline.is_some() && self.pbr_gb_bind.is_some();
            let (tp, tb) = if use_pbr { (self.pbr_gb_pipeline.as_ref(), self.pbr_gb_bind.as_ref()) }
                           else { (self.terrain_gb_pipeline.as_ref(), self.terrain_gb_bind.as_ref()) };
            match (tp, tb, self.terrain_vbuf.as_ref(), self.terrain_ibuf.as_ref(),
                   self.probe_cap_gbuf.as_ref(), self.probe_cap_depth.as_ref(),
                   self.probe_cap_resolve_bind.as_ref(), self.resolve_pipeline.as_ref()) {
                (Some(tp), Some(tb), Some(vb), Some(ib), Some(gb), Some(dep), Some(rb), Some(rp)) =>
                    Some((tp, tb, vb, ib, gb, dep, rb, rp)),
                _ => None,
            }
        } else { None };
        let had_terrain = terrain.is_some();
        let terrain_indices = self.terrain_index_count;

        let wl = self.sky_fs_data; // current WL params (copy); we swap only the camera per face
        let eye = glam::Vec3::new(wl[16], wl[17], wl[18]);
        // Standard cube-map face basis (+X,-X,+Y,-Y,+Z,-Z), matching wgpu/Vulkan cube sampling.
        let faces: [(glam::Vec3, glam::Vec3); 6] = [
            (glam::Vec3::X, glam::Vec3::NEG_Y),
            (glam::Vec3::NEG_X, glam::Vec3::NEG_Y),
            (glam::Vec3::Y, glam::Vec3::Z),
            (glam::Vec3::NEG_Y, glam::Vec3::NEG_Z),
            (glam::Vec3::Z, glam::Vec3::NEG_Y),
            (glam::Vec3::NEG_Z, glam::Vec3::NEG_Y),
        ];
        let proj = glam::Mat4::perspective_rh(std::f32::consts::FRAC_PI_2, 1.0, 0.5, 2000.0);
        // reverse-Z terrain view_proj remap (matches scene.rs terrain_ubo).
        let rev = glam::Mat4::from_cols(
            glam::Vec4::new(1.0, 0.0, 0.0, 0.0), glam::Vec4::new(0.0, 1.0, 0.0, 0.0),
            glam::Vec4::new(0.0, 0.0, -1.0, 0.0), glam::Vec4::new(0.0, 0.0, 1.0, 1.0));
        let proj_t = glam::Mat4::perspective_rh(std::f32::consts::FRAC_PI_2, 1.0, 0.5, 8192.0);
        let main_terrain = self.terrain_ubo_cache.get();
        for (f, (dir, up)) in faces.iter().enumerate() {
            let view = glam::Mat4::look_at_rh(eye, eye + *dir, *up);
            let mut ubo = wl;
            ubo[0..16].copy_from_slice(&(proj * view).inverse().to_cols_array()); // face inv_view_proj (sky)
            queue.write_buffer(&self.probe_cap_ubo, 0, bytemuck::cast_slice(&ubo));
            if had_terrain {
                let mut tu = main_terrain;
                tu[0..16].copy_from_slice(&((rev * proj_t) * view).to_cols_array()); // face reverse-Z view_proj
                queue.write_buffer(&self.terrain_typed_ubo, 0, bytemuck::cast_slice(&tu));
            }
            let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("probe-cap-enc") });
            // sky background -> cap_scene (Clear).
            {
                let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("probe-cap-sky"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &cap_scene_view, resolve_target: None,
                        ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
                    })],
                    depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None,
                });
                rp.set_pipeline(sky_pipe);
                rp.set_bind_group(0, bind, &[]);
                rp.draw(0..3, 0..1);
            }
            // terrain (deferred): G-buffer fill -> resolve the lit terrain over the sky in cap_scene (Load;
            // discards where no terrain, so the sky shows through).
            if let Some((tp, tb, vb, ib, gb, dep, rb, rpipe)) = terrain {
                {
                    let ca_load = wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store };
                    let mut gbp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("probe-cap-terrain-gb"),
                        color_attachments: &[
                            Some(wgpu::RenderPassColorAttachment { view: &gb[0], resolve_target: None, ops: ca_load }),
                            Some(wgpu::RenderPassColorAttachment { view: &gb[1], resolve_target: None, ops: ca_load }),
                            Some(wgpu::RenderPassColorAttachment { view: &gb[2], resolve_target: None, ops: ca_load }),
                            Some(wgpu::RenderPassColorAttachment { view: &gb[3], resolve_target: None, ops: ca_load }),
                        ],
                        depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                            view: dep,
                            depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(0.0), store: wgpu::StoreOp::Store }),
                            stencil_ops: None,
                        }),
                        timestamp_writes: None, occlusion_query_set: None,
                    });
                    gbp.set_pipeline(tp);
                    gbp.set_bind_group(0, tb, &[]);
                    gbp.set_vertex_buffer(0, vb.slice(..));
                    gbp.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint32);
                    gbp.draw_indexed(0..terrain_indices, 0, 0..1);
                }
                {
                    let mut rpass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("probe-cap-terrain-resolve"),
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: &cap_scene_view, resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                        })],
                        depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None,
                    });
                    rpass.set_pipeline(rpipe);
                    rpass.set_bind_group(0, rb, &[]);
                    rpass.draw(0..3, 0..1);
                }
            }
            // water (self-contained forward: fog + fresnel tint + waves) -> cap_scene, depth-tested against
            // the captured terrain in cap_depth (so terrain occludes water). Region-space geometry with a
            // per-face reverse-Z mvp; the draw's water params (aux) come from the staged UBO.
            if !waters.is_empty() {
                if let (Some(wp_pipe), Some(cdep)) = (water_pipe, cap_depth) {
                    for (i, w) in waters.iter().enumerate() {
                        let mut block = [0f32; 64];
                        block[..16].copy_from_slice(&((rev * proj_t) * view).to_cols_array());
                        let aux: &[f32] = bytemuck::cast_slice(&self.ubo_stage[w.ubo_off as usize + 64..w.ubo_off as usize + 256]);
                        block[16..64].copy_from_slice(aux);
                        queue.write_buffer(&self.probe_cap_water_ubo, (i as u64) * 256, bytemuck::cast_slice(&block));
                    }
                    let mut wp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("probe-cap-water"),
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: &cap_scene_view, resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                        })],
                        depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                            view: cdep,
                            depth_ops: Some(wgpu::Operations { load: if had_terrain { wgpu::LoadOp::Load } else { wgpu::LoadOp::Clear(0.0) }, store: wgpu::StoreOp::Store }),
                            stencil_ops: None,
                        }),
                        timestamp_writes: None, occlusion_query_set: None,
                    });
                    wp.set_pipeline(wp_pipe);
                    for (i, w) in waters.iter().enumerate() {
                        let (Some(bg), Some((vbuf, _))) = (self.probe_cap_water_binds.get(&w.sw_key), self.geo.get(&w.vptr)) else { continue };
                        wp.set_bind_group(0, bg, &[(i as u32) * 256]);
                        wp.set_vertex_buffer(0, vbuf.slice(w.v_off as u64..));
                        if let Some(ip) = w.iptr {
                            let Some((ib, _)) = self.geo.get(&ip) else { continue };
                            wp.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint16);
                            wp.draw_indexed(w.first..w.first + w.icount, 0, 0..1);
                        } else {
                            wp.draw(w.first..w.first + w.vcount, 0..1);
                        }
                    }
                }
            }
            // clouds -> half-res target, then composite over cap_scene (Load, alpha blend).
            if let Some((cp, ccp, cv, ccb)) = clouds {
                {
                    let mut clp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("probe-cap-cloud"),
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: cv, resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store },
                        })],
                        depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None,
                    });
                    clp.set_pipeline(cp);
                    clp.set_bind_group(0, bind, &[]); // cloud.frag reads the same sky UBO (face camera)
                    clp.draw(0..3, 0..1);
                }
                {
                    let mut cmp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("probe-cap-cloud-comp"),
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: &cap_scene_view, resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                        })],
                        depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None,
                    });
                    cmp.set_pipeline(ccp);
                    cmp.set_bind_group(0, ccb, &[]);
                    cmp.draw(0..3, 0..1);
                }
            }
            // S3 downsample: gaussian pre-blur (H: cap_scene -> gauss_tmp; V: gauss_tmp -> cap_scene) then the
            // reflection-mip chain (scratch2d[0] <- cap_scene @512; scratch2d[i] <- scratch2d[i-1]), and copy
            // each mip into probe_scratch face f. The convolution (radiance/irradiance gen) reads probe_scratch.
            if let (Some((gh, gv, rm_binds)), Some(gp), Some(rmp), Some(scratch_tex), Some(gtmp)) = (
                convolve_binds.as_ref(), self.gauss_pipeline.as_ref(), self.refmip_pipeline.as_ref(),
                self.probe_scratch.as_ref(), self.probe_gauss_tmp.as_ref(),
            ) {
                let blit = |enc: &mut wgpu::CommandEncoder, pipe: &wgpu::RenderPipeline, dst: &wgpu::TextureView, bindg: &wgpu::BindGroup, dyn_off: &[u32]| {
                    let mut p = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("probe-resample"),
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: dst, resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
                        })],
                        depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None,
                    });
                    p.set_pipeline(pipe);
                    p.set_bind_group(0, bindg, dyn_off);
                    p.draw(0..3, 0..1);
                };
                blit(&mut enc, gp, gtmp, gh, &[0]);                        // horizontal -> gauss_tmp
                blit(&mut enc, gp, &cap_scene_view, gv, &[GAUSS_SLOT as u32]); // vertical -> cap_scene
                for i in 0..self.probe_scratch2d.len() {
                    blit(&mut enc, rmp, &self.probe_scratch2d[i].1, &rm_binds[i], &[]); // reduce into scratch2d[i]
                }
                for i in 0..self.probe_scratch2d.len() {
                    let (tex, _, r) = &self.probe_scratch2d[i];
                    enc.copy_texture_to_texture(
                        wgpu::ImageCopyTexture { texture: tex, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
                        wgpu::ImageCopyTexture { texture: scratch_tex, mip_level: i as u32, origin: wgpu::Origin3d { x: 0, y: 0, z: f as u32 }, aspect: wgpu::TextureAspect::All },
                        wgpu::Extent3d { width: *r, height: *r, depth_or_array_layers: 1 });
                }
            }
            queue.submit([enc.finish()]);
        }
        if had_terrain {
            queue.write_buffer(&self.terrain_typed_ubo, 0, bytemuck::cast_slice(&main_terrain)); // restore main view_proj
        }
        self.probe_captured = true;
    }

    /// S3: convolve the captured environment (probe_scratch mip chain) into the DEFAULT probe's radiance +
    /// irradiance (slot 0). Radiance = per-mip GGX prefilter (radianceGenF): mip i, roughness i/max_lod, one
    /// clip-quad per cube face reading the scratch mip chain at the solid-angle LOD. Irradiance = cosine
    /// convolution (irradianceGenF) at res 16, one quad per face. Per-face rotation = look_at(dir,up).inverse()
    /// using the SAME face basis as the capture, so scratch (input) and radiance/irradiance (output) share
    /// orientation. Runs once after capture; the resolve then samples real sky ambient + reflections at slot 0.
    fn convolve_probe(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) {
        if self.probe_convolved || !self.probe_captured {
            return;
        }
        let (Some(scratch_view), Some(rad_tex), Some(irr_tex), Some(rad_pipe), Some(irr_pipe), Some(gen_bgl), Some(vbuf)) = (
            self.probe_scratch_view.as_ref(), self.probe_radiance.as_ref(), self.probe_irradiance.as_ref(),
            self.radiance_gen_pipeline.as_ref(), self.irradiance_gen_pipeline.as_ref(),
            self.gen_bgl.as_ref(), self.gen_vbuf.as_ref(),
        ) else { return; };
        let res = self.probe_res;
        let mips = probe_mip_count(res);
        let max_lod = (res as f32).log2() - 1.0; // mMaxProbeLOD (6 for 128)
        // Same face basis as capture (standard +X,-X,+Y,-Y,+Z,-Z), so input/output cube orientation matches.
        let faces: [(glam::Vec3, glam::Vec3); 6] = [
            (glam::Vec3::X, glam::Vec3::NEG_Y),
            (glam::Vec3::NEG_X, glam::Vec3::NEG_Y),
            (glam::Vec3::Y, glam::Vec3::Z),
            (glam::Vec3::NEG_Y, glam::Vec3::NEG_Z),
            (glam::Vec3::Z, glam::Vec3::NEG_Y),
            (glam::Vec3::NEG_Z, glam::Vec3::NEG_Y),
        ];
        // The face rotation: modelview*(0,0,-1) = dir, modelview*(0,1,0) = up (the gen quad is at z=-1).
        let face_mv = |cf: usize| glam::Mat4::look_at_rh(glam::Vec3::ZERO, faces[cf].0, faces[cf].1).inverse();

        // gen bind: scratch cube (sourceIdx 0) + sampler + GenParams (dynamic offset, 96-byte std140 slot).
        let gen_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("gen-bind"), layout: gen_bgl, entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(scratch_view) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&self.probe_sampler) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                    buffer: &self.gen_params_ubo, offset: 0, size: std::num::NonZeroU64::new(96) }) },
            ],
        });

        // Write all GenParams slots: radiance = 6*mips (mipLevel i), irradiance = 6 (mipLevel unused).
        let put = |slot: u64, mv: &glam::Mat4, mip_level: f32| {
            let mut b = vec![0u8; 96];
            b[0..64].copy_from_slice(bytemuck::cast_slice(&mv.to_cols_array()));
            b[64..68].copy_from_slice(&mip_level.to_le_bytes());
            b[68..72].copy_from_slice(&max_lod.to_le_bytes());
            b[72..76].copy_from_slice(&1.0f32.to_le_bytes());          // probe_strength
            b[76..80].copy_from_slice(&0i32.to_le_bytes());            // sourceIdx = 0 (scratch cube layer 0)
            b[80..84].copy_from_slice(&(res as i32).to_le_bytes());    // u_width (radiance; irradiance hardcodes 64)
            queue.write_buffer(&self.gen_params_ubo, slot * GEN_SLOT, &b);
        };
        for i in 0..mips {
            for cf in 0..6 {
                put((i * 6 + cf as u32) as u64, &face_mv(cf), i as f32);
            }
        }
        for cf in 0..6 {
            put((mips * 6 + cf as u32) as u64, &face_mv(cf), 0.0);
        }

        // Render: radiance mip i / face cf -> rad slot 0 (layers 0..5); irradiance face cf -> irr slot 0.
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("probe-convolve-enc") });
        let face2d = |tex: &wgpu::Texture, mip: u32, layer: u32| tex.create_view(&wgpu::TextureViewDescriptor {
            dimension: Some(wgpu::TextureViewDimension::D2),
            base_mip_level: mip, mip_level_count: Some(1),
            base_array_layer: layer, array_layer_count: Some(1),
            ..Default::default()
        });
        let gen_pass = |enc: &mut wgpu::CommandEncoder, pipe: &wgpu::RenderPipeline, dst: &wgpu::TextureView, slot: u64| {
            let mut p = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("probe-gen"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: dst, resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
                })],
                depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None,
            });
            p.set_pipeline(pipe);
            p.set_bind_group(0, &gen_bind, &[(slot * GEN_SLOT) as u32]);
            p.set_vertex_buffer(0, vbuf.slice(..));
            p.draw(0..4, 0..1);
        };
        for i in 0..mips {
            for cf in 0..6u32 {
                let v = face2d(rad_tex, i, cf); // slot 0 default probe = layers 0..5
                gen_pass(&mut enc, rad_pipe, &v, (i * 6 + cf) as u64);
            }
        }
        for cf in 0..6u32 {
            let v = face2d(irr_tex, 0, cf);
            gen_pass(&mut enc, irr_pipe, &v, (mips * 6 + cf) as u64);
        }
        queue.submit([enc.finish()]);
        self.probe_convolved = true;
    }

    /// #4: the deferred resolve pipeline (fullscreen post.vert + resolve.frag) + its bind
    /// group (Env UBO + the two G-buffer textures).
    fn ensure_resolve(&mut self, device: &wgpu::Device) {
        if self.resolve_pipeline.is_none() {
            let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/post.vert.spv"));
            let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/resolve.frag.spv"));
            let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("resolve-pl"),
                bind_group_layouts: &[&self.resolve_bgl],
                push_constant_ranges: &[],
            });
            self.resolve_pipeline = Some(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("resolve"),
                layout: Some(&layout),
                vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
                fragment: Some(wgpu::FragmentState {
                    module: &fs,
                    entry_point: "main",
                    targets: &[Some(wgpu::ColorTargetState { format: SCENE_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL })],
                }),
                primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            }));
        }
        if self.resolve_bind.is_none() {
            let bgl = &self.resolve_bgl;
            // P2b: the faithful resolve reads the shared 60-float SKY UBO (all WL params) at binding 0,
            // not the old 4-vec4 Env UBO. P3: + probe resources at bindings 5-11 (all must be present).
            let sky = &self.sky_fs_ubo;
            if let (Some((alb, _, _)), Some((nrm, _, _)), Some((spc, _, _)), Some((emi, _, _)),
                    Some(depth), Some(rad), Some(irr), Some(brdf)) = (
                self.gbuf_albedo.as_ref(), self.gbuf_normal.as_ref(), self.gbuf_spec.as_ref(), self.gbuf_emissive.as_ref(),
                self.probe_depth_view.as_ref(), self.probe_radiance_view.as_ref(), self.probe_irradiance_view.as_ref(), self.brdf_lut_view.as_ref(),
            ) {
                let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("resolve-bind"),
                    layout: bgl,
                    entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: sky.as_entire_binding() },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(alb) },
                        wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(nrm) },
                        wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::TextureView(spc) },
                        wgpu::BindGroupEntry { binding: 4, resource: wgpu::BindingResource::TextureView(emi) },
                        wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::TextureView(depth) },
                        wgpu::BindGroupEntry { binding: 6, resource: self.probe_ubo.as_entire_binding() },
                        wgpu::BindGroupEntry { binding: 7, resource: wgpu::BindingResource::TextureView(rad) },
                        wgpu::BindGroupEntry { binding: 8, resource: wgpu::BindingResource::TextureView(irr) },
                        wgpu::BindGroupEntry { binding: 9, resource: wgpu::BindingResource::Sampler(&self.probe_sampler) },
                        wgpu::BindGroupEntry { binding: 10, resource: self.probe_params_ubo.as_entire_binding() },
                        wgpu::BindGroupEntry { binding: 11, resource: wgpu::BindingResource::TextureView(brdf) },
                    ],
                });
                self.resolve_bind = Some(bind);
            }
        }
    }

    /// Phase A.1: the fullscreen sky pipeline (post.vert + sky_fullscreen.frag) + its bind group.
    fn ensure_sky_fs(&mut self, device: &wgpu::Device) {
        if self.sky_fs_pipeline.is_none() {
            let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/post.vert.spv"));
            let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/sky_fullscreen.frag.spv"));
            let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("sky-fs-pl"),
                bind_group_layouts: &[&self.sky_fs_bgl],
                push_constant_ranges: &[],
            });
            self.sky_fs_pipeline = Some(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("sky-fs"),
                layout: Some(&layout),
                vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
                fragment: Some(wgpu::FragmentState {
                    module: &fs,
                    entry_point: "main",
                    targets: &[Some(wgpu::ColorTargetState { format: SCENE_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL })],
                }),
                primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            }));
        }
        if self.sky_fs_bind.is_none() {
            self.sky_fs_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("sky-fs-bind"),
                layout: &self.sky_fs_bgl,
                entries: &[wgpu::BindGroupEntry { binding: 0, resource: self.sky_fs_ubo.as_entire_binding() }],
            }));
        }
    }

    /// Half-res volumetric cloud pass: a cloud_target at (w/2, h/2), the raymarch pipeline (reusing the
    /// sky UBO bind group), and the composite pipeline that alpha-blends the upscaled clouds into scene_hdr.
    fn ensure_cloud(&mut self, device: &wgpu::Device, w: u32, h: u32) {
        let hw = (w / 2).max(1);
        let hh = (h / 2).max(1);
        let need = match &self.cloud_target {
            Some((_, cw, ch)) => *cw != hw || *ch != hh,
            None => true,
        };
        if need {
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("cloud-target"),
                size: wgpu::Extent3d { width: hw, height: hh, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: SCENE_FORMAT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            self.cloud_target = Some((t.create_view(&wgpu::TextureViewDescriptor::default()), hw, hh));
            self.cloud_comp_bind = None;
        }
        if self.cloud_pipeline.is_none() {
            let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/post.vert.spv"));
            let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/cloud.frag.spv"));
            let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("cloud-pl"),
                bind_group_layouts: &[&self.sky_fs_bgl],
                push_constant_ranges: &[],
            });
            self.cloud_pipeline = Some(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("cloud"),
                layout: Some(&layout),
                vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
                fragment: Some(wgpu::FragmentState {
                    module: &fs,
                    entry_point: "main",
                    targets: &[Some(wgpu::ColorTargetState { format: SCENE_FORMAT, blend: None, write_mask: wgpu::ColorWrites::ALL })],
                }),
                primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            }));
        }
        if self.cloud_comp_pipeline.is_none() {
            let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/post.vert.spv"));
            let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/cloud_composite.frag.spv"));
            let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("cloud-comp-pl"),
                bind_group_layouts: &[&self.cloud_comp_bgl],
                push_constant_ranges: &[],
            });
            self.cloud_comp_pipeline = Some(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("cloud-comp"),
                layout: Some(&layout),
                vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
                fragment: Some(wgpu::FragmentState {
                    module: &fs,
                    entry_point: "main",
                    targets: &[Some(wgpu::ColorTargetState {
                        format: SCENE_FORMAT,
                        // out = cloud.rgb*cloud.a + scene_hdr*(1-cloud.a) = mix(scene_hdr, cloud, alpha)
                        blend: Some(wgpu::BlendState {
                            color: wgpu::BlendComponent { src_factor: wgpu::BlendFactor::SrcAlpha, dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha, operation: wgpu::BlendOperation::Add },
                            alpha: wgpu::BlendComponent::REPLACE,
                        }),
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            }));
        }
        if self.cloud_comp_bind.is_none() {
            if let Some((cv, _, _)) = &self.cloud_target {
                self.cloud_comp_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("cloud-comp-bind"),
                    layout: &self.cloud_comp_bgl,
                    entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(cv) },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&self.cloud_sampler) },
                    ],
                }));
            }
        }
    }

    /// Phase A.3: reset the per-frame UI list. Called at frame start (fsr_ui_begin).
    pub fn ui_begin(&mut self) {
        self.ui_verts.clear();
        self.ui_draws.clear();
    }

    /// Phase A.3: record one LLRender::flush as a UI draw. `verts` is interleaved
    /// {vec3 pos, vec2 uv, u8x4 color} (24B/vtx); `mode` is LLRender::eGeomModes. Strips/fans are
    /// expanded to lists so one triangle (or line) pipeline covers the whole frame in painter's
    /// order. `mvp` is the viewer's OWN projection*modelview -- no glGet, no tap.
    pub fn ui_submit(&mut self, mvp: &[f32; 16], tex_id: u32, mode: u32, verts: &[u8]) {
        const STRIDE: usize = 24;
        let n = verts.len() / STRIDE;
        if n == 0 { return; }
        let first_vertex = (self.ui_verts.len() / STRIDE) as u32;
        let line = mode == 4 || mode == 5;
        let dst = &mut self.ui_verts;
        let mut push = |i: usize| { if i < n { dst.extend_from_slice(&verts[i * STRIDE..(i + 1) * STRIDE]); } };
        match mode {
            0 => { for k in (0..n - (n % 3)).step_by(3) { push(k); push(k + 1); push(k + 2); } } // TRIANGLES
            1 => { for k in 2..n { if k % 2 == 0 { push(k - 2); push(k - 1); push(k); } else { push(k - 1); push(k - 2); push(k); } } } // STRIP
            2 => { for k in 1..n.saturating_sub(1) { push(0); push(k); push(k + 1); } } // FAN
            4 => { for k in (0..n - (n % 2)).step_by(2) { push(k); push(k + 1); } } // LINES
            5 => { for k in 1..n { push(k - 1); push(k); } } // LINE_STRIP
            _ => { return; } // POINTS etc: not used by the 2D UI
        }
        let vertex_count = (self.ui_verts.len() / STRIDE) as u32 - first_vertex;
        if vertex_count == 0 { return; }
        self.ui_draws.push(UiDraw { mvp: *mvp, tex_id, first_vertex, vertex_count, line });
    }

    /// Flash diagnostic: dump the LARGE UI draws (screen coverage > 0.3) captured this frame, to
    /// identify a full-screen draw leaking into the overlay via the LLRender::flush hook (the white
    /// flash). Reports each draw's texture id, vertex count, NDC bbox, coverage, and average color.
    pub fn dump_ui_large(&self) -> String {
        const STRIDE: usize = 24;
        let mut out = String::new();
        for (i, d) in self.ui_draws.iter().enumerate() {
            let mvp = glam::Mat4::from_cols_array(&d.mvp);
            let (mut minx, mut maxx, mut miny, mut maxy) = (f32::MAX, f32::MIN, f32::MAX, f32::MIN);
            let (mut cr, mut cg, mut cb, mut ca) = (0u32, 0u32, 0u32, 0u32);
            let base = d.first_vertex as usize * STRIDE;
            let n = d.vertex_count as usize;
            for v in 0..n {
                let o = base + v * STRIDE;
                if o + 24 > self.ui_verts.len() { break; }
                let px = f32::from_le_bytes([self.ui_verts[o], self.ui_verts[o + 1], self.ui_verts[o + 2], self.ui_verts[o + 3]]);
                let py = f32::from_le_bytes([self.ui_verts[o + 4], self.ui_verts[o + 5], self.ui_verts[o + 6], self.ui_verts[o + 7]]);
                let pz = f32::from_le_bytes([self.ui_verts[o + 8], self.ui_verts[o + 9], self.ui_verts[o + 10], self.ui_verts[o + 11]]);
                let clip = mvp * glam::Vec4::new(px, py, pz, 1.0);
                let (nx, ny) = if clip.w.abs() > 1e-6 { (clip.x / clip.w, clip.y / clip.w) } else { (clip.x, clip.y) };
                minx = minx.min(nx); maxx = maxx.max(nx); miny = miny.min(ny); maxy = maxy.max(ny);
                cr += self.ui_verts[o + 20] as u32; cg += self.ui_verts[o + 21] as u32; cb += self.ui_verts[o + 22] as u32; ca += self.ui_verts[o + 23] as u32;
            }
            let cov = ((maxx - minx).max(0.0) * (maxy - miny).max(0.0)) / 4.0;
            if cov > 0.3 && n > 0 {
                let nn = n as u32;
                out.push_str(&format!("  [{}] tex={} nv={} ndc=({:.2},{:.2})..({:.2},{:.2}) cov={:.2} avgcol=({},{},{},{})\n",
                    i, d.tex_id, n, minx, miny, maxx, maxy, cov, cr / nn, cg / nn, cb / nn, ca / nn));
            }
        }
        if out.is_empty() { out.push_str("  (no large UI draws)\n"); }
        out
    }

    /// Phase A.3: lazily build the UI bind-group layout, sampler, and the two pipelines
    /// (triangle-list + line-list), both targeting the swapchain format with straight-alpha blend.
    fn ensure_ui(&mut self, device: &wgpu::Device) {
        if self.ui_bgl.is_none() {
            self.ui_bgl = Some(device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("ui-bgl"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::VERTEX,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: true,
                            min_binding_size: wgpu::BufferSize::new(64),
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: true },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                        count: None,
                    },
                ],
            }));
        }
        if self.ui_sampler.is_none() {
            self.ui_sampler = Some(device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some("ui-sampler"),
                address_mode_u: wgpu::AddressMode::ClampToEdge,
                address_mode_v: wgpu::AddressMode::ClampToEdge,
                address_mode_w: wgpu::AddressMode::ClampToEdge,
                mag_filter: wgpu::FilterMode::Linear,
                min_filter: wgpu::FilterMode::Linear,
                mipmap_filter: wgpu::FilterMode::Nearest,
                ..Default::default()
            }));
        }
        if self.ui_tri_pipeline.is_none() {
            let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/ui.vert.spv"));
            let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/ui.frag.spv"));
            let bgl = self.ui_bgl.as_ref().unwrap();
            let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("ui-pl"),
                bind_group_layouts: &[bgl],
                push_constant_ranges: &[],
            });
            let attrs = [
                wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 0, shader_location: 0 },
                wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x2, offset: 12, shader_location: 1 },
                wgpu::VertexAttribute { format: wgpu::VertexFormat::Unorm8x4, offset: 20, shader_location: 2 },
            ];
            let blend = wgpu::BlendState {
                color: wgpu::BlendComponent { src_factor: wgpu::BlendFactor::SrcAlpha, dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha, operation: wgpu::BlendOperation::Add },
                alpha: wgpu::BlendComponent { src_factor: wgpu::BlendFactor::One, dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha, operation: wgpu::BlendOperation::Add },
            };
            let fmt = self.format;
            let mk = |topo: wgpu::PrimitiveTopology, label: &str| device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&layout),
                vertex: wgpu::VertexState {
                    module: &vs,
                    entry_point: "main",
                    buffers: &[wgpu::VertexBufferLayout { array_stride: 24, step_mode: wgpu::VertexStepMode::Vertex, attributes: &attrs }],
                },
                fragment: Some(wgpu::FragmentState {
                    module: &fs,
                    entry_point: "main",
                    targets: &[Some(wgpu::ColorTargetState { format: fmt, blend: Some(blend), write_mask: wgpu::ColorWrites::ALL })],
                }),
                primitive: wgpu::PrimitiveState { topology: topo, cull_mode: None, ..Default::default() },
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            });
            self.ui_tri_pipeline = Some(mk(wgpu::PrimitiveTopology::TriangleList, "ui-tri"));
            self.ui_line_pipeline = Some(mk(wgpu::PrimitiveTopology::LineList, "ui-line"));
        }
    }

    pub fn flush_clear(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        target: &wgpu::TextureView,
        w: u32,
        h: u32,
        clear: wgpu::Color,
    ) -> u64 {
        if !self.ubo_stage.is_empty() {
            self.ensure_ring(device, self.ubo_stage.len() as u64);
            if let Some(ring) = &self.ubo_ring {
                queue.write_buffer(ring, 0, &self.ubo_stage);
            }
        }
        if !self.palette_stage.is_empty() {
            self.ensure_palette_ring(device, self.palette_stage.len() as u64);
            if let Some(pr) = &self.palette_ring {
                queue.write_buffer(pr, 0, &self.palette_stage);
            }
        }
        self.ensure_depth(device, queue, w, h);
        self.ensure_msaa_color(device, w, h);
        self.ensure_scene_hdr(device, w, h);
        self.ensure_cloud(device, w, h); // half-res cloud target + pipelines
        self.ensure_exp(device); // before ensure_post: post_bind references exp_lum's view
        self.ensure_post(device);
        // S1: push the Post-params UBO (std140: 6 floats + 2 ints, 32 bytes) consumed by
        // post_tonemap.frag every frame, before the composite pass runs. exp_min/exp_max (24..32)
        // are the stock exposureF dynamic-exposure bounds (from sky hdr_scale).
        {
            let mut pp = [0u8; 32];
            pp[0..4].copy_from_slice(&self.post_exposure.to_le_bytes());
            pp[4..8].copy_from_slice(&self.post_exp_scale.to_le_bytes());
            pp[8..12].copy_from_slice(&self.post_tonemap_mix.to_le_bytes());
            pp[12..16].copy_from_slice(&self.post_gamma.to_le_bytes());
            pp[16..20].copy_from_slice(&self.post_tonemap_type.to_le_bytes());
            pp[20..24].copy_from_slice(&self.post_legacy_gamma.to_le_bytes());
            pp[24..28].copy_from_slice(&self.post_exp_min.to_le_bytes());
            pp[28..32].copy_from_slice(&self.post_exp_max.to_le_bytes());
            queue.write_buffer(&self.post_ubo, 0, &pp);
        }
        // Phase A.1: the fullscreen sky UBO (parallel-read camera + EepSkyBlock), pushed when set.
        if self.sky_fs_enabled {
            self.ensure_sky_fs(device);
            queue.write_buffer(&self.sky_fs_ubo, 0, bytemuck::cast_slice(&self.sky_fs_data));
        }
        // #3/#4: any opaque generic-with-normal draws this frame -> build the deferred path.
        let has_gb = self.queued.iter().any(|q| q.is_gb);
        // Ground P2: terrain drives the deferred path (G-buffer fill + softenLight resolve).
        let has_terrain = self.terrain_index_count > 0
            && self.terrain_vbuf.is_some()
            && self.terrain_gb_pipeline.is_some()
            && self.terrain_gb_bind.is_some();
        if has_gb || has_terrain {
            self.ensure_gbuffer(device, w, h);
            self.ensure_probes(device, queue); // P3: probe resources exist before the resolve binds them
            self.ensure_resolve(device);       // resolve pipeline needed by the terrain capture (below)
            self.capture_probe_environment(device, queue); // S2: fill probe_scratch (sky + terrain + water + clouds)
            self.convolve_probe(device, queue);            // S3: GGX-prefilter/convolve scratch -> radiance + irradiance slot 0
        }
        if has_gb {
            self.ensure_pipeline_gb(device);
        }
        {
            // Env UBO = sun_dir, sunlight, ambient (frame_env) + bg (the viewer clear, so the
            // resolve can paint the background before the forward pass draws sky over it).
            let mut env = [0f32; 16];
            env[..12].copy_from_slice(&self.frame_env);
            env[12] = clear.r as f32;
            env[13] = clear.g as f32;
            env[14] = clear.b as f32;
            env[15] = clear.a as f32;
            queue.write_buffer(&self.env_ubo, 0, bytemuck::cast_slice(&env));
        }
        let depth_view = match &self.depth {
            Some((v, _, _, _)) => v,
            None => return 0,
        };
        let scene_view = match &self.scene_hdr {
            Some((v, _, _)) => v,
            None => return 0,
        };
        let gb_alb = self.gbuf_albedo.as_ref().map(|(v, _, _)| v);
        let gb_nrm = self.gbuf_normal.as_ref().map(|(v, _, _)| v);
        let gb_spc = self.gbuf_spec.as_ref().map(|(v, _, _)| v);
        let gb_emi = self.gbuf_emissive.as_ref().map(|(v, _, _)| v);
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("live") });
        // #3 PASS 1: deferred G-buffer fill (opaque generic-with-normal draws) into
        // [albedo, normal] + depth. Everything else stays forward (next pass).
        if has_gb {
            if let (Some(alb), Some(nrm), Some(spc), Some(emi), Some(p)) = (gb_alb, gb_nrm, gb_spc, gb_emi, self.gb_pipeline.as_ref()) {
                let mut gp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("gbuffer-pass"),
                    color_attachments: &[
                        Some(wgpu::RenderPassColorAttachment { view: alb, resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store } }),
                        Some(wgpu::RenderPassColorAttachment { view: nrm, resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store } }),
                        Some(wgpu::RenderPassColorAttachment { view: spc, resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store } }),
                        Some(wgpu::RenderPassColorAttachment { view: emi, resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store } }),
                    ],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: depth_view,
                        depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(0.0), store: wgpu::StoreOp::Store }),
                        stencil_ops: None,
                    }),
                    timestamp_writes: None,
                    occlusion_query_set: None,
                });
                for q in &self.queued {
                    if !q.is_gb { continue; }
                    let Some((vbuf, _)) = self.geo.get(&q.vptr) else { continue };
                    let Some(bind) = self.binds.get(&q.bind_key) else { continue };
                    let vo = q.voffsets;
                    gp.set_pipeline(p);
                    gp.set_bind_group(0, bind, &[q.ubo_off, q.pal_off]);
                    gp.set_vertex_buffer(0, vbuf.slice(vo[TYPE_VERTEX] as u64..));
                    if vo[TYPE_TEXCOORD0] != u32::MAX { gp.set_vertex_buffer(1, vbuf.slice(vo[TYPE_TEXCOORD0] as u64..)); } else { gp.set_vertex_buffer(1, self.uv_zero.slice(..)); }
                    if vo[TYPE_COLOR] != u32::MAX { gp.set_vertex_buffer(2, vbuf.slice(vo[TYPE_COLOR] as u64..)); } else { gp.set_vertex_buffer(2, self.color_white.slice(..)); }
                    if vo[TYPE_NORMAL] != u32::MAX { gp.set_vertex_buffer(3, vbuf.slice(vo[TYPE_NORMAL] as u64..)); } else { gp.set_vertex_buffer(3, self.normal_up.slice(..)); }
                    if let Some(ip) = q.iptr {
                        let Some((ib, _)) = self.geo.get(&ip) else { continue };
                        gp.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint16);
                        gp.draw_indexed(q.first..q.first + q.icount, 0, 0..1);
                    } else {
                        gp.draw(q.first..q.first + q.vcount, 0..1);
                    }
                }
            }
        }
        // (the generic deferred resolve moved to AFTER the sky-fs-pass below -- the sky pass does
        //  LoadOp::Clear(BLACK) with no depth test, so running the resolve BEFORE it wiped the lit
        //  generic G-buffer. Terrain already resolves after the sky; this makes generic match.)
        // Phase A.1: fullscreen procedural sky into scene_hdr (the background), parallel-read from
        // the typed camera + EepSkyBlock -- no tap, no dome. The forward pass then LOADs scene_hdr
        // so the sky survives. (Phase B reorders this as the true background under geometry; the
        // MSAA-resolve path is single-sample for now.)
        if self.sky_fs_enabled {
            if let (Some(p), Some(bind)) = (self.sky_fs_pipeline.as_ref(), self.sky_fs_bind.as_ref()) {
                let mut sp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("sky-fs-pass"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: scene_view,
                        resolve_target: None,
                        ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
                    })],
                    depth_stencil_attachment: None,
                    timestamp_writes: None,
                    occlusion_query_set: None,
                });
                sp.set_pipeline(p);
                sp.set_bind_group(0, bind, &[]);
                sp.draw(0..3, 0..1);
            }
        }
        // #3/#4 PASS 2: deferred resolve -- light the generic G-buffer into scene_hdr (lit where
        // HAS_ATMOS/HAS_PBR, discard elsewhere so the sky shows through). MUST run AFTER the sky-fs
        // pass (which clears scene_hdr to black) or the sky wipes it; still BEFORE the forward pass
        // so forward geometry draws over it with depth-test.
        if has_gb {
            if let (Some(p), Some(bind)) = (self.resolve_pipeline.as_ref(), self.resolve_bind.as_ref()) {
                let mut rp2 = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("resolve-pass"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: scene_view,
                        resolve_target: None,
                        ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                    })],
                    depth_stencil_attachment: None,
                    timestamp_writes: None,
                    occlusion_query_set: None,
                });
                rp2.set_pipeline(p);
                rp2.set_bind_group(0, bind, &[]);
                rp2.draw(0..3, 0..1);
            }
        }
        {
            let load_scene = has_gb || self.sky_fs_enabled;
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("live-pass"),
                color_attachments: &[Some(match &self.msaa_color {
                    Some((ms_view, _, _, _)) => wgpu::RenderPassColorAttachment {
                        view: ms_view,
                        resolve_target: Some(scene_view), // world MSAA resolves into scene_hdr
                        ops: wgpu::Operations { load: if load_scene { wgpu::LoadOp::Load } else { wgpu::LoadOp::Clear(clear) }, store: wgpu::StoreOp::Store },
                    },
                    None => wgpu::RenderPassColorAttachment {
                        view: scene_view, // #3: LOAD the resolve's output when it ran; else clear
                        resolve_target: None,
                        ops: wgpu::Operations { load: if load_scene { wgpu::LoadOp::Load } else { wgpu::LoadOp::Clear(clear) }, store: wgpu::StoreOp::Store },
                    },
                })],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: depth_view,
                    // #3: when the G-buffer pass ran it already wrote opaque depth -- LOAD it so
                    // forward draws occlude against opaque generic; otherwise clear it here.
                    depth_ops: Some(wgpu::Operations { load: if has_gb { wgpu::LoadOp::Load } else { wgpu::LoadOp::Clear(0.0) }, store: wgpu::StoreOp::Store }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            for q in &self.queued {
                if q.is_gb { continue; } // #3: opaque generic-with-normal is drawn in the G-buffer pass
                let Some((vbuf, _)) = self.geo.get(&q.vptr) else { continue };
                let vo = q.voffsets;
                if q.draw_class == CLASS_SKY_DOME || q.draw_class == CLASS_WATER {
                    let sw_key = [q.terrain_key[0], q.terrain_key[1]];
                    let pipe = if q.draw_class == CLASS_SKY_DOME { self.sky_pipeline.as_ref() } else { self.water_pipeline.as_ref() };
                    let Some(p) = pipe else { continue };
                    let Some(bind) = self.skywater_binds.get(&sw_key) else { continue };
                    rp.set_pipeline(p);
                    rp.set_bind_group(0, bind, &[q.ubo_off]);
                    rp.set_vertex_buffer(0, vbuf.slice(vo[TYPE_VERTEX] as u64..));
                    if q.draw_class == CLASS_SKY_DOME {
                        let uvo = if vo[TYPE_TEXCOORD0] != u32::MAX { vo[TYPE_TEXCOORD0] } else { vo[TYPE_VERTEX] };
                        rp.set_vertex_buffer(1, vbuf.slice(uvo as u64..));
                    }
                    if let Some(ip) = q.iptr {
                        let Some((ib, _)) = self.geo.get(&ip) else { continue };
                        rp.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint16);
                        rp.draw_indexed(q.first..q.first + q.icount, 0, 0..1);
                    } else {
                        rp.draw(q.first..q.first + q.vcount, 0..1);
                    }
                    continue;
                }
                if q.draw_class == CLASS_TERRAIN {
                    let Some(p) = self.terrain_pipeline.as_ref() else { continue };
                    let Some(bind) = self.terrain_binds.get(&q.terrain_key) else { continue };
                    rp.set_pipeline(p);
                    rp.set_bind_group(0, bind, &[q.ubo_off]);
                    rp.set_vertex_buffer(0, vbuf.slice(vo[TYPE_VERTEX] as u64..));
                    let tc1 = if vo[TYPE_TEXCOORD1] != u32::MAX { vo[TYPE_TEXCOORD1] } else { vo[TYPE_VERTEX] };
                    rp.set_vertex_buffer(1, vbuf.slice(tc1 as u64..));
                    if let Some(ip) = q.iptr {
                        let Some((ib, _)) = self.geo.get(&ip) else { continue };
                        rp.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint16);
                        rp.draw_indexed(q.first..q.first + q.icount, 0, 0..1);
                    } else {
                        rp.draw(q.first..q.first + q.vcount, 0..1);
                    }
                    continue;
                }
                let p_opt = if q.blend_add { self.pipelines_add.get(&q.pipe_key) } else { self.pipelines.get(&q.pipe_key) };
                let Some(p) = p_opt else { continue };
                let Some(bind) = self.binds.get(&q.bind_key) else { continue };
                rp.set_pipeline(p);
                rp.set_bind_group(0, bind, &[q.ubo_off, q.pal_off]);
                rp.set_vertex_buffer(0, vbuf.slice(vo[TYPE_VERTEX] as u64..));
                if vo[TYPE_TEXCOORD0] != u32::MAX {
                    rp.set_vertex_buffer(1, vbuf.slice(vo[TYPE_TEXCOORD0] as u64..));
                } else {
                    rp.set_vertex_buffer(1, self.uv_zero.slice(..));
                }
                if vo[TYPE_COLOR] != u32::MAX {
                    rp.set_vertex_buffer(2, vbuf.slice(vo[TYPE_COLOR] as u64..));
                } else {
                    rp.set_vertex_buffer(2, self.color_white.slice(..));
                }
                if q.skinned {
                    let woff = if vo[TYPE_WEIGHT4] != u32::MAX { vo[TYPE_WEIGHT4] } else { vo[TYPE_VERTEX] };
                    rp.set_vertex_buffer(3, vbuf.slice(woff as u64..));
                }
                if let Some(ip) = q.iptr {
                    let Some((ib, _)) = self.geo.get(&ip) else { continue };
                    rp.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint16);
                    rp.draw_indexed(q.first..q.first + q.icount, 0, 0..1);
                } else {
                    rp.draw(q.first..q.first + q.vcount, 0..1);
                }
            }
        }
        // Ground P2: DEFERRED terrain. (1) Fill the G-buffer [albedo+FLAG, world-normal] + depth,
        // then (2) resolve (faithful softenLight) into scene_hdr OVER the sky -- LoadOp::Load keeps
        // the sky, and the resolve discards non-HAS_ATMOS pixels. Replaces the P1 forward pass.
        if has_terrain {
            // PBR-2: select the PBR base-color splat in a PBR region (else the legacy detail splat).
            let use_pbr = self.terrain_is_pbr && self.pbr_gb_pipeline.is_some() && self.pbr_gb_bind.is_some();
            let (sel_pipeline, sel_bind) = if use_pbr {
                (self.pbr_gb_pipeline.as_ref(), self.pbr_gb_bind.as_ref())
            } else {
                (self.terrain_gb_pipeline.as_ref(), self.terrain_gb_bind.as_ref())
            };
            if let (Some(gp), Some(tb), Some(vb), Some(ib), Some(alb), Some(nrm), Some(spc), Some(emi)) = (
                sel_pipeline, sel_bind,
                self.terrain_vbuf.as_ref(), self.terrain_ibuf.as_ref(), gb_alb, gb_nrm, gb_spc, gb_emi,
            ) {
                {
                    let mut gbp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("terrain-gb-pass"),
                        color_attachments: &[
                            Some(wgpu::RenderPassColorAttachment { view: alb, resolve_target: None,
                                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store } }),
                            Some(wgpu::RenderPassColorAttachment { view: nrm, resolve_target: None,
                                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store } }),
                            Some(wgpu::RenderPassColorAttachment { view: spc, resolve_target: None,
                                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store } }),
                            Some(wgpu::RenderPassColorAttachment { view: emi, resolve_target: None,
                                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store } }),
                        ],
                        depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                            view: depth_view,
                            depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(0.0), store: wgpu::StoreOp::Store }),
                            stencil_ops: None,
                        }),
                        timestamp_writes: None,
                        occlusion_query_set: None,
                    });
                    gbp.set_pipeline(gp);
                    gbp.set_bind_group(0, tb, &[]);
                    gbp.set_vertex_buffer(0, vb.slice(..));
                    gbp.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint32);
                    gbp.draw_indexed(0..self.terrain_index_count, 0, 0..1);
                }
                if let (Some(rp3), Some(rbind)) = (self.resolve_pipeline.as_ref(), self.resolve_bind.as_ref()) {
                    let mut rpass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("terrain-resolve-pass"),
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: scene_view,
                            resolve_target: None,
                            ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                        })],
                        depth_stencil_attachment: None,
                        timestamp_writes: None,
                        occlusion_query_set: None,
                    });
                    rpass.set_pipeline(rp3);
                    rpass.set_bind_group(0, rbind, &[]);
                    rpass.draw(0..3, 0..1);
                }
            }
        }
        // Half-res VOLUMETRIC cloud pass: raymarch clouds into cloud_target (1/2 res), then composite
        // (bilinear upscale + alpha blend) over scene_hdr. Runs after sky+world, before the meter, so
        // clouds are in scene_hdr for exposure + tonemap. The upscale dissolves the raymarch grain and
        // the 1/2-res raymarch is ~4x cheaper than full-res.
        if self.clouds_enabled {
        if let (Some(cp), Some(sb), Some((cv, _, _))) = (self.cloud_pipeline.as_ref(), self.sky_fs_bind.as_ref(), self.cloud_target.as_ref()) {
            let mut clp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("cloud-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: cv,
                    resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            clp.set_pipeline(cp);
            clp.set_bind_group(0, sb, &[]);
            clp.draw(0..3, 0..1);
        }
        if let (Some(cp), Some(cb)) = (self.cloud_comp_pipeline.as_ref(), self.cloud_comp_bind.as_ref()) {
            let mut cmp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("cloud-composite-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: scene_view,
                    resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            cmp.set_pipeline(cp);
            cmp.set_bind_group(0, cb, &[]);
            cmp.draw(0..3, 0..1);
        }
        } // clouds_enabled
        // Metered auto-exposure: average scene_hdr luminance into the 1x1 exp_lum, read by the
        // tonemap below to normalize the HDR (so radiance > 1 doesn't hard-clamp to white). Runs
        // after the scene is fully composited (sky + world), before the post pass.
        let mut ran_exp = false;
        if let (Some(p), Some(b), Some(lum)) = (self.exp_pipeline.as_ref(), self.exp_bind.as_ref(), self.exp_lum.as_ref()) {
            // Temporal eye-adaptation: blend the current-frame avg luminance into the persisted 1x1
            // lum. First real frame primes it (rate=1, cleared); after that a small rate damps the
            // exposure so panning through the darkening zenith no longer flickers (~0.5s adaptation).
            let primed = self.exp_primed;
            let rate: f64 = if primed { 0.04 } else { 1.0 };
            let load = if primed { wgpu::LoadOp::Load } else { wgpu::LoadOp::Clear(wgpu::Color::BLACK) };
            let mut mp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("exp-measure-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: lum,
                    resolve_target: None,
                    ops: wgpu::Operations { load, store: wgpu::StoreOp::Store },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            mp.set_pipeline(p);
            mp.set_bind_group(0, b, &[]);
            mp.set_blend_constant(wgpu::Color { r: rate, g: rate, b: rate, a: rate });
            mp.draw(0..3, 0..1);
            ran_exp = true;
        }
        if ran_exp { self.exp_primed = true; }
        // Final composite: tonemap the linear-HDR scene target (PBRNeutral) to the sRGB
        // swapchain. The mandatory HDR->LDR step; the swapchain ROP does the sRGB encode.
        {
            let mut pp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("post-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: target,
                    resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Clear(clear), store: wgpu::StoreOp::Store },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            if let (Some(p), Some(b)) = (self.post_pipeline.as_ref(), self.post_bind.as_ref()) {
                pp.set_pipeline(p);
                pp.set_bind_group(0, b, &[]);
                pp.draw(0..3, 0..1);
            }
        }
        // Phase A.3: native-VK UI pass -- the honest LLRender::flush feed, painter's order, drawn
        // OVER the tonemapped swapchain (LDR; not tonemapped again). Zero tap, zero glGet. This is
        // the whole reason native-vulkan is usable: real menus/text, not a stub.
        if !self.ui_draws.is_empty() {
            self.ensure_ui(device);
            const ALIGN: u64 = 256;
            let vbytes = self.ui_verts.len() as u64;
            let grow_vb = self.ui_vbuf.as_ref().map_or(true, |(_, cap)| *cap < vbytes);
            if grow_vb {
                let cap = vbytes.next_power_of_two().max(64 * 1024);
                self.ui_vbuf = Some((device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("ui-vbuf"), size: cap,
                    usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                }), cap));
            }
            let ubo_need = self.ui_draws.len() as u64 * ALIGN;
            let grow_ubo = self.ui_ubo.as_ref().map_or(true, |(_, cap)| *cap < ubo_need);
            if grow_ubo {
                let cap = ubo_need.next_power_of_two().max(64 * 1024);
                self.ui_ubo = Some((device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("ui-ubo"), size: cap,
                    usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                }), cap));
            }
            let mut ubo_bytes = vec![0u8; self.ui_draws.len() * ALIGN as usize];
            for (i, d) in self.ui_draws.iter().enumerate() {
                let off = i * ALIGN as usize;
                ubo_bytes[off..off + 64].copy_from_slice(bytemuck::cast_slice(&d.mvp));
            }
            let ui_vbuf = &self.ui_vbuf.as_ref().unwrap().0;
            let ui_ubo = &self.ui_ubo.as_ref().unwrap().0;
            queue.write_buffer(ui_vbuf, 0, &self.ui_verts);
            queue.write_buffer(ui_ubo, 0, &ubo_bytes);
            // One bind group per unique texture id; all share the mvp ring (dynamic offset per
            // draw selects the mvp). Immutable self borrows only -> no aliasing with the encoder.
            let bgl = self.ui_bgl.as_ref().unwrap();
            let sampler = self.ui_sampler.as_ref().unwrap();
            let mut binds: HashMap<u32, wgpu::BindGroup> = HashMap::new();
            for d in &self.ui_draws {
                if binds.contains_key(&d.tex_id) { continue; }
                let tv = self.textures.get(&d.tex_id).map(|(_, v)| v).unwrap_or(&self.white);
                let bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("ui-bind"),
                    layout: bgl,
                    entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding { buffer: ui_ubo, offset: 0, size: wgpu::BufferSize::new(64) }) },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(tv) },
                        wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(sampler) },
                    ],
                });
                binds.insert(d.tex_id, bg);
            }
            let tri = self.ui_tri_pipeline.as_ref().unwrap();
            let line = self.ui_line_pipeline.as_ref().unwrap();
            let mut up = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("ui-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: target,
                    resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            up.set_vertex_buffer(0, ui_vbuf.slice(..));
            for (i, d) in self.ui_draws.iter().enumerate() {
                let Some(bind) = binds.get(&d.tex_id) else { continue };
                up.set_pipeline(if d.line { line } else { tri });
                up.set_bind_group(0, bind, &[(i as u64 * ALIGN) as u32]);
                up.draw(d.first_vertex..d.first_vertex + d.vertex_count, 0..1);
            }
        }
        queue.submit([enc.finish()]);
        let n = self.queued.len() as u64;
        self.drawn += n;
        self.queued.clear();
        self.ui_verts.clear();
        self.ui_draws.clear();
        n
    }
}

/// Expand STRIP (1) / FAN (2) / LINE_STRIP (5) index sequences to list form.
fn expand_to_list(mode: u32, src: &[u16]) -> Vec<u16> {
    match mode {
        1 => strip_to_list(src),
        2 => {
            let mut list = Vec::new();
            for k in 1..src.len().saturating_sub(1) {
                list.extend_from_slice(&[src[0], src[k], src[k + 1]]);
            }
            list
        }
        5 => {
            let mut list = Vec::new();
            for k in 1..src.len() {
                list.extend_from_slice(&[src[k - 1], src[k]]);
            }
            list
        }
        _ => Vec::new(),
    }
}

fn strip_to_list(src: &[u16]) -> Vec<u16> {
    let mut list = Vec::new();
    for k in 2..src.len() {
        if k % 2 == 0 {
            list.extend_from_slice(&[src[k - 2], src[k - 1], src[k]]);
        } else {
            list.extend_from_slice(&[src[k - 1], src[k - 2], src[k]]);
        }
    }
    list
}

