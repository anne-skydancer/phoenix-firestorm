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
            else if samples >= 8 { 8 } else if samples >= 4 { 4 } else if samples >= 2 { 2 } else { 1 };
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
                format: self.format,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                view_formats: &[],
            });
            self.msaa_color = Some((t.create_view(&wgpu::TextureViewDescriptor::default()), w, h, self.msaa));
        }
    }

    pub fn queued_len(&self) -> usize {
        self.queued.len()
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
                    format: self.format,
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
        self.sky_pipeline = Some(make(device, sky_vs, sky_fs, 2, false, self.format, &layout));
        let water_vs = device.create_shader_module(wgpu::include_spirv!("../shaders/water.vert.spv"));
        let water_fs = device.create_shader_module(wgpu::include_spirv!("../shaders/water.frag.spv"));
        self.water_pipeline = Some(make(device, water_vs, water_fs, 1, true, self.format, &layout));
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
                    format: self.format,
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
        const STRIDE: usize = 256;
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
            let mut block = [0f32; 56]; // mvp + color + planes + texmat + khr + flags
            block[..16].copy_from_slice(&mvp.to_cols_array());
            block[16..20].copy_from_slice(&d.color);
            block[20..28].copy_from_slice(&d.aux[..8]);
            block[28..44].copy_from_slice(&d.texmat);
            block[44..52].copy_from_slice(&d.khr);
            block[52] = if d.blend != 0 { 1.0 } else { 0.0 }; // flags.x: blending enabled
            block[53] = if d.indexed_ch > 0 { 1.0 } else { 0.0 }; // flags.y: pos.w is a tex index
            block[54] = d.min_alpha; // flags.z: MASK cutoff (-1 disabled)
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
        self.queued.push(Queued { bind_key, ubo_off: (slot * STRIDE) as u32, pal_off, skinned, draw_class: d.draw_class, terrain_key: tk, vptr, iptr, icount, voffsets, pipe_key, first, vcount, blend_add: d.blend_add != 0 });
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
                    format: self.format,
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

    fn ensure_depth(&mut self, device: &wgpu::Device, w: u32, h: u32) {
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
        self.ensure_depth(device, w, h);
        self.ensure_msaa_color(device, w, h);
        let depth_view = match &self.depth {
            Some((v, _, _, _)) => v,
            None => return 0,
        };
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("live") });
        {
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("live-pass"),
                color_attachments: &[Some(match &self.msaa_color {
                    Some((ms_view, _, _, _)) => wgpu::RenderPassColorAttachment {
                        view: ms_view,
                        resolve_target: Some(target), // MSAA resolves into the swapchain
                        ops: wgpu::Operations { load: wgpu::LoadOp::Clear(clear), store: wgpu::StoreOp::Discard },
                    },
                    None => wgpu::RenderPassColorAttachment {
                        view: target,
                        resolve_target: None,
                        ops: wgpu::Operations { load: wgpu::LoadOp::Clear(clear), store: wgpu::StoreOp::Store },
                    },
                })],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: depth_view,
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(0.0), store: wgpu::StoreOp::Store }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            for q in &self.queued {
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
        queue.submit([enc.finish()]);
        let n = self.queued.len() as u64;
        self.drawn += n;
        self.queued.clear();
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

