//! U0 -- the native-VK UI ORACLE (see fs_render/UI_STRATIGRAPHY.md).
//!
//! Renders an `fsr_ui_submit`-format draw stream under a CONFIGURABLE fixed-function state, using the
//! REAL `ui.vert`/`ui.frag` from fs_render (loaded by path -> zero shader drift). The whole UI fidelity
//! question is fixed-function STATE, not shader logic (ui.frag is just `v_color * texel`), so the A/B
//! is: render the SAME fixtures under the STOCK contract vs the ENGINE-CURRENT contract and diff.
//!
//! The four axes of `UiState` are the four strata:
//!   - target_srgb + tex_srgb  -> Stratum A (color space): stock blends RAW sRGB bytes in sRGB space
//!     (UNORM target, no texture decode); engine-current does a full linear round-trip (sRGB target =>
//!     decode dst, treat vertex color as linear, blend in linear, re-encode) + sRGB-decoded textures.
//!   - respect_blend           -> Stratum C: stock honors setSceneBlendType; engine-current forces alpha.
//!   - per_surface_filter      -> Stratum D: stock samples the font atlas NEAREST; engine-current LINEAR.
//!   - scissor                 -> Stratum B: stock clips via the scissor stack; engine-current does not.

use std::collections::HashMap;
use wgpu::util::DeviceExt;

/// STOCK target: raw bytes, blend in sRGB (perceptual) space -- `GL_FRAMEBUFFER_SRGB` disabled for UI.
pub const STOCK_TARGET: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8Unorm;
/// ENGINE-CURRENT target: sRGB => Vulkan blends in LINEAR space + auto-encodes on store (the bug).
pub const ENGINE_TARGET: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8UnormSrgb;

/// sRGB EOTF (byte -> linear) and its inverse, the standard piecewise curve. Used to (a) clear each
/// target to the SAME display byte regardless of format, and (b) compute analytic expectations.
pub fn srgb_to_linear(s: f64) -> f64 {
    if s <= 0.04045 { s / 12.92 } else { ((s + 0.055) / 1.055).powf(2.4) }
}
pub fn linear_to_srgb(l: f64) -> f64 {
    if l <= 0.0031308 { l * 12.92 } else { 1.055 * l.powf(1.0 / 2.4) - 0.055 }
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub enum Blend { Alpha, Add, Mult }

#[derive(Clone, Copy)]
pub struct UiState {
    pub target_srgb: bool,        // A: engine-current true (linear blend + encode); stock false (raw)
    pub tex_srgb: bool,           // A: engine-current true (decoded on sample); stock false (raw)
    pub respect_blend: bool,      // C: stock true; engine-current false (forces Alpha -- no blend capture)
    pub per_surface_filter: bool, // D: stock true (font NEAREST); engine-current false (all LINEAR)
    pub scissor: bool,            // B: stock true; engine-current false (no clip plumbed)
}
impl UiState {
    /// The faithful stock LLRender/LLUI contract (the reference the engine must converge to).
    pub fn stock() -> Self {
        UiState { target_srgb: false, tex_srgb: false, respect_blend: true, per_surface_filter: true, scissor: true }
    }
    /// fs_render's UI pass TODAY (src/live.rs): sRGB swapchain, sRGB textures, one alpha pipeline,
    /// one LINEAR sampler, no scissor. The A/B vs stock() is the measured gap per stratum.
    pub fn engine_current() -> Self {
        UiState { target_srgb: true, tex_srgb: true, respect_blend: false, per_surface_filter: false, scissor: false }
    }
    fn target_format(&self) -> wgpu::TextureFormat {
        if self.target_srgb { ENGINE_TARGET } else { STOCK_TARGET }
    }
}

/// A UI texture (image or font atlas), raw RGBA8 bytes as the tap would upload them.
pub struct Tex {
    pub id: u32,
    pub w: u32,
    pub h: u32,
    pub rgba: Vec<u8>,
    pub is_font: bool, // stock samples fonts NEAREST (TFO_POINT); images LINEAR
}

/// One flushed UI draw. `verts` is ALREADY a triangle/line LIST (pre-expanded, 24-byte stride:
/// {f32 x,y,z; f32 u,v; u8 r,g,b,a}) -- matching what `LiveRenderer::ui_submit` produces after it
/// expands strips/fans, so the oracle and the engine consume identical geometry.
pub struct Draw {
    pub mvp: [f32; 16],
    pub tex_id: u32, // 0 -> white fallback
    pub verts: Vec<u8>,
    pub line: bool,
    pub blend: Blend,
    pub clip: Option<[u32; 4]>, // scissor x,y,w,h (screen px), applied when state.scissor
}

/// Pack one interleaved UI vertex (the fsr_ui_submit wire format).
pub fn vtx(out: &mut Vec<u8>, x: f32, y: f32, u: f32, v: f32, c: [u8; 4]) {
    out.extend_from_slice(&x.to_le_bytes());
    out.extend_from_slice(&y.to_le_bytes());
    out.extend_from_slice(&0f32.to_le_bytes());
    out.extend_from_slice(&u.to_le_bytes());
    out.extend_from_slice(&v.to_le_bytes());
    out.extend_from_slice(&c);
}

/// Two axis-aligned triangles (a quad) in NDC, all one color, sampling the full [0,1] UV. `mode 0`
/// TRIANGLES, already a list.
pub fn quad(x0: f32, y0: f32, x1: f32, y1: f32, c: [u8; 4]) -> Vec<u8> {
    let mut o = Vec::new();
    vtx(&mut o, x0, y0, 0.0, 1.0, c);
    vtx(&mut o, x1, y0, 1.0, 1.0, c);
    vtx(&mut o, x1, y1, 1.0, 0.0, c);
    vtx(&mut o, x0, y0, 0.0, 1.0, c);
    vtx(&mut o, x1, y1, 1.0, 0.0, c);
    vtx(&mut o, x0, y1, 0.0, 0.0, c);
    o
}

const UI_VS: &[u8] = include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/../fs_render/shaders/ui.vert.spv"));
const UI_FS: &[u8] = include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/../fs_render/shaders/ui.frag.spv"));

fn blend_state(b: Blend) -> wgpu::BlendState {
    match b {
        // BT_ALPHA: glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA), applied uniformly to RGBA.
        Blend::Alpha => wgpu::BlendState {
            color: wgpu::BlendComponent { src_factor: wgpu::BlendFactor::SrcAlpha, dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha, operation: wgpu::BlendOperation::Add },
            alpha: wgpu::BlendComponent { src_factor: wgpu::BlendFactor::SrcAlpha, dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha, operation: wgpu::BlendOperation::Add },
        },
        // BT_ADD: glBlendFunc(ONE, ONE).
        Blend::Add => wgpu::BlendState {
            color: wgpu::BlendComponent { src_factor: wgpu::BlendFactor::One, dst_factor: wgpu::BlendFactor::One, operation: wgpu::BlendOperation::Add },
            alpha: wgpu::BlendComponent { src_factor: wgpu::BlendFactor::One, dst_factor: wgpu::BlendFactor::One, operation: wgpu::BlendOperation::Add },
        },
        // BT_MULT: glBlendFunc(DST_COLOR, ZERO).
        Blend::Mult => wgpu::BlendState {
            color: wgpu::BlendComponent { src_factor: wgpu::BlendFactor::Dst, dst_factor: wgpu::BlendFactor::Zero, operation: wgpu::BlendOperation::Add },
            alpha: wgpu::BlendComponent { src_factor: wgpu::BlendFactor::Dst, dst_factor: wgpu::BlendFactor::Zero, operation: wgpu::BlendOperation::Add },
        },
    }
}

/// Render the fixtures under `state` into a fresh `w`x`h` target cleared to display byte `bg`
/// (matched across formats), and read back RGBA display bytes (row-major).
pub fn render(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    w: u32,
    h: u32,
    bg: [u8; 4],
    state: UiState,
    texes: &[Tex],
    draws: &[Draw],
) -> Vec<[u8; 4]> {
    let fmt = state.target_format();
    let target = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("ui-oracle-target"),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: fmt,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = target.create_view(&wgpu::TextureViewDescriptor::default());

    // Clear to the SAME display byte on both formats: UNORM stores the value raw; sRGB stores
    // linear->sRGB, so pre-linearize for the sRGB target. This models "the tonemapped scene already
    // on the swapchain" identically, isolating the DRAW-compositing gap.
    let enc_lin = |b: u8| -> f64 {
        let s = b as f64 / 255.0;
        if state.target_srgb { srgb_to_linear(s) } else { s }
    };
    let clear = wgpu::Color { r: enc_lin(bg[0]), g: enc_lin(bg[1]), b: enc_lin(bg[2]), a: bg[3] as f64 / 255.0 };

    // Textures: raw Unorm (stock) vs Srgb (engine-current, decoded on sample).
    let tex_fmt = if state.tex_srgb { wgpu::TextureFormat::Rgba8UnormSrgb } else { wgpu::TextureFormat::Rgba8Unorm };
    let white_bytes = [255u8, 255, 255, 255];
    let mk_view = |bytes: &[u8], tw: u32, th: u32| -> wgpu::TextureView {
        let t = device.create_texture_with_data(
            queue,
            &wgpu::TextureDescriptor {
                label: Some("ui-oracle-tex"),
                size: wgpu::Extent3d { width: tw, height: th, depth_or_array_layers: 1 },
                mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
                format: tex_fmt,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            },
            wgpu::util::TextureDataOrder::LayerMajor,
            bytes,
        );
        t.create_view(&wgpu::TextureViewDescriptor::default())
    };
    let white = mk_view(&white_bytes, 1, 1);
    let mut tex_views: HashMap<u32, (wgpu::TextureView, bool)> = HashMap::new();
    for t in texes {
        tex_views.insert(t.id, (mk_view(&t.rgba, t.w, t.h), t.is_font));
    }

    let nearest = device.create_sampler(&wgpu::SamplerDescriptor {
        label: Some("ui-nearest"), address_mode_u: wgpu::AddressMode::ClampToEdge, address_mode_v: wgpu::AddressMode::ClampToEdge, address_mode_w: wgpu::AddressMode::ClampToEdge,
        mag_filter: wgpu::FilterMode::Nearest, min_filter: wgpu::FilterMode::Nearest, mipmap_filter: wgpu::FilterMode::Nearest, ..Default::default()
    });
    let linear = device.create_sampler(&wgpu::SamplerDescriptor {
        label: Some("ui-linear"), address_mode_u: wgpu::AddressMode::ClampToEdge, address_mode_v: wgpu::AddressMode::ClampToEdge, address_mode_w: wgpu::AddressMode::ClampToEdge,
        mag_filter: wgpu::FilterMode::Linear, min_filter: wgpu::FilterMode::Linear, mipmap_filter: wgpu::FilterMode::Nearest, ..Default::default()
    });

    let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("ui-oracle-bgl"),
        entries: &[
            wgpu::BindGroupLayoutEntry { binding: 0, visibility: wgpu::ShaderStages::VERTEX, ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: wgpu::BufferSize::new(64) }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 1, visibility: wgpu::ShaderStages::FRAGMENT, ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 2, visibility: wgpu::ShaderStages::FRAGMENT, ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering), count: None },
        ],
    });
    let pll = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some("ui-oracle-pll"), bind_group_layouts: &[&bgl], push_constant_ranges: &[] });
    let vs = device.create_shader_module(wgpu::ShaderModuleDescriptor { label: Some("ui.vert"), source: wgpu::util::make_spirv(UI_VS) });
    let fs = device.create_shader_module(wgpu::ShaderModuleDescriptor { label: Some("ui.frag"), source: wgpu::util::make_spirv(UI_FS) });
    let attrs = [
        wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 0, shader_location: 0 },
        wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x2, offset: 12, shader_location: 1 },
        wgpu::VertexAttribute { format: wgpu::VertexFormat::Unorm8x4, offset: 20, shader_location: 2 },
    ];
    let mut pipes: HashMap<(Blend, bool), wgpu::RenderPipeline> = HashMap::new();
    let ensure_pipe = |key: (Blend, bool), pipes: &mut HashMap<(Blend, bool), wgpu::RenderPipeline>| {
        if pipes.contains_key(&key) { return; }
        let topo = if key.1 { wgpu::PrimitiveTopology::LineList } else { wgpu::PrimitiveTopology::TriangleList };
        let p = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("ui-oracle-pipe"),
            layout: Some(&pll),
            vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[wgpu::VertexBufferLayout { array_stride: 24, step_mode: wgpu::VertexStepMode::Vertex, attributes: &attrs }] },
            fragment: Some(wgpu::FragmentState { module: &fs, entry_point: "main", targets: &[Some(wgpu::ColorTargetState { format: fmt, blend: Some(blend_state(key.0)), write_mask: wgpu::ColorWrites::ALL })] }),
            primitive: wgpu::PrimitiveState { topology: topo, cull_mode: None, ..Default::default() },
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });
        pipes.insert(key, p);
    };

    // Per-draw resources (UBO + bind group), built up front so borrows don't fight the render pass.
    struct DrawRes { pipe_key: (Blend, bool), bind: wgpu::BindGroup, vbuf: wgpu::Buffer, count: u32, clip: Option<[u32; 4]> }
    let mut res: Vec<DrawRes> = Vec::new();
    for d in draws {
        let blend = if state.respect_blend { d.blend } else { Blend::Alpha };
        let key = (blend, d.line);
        ensure_pipe(key, &mut pipes);
        let ubo = device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: Some("ui-oracle-ubo"), contents: bytemuck::cast_slice(&d.mvp), usage: wgpu::BufferUsages::UNIFORM });
        let (tv, is_font) = tex_views.get(&d.tex_id).map(|(v, f)| (v, *f)).unwrap_or((&white, false));
        let samp = if state.per_surface_filter && is_font { &nearest } else { &linear };
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("ui-oracle-bind"), layout: &bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding { buffer: &ubo, offset: 0, size: wgpu::BufferSize::new(64) }) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(tv) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(samp) },
            ],
        });
        let vbuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: Some("ui-oracle-vb"), contents: &d.verts, usage: wgpu::BufferUsages::VERTEX });
        res.push(DrawRes { pipe_key: key, bind, vbuf, count: (d.verts.len() / 24) as u32, clip: if state.scissor { d.clip } else { None } });
    }

    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("ui-oracle-enc") });
    {
        let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("ui-oracle-pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment { view: &view, resolve_target: None, ops: wgpu::Operations { load: wgpu::LoadOp::Clear(clear), store: wgpu::StoreOp::Store } })],
            depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None,
        });
        for r in &res {
            rp.set_pipeline(pipes.get(&r.pipe_key).unwrap());
            if let Some([cx, cy, cw, ch]) = r.clip {
                // Clamp to target so a fixture clip that runs off-screen doesn't trip validation.
                let cw = cw.min(w.saturating_sub(cx.min(w)));
                let ch = ch.min(h.saturating_sub(cy.min(h)));
                if cw > 0 && ch > 0 { rp.set_scissor_rect(cx.min(w), cy.min(h), cw, ch); }
            } else {
                rp.set_scissor_rect(0, 0, w, h);
            }
            rp.set_bind_group(0, &r.bind, &[]);
            rp.set_vertex_buffer(0, r.vbuf.slice(..));
            rp.draw(0..r.count, 0..1);
        }
    }

    // Readback -> RGBA display bytes (row-major). For both formats the stored bytes ARE the display
    // (sRGB-encoded) values, so the two configs are directly comparable.
    let stride = ((w * 4 + 255) / 256) * 256;
    let buf = device.create_buffer(&wgpu::BufferDescriptor { label: Some("ui-oracle-rb"), size: (stride * h) as u64, usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ, mapped_at_creation: false });
    enc.copy_texture_to_buffer(
        wgpu::ImageCopyTexture { texture: &target, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        wgpu::ImageCopyBuffer { buffer: &buf, layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(stride), rows_per_image: Some(h) } },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
    queue.submit([enc.finish()]);
    let slice = buf.slice(..);
    slice.map_async(wgpu::MapMode::Read, |_| {});
    device.poll(wgpu::Maintain::Wait);
    let data = slice.get_mapped_range();
    let mut out = vec![[0u8; 4]; (w * h) as usize];
    for y in 0..h {
        for x in 0..w {
            let o = (y * stride + x * 4) as usize;
            out[(y * w + x) as usize] = [data[o], data[o + 1], data[o + 2], data[o + 3]];
        }
    }
    drop(data);
    buf.unmap();
    out
}

/// Max + mean absolute RGB delta between two same-size readbacks (alpha ignored -- display is opaque).
pub fn diff_rgb(a: &[[u8; 4]], b: &[[u8; 4]]) -> (u32, f64) {
    let mut max_d = 0u32;
    let mut sum = 0u64;
    let n = a.len().min(b.len());
    for i in 0..n {
        for c in 0..3 {
            let d = (a[i][c] as i32 - b[i][c] as i32).unsigned_abs();
            if d > max_d { max_d = d; }
            sum += d as u64;
        }
    }
    (max_d, sum as f64 / (n * 3).max(1) as f64)
}
