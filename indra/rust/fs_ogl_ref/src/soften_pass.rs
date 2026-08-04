//! The real OGL softenLight (atmospherics/deferred-lighting) pass, headless. Feeds a fixture:
//! - a flat G-buffer (a known albedo + up-normal + HAS_ATMOS flag) so we're lighting a plain surface,
//! - NEUTRAL shadow/AO/probe inputs (shadow=lit, ao=full, probes=black) -- isolates the ATMOSPHERICS
//!   look, which is the divergence we're chasing,
//! - the SoftenFrameBlock UBO filled from a fixture EEP sky (std140.rs), + a zeroed ReflectionProbes UBO.
//! Renders into a linear-HDR scene target; the OGL present (tonemap/gamma) runs after (added next).
//!
//! Bindings (from the emitted transform): 0 = SoftenFrameBlock, 1..41 = 20 samplers, 35 = ReflectionProbes.

use crate::std140::Block;
use wgpu::util::DeviceExt;

/// A perspective projection's INVERSE (column-major), the analytic inverse of the OpenGL clip-[-1,1]
/// perspective matrix. getPositionWithDepth needs a real inv_proj or the reconstructed eye-space
/// position is NaN (zeroed matrices) -- which silently zeroed calcAtmosphericVars and made the oracle
/// dark. fovy/aspect/near/far match a plausible viewer camera; both engines use the identical matrix.
fn perspective_inv(fovy_rad: f32, aspect: f32, near: f32, far: f32) -> [f32; 16] {
    let f = 1.0 / (fovy_rad * 0.5).tan();
    let c = (far + near) / (near - far);
    let d = (2.0 * far * near) / (near - far);
    // column-major: [col0(4), col1(4), col2(4), col3(4)]
    [
        aspect / f, 0.0, 0.0, 0.0,
        0.0, 1.0 / f, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0 / d,
        0.0, 0.0, -1.0, c / d,
    ]
}

/// A fixture EEP sky (a plain daytime WindLight sky, the LLSettingsSky defaults from the stratigraphy).
/// Fills the atmospherics members of SoftenFrameBlock; everything else stays zero/neutral.
/// `classic` selects the HDR regime: legacy WindLight sky (classic_mode=1, the auto-adjust-off default)
/// vs a PBR/EEP sky (classic_mode=0). softenLightF's lit branches differ sharply between the two.
pub fn fixture_sky_block(classic: bool) -> Block {
    let mut b = Block::new();
    // Atmospherics (WindLight defaults, llsettingssky.cpp:900-919,1227-1233):
    b.vec3("blue_horizon", [0.4954, 0.4954, 0.6399])
        .vec3("blue_density", [0.2447, 0.4487, 0.7599])
        .f32("haze_horizon", 0.19)
        .f32("haze_density", 0.7)
        .f32("cloud_shadow", 0.2699)
        .f32("density_multiplier", 0.0001)
        .f32("distance_multiplier", 0.8)
        .f32("max_y", 1605.0)
        .vec3("ambient_color", [0.25, 0.25, 0.25])
        .vec3("sunlight_color", [0.7342, 0.7815, 0.8999])
        .vec3("moonlight_color", [0.0, 0.0, 0.0])
        .vec3("glow", [5.0, 0.001, -0.4799])
        .f32("sun_moon_glow_factor", 1.0)
        // sun at ~45deg (lightnorm is OGL Y-up); sun_dir SL Z-up.
        .vec3("lightnorm", [0.0, 0.707, 0.707])
        .vec3("sun_dir", [0.0, 0.707, 0.707])
        .vec3("moon_dir", [0.0, -0.707, -0.707])
        .i32("sun_up_factor", 1)
        // HDR/classic regime seam (llsettingsvo.cpp:810). Legacy WindLight (auto-adjust off) ->
        // classic_mode=1; PBR/EEP sky -> classic_mode=0. sky_hdr_scale=1 either way for this fixture.
        .i32("classic_mode", if classic { 1 } else { 0 })
        .f32("sky_hdr_scale", 1.0)
        .f32("sky_sunlight_scale", 1.5)
        .f32("sky_ambient_scale", 1.5)
        .f32("scene_light_strength", 2.0)
        // screen res (the fullscreen target).
        .vec2("screen_res", [256.0, 256.0])
        // a real perspective inverse so getPositionWithDepth reconstructs a sane eye-space fragment
        // (~10m out at depth 0.95). Without this the position is NaN and calcAtmosphericVars collapses.
        .mat4("inv_proj", perspective_inv(60.0f32.to_radians(), 1.0, 0.5, 256.0))
        // neutral shadows/ssao (no darkening) in case sampled.
        .f32("shadow_bias", 0.0)
        .f32("ssao_irradiance_scale", 1.0)
        .f32("ssao_irradiance_max", 1.0);
    b
}

/// A 1x1 texture2D of a given format + RGBA8/float bytes, RENDER-neutral for the fixture.
fn tex_1x1(device: &wgpu::Device, queue: &wgpu::Queue, format: wgpu::TextureFormat, data: &[u8], label: &str) -> wgpu::TextureView {
    let t = device.create_texture_with_data(
        queue,
        &wgpu::TextureDescriptor {
            label: Some(label),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        },
        wgpu::util::TextureDataOrder::LayerMajor,
        data,
    );
    t.create_view(&wgpu::TextureViewDescriptor::default())
}

/// A 1x1 depth texture (for the shadow-comparison samplers). Cleared to 1.0 (far) = nothing occludes.
fn depth_1x1(device: &wgpu::Device, queue: &wgpu::Queue, label: &str) -> wgpu::TextureView {
    // Depth32Float can't be create_texture_with_data; render-clear it.
    let t = device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Depth32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let view = t.create_view(&wgpu::TextureViewDescriptor::default());
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("depth-clear") });
    enc.begin_render_pass(&wgpu::RenderPassDescriptor {
        label: Some("depth-clear"),
        color_attachments: &[],
        depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
            view: &view,
            depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Store }),
            stencil_ops: None,
        }),
        timestamp_writes: None,
        occlusion_query_set: None,
    });
    queue.submit([enc.finish()]);
    view
}

/// A 1x1 cube-array (6 faces, 1 cube) of black -- neutral reflection/irradiance probe.
fn cube_array_1x1(device: &wgpu::Device, queue: &wgpu::Queue, label: &str) -> wgpu::TextureView {
    let t = device.create_texture_with_data(
        queue,
        &wgpu::TextureDescriptor {
            label: Some(label),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 6 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba16Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        },
        wgpu::util::TextureDataOrder::LayerMajor,
        &[0u8; 2 * 4 * 6], // 6 faces x rgba16f (8 bytes) = but 1px each -> 8*6=48 bytes
    );
    t.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::CubeArray),
        ..Default::default()
    })
}

// f16 helpers for the Rgba16Float G-buffer/probe fixtures.
fn f16(x: f32) -> [u8; 2] {
    // minimal f32->f16 (round-to-nearest-even not needed for fixtures).
    let bits = x.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exp = ((bits >> 23) & 0xff) as i32 - 127 + 15;
    let mant = (bits >> 13) & 0x3ff;
    let h = if exp <= 0 { sign } else if exp >= 31 { sign | 0x7c00 } else { sign | ((exp as u16) << 10) | mant as u16 };
    h.to_le_bytes()
}
fn rgba16f(r: f32, g: f32, b: f32, a: f32) -> Vec<u8> {
    [f16(r), f16(g), f16(b), f16(a)].concat()
}

pub const SCENE_HDR: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

/// A fullscreen-triangle vertex that feeds softenLightF's varyings. softenLightF recomputes the
/// atmospherics in-fragment (calcAtmosphericVarsLinear), so vary_Additive/Atmos are neutral here;
/// vary_fragcoord is the 0..1 screen UV. Locations match softenLightF's `in` declaration order
/// (vary_AdditiveColor, vary_AtmosAttenuation, vary_fragcoord = 0,1,2 under auto-map).
const SOFTEN_VERT: &str = r#"#version 450
layout(location=0) out vec3 vary_AdditiveColor;
layout(location=1) out vec3 vary_AtmosAttenuation;
layout(location=2) out vec2 vary_fragcoord;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vary_fragcoord = p;
    vary_AdditiveColor = vec3(0.0);
    vary_AtmosAttenuation = vec3(1.0);
}
"#;

/// Result of rendering softenLight on the fixture: the lit HDR scene target + its view, read back by grab.
pub struct SoftenResult {
    pub target: wgpu::Texture,
    pub view: wgpu::TextureView,
}

/// binding index -> what it is, for building the layout + bind group programmatically.
enum Slot {
    Ubo(u64),            // UBO of at least this size
    Tex,                 // texture2D, Float filterable
    TexCube,             // textureCubeArray, Float filterable
    TexDepth,            // texture2D sampled by a comparison sampler (shadowMap)
    Samp,                // filtering sampler
    SampCmp,             // comparison sampler
}

/// Render the real softenLight fragment over the fixture into a `size`x`size` linear-HDR target.
/// Returns the lit scene (pre-tonemap), the oracle side of the atmospherics diff.
pub fn render(device: &wgpu::Device, queue: &wgpu::Queue, frag: &wgpu::ShaderModule, size: u32, classic: bool) -> SoftenResult {
    // --- fixture textures ---
    let white = tex_1x1(device, queue, wgpu::TextureFormat::Rgba16Float, &rgba16f(1.0, 1.0, 1.0, 1.0), "white");
    // G-buffer: a plain PBR ground -- base color 0.5 grey, up-ish encoded normal + HAS_PBR flag.
    let diffuse = tex_1x1(device, queue, wgpu::TextureFormat::Rgba16Float, &rgba16f(0.5, 0.5, 0.5, 0.0), "diffuseRect");
    // specularRect = ORM (occlusion, roughness, metallic). Plain rough ground: ao=1, rough=1, metal=0.
    // (My earlier ao=0 zeroed the ambient term -- part of the 0.030 darkness.)
    let specular = tex_1x1(device, queue, wgpu::TextureFormat::Rgba16Float, &rgba16f(1.0, 1.0, 0.0, 0.0), "specularRect");
    let emissive = tex_1x1(device, queue, wgpu::TextureFormat::Rgba16Float, &rgba16f(0.0, 0.0, 0.0, 0.0), "emissiveRect");
    // normalMap: spheremap-encoded up normal (~0.5,0.5), env 0, gbufferFlag HAS_PBR (0.67) in .w.
    let normal = tex_1x1(device, queue, wgpu::TextureFormat::Rgba16Float, &rgba16f(0.5, 0.5, 0.0, 0.67), "normalMap");
    let depthv = tex_1x1(device, queue, wgpu::TextureFormat::R32Float, &0.95f32.to_le_bytes(), "depthMap");
    let black = tex_1x1(device, queue, wgpu::TextureFormat::Rgba16Float, &rgba16f(0.0, 0.0, 0.0, 1.0), "black");
    let shadow = depth_1x1(device, queue, "shadowMap"); // 1.0 = far = nothing occludes
    let cube = cube_array_1x1(device, queue, "probe");
    // lightMap: rg = (shadow, ambient-occlusion). (1,1) = full sun, no AO.
    let lightmap = tex_1x1(device, queue, wgpu::TextureFormat::Rgba16Float, &rgba16f(1.0, 1.0, 1.0, 1.0), "lightMap");

    let samp = device.create_sampler(&wgpu::SamplerDescriptor {
        label: Some("filt"),
        mag_filter: wgpu::FilterMode::Linear, min_filter: wgpu::FilterMode::Linear,
        ..Default::default()
    });
    let samp_cmp = device.create_sampler(&wgpu::SamplerDescriptor {
        label: Some("cmp"),
        compare: Some(wgpu::CompareFunction::LessEqual),
        ..Default::default()
    });

    // UBOs.
    let frame_ubo = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("SoftenFrameBlock"),
        contents: fixture_sky_block(classic).bytes(),
        usage: wgpu::BufferUsages::UNIFORM,
    });
    // ReflectionProbes: all-zero, oversized so it covers the block whatever MAX_REFMAP_COUNT is.
    let probe_ubo = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("ReflectionProbes"),
        size: 128 * 1024,
        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });

    // binding -> (Slot, resource). Order + types match the emitted transform exactly.
    // 0=SoftenFrameBlock, 1-18 G-buffer/misc, 19-30 shadowMap0-5, 31-34 refl/irr probes, 35 ReflectionProbes UBO,
    // 36-37 heroProbes, 38-41 lightMap/lightFunc.
    let mut le: Vec<wgpu::BindGroupLayoutEntry> = Vec::new();
    let mut ge: Vec<wgpu::BindGroupEntry> = Vec::new();
    // Macros (inline -> sidestep closure lifetime invariance on BindGroupEntry<'_>).
    macro_rules! ubo { ($b:expr, $buf:expr) => {{
        le.push(wgpu::BindGroupLayoutEntry { binding: $b, visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None });
        ge.push(wgpu::BindGroupEntry { binding: $b, resource: $buf.as_entire_binding() });
    }}; }
    macro_rules! tex { ($b:expr, $view:expr, $dim:expr, $depth:expr) => {{
        le.push(wgpu::BindGroupLayoutEntry { binding: $b, visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: if $depth { wgpu::TextureSampleType::Depth } else { wgpu::TextureSampleType::Float { filterable: true } },
                view_dimension: $dim, multisampled: false }, count: None });
        ge.push(wgpu::BindGroupEntry { binding: $b, resource: wgpu::BindingResource::TextureView($view) });
    }}; }
    macro_rules! smp { ($b:expr, $cmp:expr) => {{
        le.push(wgpu::BindGroupLayoutEntry { binding: $b, visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Sampler(if $cmp { wgpu::SamplerBindingType::Comparison } else { wgpu::SamplerBindingType::Filtering }), count: None });
        ge.push(wgpu::BindGroupEntry { binding: $b, resource: wgpu::BindingResource::Sampler(if $cmp { &samp_cmp } else { &samp }) });
    }}; }
    let d2 = wgpu::TextureViewDimension::D2;
    let ca = wgpu::TextureViewDimension::CubeArray;
    ubo!(0, frame_ubo);
    // texture2D/sampler pairs (binding, view, is-shadow); sampler at binding+1.
    let tex_pairs: [(u32, &wgpu::TextureView, bool); 17] = [
        (1, &normal, false), (3, &depthv, false), (5, &black, false), (7, &white, false),
        (9, &diffuse, false), (11, &specular, false), (13, &emissive, false),
        (15, &black, false), (17, &depthv, false),
        (19, &shadow, true), (21, &shadow, true), (23, &shadow, true), (25, &shadow, true),
        (27, &shadow, true), (29, &shadow, true),
        (38, &lightmap, false), (40, &white, false),
    ];
    for (b, view, is_shadow) in tex_pairs {
        tex!(b, view, d2, is_shadow);
        smp!(b + 1, is_shadow);
    }
    for b in [31u32, 33, 36] {
        tex!(b, &cube, ca, false);
        smp!(b + 1, false);
    }
    ubo!(35, probe_ubo);

    let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: Some("soften-bgl"), entries: &le });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor { label: Some("soften-bind"), layout: &bgl, entries: &ge });

    // vertex module (regular; the fragment is passthrough).
    let vspv = crate::shaders::compile_glsl(SOFTEN_VERT, shaderc::ShaderKind::Vertex, "soften.vert");
    let vmod = unsafe {
        device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV { label: Some("soften.vert"), source: std::borrow::Cow::Borrowed(&vspv) })
    };
    let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some("soften-pl"), bind_group_layouts: &[&bgl], push_constant_ranges: &[] });
    let pipe = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("soften-pipe"), layout: Some(&pl),
        vertex: wgpu::VertexState { module: &vmod, entry_point: "main", buffers: &[] },
        fragment: Some(wgpu::FragmentState { module: frag, entry_point: "main",
            targets: &[Some(wgpu::ColorTargetState { format: SCENE_HDR, blend: None, write_mask: wgpu::ColorWrites::ALL })] }),
        primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, ..Default::default() },
        depth_stencil: None, multisample: wgpu::MultisampleState::default(), multiview: None,
    });

    let target = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("soften-scene-hdr"),
        size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
        mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
        format: SCENE_HDR, usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let view = target.create_view(&wgpu::TextureViewDescriptor::default());
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("soften-render") });
    {
        let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("soften"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment { view: &view, resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store } })],
            depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None,
        });
        rp.set_pipeline(&pipe);
        rp.set_bind_group(0, &bind, &[]);
        rp.draw(0..3, 0..1);
    }
    queue.submit([enc.finish()]);
    SoftenResult { target, view }
}
