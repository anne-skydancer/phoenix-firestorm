//! P2c — replay a P2b scene dump (fsscenedump.cpp, fs/scene-dump viewer branch) through
//! wgpu: `cargo run -- replay <dump-dir>`. Gate 1 = a RECOGNIZABLE sim frame from the
//! captured draw stream (unlit-textured: position + texcoord0 + diffuse texture + vertex
//! color, per-draw MVP, depth). Gate 2 (later) = SSIM vs the GL screenshot.
//!
//! Buffer layout: the viewer's SoA blocks (llvertexbuffer.cpp calcOffsets): per-attribute
//! CONTIGUOUS blocks in typemask bit order, each 16-byte aligned; TYPE_VERTEX is vec4
//! stride 16 (w = texture index). We bind the same wgpu buffer at three slots (pos, uv,
//! color) with per-slot offsets from the calcOffsets replica.

use glam::Mat4;
use std::collections::HashMap;

use crate::table_diff::{extract_i32, extract_str};

// LLVertexBuffer type order + sizes (llvertexbuffer.cpp:714, enum order in llvertexbuffer.h)
const TYPE_SIZES: [u32; 14] = [16, 16, 8, 8, 8, 8, 4, 4, 16, 4, 16, 16, 8, 16];
const TYPE_VERTEX: usize = 0;
const TYPE_TEXCOORD0: usize = 2;
const TYPE_COLOR: usize = 6;

/// calcOffsets replica: per-attribute block offsets for a typemask + vertex count.
fn calc_offsets(typemask: u32, num_verts: u32) -> [u32; 14] {
    let mut offsets = [u32::MAX; 14];
    let mut offset = 0u32;
    for i in 0..13 {
        // TYPE_TEXTURE_INDEX excluded from the loop (viewer: i < TYPE_TEXTURE_INDEX)
        if typemask & (1 << i) != 0 && TYPE_SIZES[i] > 0 {
            offsets[i] = offset;
            offset += TYPE_SIZES[i] * num_verts;
            offset = (offset + 0xF) & !0xF;
        }
    }
    offsets
}

struct Draw {
    mode: u32,
    count: u32,
    offset: u32,
    indexed: bool,
    typemask: u32,
    num_verts: u32,
    vb: String,
    ib: String,
    prog: String,
    tex0: u32,
    fbo: i32,
    depth_test: bool,
    depth_write: bool,
    blend: bool,
    mvp: Mat4,
}

fn parse_f32_array(line: &str, key: &str) -> Option<[f32; 16]> {
    let pat = format!("\"{key}\":[");
    let s = line.find(&pat)? + pat.len();
    let e = line[s..].find(']')? + s;
    let mut out = [0f32; 16];
    for (i, v) in line[s..e].split(',').enumerate() {
        if i < 16 {
            out[i] = v.trim().parse().ok()?;
        }
    }
    Some(out)
}

fn parse_draws(path: &std::path::Path) -> Vec<Draw> {
    let text = std::fs::read_to_string(path).unwrap_or_default();
    let mut out = Vec::new();
    for line in text.lines() {
        let (Some(mode), Some(count)) = (extract_i32(line, "mode"), extract_i32(line, "count")) else { continue };
        let mv = parse_f32_array(line, "mv").unwrap_or([0.0; 16]);
        let pj = parse_f32_array(line, "proj").unwrap_or([0.0; 16]);
        out.push(Draw {
            mode: mode as u32,
            count: count as u32,
            offset: extract_i32(line, "offset").unwrap_or(0) as u32,
            indexed: extract_i32(line, "indexed").unwrap_or(0) != 0,
            typemask: extract_i32(line, "typemask").unwrap_or(0) as u32,
            num_verts: extract_i32(line, "num_verts").unwrap_or(0) as u32,
            vb: extract_str(line, "vb").unwrap_or("").to_string(),
            ib: extract_str(line, "ib").unwrap_or("").to_string(),
            prog: extract_str(line, "prog").unwrap_or("?").to_string(),
            tex0: extract_i32(line, "tex0").unwrap_or(0) as u32,
            fbo: extract_i32(line, "fbo").unwrap_or(0),
            depth_test: extract_i32(line, "depth_test").unwrap_or(1) != 0,
            depth_write: extract_i32(line, "depth_write").unwrap_or(1) != 0,
            blend: extract_i32(line, "blend").unwrap_or(0) != 0,
            mvp: Mat4::from_cols_array(&pj) * Mat4::from_cols_array(&mv),
        });
    }
    out
}

/// Include-list heuristic for gate 1: world geometry into the G-buffer / forward passes.
/// Excludes shadow casters, glow extraction, UI, post. Tuned against real dump contents.
fn is_world_draw(d: &Draw) -> bool {
    let p = d.prog.to_lowercase();
    if p.contains("shadow") || p.contains("glow") || p.contains("ui") || p.contains("post")
        || p.contains("fxaa") || p.contains("smaa") || p.contains("soften") || p.contains("blur")
        || p.contains("exposure") || p.contains("tonemap") || p.contains("screen space")
        || p.contains("benchmark") || p.contains("splat") || p.contains("occlusion")
        || p.contains("downsample") || p.contains("combine") || p.contains("visibility")
    {
        return false;
    }
    // triangles only; must have geometry + a vertex block
    (d.mode == 4 || d.mode == 5) && d.num_verts > 0 && d.typemask & 1 != 0
}

const REPLAY_VERT: &str = r#"#version 450
layout(location = 0) in vec4 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(set = 0, binding = 0) uniform U { mat4 mvp; } u;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
    v_uv = uv;
    v_color = color;
    gl_Position = u.mvp * vec4(pos.xyz, 1.0);
}
"#;

const REPLAY_FRAG: &str = r#"#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(set = 0, binding = 1) uniform texture2D tex;
layout(set = 0, binding = 2) uniform sampler smp;
layout(location = 0) out vec4 frag;
void main() {
    vec4 t = texture(sampler2D(tex, smp), v_uv);
    frag = vec4(t.rgb * v_color.rgb, 1.0);
}
"#;

pub fn run_replay(dump_dir: &str) -> bool {
    let dir = std::path::Path::new(dump_dir);
    let draws = parse_draws(&dir.join("draws.jsonl"));
    if draws.is_empty() {
        log::error!("replay: no draws parsed from {dump_dir}/draws.jsonl");
        return false;
    }
    // meta: viewport for output size
    let meta = std::fs::read_to_string(dir.join("meta.json")).unwrap_or_default();
    let (w, h) = {
        let vp = parse_f32_array(&meta, "viewport").map(|v| (v[2] as u32, v[3] as u32));
        vp.filter(|&(w, h)| w > 63 && h > 63).unwrap_or((1280, 800))
    };
    // per-prog census (drives include-list tuning)
    let mut census: HashMap<String, usize> = HashMap::new();
    for d in &draws {
        *census.entry(d.prog.clone()).or_default() += 1;
    }
    let mut census: Vec<_> = census.into_iter().collect();
    census.sort_by(|a, b| b.1.cmp(&a.1));
    log::info!("replay: {} draws total; programs:", draws.len());
    for (p, n) in census.iter().take(18) {
        log::info!("  {:>5}  {}", n, p);
    }

    let world: Vec<&Draw> = draws.iter().filter(|d| is_world_draw(d)).collect();
    log::info!("replay: {} world draws selected (gate-1 include list)", world.len());
    if world.is_empty() {
        return false;
    }

    // wgpu setup
    let (device, queue) = crate::headless_vulkan_device();
    let vs = crate::glsl_module(&device, REPLAY_VERT, shaderc::ShaderKind::Vertex, "replay.vert");
    let fs = crate::glsl_module(&device, REPLAY_FRAG, shaderc::ShaderKind::Fragment, "replay.frag");

    let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("replay-bgl"),
        entries: &[
            wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX,
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
                ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                count: None,
            },
        ],
    });
    let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("replay-pl"),
        bind_group_layouts: &[&bgl],
        push_constant_ranges: &[],
    });
    // three slots over the same buffer: pos(vec4) / uv(vec2) / color(rgba8)
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
        wgpu::VertexBufferLayout {
            array_stride: 4,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 2, format: wgpu::VertexFormat::Unorm8x4 }],
        },
    ];
    let make_pipeline = |depth_test: bool, depth_write: bool, blend: bool| {
        device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("replay"),
            layout: Some(&pl),
            vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &vlayouts },
            fragment: Some(wgpu::FragmentState {
                module: &fs,
                entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: wgpu::TextureFormat::Rgba8UnormSrgb,
                    blend: if blend { Some(wgpu::BlendState::ALPHA_BLENDING) } else { Some(wgpu::BlendState::REPLACE) },
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: wgpu::TextureFormat::Depth32Float,
                depth_write_enabled: depth_write,
                depth_compare: if depth_test { wgpu::CompareFunction::LessEqual } else { wgpu::CompareFunction::Always },
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        })
    };
    // pipeline variants keyed (depth_test, depth_write, blend)
    let mut pipelines: HashMap<(bool, bool, bool), wgpu::RenderPipeline> = HashMap::new();

    // GPU resources: buffers by hash, textures by id
    use wgpu::util::DeviceExt;
    let mut gpu_bufs: HashMap<String, wgpu::Buffer> = HashMap::new();
    let mut load_buf = |name: &str, prefix: &str, usage: wgpu::BufferUsages, gpu_bufs: &mut HashMap<String, wgpu::Buffer>| -> bool {
        if name.is_empty() || gpu_bufs.contains_key(name) {
            return gpu_bufs.contains_key(name);
        }
        let p = dir.join(format!("{prefix}_{name}.bin"));
        match std::fs::read(&p) {
            Ok(mut bytes) => {
                while bytes.len() % 4 != 0 {
                    bytes.push(0);
                }
                let b = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some(prefix),
                    contents: &bytes,
                    usage,
                });
                gpu_bufs.insert(name.to_string(), b);
                true
            }
            Err(_) => false,
        }
    };
    // textures.json: id -> w,h
    let texmeta = std::fs::read_to_string(dir.join("textures.json")).unwrap_or_default();
    let white = device.create_texture_with_data(
        &queue,
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
    let white_view = white.create_view(&wgpu::TextureViewDescriptor::default());
    let mut tex_views: HashMap<u32, wgpu::TextureView> = HashMap::new();
    let mut load_tex = |id: u32, tex_views: &mut HashMap<u32, wgpu::TextureView>| {
        if id == 0 || tex_views.contains_key(&id) {
            return;
        }
        let pat = format!("\"{id}\":{{\"w\":");
        let Some(s) = texmeta.find(&pat).map(|p| p + pat.len()) else { return };
        let rest = &texmeta[s..];
        let tw: u32 = rest.split(|c: char| !c.is_ascii_digit()).next().and_then(|v| v.parse().ok()).unwrap_or(0);
        let hs = rest.find("\"h\":").map(|p| p + 4).unwrap_or(0);
        let th: u32 = rest[hs..].split(|c: char| !c.is_ascii_digit()).next().and_then(|v| v.parse().ok()).unwrap_or(0);
        if tw == 0 || th == 0 {
            return;
        }
        let Ok(bytes) = std::fs::read(dir.join(format!("tex_{id}.bin"))) else { return };
        if bytes.len() < (tw * th * 4) as usize {
            return;
        }
        let t = device.create_texture_with_data(
            &queue,
            &wgpu::TextureDescriptor {
                label: Some("dumptex"),
                size: wgpu::Extent3d { width: tw, height: th, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8UnormSrgb,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            },
            wgpu::util::TextureDataOrder::LayerMajor,
            &bytes[..(tw * th * 4) as usize],
        );
        tex_views.insert(id, t.create_view(&wgpu::TextureViewDescriptor::default()));
    };
    let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
        label: Some("replay-samp"),
        address_mode_u: wgpu::AddressMode::Repeat,
        address_mode_v: wgpu::AddressMode::Repeat,
        mag_filter: wgpu::FilterMode::Linear,
        min_filter: wgpu::FilterMode::Linear,
        mipmap_filter: wgpu::FilterMode::Nearest,
        ..Default::default()
    });

    // color + depth targets
    let color = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("replay-color"),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8UnormSrgb,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let color_view = color.create_view(&wgpu::TextureViewDescriptor::default());
    let depth = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("replay-depth"),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Depth32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
        view_formats: &[],
    });
    let depth_view = depth.create_view(&wgpu::TextureViewDescriptor::default());

    // preload resources + build per-draw bind groups
    let mut ready: Vec<(usize, wgpu::BindGroup, (bool, bool, bool))> = Vec::new();
    let mut skipped = 0usize;
    for (i, d) in world.iter().enumerate() {
        let vb_ok = load_buf(&d.vb, "vb", wgpu::BufferUsages::VERTEX, &mut gpu_bufs);
        let ib_ok = !d.indexed || load_buf(&d.ib, "ib", wgpu::BufferUsages::INDEX, &mut gpu_bufs);
        if !vb_ok || !ib_ok {
            skipped += 1;
            continue;
        }
        load_tex(d.tex0, &mut tex_views);
        let ubo = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("replay-ubo"),
            contents: bytemuck::cast_slice(&d.mvp.to_cols_array()),
            usage: wgpu::BufferUsages::UNIFORM,
        });
        let tv = tex_views.get(&d.tex0).unwrap_or(&white_view);
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("replay-bind"),
            layout: &bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: ubo.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(tv) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(&sampler) },
            ],
        });
        let key = (d.depth_test, d.depth_write, d.blend);
        pipelines.entry(key).or_insert_with(|| make_pipeline(key.0, key.1, key.2));
        ready.push((i, bind, key));
    }
    log::info!("replay: {} draws ready, {} skipped (missing blobs)", ready.len(), skipped);

    // encode
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("replay") });
    {
        let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("replay"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &color_view,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color { r: 0.25, g: 0.3, b: 0.4, a: 1.0 }),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                view: &depth_view,
                depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Store }),
                stencil_ops: None,
            }),
            timestamp_writes: None,
            occlusion_query_set: None,
        });
        for (i, bind, key) in &ready {
            let d = world[*i];
            let offsets = calc_offsets(d.typemask, d.num_verts);
            let vbuf = gpu_bufs.get(&d.vb).unwrap();
            rp.set_pipeline(pipelines.get(key).unwrap());
            rp.set_bind_group(0, bind, &[]);
            rp.set_vertex_buffer(0, vbuf.slice(offsets[TYPE_VERTEX] as u64..));
            let uv_off = if offsets[TYPE_TEXCOORD0] != u32::MAX { offsets[TYPE_TEXCOORD0] } else { offsets[TYPE_VERTEX] };
            rp.set_vertex_buffer(1, vbuf.slice(uv_off as u64..));
            let col_off = if offsets[TYPE_COLOR] != u32::MAX { offsets[TYPE_COLOR] } else { offsets[TYPE_VERTEX] };
            rp.set_vertex_buffer(2, vbuf.slice(col_off as u64..));
            if d.indexed {
                let ibuf = gpu_bufs.get(&d.ib).unwrap();
                rp.set_index_buffer(ibuf.slice(..), wgpu::IndexFormat::Uint16);
                rp.draw_indexed(d.offset..d.offset + d.count, 0, 0..1);
            } else {
                rp.draw(d.offset..d.offset + d.count, 0..1);
            }
        }
    }
    // readback -> PNG
    let bpr_raw = w * 4;
    let bpr = (bpr_raw + 255) & !255; // wgpu row alignment
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("replay-readback"),
        size: (bpr * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    enc.copy_texture_to_buffer(
        wgpu::ImageCopyTexture { texture: &color, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        wgpu::ImageCopyBuffer { buffer: &readback, layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(bpr), rows_per_image: Some(h) } },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
    queue.submit([enc.finish()]);
    let slice = readback.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| tx.send(r).unwrap());
    device.poll(wgpu::Maintain::Wait);
    rx.recv().unwrap().unwrap();
    let padded = slice.get_mapped_range().to_vec();
    readback.unmap();
    // strip row padding + flip vertically? GL viewport origin is bottom-left; our NDC handling
    // via MVP already matches GL clip space -- wgpu flips Y in viewport, so flip rows at output.
    let mut px = vec![0u8; (w * h * 4) as usize];
    for y in 0..h {
        let src = ((h - 1 - y) * bpr) as usize;
        let dst = (y * bpr_raw) as usize;
        px[dst..dst + bpr_raw as usize].copy_from_slice(&padded[src..src + bpr_raw as usize]);
    }
    let out = std::path::Path::new(dump_dir).join("replay.png");
    let file = std::fs::File::create(&out).expect("png");
    let mut e = png::Encoder::new(std::io::BufWriter::new(file), w, h);
    e.set_color(png::ColorType::Rgba);
    e.set_depth(png::BitDepth::Eight);
    e.write_header().unwrap().write_image_data(&px).unwrap();
    log::info!("replay: wrote {}x{} -> {}", w, h, out.display());
    true
}
