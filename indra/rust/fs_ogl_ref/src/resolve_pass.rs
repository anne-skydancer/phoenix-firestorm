//! The TEST side of the atmospherics A/B bench: runs fs_render's own `resolve.frag` (read from disk,
//! the authoritative source) on a fixture that is the LOGICAL EQUIVALENT of the softenLight oracle
//! fixture (same base color, world normal chosen so dot(n,sun)=nl, same sky params, same regime).
//! fs_ogl_ref temporarily hosts BOTH sides because it owns the GLSL->SPIR-V + fixture infrastructure;
//! the point is the empirical divergence number, not the crate boundary.
//!
//! resolve.frag is plain Vulkan GLSL (its own 60-float std140 Sky UBO + 2 texelFetch G-buffer RTs, no
//! samplers) -- compiles directly via shaderc, no UBO transform needed.

use wgpu::util::DeviceExt;

pub const SCENE_HDR: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

fn f16(x: f32) -> [u8; 2] {
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

/// A `size`x`size` texture uniformly filled with one rgba16f texel. resolve.frag uses texelFetch with
/// the actual integer fragcoord (0..size), so the G-buffer must be full-resolution -- a 1x1 would read
/// out-of-bounds (returns 0) everywhere and discard the whole frame.
fn tex_full(device: &wgpu::Device, queue: &wgpu::Queue, size: u32, texel: &[u8], label: &str) -> wgpu::TextureView {
    let mut data = Vec::with_capacity((size * size) as usize * texel.len());
    for _ in 0..(size * size) { data.extend_from_slice(texel); }
    let t = device.create_texture_with_data(
        queue,
        &wgpu::TextureDescriptor {
            label: Some(label),
            size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
            format: SCENE_HDR, usage: wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
        },
        wgpu::util::TextureDataOrder::LayerMajor,
        &data,
    );
    t.create_view(&wgpu::TextureViewDescriptor::default())
}

/// Build resolve.frag's 60-float Sky UBO, filled to match fixture_sky_block(). Indices match the
/// `layout(...) uniform Sky` comments in resolve.frag exactly.
fn resolve_sky_ubo() -> [f32; 60] {
    let mut u = [0.0f32; 60];
    // u0-15 inv_view_proj: unused by the lit path (haze/pos = P2c). Leave zero.
    // u16-19: cam.xyz (unused), max_y
    u[19] = 1605.0;
    // u20-23: lightnorm.xyz (OGL Y-up), sun_up_factor
    u[20] = 0.0; u[21] = 0.707; u[22] = 0.707; u[23] = 1.0;
    // u24-27: sun_color.rgb, density_multiplier
    u[24] = 0.7342; u[25] = 0.7815; u[26] = 0.8999; u[27] = 0.0001;
    // u28-31: moon_color.rgb, sun_moon_glow_factor
    u[28] = 0.0; u[29] = 0.0; u[30] = 0.0; u[31] = 1.0;
    // u32-35: ambient.rgb, haze_density
    u[32] = 0.25; u[33] = 0.25; u[34] = 0.25; u[35] = 0.7;
    // u36-39: blue_horizon.rgb, haze_horizon
    u[36] = 0.4954; u[37] = 0.4954; u[38] = 0.6399; u[39] = 0.19;
    // u40-43: blue_density.rgb, cloud_shadow
    u[40] = 0.2447; u[41] = 0.4487; u[42] = 0.7599; u[43] = 0.2699;
    // u44-47: glow.xyz, sky_sunlight_scale
    u[44] = 5.0; u[45] = 0.001; u[46] = -0.4799; u[47] = 1.5;
    // u48-51: sky_hdr_scale, sky_ambient_scale, vp_w, vp_h
    u[48] = 1.0; u[49] = 1.5; u[50] = 256.0; u[51] = 256.0;
    // u52-55: sun_dir.xyz (world, SL Z-up), distance_multiplier
    u[52] = 0.0; u[53] = 0.707; u[54] = 0.707; u[55] = 0.8;
    // u56-59: moon_dir.xyz (world), classic_mode (w) -- matches scene.rs fullscreen_sky_ubo()
    // u[59] set by the caller per regime.
    u
}

pub struct ResolveResult {
    pub target: wgpu::Texture,
    pub view: wgpu::TextureView,
}

/// Compile + run resolve.frag on the equivalent fixture. `classic` is written into u56 (extra.x) so a
/// regime-aware resolve.frag can read it; the current resolve.frag ignores it (always non-classic).
pub fn render(device: &wgpu::Device, queue: &wgpu::Queue, size: u32, classic: bool) -> ResolveResult {
    let src_path = concat!(env!("CARGO_MANIFEST_DIR"), "/../fs_render/shaders/resolve.frag");
    let src = std::fs::read_to_string(src_path)
        .unwrap_or_else(|e| panic!("cannot read resolve.frag at {src_path}: {e}"));
    let compiler = shaderc::Compiler::new().unwrap();
    let mut opts = shaderc::CompileOptions::new().unwrap();
    opts.set_target_env(shaderc::TargetEnv::Vulkan, shaderc::EnvVersion::Vulkan1_2 as u32);
    let spv = compiler
        .compile_into_spirv(&src, shaderc::ShaderKind::Fragment, "resolve.frag", "main", Some(&opts))
        .unwrap_or_else(|e| panic!("resolve.frag compile failed:\n{e}"));
    let fmod = unsafe {
        device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV {
            label: Some("resolve.frag"),
            source: std::borrow::Cow::Borrowed(spv.as_binary()),
        })
    };

    // fixture G-buffer: PBR ground (flag 0.67, base 0.5 LINEAR), world normal (0,0,1) so
    // dot(n, sun_dir=(0,0.707,0.707)) = 0.707 = the oracle's nl.
    let albedo = tex_full(device, queue, size, &rgba16f(0.5, 0.5, 0.5, 0.67), "g_albedo");
    let normal = tex_full(device, queue, size, &rgba16f(0.0, 0.0, 1.0, 0.0), "g_normal");

    let mut ubo = resolve_sky_ubo();
    ubo[59] = if classic { 1.0 } else { 0.0 }; // classic_mode at moondir_classic.w (u59)
    let ubo_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("resolve-sky"),
        contents: bytemuck::cast_slice(&ubo),
        usage: wgpu::BufferUsages::UNIFORM,
    });

    let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("resolve-bgl"),
        entries: &[
            wgpu::BindGroupLayoutEntry { binding: 0, visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 1, visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: false }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 2, visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: false }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false }, count: None },
        ],
    });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("resolve-bind"), layout: &bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: ubo_buf.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&albedo) },
            wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(&normal) },
        ],
    });

    // fullscreen triangle (resolve.frag uses gl_FragCoord; any full-cover vertex works).
    let vsrc = r#"#version 450
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"#;
    let vspv = compiler.compile_into_spirv(vsrc, shaderc::ShaderKind::Vertex, "resolve.vert", "main", Some(&opts)).unwrap();
    let vmod = unsafe {
        device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV { label: Some("resolve.vert"), source: std::borrow::Cow::Borrowed(vspv.as_binary()) })
    };

    let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some("resolve-pl"), bind_group_layouts: &[&bgl], push_constant_ranges: &[] });
    let pipe = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("resolve-pipe"),
        layout: Some(&pl),
        vertex: wgpu::VertexState { module: &vmod, entry_point: "main", buffers: &[] },
        primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, ..Default::default() },
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module: &fmod, entry_point: "main",
            targets: &[Some(wgpu::ColorTargetState { format: SCENE_HDR, blend: None, write_mask: wgpu::ColorWrites::ALL })],
        }),
        multiview: None,
    });

    let target = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("resolve-scene-hdr"),
        size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
        mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
        format: SCENE_HDR, usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC, view_formats: &[],
    });
    let view = target.create_view(&wgpu::TextureViewDescriptor::default());

    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("resolve-enc") });
    {
        let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("resolve-rp"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &view, resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None,
        });
        rp.set_pipeline(&pipe);
        rp.set_bind_group(0, &bind, &[]);
        rp.draw(0..3, 0..1);
    }
    queue.submit([enc.finish()]);

    ResolveResult { target, view }
}
