//! PROBE GENERATION A/B: verify fs_render's convolution generators (radiance_gen.frag / irradiance_gen.frag)
//! against the REAL radianceGenF / irradianceGenF, in isolation. Both sides sample an identical source
//! cube array with an identical per-pixel direction (a shared fullscreen vertex), so the ONLY variable is
//! the fragment math + the wgpu transform (combined samplerCubeArray -> separate texture+sampler, loose
//! uniforms -> UBO). A direction-varying source cube (6 distinct face colors) makes the importance-sampled
//! DIRECTIONS observable; the uniforms (sourceIdx / mipLevel / max_probe_lod / u_width / probe_strength)
//! drive roughness + sample count + LOD, so a transform bug shows as a per-pixel divergence.

use wgpu::util::DeviceExt;

pub const HDR: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

#[derive(Clone, Copy, PartialEq)]
pub enum GenKind { Radiance, Irradiance }

fn f16(x: f32) -> [u8; 2] {
    let bits = x.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exp = ((bits >> 23) & 0xff) as i32 - 127 + 15;
    let mant = (bits >> 13) & 0x3ff;
    let h = if exp <= 0 { sign } else if exp >= 31 { sign | 0x7c00 } else { sign | ((exp as u16) << 10) | mant as u16 };
    h.to_le_bytes()
}
fn rgba16f(r: f32, g: f32, b: f32, a: f32) -> Vec<u8> { [f16(r), f16(g), f16(b), f16(a)].concat() }

/// Source cube array (1 cube, 6 faces, 1x1, single mip) with a distinct HDR color per face -- so the
/// importance-sampled directions actually change the prefilter output.
fn source_cube(device: &wgpu::Device, queue: &wgpu::Queue) -> wgpu::TextureView {
    let faces: [[f32; 3]; 6] = [
        [0.90, 0.20, 0.15], // +X
        [0.15, 0.60, 0.20], // -X
        [0.20, 0.35, 0.95], // +Y
        [0.85, 0.80, 0.20], // -Y
        [0.20, 0.80, 0.85], // +Z
        [0.80, 0.25, 0.75], // -Z
    ];
    let mut data = Vec::with_capacity(6 * 8);
    for c in faces { data.extend_from_slice(&rgba16f(c[0], c[1], c[2], 1.0)); }
    let t = device.create_texture_with_data(
        queue,
        &wgpu::TextureDescriptor {
            label: Some("gen-source-cube"),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 6 },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
            format: HDR, usage: wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
        },
        wgpu::util::TextureDataOrder::LayerMajor,
        &data,
    );
    t.create_view(&wgpu::TextureViewDescriptor { dimension: Some(wgpu::TextureViewDimension::CubeArray), ..Default::default() })
}

// A shared fullscreen-triangle vertex: vary_dir = (ndc.x, ndc.y, 1) -- a spread of directions around +Z.
// Identical on both sides, so the prefilter's per-pixel direction is apples-to-apples.
const GEN_VERT: &str = r#"#version 450
layout(location=0) out vec3 vary_dir;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vary_dir = vec3(p * 2.0 - 1.0, 1.0);
}
"#;

/// Assemble the REAL gen fragment (radianceGenF/irradianceGenF) the viewer's way: version + defines +
/// a marker (so ubo_transform injects the UBO block before the body) + source, then the loose-uniform ->
/// std140-UBO + combined-sampler -> separate transform, compiled to Vulkan SPIR-V.
fn oracle_gen_spirv(kind: GenKind) -> Vec<u32> {
    let (path, defines) = match kind {
        GenKind::Radiance => ("class1/interface/radianceGenF.glsl", "#define PROBE_FILTER_SAMPLES 32\n"),
        GenKind::Irradiance => ("class2/interface/irradianceGenF.glsl", ""),
    };
    let body = std::fs::read_to_string(format!("{}/{}", crate::shaders::SHADER_ROOT, path))
        .unwrap_or_else(|e| panic!("cannot read {path}: {e}"));
    let src = format!("#version 450\nprecision highp float;\n{defines}\n// ===== gen =====\n{body}");
    let (transformed, _members, _samplers) = crate::shaders::ubo_transform(&src);
    let compiler = shaderc::Compiler::new().unwrap();
    let mut opts = shaderc::CompileOptions::new().unwrap();
    opts.set_target_env(shaderc::TargetEnv::Vulkan, shaderc::EnvVersion::Vulkan1_2 as u32);
    opts.set_auto_map_locations(true);
    compiler.compile_into_spirv(&transformed, shaderc::ShaderKind::Fragment, "gen.oracle", "main", Some(&opts))
        .unwrap_or_else(|e| panic!("oracle gen compile failed:\n{e}\n---\n{transformed}"))
        .as_binary().to_vec()
}

/// std140 bytes for the oracle's transformed UBO (uniforms in source order).
/// Radiance:  int sourceIdx@0, float mipLevel@4, int u_width@8, float max_probe_lod@12, float probe_strength@16.
/// Irradiance: int sourceIdx@0, float max_probe_lod@4.
fn oracle_ubo_bytes(kind: GenKind, source_idx: i32, mip_level: f32, u_width: i32, max_probe_lod: f32, probe_strength: f32) -> Vec<u8> {
    let mut b = vec![0u8; 32];
    match kind {
        GenKind::Radiance => {
            b[0..4].copy_from_slice(&source_idx.to_le_bytes());
            b[4..8].copy_from_slice(&mip_level.to_le_bytes());
            b[8..12].copy_from_slice(&u_width.to_le_bytes());
            b[12..16].copy_from_slice(&max_probe_lod.to_le_bytes());
            b[16..20].copy_from_slice(&probe_strength.to_le_bytes());
        }
        GenKind::Irradiance => {
            b[0..4].copy_from_slice(&source_idx.to_le_bytes());
            b[4..8].copy_from_slice(&max_probe_lod.to_le_bytes());
        }
    }
    b
}

/// GenParams UBO bytes for OUR ported shaders (gen.vert layout): mat4 modelview (identity, frag ignores it)
/// then float mipLevel, float max_probe_lod, float probe_strength, int sourceIdx, int u_width.
fn ours_genparams_bytes(source_idx: i32, mip_level: f32, u_width: i32, max_probe_lod: f32, probe_strength: f32) -> Vec<u8> {
    let mut v: Vec<u8> = Vec::new();
    let id: [f32; 16] = [1.0,0.0,0.0,0.0, 0.0,1.0,0.0,0.0, 0.0,0.0,1.0,0.0, 0.0,0.0,0.0,1.0];
    for f in id { v.extend_from_slice(&f.to_le_bytes()); }
    v.extend_from_slice(&mip_level.to_le_bytes());
    v.extend_from_slice(&max_probe_lod.to_le_bytes());
    v.extend_from_slice(&probe_strength.to_le_bytes());
    v.extend_from_slice(&source_idx.to_le_bytes());
    v.extend_from_slice(&u_width.to_le_bytes());
    v
}

/// Render one gen frag (oracle or ours) into a `size`x`size` HDR target and return it.
pub fn render(device: &wgpu::Device, queue: &wgpu::Queue, size: u32, kind: GenKind, oracle: bool,
              mip_level: f32, max_probe_lod: f32) -> wgpu::Texture {
    let compiler = shaderc::Compiler::new().unwrap();
    let mut opts = shaderc::CompileOptions::new().unwrap();
    opts.set_target_env(shaderc::TargetEnv::Vulkan, shaderc::EnvVersion::Vulkan1_2 as u32);

    let cube = source_cube(device, queue);
    let samp = device.create_sampler(&wgpu::SamplerDescriptor {
        label: Some("gen-smp"), mag_filter: wgpu::FilterMode::Linear, min_filter: wgpu::FilterMode::Linear,
        mipmap_filter: wgpu::FilterMode::Linear, ..Default::default() });

    let vspv = compiler.compile_into_spirv(GEN_VERT, shaderc::ShaderKind::Vertex, "gen.vert", "main", Some(&opts)).unwrap();
    let vmod = unsafe { device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV { label: Some("gen.vert"), source: std::borrow::Cow::Borrowed(vspv.as_binary()) }) };

    // fragment + bindings differ per side.
    let (fmod, bgl, bind);
    if oracle {
        let spv = oracle_gen_spirv(kind);
        fmod = unsafe { device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV { label: Some("gen.oracle"), source: std::borrow::Cow::Borrowed(&spv) }) };
        // oracle bindings: 0 = UBO, 1 = cube tex, 2 = sampler.
        let ubo = device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: Some("gen-ubo"),
            contents: &oracle_ubo_bytes(kind, 0, mip_level, 64, max_probe_lod, 1.0), usage: wgpu::BufferUsages::UNIFORM });
        bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: Some("gen-bgl-o"), entries: &[
            wgpu::BindGroupLayoutEntry { binding: 0, visibility: wgpu::ShaderStages::FRAGMENT, ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 1, visibility: wgpu::ShaderStages::FRAGMENT, ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::CubeArray, multisampled: false }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 2, visibility: wgpu::ShaderStages::FRAGMENT, ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering), count: None },
        ]});
        bind = device.create_bind_group(&wgpu::BindGroupDescriptor { label: Some("gen-bind-o"), layout: &bgl, entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: ubo.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&cube) },
            wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(&samp) },
        ]});
        return finish_render(device, queue, size, &vmod, &fmod, &bgl, &bind);
    } else {
        let path = match kind { GenKind::Radiance => "/../fs_render/shaders/radiance_gen.frag", GenKind::Irradiance => "/../fs_render/shaders/irradiance_gen.frag" };
        let src = std::fs::read_to_string(format!("{}{}", env!("CARGO_MANIFEST_DIR"), path)).unwrap();
        let spv = compiler.compile_into_spirv(&src, shaderc::ShaderKind::Fragment, "gen.ours", "main", Some(&opts))
            .unwrap_or_else(|e| panic!("ours gen compile failed:\n{e}"));
        fmod = unsafe { device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV { label: Some("gen.ours"), source: std::borrow::Cow::Borrowed(spv.as_binary()) }) };
        // ours bindings: 0 = cube tex, 1 = sampler, 2 = GenParams UBO.
        let ubo = device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: Some("genparams"),
            contents: &ours_genparams_bytes(0, mip_level, 64, max_probe_lod, 1.0), usage: wgpu::BufferUsages::UNIFORM });
        bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: Some("gen-bgl-u"), entries: &[
            wgpu::BindGroupLayoutEntry { binding: 0, visibility: wgpu::ShaderStages::FRAGMENT, ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::CubeArray, multisampled: false }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 1, visibility: wgpu::ShaderStages::FRAGMENT, ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering), count: None },
            wgpu::BindGroupLayoutEntry { binding: 2, visibility: wgpu::ShaderStages::FRAGMENT, ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None },
        ]});
        bind = device.create_bind_group(&wgpu::BindGroupDescriptor { label: Some("gen-bind-u"), layout: &bgl, entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&cube) },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&samp) },
            wgpu::BindGroupEntry { binding: 2, resource: ubo.as_entire_binding() },
        ]});
        return finish_render(device, queue, size, &vmod, &fmod, &bgl, &bind);
    }
}

fn finish_render(device: &wgpu::Device, queue: &wgpu::Queue, size: u32, vmod: &wgpu::ShaderModule, fmod: &wgpu::ShaderModule, bgl: &wgpu::BindGroupLayout, bind: &wgpu::BindGroup) -> wgpu::Texture {
    let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some("gen-pl"), bind_group_layouts: &[bgl], push_constant_ranges: &[] });
    let pipe = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("gen-pipe"), layout: Some(&pl),
        vertex: wgpu::VertexState { module: vmod, entry_point: "main", buffers: &[] },
        primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, ..Default::default() },
        depth_stencil: None, multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState { module: fmod, entry_point: "main",
            targets: &[Some(wgpu::ColorTargetState { format: HDR, blend: None, write_mask: wgpu::ColorWrites::ALL })] }),
        multiview: None,
    });
    let target = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("gen-target"),
        size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
        mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
        format: HDR, usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC, view_formats: &[],
    });
    let view = target.create_view(&wgpu::TextureViewDescriptor::default());
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("gen-enc") });
    {
        let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor { label: Some("gen-rp"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment { view: &view, resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store } })],
            depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None });
        rp.set_pipeline(&pipe);
        rp.set_bind_group(0, bind, &[]);
        rp.draw(0..3, 0..1);
    }
    queue.submit([enc.finish()]);
    target
}
