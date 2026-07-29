//! vkharness -- standalone Vulkan (wgpu) rendering harness for Firestorm-FSVulkan.
//!
//! H0 milestone: open a window, bring up a wgpu device on the **Vulkan** backend,
//! configure a swapchain, and clear the screen every frame (gently pulsing so the
//! frame loop is visibly alive). This is the first Vulkan pixel we own, decoupled
//! from the viewer -- the incubator for RenderBackendWGPU and the SPIR-V shader
//! testbed.
//!
//! Controls:  F = toggle fullscreen-borderless,  Esc = quit.
//!
//! Build/run:  cd indra/rust/vkharness && cargo run --release

use std::sync::Arc;

use wgpu::util::DeviceExt;
use winit::{
    event::{ElementState, Event, KeyEvent, WindowEvent},
    event_loop::EventLoop,
    keyboard::{KeyCode, PhysicalKey},
    window::{Fullscreen, Window, WindowBuilder},
};

/// A minimal position+color vertex (H1). POD so bytemuck can cast it into buffer bytes.
#[repr(C)]
#[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
struct Vertex {
    pos: [f32; 2],
    color: [f32; 3],
}

impl Vertex {
    fn layout() -> wgpu::VertexBufferLayout<'static> {
        wgpu::VertexBufferLayout {
            array_stride: std::mem::size_of::<Vertex>() as wgpu::BufferAddress,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &[
                wgpu::VertexAttribute {
                    offset: 0,
                    shader_location: 0,
                    format: wgpu::VertexFormat::Float32x2,
                },
                wgpu::VertexAttribute {
                    offset: std::mem::size_of::<[f32; 2]>() as wgpu::BufferAddress,
                    shader_location: 1,
                    format: wgpu::VertexFormat::Float32x3,
                },
            ],
        }
    }
}

const VERTICES: &[Vertex] = &[
    Vertex { pos: [0.0, 0.6], color: [1.0, 0.2, 0.2] },
    Vertex { pos: [-0.6, -0.6], color: [0.2, 1.0, 0.2] },
    Vertex { pos: [0.6, -0.6], color: [0.2, 0.4, 1.0] },
];

/// Inline WGSL for H1. H2 replaces this with viewer GLSL → shaderc → SPIR-V.
const TRIANGLE_WGSL: &str = r#"
struct VSOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) color: vec3<f32>,
};

@vertex
fn vs_main(@location(0) pos: vec2<f32>, @location(1) color: vec3<f32>) -> VSOut {
    var out: VSOut;
    out.pos = vec4<f32>(pos, 0.0, 1.0);
    out.color = color;
    return out;
}

@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
    return vec4<f32>(in.color, 1.0);
}
"#;

// --- H2: GLSL fed through shaderc -> SPIR-V -> wgpu -----------------------------
// A self-contained GLSL fullscreen-triangle background. This proves the shaderc
// -> SPIR-V -> wgpu path end-to-end; H2b points the same path at real viewer
// .glsl (which additionally needs #version injection + the shared-include
// assembly the viewer does in attachShaderFeatures).
const BG_VERT_GLSL: &str = r#"
#version 450
layout(location = 0) out vec2 vUV;
void main() {
    // Fullscreen triangle from gl_VertexIndex; no vertex buffer.
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"#;

const BG_FRAG_GLSL: &str = r#"
#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;
void main() {
    // A recognizable gradient so "GLSL ran under Vulkan" is unmistakable.
    fragColor = vec4(vUV.x * 0.20, vUV.y * 0.06, 0.16, 1.0);
}
"#;

/// Compile GLSL to SPIR-V via shaderc, targeting Vulkan (KEEP DescriptorSet/Binding
/// -- this is Phase 2's target-env, the opposite of the GL-only decoration strip).
fn compile_glsl(source: &str, kind: shaderc::ShaderKind, name: &str) -> Vec<u32> {
    let compiler = shaderc::Compiler::new().expect("shaderc compiler init");
    let mut opts = shaderc::CompileOptions::new().expect("shaderc options");
    opts.set_target_env(
        shaderc::TargetEnv::Vulkan,
        shaderc::EnvVersion::Vulkan1_2 as u32,
    );
    opts.set_source_language(shaderc::SourceLanguage::GLSL);
    let artifact = compiler
        .compile_into_spirv(source, kind, name, "main", Some(&opts))
        .unwrap_or_else(|e| panic!("GLSL->SPIR-V failed for {name}: {e}"));
    if artifact.get_num_warnings() > 0 {
        log::warn!("{name}: shaderc warnings: {}", artifact.get_warning_messages());
    }
    artifact.as_binary().to_vec()
}

/// Build a wgpu shader module from GLSL by way of SPIR-V.
fn glsl_module(
    device: &wgpu::Device,
    source: &str,
    kind: shaderc::ShaderKind,
    name: &str,
) -> wgpu::ShaderModule {
    let spv = compile_glsl(source, kind, name);
    device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some(name),
        source: wgpu::ShaderSource::SpirV(spv.into()),
    })
}

// --- H3a: uniform -> std140 UBO -> SPIR-V, known-answer readback ----------------
// Mechanism proof (H3_PLAN.md): prove a std140 UBO delivers its values intact
// through shaderc->SPIR-V->wgpu by rendering a shader whose 1px output IS the
// uniform values, and memcmp-ing against the hand-computed answer. Catches the
// FrameUBO burn class (compiled+linked but wrong VALUE) cheaply, pre-softenLight.
// Headless (no window/surface). Run with `cargo run -- h3a` (exit 0 = PASS).

const H3A_VERT_GLSL: &str = r#"
#version 450
void main() {
    // Fullscreen triangle; no varyings -> clean vs->fs interface.
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"#;

const H3A_FRAG_GLSL: &str = r#"
#version 450
layout(location = 0) out vec4 frag_color;
layout(set = 0, binding = 0, std140) uniform H3aBlock {
    float a_float;   // std140 offset 0
    int   a_int;     //             4
    vec3  a_vec3;    //            16 (16-aligned)
    mat4  a_mat4;    //            32 (col-major, 64B)
} u;
void main() {
    // Output IS the uniform values -> the framebuffer is a direct read of the UBO.
    frag_color = vec4(u.a_float, float(u.a_int), u.a_vec3.y, u.a_mat4[3][3]);
}
"#;

/// std140 mirror of H3aBlock. Explicit padding = exactly the layout rules a wrong
/// transform would get wrong -- that's the point of the test.
#[repr(C)]
#[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
struct H3aUbo {
    a_float: f32,          // 0
    a_int: i32,            // 4
    _pad0: [u32; 2],       // 8  -> align vec3 to 16
    a_vec3: [f32; 3],      // 16
    _pad1: u32,            // 28 -> fill vec3's 16B slot, align mat4
    a_mat4: [[f32; 4]; 4], // 32 -> 96 (each inner array = one column)
}

/// H3a: prove UBO value-delivery byte-exact against a known answer. Returns pass.
fn run_h3a() -> bool {
    pollster::block_on(async {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN,
            ..Default::default()
        });
        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::HighPerformance,
                compatible_surface: None, // headless
                force_fallback_adapter: false,
            })
            .await
            .expect("H3a: no Vulkan adapter");
        let info = adapter.get_info();
        log::info!(
            "H3a adapter: {} | backend={:?} | driver={} {}",
            info.name, info.backend, info.driver, info.driver_info
        );
        let (device, queue) = adapter
            .request_device(
                &wgpu::DeviceDescriptor {
                    label: Some("h3a-device"),
                    required_features: wgpu::Features::empty(),
                    required_limits: wgpu::Limits::default(),
                },
                None,
            )
            .await
            .expect("H3a: request_device");

        // Known uniform values -- all exactly representable in f32.
        let ubo = H3aUbo {
            a_float: 0.25,
            a_int: 7,
            _pad0: [0, 0],
            a_vec3: [1.0, 0.5, 0.0],
            _pad1: 0,
            a_mat4: [
                [0.0, 0.0, 0.0, 0.0],
                [0.0, 0.0, 0.0, 0.0],
                [0.0, 0.0, 0.0, 0.0],
                [0.0, 0.0, 0.0, 0.75], // column 3 -> a_mat4[3][3] == 0.75
            ],
        };
        let expected = [ubo.a_float, ubo.a_int as f32, ubo.a_vec3[1], ubo.a_mat4[3][3]];

        let ubo_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("h3a-ubo"),
            contents: bytemuck::bytes_of(&ubo),
            usage: wgpu::BufferUsages::UNIFORM,
        });
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("h3a-bgl"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("h3a-bg"),
            layout: &bgl,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: ubo_buf.as_entire_binding(),
            }],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("h3a-pl"),
            bind_group_layouts: &[&bgl],
            push_constant_ranges: &[],
        });

        let vs = glsl_module(&device, H3A_VERT_GLSL, shaderc::ShaderKind::Vertex, "h3a.vert");
        let fs = glsl_module(&device, H3A_FRAG_GLSL, shaderc::ShaderKind::Fragment, "h3a.frag");

        let fmt = wgpu::TextureFormat::Rgba32Float; // exact float storage -> clean memcmp
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("h3a-pipeline"),
            layout: Some(&pl),
            vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
            fragment: Some(wgpu::FragmentState {
                module: &fs,
                entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: fmt,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                ..Default::default()
            },
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });

        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("h3a-target"),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: fmt,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("h3a-readback"),
            size: 256, // >= 16B payload; bytes_per_row must be 256-aligned
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });

        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("h3a-enc") });
        {
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("h3a-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            rp.set_pipeline(&pipeline);
            rp.set_bind_group(0, &bind_group, &[]);
            rp.draw(0..3, 0..1);
        }
        enc.copy_texture_to_buffer(
            wgpu::ImageCopyTexture {
                texture: &tex,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::ImageCopyBuffer {
                buffer: &readback,
                layout: wgpu::ImageDataLayout {
                    offset: 0,
                    bytes_per_row: Some(256),
                    rows_per_image: Some(1),
                },
            },
            wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
        );
        queue.submit(std::iter::once(enc.finish()));

        let slice = readback.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
        device.poll(wgpu::Maintain::Wait);
        rx.recv().unwrap().expect("H3a: map_async failed");
        let got = {
            let data = slice.get_mapped_range();
            let px: &[f32] = bytemuck::cast_slice(&data[0..16]);
            [px[0], px[1], px[2], px[3]]
        };
        readback.unmap();

        let pass = got == expected;
        log::info!(
            "H3a UBO value-delivery: expected {:?}  got {:?}  -> {}",
            expected, got, if pass { "PASS" } else { "FAIL" }
        );
        pass
    })
}

// --- H3c: the TRANSFORM tier -- a mat4 UBO applied in the VERTEX stage -----------
// H3a proved value-delivery to a fragment shader. H3c proves the biggest tier
// (per-draw matrices; modelview_projection is in 59 shaders) by applying a mat4
// from a UBO in the VERTEX stage and reading the transformed point back as colour.
// Known answer: mvp * (1,2,3,1), cols [2,0,0,0][0,3,0,0][0,0,4,0][10,20,30,1]
// = 1*c0 + 2*c1 + 3*c2 + 1*c3 = [12,26,42,1]. Headless. `cargo run -- h3c`.

#[repr(C)]
#[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
struct TransformUbo {
    mvp: [[f32; 4]; 4], // column-major, std140 offset 0, 64B (each inner = a column)
}

const H3C_VERT_GLSL: &str = r#"
#version 450
layout(set = 0, binding = 0, std140) uniform T { mat4 mvp; } t;
layout(location = 0) out vec4 vP;
void main() {
    // fullscreen coverage so the 1px target is written
    vec2 fp = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(fp * 2.0 - 1.0, 0.0, 1.0);
    // the actual test: transform a known point by the UBO matrix (constant across
    // the triangle -> the fragment reads it exactly)
    vP = t.mvp * vec4(1.0, 2.0, 3.0, 1.0);
}
"#;

const H3C_FRAG_GLSL: &str = r#"
#version 450
layout(location = 0) in vec4 vP;
layout(location = 0) out vec4 frag_color;
void main() { frag_color = vP; }
"#;

/// H3c: prove a mat4 UBO is delivered + applied correctly in the vertex stage.
fn run_h3c() -> bool {
    pollster::block_on(async {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN,
            ..Default::default()
        });
        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::HighPerformance,
                compatible_surface: None,
                force_fallback_adapter: false,
            })
            .await
            .expect("H3c: no Vulkan adapter");
        let info = adapter.get_info();
        log::info!(
            "H3c adapter: {} | backend={:?} | driver={} {}",
            info.name, info.backend, info.driver, info.driver_info
        );
        let (device, queue) = adapter
            .request_device(
                &wgpu::DeviceDescriptor {
                    label: Some("h3c-device"),
                    required_features: wgpu::Features::empty(),
                    required_limits: wgpu::Limits::default(),
                },
                None,
            )
            .await
            .expect("H3c: request_device");

        let ubo = TransformUbo {
            mvp: [
                [2.0, 0.0, 0.0, 0.0],     // col0
                [0.0, 3.0, 0.0, 0.0],     // col1
                [0.0, 0.0, 4.0, 0.0],     // col2
                [10.0, 20.0, 30.0, 1.0],  // col3 (translation)
            ],
        };
        let expected = [12.0f32, 26.0, 42.0, 1.0]; // mvp * (1,2,3,1)

        let ubo_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("h3c-ubo"),
            contents: bytemuck::bytes_of(&ubo),
            usage: wgpu::BufferUsages::UNIFORM,
        });
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("h3c-bgl"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX, // matrix used in the vertex stage
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("h3c-bg"),
            layout: &bgl,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: ubo_buf.as_entire_binding() }],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("h3c-pl"),
            bind_group_layouts: &[&bgl],
            push_constant_ranges: &[],
        });
        let vs = glsl_module(&device, H3C_VERT_GLSL, shaderc::ShaderKind::Vertex, "h3c.vert");
        let fs = glsl_module(&device, H3C_FRAG_GLSL, shaderc::ShaderKind::Fragment, "h3c.frag");
        let fmt = wgpu::TextureFormat::Rgba32Float;
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("h3c-pipeline"),
            layout: Some(&pl),
            vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
            fragment: Some(wgpu::FragmentState {
                module: &fs,
                entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: fmt,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                ..Default::default()
            },
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("h3c-target"),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: fmt,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("h3c-readback"),
            size: 256,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("h3c-enc") });
        {
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("h3c-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            rp.set_pipeline(&pipeline);
            rp.set_bind_group(0, &bind_group, &[]);
            rp.draw(0..3, 0..1);
        }
        enc.copy_texture_to_buffer(
            wgpu::ImageCopyTexture {
                texture: &tex,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::ImageCopyBuffer {
                buffer: &readback,
                layout: wgpu::ImageDataLayout {
                    offset: 0,
                    bytes_per_row: Some(256),
                    rows_per_image: Some(1),
                },
            },
            wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
        );
        queue.submit(std::iter::once(enc.finish()));
        let slice = readback.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
        device.poll(wgpu::Maintain::Wait);
        rx.recv().unwrap().expect("H3c: map_async failed");
        let got = {
            let data = slice.get_mapped_range();
            let px: &[f32] = bytemuck::cast_slice(&data[0..16]);
            [px[0], px[1], px[2], px[3]]
        };
        readback.unmap();
        let pass = got == expected;
        log::info!(
            "H3c mat4-UBO transform (vertex stage): expected {:?}  got {:?}  -> {}",
            expected, got, if pass { "PASS" } else { "FAIL" }
        );
        pass
    })
}

// --- H3d: the ARRAY tier -- std140 element stride (the shadow_matrix[6] gotcha) --
// std140's finicky rule: EVERY array element is padded up to 16 bytes. A Rust
// [f32;4] is packed (stride 4); std140 float[4] is stride 16. Get the mirror wrong
// and the shader reads garbage -- exactly what would bite shadow_matrix[6]/light_*[]
// in H3b. Prove it in isolation: scalar array + vec4 array + mat4 array, known
// answer. Headless. `cargo run -- h3d`.

#[repr(C)]
#[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
struct ArrUbo {
    vals: [[f32; 4]; 4],        // std140 float[4]: each element in its own 16B slot
    cols: [[f32; 4]; 2],        // std140 vec4[2]: stride 16 (vec4 is natural)
    mats: [[[f32; 4]; 4]; 2],   // std140 mat4[2]: stride 64 (the shadow_matrix shape)
}

const H3D_FRAG_GLSL: &str = r#"
#version 450
layout(location = 0) out vec4 frag_color;
layout(set = 0, binding = 0, std140) uniform ArrBlock {
    float vals[4];   //   0..64   (stride 16)
    vec4  cols[2];   //  64..96   (stride 16)
    mat4  mats[2];   //  96..224  (stride 64)
} u;
void main() {
    // pull specific elements so a wrong stride reads wrong values
    frag_color = vec4(u.vals[3], u.cols[1].x, u.mats[1][3][3], u.mats[0][0][0]);
}
"#;

/// H3d: prove std140 array-element stride is delivered correctly.
fn run_h3d() -> bool {
    pollster::block_on(async {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN,
            ..Default::default()
        });
        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::HighPerformance,
                compatible_surface: None,
                force_fallback_adapter: false,
            })
            .await
            .expect("H3d: no Vulkan adapter");
        let info = adapter.get_info();
        log::info!(
            "H3d adapter: {} | backend={:?} | driver={} {}",
            info.name, info.backend, info.driver, info.driver_info
        );
        let (device, queue) = adapter
            .request_device(
                &wgpu::DeviceDescriptor {
                    label: Some("h3d-device"),
                    required_features: wgpu::Features::empty(),
                    required_limits: wgpu::Limits::default(),
                },
                None,
            )
            .await
            .expect("H3d: request_device");

        let z = [0.0f32; 4];
        let zm = [[0.0f32; 4]; 4];
        let ubo = ArrUbo {
            // scalar array: value lives in element[i][0], rest is std140 pad
            vals: [[0.25, 0.0, 0.0, 0.0], z, z, [0.75, 0.0, 0.0, 0.0]],
            cols: [z, [0.375, 0.0, 0.0, 0.0]], // cols[1].x = 0.375
            mats: [
                { let mut m = zm; m[0][0] = 0.125; m }, // mats[0][0][0] = 0.125
                { let mut m = zm; m[3][3] = 0.875; m }, // mats[1][3][3] = 0.875
            ],
        };
        let expected = [
            ubo.vals[3][0],   // u.vals[3]
            ubo.cols[1][0],   // u.cols[1].x
            ubo.mats[1][3][3],// u.mats[1][3][3]
            ubo.mats[0][0][0],// u.mats[0][0][0]
        ];

        let ubo_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("h3d-ubo"),
            contents: bytemuck::bytes_of(&ubo),
            usage: wgpu::BufferUsages::UNIFORM,
        });
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("h3d-bgl"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("h3d-bg"),
            layout: &bgl,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: ubo_buf.as_entire_binding() }],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("h3d-pl"),
            bind_group_layouts: &[&bgl],
            push_constant_ranges: &[],
        });
        let vs = glsl_module(&device, H3A_VERT_GLSL, shaderc::ShaderKind::Vertex, "h3d.vert");
        let fs = glsl_module(&device, H3D_FRAG_GLSL, shaderc::ShaderKind::Fragment, "h3d.frag");
        let fmt = wgpu::TextureFormat::Rgba32Float;
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("h3d-pipeline"),
            layout: Some(&pl),
            vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
            fragment: Some(wgpu::FragmentState {
                module: &fs,
                entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: fmt,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                ..Default::default()
            },
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("h3d-target"),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: fmt,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("h3d-readback"),
            size: 256,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("h3d-enc") });
        {
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("h3d-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            rp.set_pipeline(&pipeline);
            rp.set_bind_group(0, &bind_group, &[]);
            rp.draw(0..3, 0..1);
        }
        enc.copy_texture_to_buffer(
            wgpu::ImageCopyTexture {
                texture: &tex,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::ImageCopyBuffer {
                buffer: &readback,
                layout: wgpu::ImageDataLayout {
                    offset: 0,
                    bytes_per_row: Some(256),
                    rows_per_image: Some(1),
                },
            },
            wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
        );
        queue.submit(std::iter::once(enc.finish()));
        let slice = readback.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
        device.poll(wgpu::Maintain::Wait);
        rx.recv().unwrap().expect("H3d: map_async failed");
        let got = {
            let data = slice.get_mapped_range();
            let px: &[f32] = bytemuck::cast_slice(&data[0..16]);
            [px[0], px[1], px[2], px[3]]
        };
        readback.unmap();
        let pass = got == expected;
        log::info!(
            "H3d std140 array stride (float[]/vec4[]/mat4[]): expected {:?}  got {:?}  -> {}",
            expected, got, if pass { "PASS" } else { "FAIL" }
        );
        pass
    })
}

// --- H3a-view: the UBO, made visible (a calm, per-frame-driven picture) ----------
// H3a proved a STATIC UBO delivers its values. This renders a soft drifting field
// whose look is driven by a UBO *rewritten every frame* -- so it also exercises the
// per-frame-gather update path (what the FrameUBO burn's setter-interception got
// wrong), and it's simply nice to watch. `cargo run -- h3a-view`.

#[repr(C)]
#[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
struct ViewUbo {
    params: [f32; 4],  // x=time  (all-vec4 -> std140 offsets are trivially 0/16/32)
    color_a: [f32; 4],
    color_b: [f32; 4],
}

const H3A_VIEW_FRAG_GLSL: &str = r#"
#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 frag;
layout(set = 0, binding = 0, std140) uniform ViewBlock {
    vec4 params;   // x = time
    vec4 color_a;
    vec4 color_b;
} u;
void main() {
    float t = u.params.x;
    vec2 uv = vUV;
    // three slow, out-of-phase waves -> a soft drifting field, gently in [0,1]
    float a = sin(uv.x * 3.0 + t * 0.35)
            + sin(uv.y * 3.0 - t * 0.28)
            + sin((uv.x + uv.y) * 2.0 + t * 0.19);
    a = 0.5 + 0.16 * a;
    vec3 col = mix(u.color_a.rgb, u.color_b.rgb, a);
    // soft vignette so the edges settle into the dark
    vec2 c = uv - 0.5;
    col *= 1.0 - 0.35 * dot(c, c);
    frag = vec4(col, 1.0);
}
"#;

fn build_h3a_view(
    device: &wgpu::Device,
    format: wgpu::TextureFormat,
) -> (wgpu::RenderPipeline, wgpu::BindGroup, wgpu::Buffer) {
    let ubo = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("view-ubo"),
        size: std::mem::size_of::<ViewUbo>() as u64,
        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("view-bgl"),
        entries: &[wgpu::BindGroupLayoutEntry {
            binding: 0,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        }],
    });
    let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("view-bg"),
        layout: &bgl,
        entries: &[wgpu::BindGroupEntry { binding: 0, resource: ubo.as_entire_binding() }],
    });
    let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("view-pl"),
        bind_group_layouts: &[&bgl],
        push_constant_ranges: &[],
    });
    let vs = glsl_module(device, BG_VERT_GLSL, shaderc::ShaderKind::Vertex, "view.vert");
    let fs = glsl_module(device, H3A_VIEW_FRAG_GLSL, shaderc::ShaderKind::Fragment, "view.frag");
    let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("view-pipeline"),
        layout: Some(&pl),
        vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
        fragment: Some(wgpu::FragmentState {
            module: &fs,
            entry_point: "main",
            targets: &[Some(wgpu::ColorTargetState {
                format,
                blend: Some(wgpu::BlendState::REPLACE),
                write_mask: wgpu::ColorWrites::ALL,
            })],
        }),
        primitive: wgpu::PrimitiveState {
            topology: wgpu::PrimitiveTopology::TriangleList,
            ..Default::default()
        },
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        multiview: None,
    });
    (pipeline, bind_group, ubo)
}

struct State {
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    size: winit::dpi::PhysicalSize<u32>,
    window: Arc<Window>,
    frame: u64,
    pipeline: wgpu::RenderPipeline,
    vertex_buffer: wgpu::Buffer,
    bg_pipeline: wgpu::RenderPipeline, // H2: GLSL->SPIR-V fullscreen background
    view_mode: bool,                   // h3a-view: draw the UBO-driven field instead
    view_pipeline: wgpu::RenderPipeline,
    view_bind_group: wgpu::BindGroup,
    view_ubo: wgpu::Buffer,
}

impl State {
    async fn new(window: Arc<Window>, view_mode: bool) -> State {
        let size = window.inner_size();

        // Pin to the Vulkan backend -- the whole point of the harness.
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN,
            ..Default::default()
        });

        let surface = instance
            .create_surface(window.clone())
            .expect("create_surface failed");

        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::HighPerformance,
                compatible_surface: Some(&surface),
                force_fallback_adapter: false,
            })
            .await
            .expect("no Vulkan adapter found");

        let info = adapter.get_info();
        log::info!(
            "adapter: {} | type={:?} | backend={:?} | driver={} {}",
            info.name, info.device_type, info.backend, info.driver, info.driver_info
        );

        let (device, queue) = adapter
            .request_device(
                &wgpu::DeviceDescriptor {
                    label: Some("vkharness-device"),
                    required_features: wgpu::Features::empty(),
                    required_limits: wgpu::Limits::default(),
                },
                None,
            )
            .await
            .expect("request_device failed");

        let caps = surface.get_capabilities(&adapter);
        let format = caps
            .formats
            .iter()
            .copied()
            .find(|f| f.is_srgb())
            .unwrap_or(caps.formats[0]);

        let config = wgpu::SurfaceConfiguration {
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            format,
            width: size.width.max(1),
            height: size.height.max(1),
            present_mode: wgpu::PresentMode::Fifo, // vsync; swap to Mailbox for uncapped
            alpha_mode: caps.alpha_modes[0],
            view_formats: vec![],
            desired_maximum_frame_latency: 2,
        };
        surface.configure(&device, &config);

        log::info!("swapchain: {}x{} format={:?}", config.width, config.height, format);

        // --- H1: triangle pipeline (inline WGSL, single vertex buffer, no bind groups) ---
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("triangle-wgsl"),
            source: wgpu::ShaderSource::Wgsl(TRIANGLE_WGSL.into()),
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("triangle-layout"),
            bind_group_layouts: &[],
            push_constant_ranges: &[],
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("triangle-pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: "vs_main",
                buffers: &[Vertex::layout()],
            },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: "fs_main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: config.format,
                    blend: Some(wgpu::BlendState::REPLACE),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                ..Default::default()
            },
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });
        let vertex_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("triangle-vb"),
            contents: bytemuck::cast_slice(VERTICES),
            usage: wgpu::BufferUsages::VERTEX,
        });

        // --- H2: GLSL -> shaderc -> SPIR-V -> wgpu background pipeline ---
        let bg_vs = glsl_module(&device, BG_VERT_GLSL, shaderc::ShaderKind::Vertex, "bg.vert");
        let bg_fs = glsl_module(&device, BG_FRAG_GLSL, shaderc::ShaderKind::Fragment, "bg.frag");
        let bg_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("bg-pipeline"),
            layout: Some(&pipeline_layout), // reuse empty layout (no bind groups)
            vertex: wgpu::VertexState {
                module: &bg_vs,
                entry_point: "main",
                buffers: &[], // fullscreen triangle from gl_VertexIndex
            },
            fragment: Some(wgpu::FragmentState {
                module: &bg_fs,
                entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: config.format,
                    blend: Some(wgpu::BlendState::REPLACE),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                ..Default::default()
            },
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });
        log::info!("H2: GLSL->SPIR-V background pipeline built via shaderc");

        // H3a-view: UBO-driven fullscreen field, updated per frame.
        let (view_pipeline, view_bind_group, view_ubo) = build_h3a_view(&device, config.format);
        if view_mode {
            log::info!("H3a-view: UBO-driven field (per-frame UBO rewrite) -- Esc quits, F fullscreen");
        }

        State {
            surface,
            device,
            queue,
            config,
            size,
            window,
            frame: 0,
            pipeline,
            vertex_buffer,
            bg_pipeline,
            view_mode,
            view_pipeline,
            view_bind_group,
            view_ubo,
        }
    }

    fn resize(&mut self, new_size: winit::dpi::PhysicalSize<u32>) {
        if new_size.width > 0 && new_size.height > 0 {
            self.size = new_size;
            self.config.width = new_size.width;
            self.config.height = new_size.height;
            self.surface.configure(&self.device, &self.config);
        }
    }

    fn render(&mut self) -> Result<(), wgpu::SurfaceError> {
        self.frame = self.frame.wrapping_add(1);

        // Gentle pulse so the loop is obviously alive without any timing source.
        let t = (self.frame as f64) * 0.01;
        let pulse = 0.5 - 0.5 * t.cos();
        let clear = wgpu::Color { r: 0.02, g: 0.02 + 0.10 * pulse, b: 0.14, a: 1.0 };

        // h3a-view: rewrite the whole UBO every frame (per-frame gather), so the
        // picture on screen is a live read of a UBO we refill each tick.
        if self.view_mode {
            let t = self.frame as f32 * 0.016;
            let ubo = ViewUbo {
                params: [t, self.config.width as f32, self.config.height as f32, 0.0],
                color_a: [0.04, 0.06, 0.16, 1.0], // deep night blue
                color_b: [0.16, 0.34, 0.42, 1.0], // soft twilight teal
            };
            self.queue.write_buffer(&self.view_ubo, 0, bytemuck::bytes_of(&ubo));
        }

        let output = self.surface.get_current_texture()?;
        let view = output.texture.create_view(&wgpu::TextureViewDescriptor::default());
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("frame") });
        {
            let mut rpass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("main-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(clear),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            if self.view_mode {
                // h3a-view: the UBO-driven field, fullscreen.
                rpass.set_pipeline(&self.view_pipeline);
                rpass.set_bind_group(0, &self.view_bind_group, &[]);
                rpass.draw(0..3, 0..1);
            } else {
                // H2: GLSL-sourced fullscreen background (no vertex buffer), then
                // the H1 WGSL triangle on top -- both shader front-ends, one frame.
                rpass.set_pipeline(&self.bg_pipeline);
                rpass.draw(0..3, 0..1);

                rpass.set_pipeline(&self.pipeline);
                rpass.set_vertex_buffer(0, self.vertex_buffer.slice(..));
                rpass.draw(0..VERTICES.len() as u32, 0..1);
            }
        }
        self.queue.submit(std::iter::once(encoder.finish()));
        output.present();
        Ok(())
    }

    fn toggle_fullscreen(&self) {
        if self.window.fullscreen().is_some() {
            self.window.set_fullscreen(None);
        } else {
            self.window.set_fullscreen(Some(Fullscreen::Borderless(None)));
        }
    }
}

fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    // H3a: headless UBO value-delivery proof (H3_PLAN.md). `cargo run -- h3a`.
    if std::env::args().skip(1).any(|a| a == "h3a") {
        let ok = run_h3a();
        std::process::exit(if ok { 0 } else { 1 });
    }
    // H3c: mat4 UBO applied in the vertex stage (transform tier). `cargo run -- h3c`.
    if std::env::args().skip(1).any(|a| a == "h3c") {
        let ok = run_h3c();
        std::process::exit(if ok { 0 } else { 1 });
    }
    // H3d: std140 array-element stride (array tier). `cargo run -- h3d`.
    if std::env::args().skip(1).any(|a| a == "h3d") {
        let ok = run_h3d();
        std::process::exit(if ok { 0 } else { 1 });
    }

    // h3a-view: the same UBO path as the headless gate, but drawn to the window.
    let view_mode = std::env::args().skip(1).any(|a| a == "h3a-view");

    let event_loop = EventLoop::new().expect("event loop");
    let title = if view_mode {
        "vkharness -- H3a-view (UBO-driven, Vulkan)"
    } else {
        "vkharness -- Firestorm-FSVulkan (Vulkan)"
    };
    let window = Arc::new(
        WindowBuilder::new()
            .with_title(title)
            .with_inner_size(winit::dpi::LogicalSize::new(1280.0, 720.0))
            .build(&event_loop)
            .expect("window"),
    );

    let mut state = pollster::block_on(State::new(window.clone(), view_mode));

    event_loop
        .run(move |event, elwt| {
            elwt.set_control_flow(winit::event_loop::ControlFlow::Poll);
            match event {
                Event::WindowEvent { event, window_id } if window_id == state.window.id() => {
                    match event {
                        WindowEvent::CloseRequested => elwt.exit(),
                        WindowEvent::KeyboardInput {
                            event:
                                KeyEvent {
                                    physical_key: PhysicalKey::Code(code),
                                    state: ElementState::Pressed,
                                    ..
                                },
                            ..
                        } => match code {
                            KeyCode::Escape => elwt.exit(),
                            KeyCode::KeyF => state.toggle_fullscreen(),
                            _ => {}
                        },
                        WindowEvent::Resized(new_size) => state.resize(new_size),
                        WindowEvent::RedrawRequested => match state.render() {
                            Ok(()) => {}
                            Err(wgpu::SurfaceError::Lost | wgpu::SurfaceError::Outdated) => {
                                state.resize(state.size)
                            }
                            Err(wgpu::SurfaceError::OutOfMemory) => elwt.exit(),
                            Err(e) => log::warn!("surface error: {e:?}"),
                        },
                        _ => {}
                    }
                }
                Event::AboutToWait => state.window.request_redraw(),
                _ => {}
            }
        })
        .expect("event loop run");
}
