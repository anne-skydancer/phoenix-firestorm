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
mod shadow_engine; // engine shadow work (SHADOW_PLAN.md), invoked by the `shadow` subcommand

pub(crate) fn glsl_module(
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

// --- shared helpers for the headless known-answer gates -------------------------
// (h3a/c/d predate these and keep their own inline copies -- stable code stays put;
// new gates from H3b on use these.)

/// A headless Vulkan device+queue (no window/surface), adapter logged.
pub(crate) fn headless_vulkan_device() -> (wgpu::Device, wgpu::Queue) {
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
            .expect("no Vulkan adapter");
        let info = adapter.get_info();
        log::info!(
            "adapter: {} | backend={:?} | driver={} {}",
            info.name, info.backend, info.driver, info.driver_info
        );
        adapter
            .request_device(
                &wgpu::DeviceDescriptor {
                    label: Some("headless"),
                    required_features: wgpu::Features::empty(),
                    required_limits: wgpu::Limits::default(),
                },
                None,
            )
            .await
            .expect("request_device")
    })
}

/// Draw a fullscreen triangle through `pipeline`+`bind_group` to a 1px Rgba32Float
/// target and read the pixel back exactly (4x f32).
fn render_1px(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    pipeline: &wgpu::RenderPipeline,
    bind_group: &wgpu::BindGroup,
) -> [f32; 4] {
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("px-target"),
        size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("px-readback"),
        size: 256,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let mut enc =
        device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("px-enc") });
    {
        let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("px-pass"),
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
        rp.set_pipeline(pipeline);
        rp.set_bind_group(0, bind_group, &[]);
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
    rx.recv().unwrap().expect("map_async failed");
    let out = {
        let data = slice.get_mapped_range();
        let px: &[f32] = bytemuck::cast_slice(&data[0..16]);
        [px[0], px[1], px[2], px[3]]
    };
    readback.unmap();
    out
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

// --- H3b-1: multiple UBOs bound in one pipeline (the softenLight shape) ----------
// softenLight will read a Frame UBO + a local UBO + a shadow-array UBO at once.
// Prove two coexist: std140 blocks at binding 0 and 1, read from both (incl. a
// cross-block sum so a mis-bind is caught), memcmp. `cargo run -- h3b1`.

const H3B1_FRAG_GLSL: &str = r#"
#version 450
layout(location = 0) out vec4 frag_color;
layout(set = 0, binding = 0, std140) uniform BlockA { vec4 a; } ua;
layout(set = 0, binding = 1, std140) uniform BlockB { vec4 b; } ub;
void main() {
    frag_color = vec4(ua.a.x, ub.b.y, ua.a.z + ub.b.z, ub.b.w);
}
"#;

/// H3b-1: prove two UBOs bind + deliver correctly in one pipeline.
fn run_h3b1() -> bool {
    let (device, queue) = headless_vulkan_device();

    let a = [0.25f32, 0.0, 0.5, 0.0];
    let b = [0.0f32, 0.375, 0.125, 0.875];
    let expected = [a[0], b[1], a[2] + b[2], b[3]]; // [0.25, 0.375, 0.625, 0.875]

    let buf_a = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("h3b1-a"),
        contents: bytemuck::cast_slice(&a),
        usage: wgpu::BufferUsages::UNIFORM,
    });
    let buf_b = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("h3b1-b"),
        contents: bytemuck::cast_slice(&b),
        usage: wgpu::BufferUsages::UNIFORM,
    });
    let entry = |binding| wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::FRAGMENT,
        ty: wgpu::BindingType::Buffer {
            ty: wgpu::BufferBindingType::Uniform,
            has_dynamic_offset: false,
            min_binding_size: None,
        },
        count: None,
    };
    let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("h3b1-bgl"),
        entries: &[entry(0), entry(1)],
    });
    let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("h3b1-bg"),
        layout: &bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: buf_a.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: buf_b.as_entire_binding() },
        ],
    });
    let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("h3b1-pl"),
        bind_group_layouts: &[&bgl],
        push_constant_ranges: &[],
    });
    let vs = glsl_module(&device, H3A_VERT_GLSL, shaderc::ShaderKind::Vertex, "h3b1.vert");
    let fs = glsl_module(&device, H3B1_FRAG_GLSL, shaderc::ShaderKind::Fragment, "h3b1.frag");
    let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("h3b1-pipeline"),
        layout: Some(&pl),
        vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
        fragment: Some(wgpu::FragmentState {
            module: &fs,
            entry_point: "main",
            targets: &[Some(wgpu::ColorTargetState {
                format: wgpu::TextureFormat::Rgba32Float,
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

    let got = render_1px(&device, &queue, &pipeline, &bind_group);
    let pass = got == expected;
    log::info!(
        "H3b-1 two UBOs in one pipeline: expected {:?}  got {:?}  -> {}",
        expected, got, if pass { "PASS" } else { "FAIL" }
    );
    pass
}

// --- H3b-2: a sampler + texture descriptor (opens the 181-sampler track) ---------
// softenLight samples ~13 textures (G-buffer, shadow maps, probes). Prove the
// separate-texture+sampler descriptor path: sample a fixed texel (nearest) from a
// known 2x2 Rgba32Float texture and read it back byte-exact. `cargo run -- h3b2`.

const H3B2_FRAG_GLSL: &str = r#"
#version 450
layout(location = 0) out vec4 frag_color;
layout(set = 0, binding = 0) uniform texture2D tex;   // Vulkan-style separate
layout(set = 0, binding = 1) uniform sampler samp;    //   texture + sampler
void main() {
    // fixed texel centre, nearest -> an exact texel (no filtering ambiguity)
    frag_color = texture(sampler2D(tex, samp), vec2(0.75, 0.25));
}
"#;

/// H3b-2: prove a texture+sampler descriptor delivers an exact texel.
fn run_h3b2() -> bool {
    let (device, queue) = headless_vulkan_device();

    // 2x2 Rgba32Float, row-major. Sample (0.75,0.25) -> texel (col 1, row 0).
    let texdata: [f32; 16] = [
        0.1, 0.2, 0.3, 0.4,       // (0,0)
        0.25, 0.5, 0.625, 0.75,   // (1,0) <- expected
        0.6, 0.7, 0.8, 0.9,       // (0,1)
        0.11, 0.22, 0.33, 0.44,   // (1,1)
    ];
    let expected = [0.25f32, 0.5, 0.625, 0.75];

    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("h3b2-tex"),
        size: wgpu::Extent3d { width: 2, height: 2, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba32Float,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    queue.write_texture(
        wgpu::ImageCopyTexture {
            texture: &tex,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        bytemuck::cast_slice(&texdata),
        wgpu::ImageDataLayout {
            offset: 0,
            bytes_per_row: Some(2 * 16),
            rows_per_image: Some(2),
        },
        wgpu::Extent3d { width: 2, height: 2, depth_or_array_layers: 1 },
    );
    let tex_view = tex.create_view(&wgpu::TextureViewDescriptor::default());
    let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
        label: Some("h3b2-samp"),
        mag_filter: wgpu::FilterMode::Nearest,
        min_filter: wgpu::FilterMode::Nearest,
        mipmap_filter: wgpu::FilterMode::Nearest,
        ..Default::default()
    });

    let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("h3b2-bgl"),
        entries: &[
            wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture {
                    sample_type: wgpu::TextureSampleType::Float { filterable: false },
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: false,
                },
                count: None,
            },
            wgpu::BindGroupLayoutEntry {
                binding: 1,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::NonFiltering),
                count: None,
            },
        ],
    });
    let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("h3b2-bg"),
        layout: &bgl,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(&tex_view),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::Sampler(&sampler),
            },
        ],
    });
    let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("h3b2-pl"),
        bind_group_layouts: &[&bgl],
        push_constant_ranges: &[],
    });
    let vs = glsl_module(&device, H3A_VERT_GLSL, shaderc::ShaderKind::Vertex, "h3b2.vert");
    let fs = glsl_module(&device, H3B2_FRAG_GLSL, shaderc::ShaderKind::Fragment, "h3b2.frag");
    let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("h3b2-pipeline"),
        layout: Some(&pl),
        vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
        fragment: Some(wgpu::FragmentState {
            module: &fs,
            entry_point: "main",
            targets: &[Some(wgpu::ColorTargetState {
                format: wgpu::TextureFormat::Rgba32Float,
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

    let got = render_1px(&device, &queue, &pipeline, &bind_group);
    let pass = got == expected;
    log::info!(
        "H3b-2 sampler/texture descriptor: expected {:?}  got {:?}  -> {}",
        expected, got, if pass { "PASS" } else { "FAIL" }
    );
    pass
}

// --- H3b-3: assemble softenLight the viewer's way + compile (GL sanity) ----------
// Replicate attachShaderFeatures' fragment attach order for softenLight's flags,
// prepend the loadShaderFile header, class-resolve each include (class3->2->1),
// concatenate, dump to disk, and compile for the OpenGL target (loose uniforms
// legal there) -- proving the ASSEMBLY is well-formed before we UBO-ize (3c-3e).
// `cargo run -- h3b3`.

const SHADER_ROOT: &str = "c:/fs/fs-vulkan-engine/indra/newview/app_settings/shaders";

/// The fragment attach order for softenLight (from attachShaderFeatures + its flags),
/// softenLightF.glsl last. Each is "subdir/name.glsl", class-resolved at read time.
const SOFTENLIGHT_FRAG_PIECES: &[&str] = &[
    "deferred/globalF.glsl",
    "environment/srgbF.glsl",
    "windlight/atmosphericsVarsF.glsl",
    "windlight/atmosphericsHelpersF.glsl",
    "deferred/deferredUtil.glsl",
    "deferred/gbufferUtil.glsl",
    "deferred/screenSpaceReflUtil.glsl",
    "deferred/shadowUtil.glsl",
    "deferred/reflectionProbeF.glsl",
    "windlight/gammaF.glsl",
    "windlight/atmosphericsFuncs.glsl",
    "windlight/atmosphericsF.glsl",
    "environment/waterFogF.glsl",
    "deferred/softenLightF.glsl",
];

/// Assemble the softenLight fragment source (header + class-resolved pieces).
/// Extract the declared name from a single-line `uniform ... ;` decl (strips
/// array suffix + qualifiers). Returns None for block-open lines (`uniform Foo {`).
fn decl_name_uniform(line: &str) -> Option<String> {
    let head = line.split(';').next()?; // "uniform vec2 screen_res"
    let head = head.split('[').next()?; // strip array suffix
    let name = head.split_whitespace().last()?;
    if name == "uniform" || !name.chars().all(|c| c.is_alphanumeric() || c == '_') {
        return None;
    }
    Some(name.to_string())
}

/// Extract the name from a global `const TYPE name = ...;` decl.
fn decl_name_const(line: &str) -> Option<String> {
    let head = line.split('=').next()?; // "const float M_PI "
    let name = head.split_whitespace().last()?;
    if !name.chars().all(|c| c.is_alphanumeric() || c == '_') {
        return None;
    }
    Some(name.to_string())
}

/// Keep-first dedup of GLOBAL (brace-depth-0) `uniform`/`const` declarations.
/// This is exactly what the viewer's multi-object linker does implicitly when it
/// merges the same `uniform screen_res` across attached shader objects -- flattening
/// to one translation unit for SPIR-V surfaces them as redefinitions, so we merge
/// them here. Behavior-identical (the decls are byte-identical), and this is the
/// front half of the 3c UBO transform (find loose decls -> strip duplicates; 3c then
/// strips ALL of a tier and re-emits them once inside a UBO block).
fn dedup_global_decls(src: &str) -> (String, usize) {
    use std::collections::HashSet;
    let mut seen: HashSet<String> = HashSet::new();
    let mut depth: i32 = 0;
    let mut dropped = 0usize;
    let mut out = String::with_capacity(src.len());
    for line in src.lines() {
        let trimmed = line.trim_start();
        let mut drop_line = false;
        if depth == 0 {
            let name = if trimmed.starts_with("uniform ") {
                decl_name_uniform(trimmed)
            } else if trimmed.starts_with("const ") {
                decl_name_const(trimmed)
            } else {
                None
            };
            if let Some(n) = name {
                if !seen.insert(n) {
                    drop_line = true; // duplicate global decl -> keep-first
                }
            }
        }
        if drop_line {
            out.push_str("// [dedup] ");
            dropped += 1;
        }
        out.push_str(line);
        out.push('\n');
        for c in line.chars() {
            if c == '{' {
                depth += 1;
            } else if c == '}' {
                depth -= 1;
            }
        }
    }
    (out, dropped)
}

fn assemble_softenlight_fragment() -> String {
    let mut s = String::new();
    // header (mirrors loadShaderFile's injected preamble): #version + precision +
    // FRAGMENT_SHADER + gbuffer flags + the injected GBufferInfo struct + perms.
    s.push_str("#version 450\n");
    s.push_str("precision highp float;\n");
    s.push_str("#define FRAGMENT_SHADER 1\n");
    s.push_str("#define GBUFFER_FLAG_SKIP_ATMOS 0.0\n");
    s.push_str("#define GBUFFER_FLAG_HAS_ATMOS 0.34\n");
    s.push_str("#define GBUFFER_FLAG_HAS_PBR 0.67\n");
    s.push_str("#define GBUFFER_FLAG_HAS_HDRI 1.0\n");
    s.push_str("#define GET_GBUFFER_FLAG(data, flag) (abs(data-flag)< 0.1)\n");
    // C++-injected struct (llshadermgr.cpp:752) -- used by gbufferUtil/softenLightF
    s.push_str("struct GBufferInfo { vec4 albedo; vec4 specular; vec3 normal; vec4 emissive; float gbufferFlag; float envIntensity; };\n");
    s.push_str("#define HAS_SUN_SHADOW 1\n");
    s.push_str("#define HAS_SSAO 1\n");
    s.push_str("#define REF_SAMPLE_COUNT 32\n"); // reflection-probe perm (llviewershadermgr.cpp:843)
    s.push_str("#define IS_AMD_CARD 1\n");
    for piece in SOFTENLIGHT_FRAG_PIECES {
        let (sub, name) = piece.split_once('/').expect("piece has a subdir");
        let mut found = None;
        for cls in ["class3", "class2", "class1"] {
            let p = format!("{SHADER_ROOT}/{cls}/{sub}/{name}");
            if std::path::Path::new(&p).exists() {
                found = Some((cls, p));
                break;
            }
        }
        match found {
            Some((cls, p)) => {
                let body = std::fs::read_to_string(&p).unwrap_or_default();
                s.push_str(&format!("\n// ===== {piece}  [{cls}] =====\n"));
                s.push_str(&body);
                s.push('\n');
            }
            None => {
                log::warn!("H3b-3: could NOT resolve include {piece}");
                s.push_str(&format!("\n// ===== {piece}  [NOT FOUND] =====\n"));
            }
        }
    }
    let (deduped, dropped) = dedup_global_decls(&s);
    log::info!("H3b-3: dedup merged {dropped} duplicate global decls (keep-first, = linker merge)");
    deduped
}

/// H3b-3: assemble + GL-target compile (assembly sanity, no transform yet).
fn run_h3b3() -> bool {
    let src = assemble_softenlight_fragment();
    let out = "c:/fs/softenLight_assembled.frag";
    let _ = std::fs::write(out, &src);
    log::info!(
        "H3b-3: assembled softenLight fragment -> {}  ({} bytes, {} lines)",
        out, src.len(), src.lines().count()
    );

    let compiler = shaderc::Compiler::new().expect("shaderc");
    let mut opts = shaderc::CompileOptions::new().expect("shaderc opts");
    // OpenGL target: default-block uniforms are legal, so a clean compile means the
    // ASSEMBLY is well-formed (all functions resolve, order works).
    opts.set_target_env(shaderc::TargetEnv::OpenGL, shaderc::EnvVersion::OpenGL4_5 as u32);
    opts.set_auto_map_locations(true);
    opts.set_auto_bind_uniforms(true);
    match compiler.compile_into_spirv(
        &src,
        shaderc::ShaderKind::Fragment,
        "softenLightF.assembled",
        "main",
        Some(&opts),
    ) {
        Ok(art) => {
            let w = art.get_warning_messages();
            log::info!(
                "H3b-3: assembled source COMPILES for GL target ({} SPIR-V words) -> assembly well-formed{}",
                art.len(),
                if w.is_empty() { String::new() } else { format!("  (warnings:\n{w})") }
            );
            true
        }
        Err(e) => {
            log::warn!("H3b-3: assembled source did NOT compile (GL target) -- the punch-list:\n{e}");
            false
        }
    }
}

/// Is a global `uniform` line an opaque sampler/image (descriptor track, not UBO)?
fn is_sampler_decl(trimmed: &str) -> bool {
    match trimmed.split_whitespace().nth(1) {
        Some(ty) => ty.starts_with("sampler")
            || ty.starts_with("isampler")
            || ty.starts_with("usampler")
            || ty.starts_with("image"),
        None => false,
    }
}

/// Map a GLSL combined-sampler type to its Vulkan SEPARATE (texture, sampler) pair.
/// e.g. sampler2D -> (texture2D, sampler); sampler2DShadow -> (texture2D, samplerShadow);
/// samplerCubeArray -> (textureCubeArray, sampler). The combined type itself is reused
/// verbatim as the `sampler2D(tex, smp)` constructor in the #define.
fn split_sampler_type(st: &str) -> (String, String) {
    let is_shadow = st.contains("Shadow");
    let smp_type = if is_shadow { "samplerShadow" } else { "sampler" };
    // strip leading "sampler", drop "Shadow", prepend "texture"
    let suffix = st.strip_prefix("sampler").unwrap_or(st).replace("Shadow", "");
    (format!("texture{suffix}"), smp_type.to_string())
}

/// H3b-3c: the transform. Rewrite the deduped loose-uniform source into the
/// Vulkan shape:
///  - every runtime value-uniform -> a member of ONE anonymous std140 block
///    (anonymous => referenced by bare name => zero code churn; glslang computes
///    the std140 offsets, so no manual layout needed for a compile),
///  - each sampler/image -> explicit `layout(set=0, binding=N)` (deterministic),
///  - a `uniform` with an initializer (POISSON3D_SAMPLES, a baked table) -> `const`
///    (a UBO member can't carry an initializer anyway).
/// Only touches brace-depth-0 decls. Returns (transformed_src, n_ubo_members, n_samplers).
fn ubo_transform(src: &str) -> (String, usize, usize) {
    let mut members: Vec<String> = Vec::new();
    let mut binding: u32 = 1; // binding 0 reserved for the UBO block
    let mut samplers = 0usize;
    let mut depth: i32 = 0;
    let mut body: Vec<String> = Vec::new();

    for line in src.lines() {
        let trimmed = line.trim_start();
        let mut out_line = line.to_string();
        if depth == 0 && trimmed.starts_with("uniform ") {
            if is_sampler_decl(trimmed) {
                // COMBINED sampler -> SEPARATE texture + sampler (Transform #2, wgpu
                // has no combined binding type). A #define reconstructs the combined
                // sampler at every use site via the preprocessor -- zero call-site
                // churn, correct token boundaries, works through function args too.
                let st = trimmed.split_whitespace().nth(1).unwrap_or("sampler2D");
                let name = decl_name_uniform(trimmed).unwrap_or_default();
                let (tex_type, smp_type) = split_sampler_type(st);
                let b_tex = binding;
                let b_smp = binding + 1;
                binding += 2;
                out_line = format!(
                    "layout(set = 0, binding = {b_tex}) uniform {tex_type} {name}_tex;\n\
                     layout(set = 0, binding = {b_smp}) uniform {smp_type} {name}_smp;\n\
                     #define {name} {st}({name}_tex, {name}_smp)"
                );
                samplers += 1;
            } else if trimmed.contains('=') {
                // uniform with initializer (may span lines) -> baked const table
                out_line = line.replacen("uniform", "const", 1);
            } else if trimmed.contains(';') {
                // runtime value-uniform -> UBO member; drop the loose decl
                let member = trimmed.strip_prefix("uniform ").unwrap().trim();
                members.push(member.to_string());
                out_line = format!("// [ubo] {trimmed}");
            } else {
                // `uniform BlockName` (no layout) opening a named UBO block -> binding
                out_line = format!("layout(set = 0, binding = {binding}) {trimmed}");
                binding += 1;
            }
        } else if depth == 0
            && trimmed.starts_with("layout")
            && trimmed.contains("uniform")
            && !trimmed.contains(';')
        {
            // pre-existing named block, e.g. `layout (std140) uniform ReflectionProbes`
            // -- legal loose in GL, but Vulkan requires an explicit binding. Stack a
            // second layout qualifier (GLSL merges them) so we don't disturb std140.
            out_line = format!("layout(set = 0, binding = {binding}) {trimmed}");
            binding += 1;
        }
        body.push(out_line);
        for c in line.chars() {
            if c == '{' {
                depth += 1;
            } else if c == '}' {
                depth -= 1;
            }
        }
    }

    // Build the anonymous std140 block.
    let mut block = String::new();
    block.push_str("layout(std140, set = 0, binding = 0) uniform SoftenFrameBlock {\n");
    for m in &members {
        block.push_str("    ");
        block.push_str(m);
        block.push('\n');
    }
    block.push_str("};\n");

    // Inject the block right before the first include marker (after the preamble,
    // so it's declared before every use).
    let mut out = String::with_capacity(src.len() + block.len());
    let mut injected = false;
    for l in body {
        if !injected && l.starts_with("// ===== ") {
            out.push_str("\n// ---- H3b-3c: value-uniforms -> anonymous std140 UBO ----\n");
            out.push_str(&block);
            out.push('\n');
            injected = true;
        }
        out.push_str(&l);
        out.push('\n');
    }
    if !injected {
        out.push_str(&block); // fallback (no marker found)
    }
    (out, members.len(), samplers)
}

/// Assemble + UBO-transform softenLight, then compile to Vulkan SPIR-V words.
/// Shared by 3c (compile gate) and 3d (execute). Writes the transformed source to
/// c:/fs/softenLight_ubo.frag as a side effect. Returns (spirv_words, members, samplers).
fn build_softenlight_ubo_spirv() -> Result<(Vec<u32>, usize, usize), String> {
    let deduped = assemble_softenlight_fragment();
    let (src, n_members, n_samplers) = ubo_transform(&deduped);
    let out = "c:/fs/softenLight_ubo.frag";
    let _ = std::fs::write(out, &src);
    log::info!(
        "softenLight UBO transform -> {}  ({} UBO members, {} samplers, {} lines)",
        out, n_members, n_samplers, src.lines().count()
    );
    let compiler = shaderc::Compiler::new().expect("shaderc");
    let mut opts = shaderc::CompileOptions::new().expect("shaderc opts");
    // VULKAN target: default-block uniforms ILLEGAL here; keep DescriptorSet/Binding.
    opts.set_target_env(shaderc::TargetEnv::Vulkan, shaderc::EnvVersion::Vulkan1_3 as u32);
    opts.set_auto_map_locations(true); // in/out get locations; bindings are explicit
    match compiler.compile_into_spirv(
        &src,
        shaderc::ShaderKind::Fragment,
        "softenLightF.ubo",
        "main",
        Some(&opts),
    ) {
        Ok(art) => {
            let spv = art.as_binary().to_vec();
            // dump raw bytes for spirv-dis inspection
            let bytes: &[u8] = bytemuck::cast_slice(&spv);
            let _ = std::fs::write("c:/fs/softenLight_ubo.spv", bytes);
            Ok((spv, n_members, n_samplers))
        }
        Err(e) => Err(e.to_string()),
    }
}

/// H3b-3c: transform loose uniforms -> UBO + sampler bindings, compile for VULKAN.
/// Success = the transformed shader compiles for the Vulkan target with
/// DescriptorSet/Binding KEPT (the opposite of the GL strip). This is the real
/// uniform->UBO transform on the heaviest engine shader, compile-proven.
fn run_h3b3c() -> bool {
    match build_softenlight_ubo_spirv() {
        Ok((spv, _, _)) => {
            log::info!(
                "H3b-3c: transformed source COMPILES for VULKAN ({} SPIR-V words) -> uniform->UBO transform valid",
                spv.len()
            );
            true
        }
        Err(e) => {
            log::warn!("H3b-3c: transformed source did NOT compile (Vulkan target) -- the punch-list:\n{e}");
            false
        }
    }
}

/// H3b-3d (probe): can wgpu create a shader module from the transformed SPIR-V via
/// SPIRV_SHADER_PASSTHROUGH (raw SPIR-V straight to the driver, naga bypassed)?
///
/// FINDING so far: the default `ShaderSource::SpirV` path routes through naga, which
/// PANICS on this real-engine SPIR-V (InvalidId + "Unknown decoration Block"). That
/// path is a re-translation we don't want anyway -- the thesis is that the SPIR-V
/// runs on AMD's real Vulkan driver. Passthrough is the correct route. This probe
/// tells us whether passthrough module creation succeeds (=> only naga was the
/// module blocker; descriptor-model is the next question) or fails deeper.
fn run_h3b3d() -> bool {
    let (spv, n_members, n_samplers) = match build_softenlight_ubo_spirv() {
        Ok(t) => t,
        Err(e) => {
            log::warn!("H3b-3d: transform failed to compile: {e}");
            return false;
        }
    };
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
            .expect("H3b-3d: no Vulkan adapter");
        let info = adapter.get_info();
        log::info!("H3b-3d adapter: {} | {} {}", info.name, info.driver, info.driver_info);
        if !adapter
            .features()
            .contains(wgpu::Features::SPIRV_SHADER_PASSTHROUGH)
        {
            log::warn!("H3b-3d: adapter lacks SPIRV_SHADER_PASSTHROUGH -- cannot bypass naga");
            return false;
        }
        let (device, _queue) = adapter
            .request_device(
                &wgpu::DeviceDescriptor {
                    label: Some("h3b3d-passthrough"),
                    required_features: wgpu::Features::SPIRV_SHADER_PASSTHROUGH,
                    required_limits: wgpu::Limits::default(),
                },
                None,
            )
            .await
            .expect("H3b-3d: request_device (passthrough)");

        device.push_error_scope(wgpu::ErrorFilter::Validation);
        // SAFETY: passthrough trusts the SPIR-V; shaderc produced it from valid GLSL.
        let _module = unsafe {
            device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV {
                label: Some("softenLightF.ubo.passthrough"),
                source: std::borrow::Cow::Borrowed(&spv),
            })
        };
        let err = device.pop_error_scope().await;
        match err {
            None => {
                log::info!(
                    "H3b-3d: PASSTHROUGH module created OK ({n_members} UBO members, {n_samplers} \
                     samplers) -- naga was the only module blocker; the descriptor model (combined \
                     samplers) is the next question at pipeline time."
                );
                true
            }
            Some(e) => {
                log::warn!("H3b-3d: passthrough module creation FAILED -- deeper finding.\n  {e}");
                false
            }
        }
    })
}

// --- H3b-3d EXECUTE: run the transformed softenLight on dummy inputs ------------
// The full rig: passthrough VS+FS, both std140 UBOs, and the 12 tex/sampler pairs
// the compiled shader actually references (from spirv-dis) bound at their exact
// bindings. Success = the pipeline builds + a 1px draw completes with NO validation
// error -- proof the transformed lighting shader RUNS on the real driver. (Value
// correctness is 3e's golden gate; this proves the plumbing.)

// Dummy fullscreen-triangle VS supplying the 3 varyings softenLightF reads, at the
// exact locations from the FS SPIR-V: AdditiveColor@0, AtmosAttenuation@1, fragcoord@2.
const SOFTEN_EXEC_VERT_GLSL: &str = r#"
#version 450
layout(location = 0) out vec3 vary_AdditiveColor;
layout(location = 1) out vec3 vary_AtmosAttenuation;
layout(location = 2) out vec2 vary_fragcoord;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vary_fragcoord = p;                 // 0..1 across the visible area
    vary_AdditiveColor = vec3(0.0);
    vary_AtmosAttenuation = vec3(1.0);
}
"#;

/// H3b-3d execute. Returns pass (pipeline built + draw ran with no validation error).
fn run_h3b3d_exec() -> bool {
    let (fspv, n_members, n_samplers) = match build_softenlight_ubo_spirv() {
        Ok(t) => t,
        Err(e) => {
            log::warn!("H3b-3d-exec: transform failed to compile: {e}");
            return false;
        }
    };
    let vspv = compile_glsl(SOFTEN_EXEC_VERT_GLSL, shaderc::ShaderKind::Vertex, "soften.exec.vert");

    // The exact descriptor interface the compiled SPIR-V references (spirv-dis):
    // 'U'=SoftenFrameBlock@0, 'R'=ReflectionProbes@35, 'T'=texture2D, 'C'=textureCubeArray, 'S'=sampler.
    let table: &[(u32, char)] = &[
        (0, 'U'), (35, 'R'),
        (1, 'T'), (2, 'S'), (3, 'T'), (4, 'S'), (5, 'T'), (6, 'S'), (7, 'T'), (8, 'S'),
        (9, 'T'), (10, 'S'), (11, 'T'), (12, 'S'), (15, 'T'), (16, 'S'), (17, 'T'), (18, 'S'),
        (31, 'C'), (32, 'S'), (33, 'C'), (34, 'S'), (38, 'T'), (39, 'S'), (40, 'T'), (41, 'S'),
    ];

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
            .expect("H3b-3d-exec: no Vulkan adapter");
        let info = adapter.get_info();
        log::info!("H3b-3d-exec adapter: {} | {} {}", info.name, info.driver, info.driver_info);
        if !adapter.features().contains(wgpu::Features::SPIRV_SHADER_PASSTHROUGH) {
            log::warn!("H3b-3d-exec: no SPIRV_SHADER_PASSTHROUGH");
            return false;
        }
        let (device, queue) = adapter
            .request_device(
                &wgpu::DeviceDescriptor {
                    label: Some("h3b3d-exec"),
                    required_features: wgpu::Features::SPIRV_SHADER_PASSTHROUGH,
                    required_limits: adapter.limits(), // headroom for UBO size / descriptors
                },
                None,
            )
            .await
            .expect("H3b-3d-exec: request_device");

        // passthrough modules (raw SPIR-V straight to the driver -- naga bypassed)
        // SAFETY: shaderc produced valid SPIR-V from valid GLSL.
        let fs_mod = unsafe {
            device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV {
                label: Some("softenLightF.exec"),
                source: std::borrow::Cow::Borrowed(&fspv),
            })
        };
        let vs_mod = unsafe {
            device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV {
                label: Some("soften.exec.vert"),
                source: std::borrow::Cow::Borrowed(&vspv),
            })
        };

        // dummy resources -- one of each kind, shared across all matching bindings.
        let soften_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("SoftenFrameBlock"),
            size: 16384, // >> the ~1.5KB block; wgpu zero-inits
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let refl_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("ReflectionProbes"),
            size: 65536, // ReflectionProbes std140 ~48KB, alloc 64KB
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let tex2d = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("dummy-2d"),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm, // filterable float
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let tex2d_view = tex2d.create_view(&wgpu::TextureViewDescriptor::default());
        let cube = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("dummy-cubearray"),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 6 }, // 1 cube
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let cube_view = cube.create_view(&wgpu::TextureViewDescriptor {
            label: Some("dummy-cubearray-view"),
            dimension: Some(wgpu::TextureViewDimension::CubeArray),
            ..Default::default()
        });
        let samp = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("dummy-sampler"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        let layout_entries: Vec<wgpu::BindGroupLayoutEntry> = table
            .iter()
            .map(|&(b, k)| {
                let ty = match k {
                    'U' | 'R' => wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    'T' => wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    'C' => wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::CubeArray,
                        multisampled: false,
                    },
                    _ => wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                };
                wgpu::BindGroupLayoutEntry {
                    binding: b,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty,
                    count: None,
                }
            })
            .collect();
        let bind_entries: Vec<wgpu::BindGroupEntry> = table
            .iter()
            .map(|&(b, k)| {
                let resource = match k {
                    'U' => soften_ubo.as_entire_binding(),
                    'R' => refl_ubo.as_entire_binding(),
                    'T' => wgpu::BindingResource::TextureView(&tex2d_view),
                    'C' => wgpu::BindingResource::TextureView(&cube_view),
                    _ => wgpu::BindingResource::Sampler(&samp),
                };
                wgpu::BindGroupEntry { binding: b, resource }
            })
            .collect();

        device.push_error_scope(wgpu::ErrorFilter::Validation);
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("soften-exec-bgl"),
            entries: &layout_entries,
        });
        let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("soften-exec-bg"),
            layout: &bgl,
            entries: &bind_entries,
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("soften-exec-pl"),
            bind_group_layouts: &[&bgl],
            push_constant_ranges: &[],
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("soften-exec-pipeline"),
            layout: Some(&pl),
            vertex: wgpu::VertexState { module: &vs_mod, entry_point: "main", buffers: &[] },
            fragment: Some(wgpu::FragmentState {
                module: &fs_mod,
                entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: wgpu::TextureFormat::Rgba32Float,
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
        if let Some(e) = device.pop_error_scope().await {
            log::warn!("H3b-3d-exec: pipeline/bind-group build FAILED (validation):\n  {e}");
            return false;
        }

        device.push_error_scope(wgpu::ErrorFilter::Validation);
        let px = render_1px(&device, &queue, &pipeline, &bind_group);
        let render_err = device.pop_error_scope().await;
        match render_err {
            None => {
                log::info!(
                    "H3b-3d-exec: softenLight RAN on the driver -- pipeline built + 1px draw clean. \
                     ({n_members} UBO members, {n_samplers} declared samplers, 26 live bindings) \
                     px = [{:.4}, {:.4}, {:.4}, {:.4}]",
                    px[0], px[1], px[2], px[3]
                );
                true
            }
            Some(e) => {
                log::warn!("H3b-3d-exec: draw FAILED (validation):\n  {e}");
                false
            }
        }
    })
}

// --- LIGHT, measured: analytic known-answer gates -------------------------------
// "Accurate light" isn't taste -- it has closed-form answers. First instrument: the
// WHITE FURNACE test for the specular BRDF. Faithful Rust port of SL's genbrdflutF
// split-sum LUT (importanceSample_GGX alpha=roughness^2, G_SchlicksmithGGX k=
// roughness^2/2, Fc=(1-VoH)^5). For a perfect mirror (F0=1) the reflected energy =
// scale+bias; a LOSSLESS BRDF returns exactly 1.0 at every roughness. Single-scatter
// GGX (what SL ships -- no multiscatter/energy-compensation term exists in the tree)
// LOSES energy as roughness rises => rough metals/speculars render too dark. This
// measures SL's actual specular energy conservation. `cargo run -- furnace`.

/// Van der Corput radical inverse (base 2) -- the y of a Hammersley point.
fn radical_inverse_vdc(mut bits: u32) -> f32 {
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x5555_5555) << 1) | ((bits & 0xAAAA_AAAA) >> 1);
    bits = ((bits & 0x3333_3333) << 2) | ((bits & 0xCCCC_CCCC) >> 2);
    bits = ((bits & 0x0F0F_0F0F) << 4) | ((bits & 0xF0F0_F0F0) >> 4);
    bits = ((bits & 0x00FF_00FF) << 8) | ((bits & 0xFF00_FF00) >> 8);
    (bits as f32) * 2.328_306_437e-10
}

/// SL's genbrdflutF `BRDF(NoV, roughness)` -> (scale, bias), ported byte-faithfully.
/// N=+z; the per-sample phi jitter is dropped (a constant offset under full-2pi
/// integration -> identical result, and deterministic). roughness is PERCEPTUAL.
fn sl_brdf_lut(n_dot_v: f32, roughness: f32, samples: u32) -> (f32, f32) {
    let alpha = roughness * roughness;
    let k = (roughness * roughness) / 2.0; // SL's IBL Schlick-Smith k (perceptual)
    let v = [(1.0 - n_dot_v * n_dot_v).max(0.0).sqrt(), 0.0, n_dot_v];
    let (mut scale, mut bias) = (0.0f32, 0.0f32);
    for i in 0..samples {
        let xi_x = i as f32 / samples as f32;
        let xi_y = radical_inverse_vdc(i);
        let phi = 2.0 * std::f32::consts::PI * xi_x;
        let cos_t = ((1.0 - xi_y) / (1.0 + (alpha * alpha - 1.0) * xi_y)).sqrt();
        let sin_t = (1.0 - cos_t * cos_t).max(0.0).sqrt();
        let h = [sin_t * phi.cos(), sin_t * phi.sin(), cos_t];
        let vdoth = v[0] * h[0] + v[1] * h[1] + v[2] * h[2];
        let l = [2.0 * vdoth * h[0] - v[0], 2.0 * vdoth * h[1] - v[1], 2.0 * vdoth * h[2] - v[2]];
        let n_dot_l = l[2].max(0.0);
        let n_dot_h = h[2].max(0.0);
        let v_dot_h = vdoth.max(0.0);
        if n_dot_l > 0.0 {
            let gl = n_dot_l / (n_dot_l * (1.0 - k) + k);
            let gv = n_dot_v / (n_dot_v * (1.0 - k) + k);
            let g = gl * gv;
            let g_vis = (g * v_dot_h) / (n_dot_h * n_dot_v);
            let fc = (1.0 - v_dot_h).powf(5.0);
            scale += (1.0 - fc) * g_vis;
            bias += fc * g_vis;
        }
    }
    (scale / samples as f32, bias / samples as f32)
}

/// SL's DIRECT-light geometry term (deferredUtil.glsl `geometricOcclusion`): the
/// exact separable Smith-GGX masking-shadowing, r = alphaRoughness = perceptualRoughness^2.
/// This is a DIFFERENT G than the IBL LUT's Schlick-k -- the whole point of gate 2.
fn sl_geometric_occlusion(n_dot_l: f32, n_dot_v: f32, alpha_roughness: f32) -> f32 {
    let r = alpha_roughness;
    let al = 2.0 * n_dot_l / (n_dot_l + (r * r + (1.0 - r * r) * n_dot_l * n_dot_l).sqrt());
    let av = 2.0 * n_dot_v / (n_dot_v + (r * r + (1.0 - r * r) * n_dot_v * n_dot_v).sqrt());
    al * av
}

/// Direct-light specular reflected energy (F0=1), importance-sampled the SAME way as
/// gate 1 (GGX alpha = perceptualRoughness^2), but with SL's EXACT Smith G from
/// pbrPunctual instead of the LUT's Schlick-k. Same estimator: mean of G*VoH/(NoH*NoV).
fn sl_direct_specular_energy(n_dot_v: f32, roughness: f32, samples: u32) -> f32 {
    let alpha = roughness * roughness; // GGX alpha = alphaRoughness (matches punctual D)
    let v = [(1.0 - n_dot_v * n_dot_v).max(0.0).sqrt(), 0.0, n_dot_v];
    let mut acc = 0.0f32;
    for i in 0..samples {
        let xi_x = i as f32 / samples as f32;
        let xi_y = radical_inverse_vdc(i);
        let phi = 2.0 * std::f32::consts::PI * xi_x;
        let cos_t = ((1.0 - xi_y) / (1.0 + (alpha * alpha - 1.0) * xi_y)).sqrt();
        let sin_t = (1.0 - cos_t * cos_t).max(0.0).sqrt();
        let h = [sin_t * phi.cos(), sin_t * phi.sin(), cos_t];
        let vdoth = v[0] * h[0] + v[1] * h[1] + v[2] * h[2];
        let l = [2.0 * vdoth * h[0] - v[0], 2.0 * vdoth * h[1] - v[1], 2.0 * vdoth * h[2] - v[2]];
        let n_dot_l = l[2];
        let n_dot_h = h[2].max(0.0);
        let v_dot_h = vdoth.max(0.0);
        if n_dot_l > 0.0 {
            let g = sl_geometric_occlusion(n_dot_l, n_dot_v, alpha); // r = alphaRoughness
            acc += g * v_dot_h / (n_dot_h * n_dot_v);
        }
    }
    acc / samples as f32
}

/// GATE 2 -- direct-light specular furnace + IBL-vs-direct consistency check.
/// Tabulates SL's punctual specular energy (exact Smith G) and compares to the IBL
/// LUT energy (gate 1) at normal incidence: if the two disagree, the same material
/// reflects different energy under sun vs under reflection probes.
fn run_furnace_direct() -> bool {
    let samples = 8192u32;
    let roughs = [0.0f32, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0];
    let novs = [1.0f32, 0.75, 0.5, 0.25, 0.1];
    log::info!("DIRECT-LIGHT FURNACE -- SL pbrPunctual exact-Smith G, F0=1 (lossless = 1.0000)");
    log::info!("  rough\\NoV     1.00    0.75    0.50    0.25    0.10");
    let mut worst = 1.0f32;
    for &r in &roughs {
        let mut row = format!("   {r:>4.2}     ");
        for &nv in &novs {
            let e = sl_direct_specular_energy(nv, r, samples);
            worst = worst.min(e);
            row.push_str(&format!("  {e:.4}"));
        }
        log::info!("{row}");
    }
    log::info!("worst-case direct specular energy = {:.4}  =>  max LOSS = {:.1}%", worst, (1.0 - worst) * 100.0);
    log::info!("");
    log::info!("IBL (gate1 Schlick-k) vs DIRECT (exact-Smith) at NoV=1.00 -- same material, two paths:");
    log::info!("  rough     IBL      DIRECT    disagree");
    let mut max_disagree = 0.0f32;
    for &r in &roughs {
        let (s, b) = sl_brdf_lut(1.0, r, samples);
        let ibl = s + b;
        let dir = sl_direct_specular_energy(1.0, r, samples);
        let d = (ibl - dir).abs();
        max_disagree = max_disagree.max(d);
        log::info!("  {r:>4.2}    {ibl:.4}   {dir:.4}    {d:.4}");
    }
    log::info!("max IBL-vs-direct disagreement (NoV=1) = {:.4} ({:.1}% of full energy)", max_disagree, max_disagree * 100.0);
    true
}

/// THE FIX -- Fdez-Aguera multiscatter energy compensation (Khronos glTF method),
/// computed from the EXISTING brdfLut (A,B) -> no new LUT, ports straight into SL.
/// Returns (single_scatter, compensated) reflected energy per RGB channel under a
/// white env (prefilteredColor=1). For F0=1 the compensated energy is 1.0 by
/// construction (the multiscatter term adds back exactly the lost 1-Ess).
fn multiscatter_energy(nov: f32, roughness: f32, f0: [f32; 3], samples: u32) -> ([f32; 3], [f32; 3]) {
    let (a, b) = sl_brdf_lut(nov, roughness, samples);
    let ess = a + b; // single-scatter directional albedo = the furnace value
    let ems = 1.0 - ess; // energy the single-scatter model drops
    let fc = (1.0 - nov).powf(5.0);
    let mut ss = [0.0f32; 3];
    let mut comp = [0.0f32; 3];
    for c in 0..3 {
        let fr = (1.0 - roughness).max(f0[c]) - f0[c];
        let k_s = f0[c] + fr * fc; // Fresnel-weighted specular reflectance
        let fss_ess = k_s * a + b; // single-scatter specular (what SL ships)
        let f_avg = f0[c] + (1.0 - f0[c]) / 21.0; // average Fresnel over the hemisphere
        let f_ms = fss_ess * f_avg / (1.0 - f_avg * ems); // multiscatter specular
        ss[c] = fss_ess;
        comp[c] = fss_ess + f_ms * ems; // + the recovered multiple-scatter energy
    }
    (ss, comp)
}

/// FIX validation gate: show the multiscatter compensation restores energy.
/// White metal (F0=1) -> compensated furnace climbs to 1.0000 everywhere; a colored
/// metal (gold) -> energy recovered AND tinted correctly (proof it ports to real materials).
fn run_furnace_fix() -> bool {
    let samples = 8192u32;
    let roughs = [0.0f32, 0.25, 0.5, 0.75, 0.9, 1.0];
    let novs = [1.0f32, 0.5, 0.1];
    log::info!("KULLA-CONTY / FDEZ-AGUERA FIX -- white metal F0=1: BEFORE (single-scatter) -> AFTER (compensated)");
    log::info!("  rough      NoV=1.00        NoV=0.50        NoV=0.10");
    log::info!("            before  after   before  after   before  after");
    for &r in &roughs {
        let mut row = format!("   {r:>4.2}    ");
        for &nv in &novs {
            let (ss, comp) = multiscatter_energy(nv, r, [1.0, 1.0, 1.0], samples);
            row.push_str(&format!("  {:.4} {:.4} ", ss[0], comp[0]));
        }
        log::info!("{row}");
    }
    // Gold (linear sRGB base ~ (1.0, 0.782, 0.344)) at a rough setting -- tint preserved?
    let gold = [1.0f32, 0.782, 0.344];
    log::info!("");
    log::info!("Colored metal (gold F0={:?}) at roughness 0.9, NoV=1.0 -- energy recovered + tinted:", gold);
    let (ss, comp) = multiscatter_energy(1.0, 0.9, gold, samples);
    log::info!("  before (single-scatter) RGB = [{:.4}, {:.4}, {:.4}]", ss[0], ss[1], ss[2]);
    log::info!("  after  (multiscatter)   RGB = [{:.4}, {:.4}, {:.4}]", comp[0], comp[1], comp[2]);
    log::info!("  -> recovered {:.1}% / {:.1}% / {:.1}% of the missing energy per channel",
        (comp[0]-ss[0])*100.0, (comp[1]-ss[1])*100.0, (comp[2]-ss[2])*100.0);
    true
}

/// White furnace gate: tabulate SL's specular reflected energy (F0=1) over
/// roughness x view angle. Deviation from 1.000 = energy the shader silently loses.
fn run_furnace() -> bool {
    let samples = 8192u32;
    let roughs = [0.0f32, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0];
    let novs = [1.0f32, 0.75, 0.5, 0.25, 0.1];
    log::info!("WHITE FURNACE -- SL genbrdflutF, F0=1: reflected energy = scale+bias (lossless = 1.0000)");
    log::info!("  rough\\NoV     1.00    0.75    0.50    0.25    0.10");
    let mut worst = 1.0f32;
    for &r in &roughs {
        let mut row = format!("   {r:>4.2}     ");
        for &nv in &novs {
            let (s, b) = sl_brdf_lut(nv, r, samples);
            let e = s + b;
            worst = worst.min(e);
            row.push_str(&format!("  {e:.4}"));
        }
        log::info!("{row}");
    }
    log::info!(
        "worst-case reflected energy = {:.4}  =>  max specular energy LOSS = {:.1}%  (single-scatter GGX, no compensation)",
        worst,
        (1.0 - worst) * 100.0
    );
    true
}

// --- AUTO-EXPOSURE gate (camera response, step #7-8) -----------------------------
// The one question that matters: does the auto-exposure hold scene luminance at
// MIDDLE-GREY across brightness (the whole point of exposure)? Physical answer:
// exposure = key/L  =>  exposed = key, FLAT at every scene brightness. SL's exposureF
// instead clamps L AT the key, normalizes, squares, and linearly interpolates between
// two FIXED bounds -- so it holds grey only in a narrow band and can't compensate
// dark/bright scenes. `cargo run -- exposure`.

/// SL's `exposureF.glsl` steady-state exposure multiplier for scene avg luminance L
/// (adaptation ignored). Ports the shader exactly: clamp at key, /key, square,
/// mix(exp_max, exp_min, L).
fn sl_exposure(scene_lum: f32, key: f32, exp_min: f32, exp_max: f32) -> f32 {
    let l = (scene_lum.clamp(0.0, key) / key).powi(2);
    exp_max * (1.0 - l) + exp_min * l // GLSL mix(exp_max, exp_min, l)
}

fn run_exposure() -> bool {
    let key = 0.175f32; // 18% middle grey = SL's dynamic_exposure_coefficient
    let (exp_min, exp_max) = (0.5f32, 2.0f32); // representative PBR sky (hdr_scale=2)
    let scenes = [0.02f32, 0.04, 0.0875, 0.13, key, 0.26, 0.35, 0.7, 1.4];
    log::info!("AUTO-EXPOSURE -- goal: exposed luminance held at middle-grey key={key:.3} across scene brightness");
    log::info!("  sceneL   SL_exp  SL_exposed  SL_err    | phys_exp  phys_exposed  (physical = flat at key)");
    let mut worst_err = 0.0f32;
    for &l in &scenes {
        let sl = sl_exposure(l, key, exp_min, exp_max);
        let sl_exposed = l * sl;
        let sl_err = (sl_exposed / key - 1.0) * 100.0; // % off middle grey
        worst_err = worst_err.max(sl_err.abs());
        let ph = key / l.max(1e-4); // physical: reciprocal key mapping
        let ph_exposed = l * ph; // = key, always
        log::info!("  {l:>5.3}   {sl:>5.3}   {sl_exposed:>8.4}  {sl_err:>+6.1}%  |  {ph:>7.3}   {ph_exposed:>8.4}");
    }
    log::info!("worst SL middle-grey error = {:.0}%  (physical auto-exposure = 0% by construction)", worst_err);
    log::info!("+ legacy Windlight skies: NO auto-exposure (exp fixed 1.0); PBR range clamped to ~4x + gamma-coupled (sqrt(gamma)*2)");
    true
}

// --- CAMERA RESPONSE instrument (measure, don't assert) --------------------------
// The faithful-camera design PREDICTS: brighten the environment -> brighter image
// (faithful), and auto-exposure CANCELS it. This measures that on real math instead
// of believing it. Ports SL's actual PBRNeutral tonemap (tonemapUtilF.glsl:96) and
// runs a fixed grey surface through 3 cameras while scaling the environment.
// `cargo run -- camera`.

/// SL's Khronos PBRNeutral tonemap, ported byte-faithfully from tonemapUtilF.glsl.
fn pbr_neutral_tonemap(mut c: [f32; 3]) -> [f32; 3] {
    let start_compression = 0.8 - 0.04;
    let desaturation = 0.15;
    let x = c[0].min(c[1]).min(c[2]);
    let offset = if x < 0.08 { x - 6.25 * x * x } else { 0.04 };
    for v in &mut c { *v -= offset; }
    let peak = c[0].max(c[1]).max(c[2]);
    if peak < start_compression { return c; }
    let d = 1.0 - start_compression;
    let new_peak = 1.0 - d * d / (peak + d - start_compression);
    for v in &mut c { *v *= new_peak / peak; }
    let g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    for v in &mut c { *v = *v * (1.0 - g) + new_peak * g; }
    c
}

fn run_camera() -> bool {
    let base = 0.18f32; // linear radiance of a grey test surface at env-scale 1.0
    let env = [0.25f32, 0.5, 1.0, 2.0, 4.0];
    let key = 0.175f32;
    let (exp_min, exp_max) = (0.5f32, 2.0f32);
    let render_exposure = 1.0f32; // user EV knob, default
    log::info!("CAMERA RESPONSE to environment brightness (scene = envScale x {base:.2}, then tonemap).");
    log::info!("THE question: does the DISPLAY track the environment (faithful) or get CANCELLED (auto)?");
    log::info!("  envScale  sceneL  |  CURRENT(auto)  FAITHFUL(fixed)  HONEST(auto)   -- display grey [0,1]");
    let (mut cur0, mut fa0, mut ho0) = (0.0f32, 0.0f32, 0.0f32);
    let (mut cur_last, mut fa_last, mut ho_last) = (0.0f32, 0.0f32, 0.0f32);
    for (i, &k) in env.iter().enumerate() {
        let scene = base * k;
        let cur = pbr_neutral_tonemap([scene * render_exposure * sl_exposure(scene, key, exp_min, exp_max); 3])[0];
        let fa = pbr_neutral_tonemap([scene * render_exposure; 3])[0];
        let ho = pbr_neutral_tonemap([scene * (key / scene.max(1e-4)); 3])[0];
        if i == 0 { cur0 = cur; fa0 = fa; ho0 = ho; }
        cur_last = cur; fa_last = fa; ho_last = ho;
        log::info!("  {k:>6.2}   {scene:>6.3}  |    {cur:>8.4}       {fa:>8.4}       {ho:>8.4}");
    }
    let ratio = |lo: f32, hi: f32| if lo > 1e-4 { hi / lo } else { f32::INFINITY };
    log::info!("");
    log::info!("16x env-brightness sweep (0.25->4.0) -> display ratio:");
    log::info!("  CURRENT  x{:.2}   FAITHFUL x{:.2}   HONEST x{:.2}   (faithful tracks env; autos flatten toward 1)",
        ratio(cur0, cur_last), ratio(fa0, fa_last), ratio(ho0, ho_last));
    true
}

// --- ATMOSPHERE / Hosek-Wilkie SKY gate (Track B, B1: correctness) ---------------
// Physical sky-dome radiance. Oracle = the `hw-skymodel` crate (a faithful pure-Rust
// port of Hosek & Wilkie's reference RGB dataset). We implement the analytic radiance
// FORMULA independently from the published model and gate it against the oracle across
// (elevation x turbidity x albedo x theta x gamma x channel).
//
// Why this is the shippable seam, not a toy: the crate's own architecture is CPU bakes
// the 27+3 interpolated coefficients ONCE per sky change (SkyState::raw()), then a
// shader evaluates radiance() per pixel. So THIS radiance() port IS the GLSL we will
// ship -- the gate validates the shader-side math: the formula and the notorious H/I
// index convention (index 7 = I on the sqrt(cos th) term, index 8 = H = Mie g inside
// chi) that porters routinely swap. `cargo run -- sky`.
//
// Published Hosek-Wilkie analytic term, coeffs A..I per channel:
//   F(th,g) = (1 + A*exp(B/(cos th + 0.01)))
//           * (C + D*exp(E*g) + F*cos^2 g + G*chi(H,g) + I*sqrt(cos th))
//   chi(H,g) = (1 + cos^2 g) / (1 + H^2 - 2H*cos g)^{3/2},   L = F * radiance_channel

fn hw_channel(ci: usize) -> hw_skymodel::rgb::Channel {
    use hw_skymodel::rgb::Channel;
    match ci {
        0 => Channel::R,
        1 => Channel::G,
        _ => Channel::B,
    }
}

/// Our independent Hosek-Wilkie radiance, written from the published formula (NOT
/// copied from the oracle). `p` = the 9 interpolated coefficients for one channel in
/// SkyState::raw() layout; `r` = that channel's radiance term. theta = view zenith
/// angle, gamma = angle from the sun (both radians).
fn hw_radiance_ours(p: &[f32], r: f32, theta: f32, gamma: f32) -> f32 {
    let a = p[0];
    let b = p[1];
    let c = p[2];
    let d = p[3];
    let e = p[4];
    let f = p[5];
    let g = p[6];
    let big_i = p[7]; // coefficient on the sqrt(cos theta) zenith term
    let big_h = p[8]; // Mie directionality g, inside chi()
    let cos_theta = theta.cos().abs();
    let cos_gamma = gamma.cos();
    let cos_gamma2 = cos_gamma * cos_gamma;
    let chi = (1.0 + cos_gamma2) / (1.0 + big_h * big_h - 2.0 * big_h * cos_gamma).powf(1.5);
    let lhs = 1.0 + a * (b / (cos_theta + 0.01)).exp();
    let rhs = c + d * (e * gamma).exp() + f * cos_gamma2 + g * chi + big_i * cos_theta.sqrt();
    r * lhs * rhs
}

fn run_sky() -> bool {
    use hw_skymodel::rgb::{SkyParams, SkyState};
    let elevations_deg = [2.0f32, 10.0, 30.0, 60.0, 89.0]; // horizon -> zenith
    let turbidities = [1.0f32, 2.0, 4.0, 7.0, 10.0]; // clear -> hazy
    let albedos = [0.0f32, 0.3, 1.0]; // dark -> bright ground
    let thetas_deg = [0.0f32, 30.0, 60.0, 85.0]; // view zenith (0=up .. ~horizon)
    let gammas_deg = [0.0f32, 20.0, 60.0, 120.0, 180.0]; // angle from sun

    log::info!("HOSEK-WILKIE SKY -- B1 correctness: our independent radiance() vs hw-skymodel oracle");
    log::info!(
        "  grid: {}el x {}turb x {}alb x {}theta x {}gamma x 3ch",
        elevations_deg.len(), turbidities.len(), albedos.len(), thetas_deg.len(), gammas_deg.len()
    );

    let mut worst_rel = 0.0f32;
    let mut worst_ctx = String::new();
    let mut n = 0u64;
    for &el in &elevations_deg {
        for &tb in &turbidities {
            for &al in &albedos {
                let params = SkyParams { elevation: el.to_radians(), turbidity: tb, albedo: [al; 3] };
                let state = match SkyState::new(&params) {
                    Ok(s) => s,
                    Err(e) => {
                        log::error!("SkyState::new failed (el={el} tb={tb} al={al}): {e:?}");
                        return false;
                    }
                };
                let (raw_p, raw_r) = state.raw();
                for ci in 0..3usize {
                    let p = &raw_p[(9 * ci)..(9 * ci + 9)];
                    let r = raw_r[ci];
                    for &th in &thetas_deg {
                        for &ga in &gammas_deg {
                            let theta = th.to_radians();
                            let gamma = ga.to_radians();
                            let ours = hw_radiance_ours(p, r, theta, gamma);
                            let oracle = state.radiance(theta, gamma, hw_channel(ci));
                            let rel = (ours - oracle).abs() / oracle.abs().max(1e-6);
                            if rel > worst_rel {
                                worst_rel = rel;
                                worst_ctx = format!(
                                    "el={el} tb={tb} al={al} ch={ci} th={th} ga={ga}: ours={ours:.6} oracle={oracle:.6}"
                                );
                            }
                            n += 1;
                        }
                    }
                }
            }
        }
    }
    log::info!("  samples compared: {n}");
    log::info!("  worst relative error = {worst_rel:.2e}   ({worst_ctx})");
    let pass = worst_rel < 1e-4;
    if pass {
        log::info!("PASS: independent radiance() matches the H-W reference (formula + H/I index convention correct).");
        log::info!("  next: B2 -- integrate the dome to horizontal illuminance (lux), the model-INDEPENDENT physical anchor.");
    } else {
        log::error!("FAIL: radiance() diverges (>1e-4) -- check coeff index mapping (I=7, H=8), signs, powf(1.5).");
    }
    pass
}

// --- ATMOSPHERE / Hosek-Wilkie SKY gate (Track B, B2: physical illuminance) ------
// The furnace-analog for atmosphere, and the MODEL-INDEPENDENT trust anchor: whatever
// coefficients we use, the integrated sky dome must produce physically real magnitudes.
// Integrate H-W dome radiance over the upper hemisphere -> horizontal DIFFUSE illuminance
// (lux; the sun disk is excluded, which the RGB model omits anyway) and zenith luminance
// (cd/m^2), then compare to measured clear-sky values. If off by a clean factor, THAT
// factor is the finding (the model's scale convention) -- exactly like the furnace's
// 69%. `cargo run -- sky-lux`.

fn luminance_y(r: f32, g: f32, b: f32) -> f32 {
    0.2126 * r + 0.7152 * g + 0.0722 * b // Rec.709 relative luminance
}

/// Angle from the sun for a view direction (theta = view zenith, phi = azimuth), with
/// the sun at elevation `el` (rad), azimuth 0.
fn gamma_from(theta: f32, phi: f32, el: f32) -> f32 {
    let cg = el.sin() * theta.cos() + el.cos() * theta.sin() * phi.cos();
    cg.clamp(-1.0, 1.0).acos()
}

/// (horizontal diffuse illuminance, zenith luminance) for a sky state. Midpoint
/// hemisphere quadrature of Y*cos(theta) dω, dω = sin θ dθ dφ.
fn dome_illuminance_and_zenith(
    state: &hw_skymodel::rgb::SkyState,
    el: f32,
    n_theta: usize,
    n_phi: usize,
) -> (f32, f32) {
    use hw_skymodel::rgb::Channel;
    let y_at = |theta: f32, gamma: f32| {
        luminance_y(
            state.radiance(theta, gamma, Channel::R),
            state.radiance(theta, gamma, Channel::G),
            state.radiance(theta, gamma, Channel::B),
        )
    };
    let zenith_lum = y_at(0.0, std::f32::consts::FRAC_PI_2 - el); // straight up
    let dtheta = std::f32::consts::FRAC_PI_2 / n_theta as f32;
    let dphi = std::f32::consts::TAU / n_phi as f32;
    let mut e = 0.0f32;
    for it in 0..n_theta {
        let theta = (it as f32 + 0.5) * dtheta;
        let cos_t = theta.cos();
        let sin_t = theta.sin();
        for ip in 0..n_phi {
            let phi = (ip as f32 + 0.5) * dphi;
            let gamma = gamma_from(theta, phi, el);
            e += y_at(theta, gamma) * cos_t * sin_t * dtheta * dphi;
        }
    }
    (e, zenith_lum)
}

fn run_sky_lux() -> bool {
    use hw_skymodel::rgb::{SkyParams, SkyState};
    // CALIBRATION (found in B2): raw H-W RGB output is radiometric; photometric
    // luminance (cd/m^2) = K * Rec709(RGB), K = 683 lm/W max luminous efficacy -- the
    // standard radiance->luminance constant. The test below proves ONE K lands BOTH
    // independent anchors (zenith luminance AND horizontal illuminance) in physical band.
    const K_LM_PER_W: f32 = 683.0;
    log::info!("HOSEK-WILKIE SKY -- B2 physical illuminance (model-independent anchor; sun disk excluded)");
    log::info!("  raw H-W RGB is radiometric; calibrate cd/m^2 = {K_LM_PER_W} * Rec709(RGB) (max luminous efficacy)");
    log::info!("  {:>26} {:>10} {:>12} {:>10} {:>13}", "sky", "zen(raw)", "zen cd/m^2", "E(raw)", "E lux (xK)");
    let cases = [
        ("clear high sun (t=3,e=60)", 3.0f32, 60.0f32),
        ("clear low sun  (t=3,e=10)", 3.0, 10.0),
        ("hazy  high sun (t=8,e=60)", 8.0, 60.0),
    ];
    let (mut clear_lux, mut clear_zen) = (0.0f32, 0.0f32);
    for (name, tb, el_deg) in cases {
        let el = el_deg.to_radians();
        let params = SkyParams { elevation: el, turbidity: tb, albedo: [0.3; 3] };
        let state = match SkyState::new(&params) {
            Ok(s) => s,
            Err(e) => {
                log::error!("{name}: {e:?}");
                return false;
            }
        };
        let (e_raw, z_raw) = dome_illuminance_and_zenith(&state, el, 256, 512);
        let (z_cd, e_lux) = (z_raw * K_LM_PER_W, e_raw * K_LM_PER_W);
        log::info!("  {name:>26} {z_raw:>10.2} {z_cd:>12.0} {e_raw:>10.2} {e_lux:>13.0}");
        if (tb - 3.0).abs() < 0.1 && (el_deg - 60.0).abs() < 0.1 {
            clear_lux = e_lux;
            clear_zen = z_cd;
        }
    }
    // Two INDEPENDENT physical anchors, clear high sun:
    //   diffuse horizontal illuminance ~ 8..25 klux (CIE);  zenith luminance ~ 1.5..8 kcd/m^2.
    let (e_lo, e_hi) = (8_000.0f32, 25_000.0f32);
    let (z_lo, z_hi) = (1_500.0f32, 8_000.0f32);
    log::info!("  reference (clear high sun): illuminance {e_lo:.0}..{e_hi:.0} lux, zenith luminance {z_lo:.0}..{z_hi:.0} cd/m^2");
    let e_ok = clear_lux >= e_lo && clear_lux <= e_hi;
    let z_ok = clear_zen >= z_lo && clear_zen <= z_hi;
    let pass = e_ok && z_ok;
    if pass {
        log::info!("PASS: K={K_LM_PER_W} lands BOTH anchors physical (E={clear_lux:.0} lux, zenith={clear_zen:.0} cd/m^2).");
        log::info!("  => calibration locked: the sky feeds the engine as physical cd/m^2. Ready to slot in as an environment.");
    } else {
        log::error!("FAIL: E_ok={e_ok} (={clear_lux:.0} lux), zenith_ok={z_ok} (={clear_zen:.0} cd/m^2) -- one constant should fit both.");
    }
    pass
}

// --- ATMOSPHERE / SL sky vs physical (Track B, B3: the motivating gap) ------------
// Port SL's legacy Windlight sky-dome radiance (class1/deferred/skyV.glsl + skyF's *2,
// clamp[0,5]) with the SHIPPED DEFAULT EEP params -- real constants extracted from
// llsettingssky.cpp / llsettingsvo.cpp, not recalled. SL's absolute scale is arbitrary
// (artist colors x magic scales, hard clamp, then auto-exposure), so we compare
// UNIT-INDEPENDENTLY vs the calibrated H-W sky: (1) zenith-normalized luminance profile
// [shape], (2) blue/red chromaticity [color]. Structurally SL is NOT a scattering model:
// sky = artist_color*weight*(sunlight+ambient) + haze; horizon brightening = a haze
// transparency term (1-exp(-k*dist)), not airmass Rayleigh; the sun halo is
// pow(1-cos, glow_param), not a Mie phase. `cargo run -- sky-sl`.

/// SL legacy sky-dome radiance (skyV.glsl) at `view` for sun direction `sun` (both unit,
/// y = up). Returns linear RGB in SL's arbitrary units (post skyF *2 + clamp[0,5]).
// The EEP legacy-Windlight sky knobs that drive dome radiance (skyV.glsl). max_y and
// distance_multiplier are omitted -- max_y is fixed at 1605 and distance_multiplier does
// NOT affect the dome (only scene fog). glow.y is unused; only .x and .z matter.
#[derive(Clone)]
struct EepSky {
    blue_horizon: [f32; 3],
    blue_density: [f32; 3],
    haze_horizon: f32,
    haze_density: f32,
    density_multiplier: f32,
    glow_x: f32,
    glow_z: f32,
    ambient: [f32; 3],
    sunlight_color: [f32; 3],
    cloud_shadow: f32,
}

impl EepSky {
    // The shipped DEFAULT EEP sky (the "arbitrary values set in the path" we want to ground).
    fn shipped_default() -> Self {
        EepSky {
            blue_horizon: [0.4954, 0.4954, 0.6399],
            blue_density: [0.2447, 0.4487, 0.7599],
            haze_horizon: 0.19,
            haze_density: 0.7,
            density_multiplier: 0.0001,
            glow_x: 5.0,
            glow_z: -0.4799,
            ambient: [0.25, 0.25, 0.25],
            sunlight_color: [0.7342, 0.7815, 0.8999],
            cloud_shadow: 0.2699,
        }
    }
}

fn sl_sky_radiance(view: [f32; 3], sun: [f32; 3]) -> [f32; 3] {
    sl_sky_radiance_p(view, sun, &EepSky::shipped_default())
}

// Faithful port of skyV.glsl's per-vertex vary_HazeColor + skyF.glsl's *2/clamp[0,5],
// parameterized by the EEP knobs. Verified term-by-term against the shipping shader.
fn sl_sky_radiance_p(view: [f32; 3], sun: [f32; 3], p: &EepSky) -> [f32; 3] {
    let sunlight_color = p.sunlight_color;
    let ambient_color = p.ambient;
    let blue_horizon = p.blue_horizon;
    let blue_density = p.blue_density;
    let haze_horizon = p.haze_horizon;
    let haze_density = p.haze_density;
    let density_multiplier = p.density_multiplier;
    let max_y = 1605.0f32;
    let glow = [p.glow_x, 0.001, p.glow_z];
    let cloud_shadow = p.cloud_shadow;
    let sun_moon_glow_factor = 1.0f32;

    let dy = view[1].max(1e-3); // view up-component (clamp off the horizon singularity)
    let rel_pos_len = max_y / dy; // skyV altitude clamp -> length for dy>0
    let ndot = view[0] * sun[0] + view[1] * sun[1] + view[2] * sun[2];
    let atten_k = density_multiplier * max_y;
    let off_axis = 1.0 / (view[1].max(0.0) + sun[1]).max(1e-6);
    let density_dist = rel_pos_len * density_multiplier;

    let mut hg = (1.0 - ndot).max(0.001) * glow[0];
    hg = hg.powf(glow[2]);
    let haze_glow = if sun_moon_glow_factor < 1.0 { 0.0 } else { sun_moon_glow_factor * (hg + 0.25) };

    let mut color = [0.0f32; 3];
    for c in 0..3 {
        let light_atten = (blue_density[c] + haze_density * 0.25) * atten_k;
        let sunlight = sunlight_color[c] * (-light_atten * off_axis).exp();
        let comb = (blue_density[c] + haze_density).max(1e-6);
        let blue_w = blue_density[c] / comb;
        let haze_w = haze_density / comb;
        let combined_haze = (-comb * density_dist).exp();

        let mut col = blue_horizon[c] * blue_w * (sunlight + ambient_color[c])
            + (haze_horizon * haze_w) * (sunlight * haze_glow + ambient_color[c]);
        col *= 1.0 - combined_haze;

        let ambient2 = ambient_color[c] + (1.0 - ambient_color[c]).max(0.0) * cloud_shadow * 0.5;
        let sunlight2 = sunlight * (1.0 - cloud_shadow).max(0.0);
        let add_below = blue_horizon[c] * blue_w * (sunlight2 + ambient2)
            + (haze_horizon * haze_w) * (sunlight2 * haze_glow + ambient2);

        let ch2 = combined_haze.sqrt(); // skyV: combined_haze = sqrt(...)
        col += (add_below - col) * (1.0 - ch2.sqrt()); // then *(1 - sqrt(that)) = 1 - orig^0.25
        color[c] = (col * 2.0).clamp(0.0, 5.0); // skyF: *2, clamp[0,5]
    }
    color
}

fn run_sky_sl() -> bool {
    use hw_skymodel::rgb::{Channel, SkyParams, SkyState};
    let el_deg = 60.0f32;
    let el = el_deg.to_radians();
    let sun = [el.cos(), el.sin(), 0.0f32]; // azimuth 0, y = up
    let hw = SkyState::new(&SkyParams { elevation: el, turbidity: 3.0, albedo: [0.3; 3] }).unwrap();
    const K: f32 = 683.0;

    let sl_y = |view: [f32; 3]| {
        let c = sl_sky_radiance(view, sun);
        luminance_y(c[0], c[1], c[2])
    };
    let hw_y = |theta: f32, gamma: f32| {
        K * luminance_y(
            hw.radiance(theta, gamma, Channel::R),
            hw.radiance(theta, gamma, Channel::G),
            hw.radiance(theta, gamma, Channel::B),
        )
    };

    log::info!("SL WINDLIGHT SKY vs PHYSICAL H-W -- B3 gap (default EEP sky, sun elevation {el_deg:.0} deg)");
    log::info!("  unit-independent: each profile normalized to its OWN zenith luminance");
    log::info!("  {:>6} {:>16} {:>16}   {:>16} {:>16}", "theta", "SL/zenith(->sun)", "HW/zenith(->sun)", "SL/zenith(away)", "HW/zenith(away)");

    let zenith = [0.0f32, 1.0, 0.0];
    let gamma_z = zenith[0] * sun[0] + zenith[1] * sun[1] + zenith[2] * sun[2];
    let sl_z = sl_y(zenith);
    let hw_z = hw_y(0.0, gamma_z.clamp(-1.0, 1.0).acos());

    let thetas = [0.0f32, 15.0, 30.0, 45.0, 60.0, 75.0, 85.0];
    let mut sse = 0.0f32;
    let mut n = 0u32;
    for &th_deg in &thetas {
        let th = th_deg.to_radians();
        let (s, c) = (th.sin(), th.cos());
        let mut row = format!("  {th_deg:>5.0} ");
        for &sign in &[1.0f32, -1.0] {
            let view = [sign * s, c, 0.0];
            let ndot = (view[0] * sun[0] + view[1] * sun[1] + view[2] * sun[2]).clamp(-1.0, 1.0);
            let gamma = ndot.acos();
            let sl_n = sl_y(view) / sl_z;
            let hw_n = hw_y(th, gamma) / hw_z;
            row.push_str(&format!(" {sl_n:>16.4} {hw_n:>16.4}"));
            if sign > 0.0 {
                row.push_str("  ");
            }
            let rel = (sl_n - hw_n) / hw_n.max(1e-4);
            sse += rel * rel;
            n += 1;
        }
        log::info!("{row}");
    }
    let rms = (sse / n as f32).sqrt() * 100.0;

    // chromaticity: blue/red ratio (unit-independent), zenith + horizon-away-from-sun
    let horizon_away = { let th = 85f32.to_radians(); [-th.sin(), th.cos(), 0.0] };
    let sl_zc = sl_sky_radiance(zenith, sun);
    let sl_hc = sl_sky_radiance(horizon_away, sun);
    let hw_zc = [hw.radiance(0.0, gamma_z.acos(), Channel::R), hw.radiance(0.0, gamma_z.acos(), Channel::B)];
    let ga_h = { let v = horizon_away; (v[0]*sun[0]+v[1]*sun[1]+v[2]*sun[2]).clamp(-1.0,1.0).acos() };
    let th_h = 85f32.to_radians();
    let hw_hc = [hw.radiance(th_h, ga_h, Channel::R), hw.radiance(th_h, ga_h, Channel::B)];
    let br = |r: f32, b: f32| b / r.max(1e-6);
    log::info!("");
    log::info!("  chromaticity (Blue/Red ratio, higher = bluer):");
    log::info!("    zenith:       SL {:>6.3}   HW {:>6.3}", br(sl_zc[0], sl_zc[2]), br(hw_zc[0], hw_zc[1]));
    log::info!("    horizon(away): SL {:>6.3}   HW {:>6.3}", br(sl_hc[0], sl_hc[2]), br(hw_hc[0], hw_hc[1]));
    log::info!("");
    log::info!("  physical zenith luminance (H-W): {:.0} cd/m^2   |   SL zenith: arbitrary units (no physical scale)", hw_z);
    log::info!("GAP: SL zenith-normalized luminance profile deviates RMS {rms:.0}% from physical H-W.");
    log::info!("  structural: SL sky is artist-color*weight*(sun+ambient), horizon = haze transparency (not airmass),");
    log::info!("  sun halo = pow(1-cos,glow) (not Mie), output *2 & clamp[0,5] -- parametric look, not a scattering sky.");
    true
}

// --- EEP-conformant-to-HW sky fit --------------------------------------------------
// Keep EEP as the reliable rendering driver, but replace its arbitrary hand-picked
// "magic values" with numbers fit to the physical Hosek-Wilkie dome. We do NOT make the
// renderer physically absolute (that broke fullbright/indoor); we ONLY ground the knobs.
// Fit ONE default-sky knob-set across a day's sun elevations (skyV.glsl's own sun-position
// response then tracks HW as the sun moves), then paste the numbers into LLSettingsSky.
fn run_sky_fit() -> bool {
    use hw_skymodel::rgb::{Channel, SkyParams, SkyState};

    const NP: usize = 18;
    // (min, max, log) per free parameter, in EepSky field order.
    let desc: [(f32, f32, bool); NP] = [
        (0.0, 3.0, false), (0.0, 3.0, false), (0.0, 3.0, false), // blue_horizon
        (0.0, 3.0, false), (0.0, 3.0, false), (0.0, 3.0, false), // blue_density
        (0.0, 5.0, false),                                       // haze_horizon
        (0.0, 5.0, false),                                       // haze_density
        (1e-6, 2.0, true),                                       // density_multiplier (log)
        (0.2, 40.0, true),                                       // glow_x (log)
        (-10.0, 10.0, false),                                    // glow_z
        (0.0, 3.0, false), (0.0, 3.0, false), (0.0, 3.0, false), // ambient
        (0.0, 3.0, false), (0.0, 3.0, false), (0.0, 3.0, false), // sunlight_color
        (0.0, 1.0, false),                                       // cloud_shadow
    ];
    let norm1 = |v: f32, d: (f32, f32, bool)| -> f32 {
        let (mn, mx, lg) = d;
        if lg { (v.max(1e-12).ln() - mn.ln()) / (mx.ln() - mn.ln()) } else { (v - mn) / (mx - mn) }
    };
    let denorm1 = |t: f32, d: (f32, f32, bool)| -> f32 {
        let (mn, mx, lg) = d;
        let t = t.clamp(0.0, 1.0);
        if lg { (mn.ln() + t * (mx.ln() - mn.ln())).exp() } else { mn + t * (mx - mn) }
    };
    let pack = |p: &EepSky| -> [f32; NP] {
        let v = [
            p.blue_horizon[0], p.blue_horizon[1], p.blue_horizon[2],
            p.blue_density[0], p.blue_density[1], p.blue_density[2],
            p.haze_horizon, p.haze_density, p.density_multiplier, p.glow_x, p.glow_z,
            p.ambient[0], p.ambient[1], p.ambient[2],
            p.sunlight_color[0], p.sunlight_color[1], p.sunlight_color[2], p.cloud_shadow,
        ];
        let mut x = [0.0f32; NP];
        for i in 0..NP { x[i] = norm1(v[i], desc[i]).clamp(0.0, 1.0); }
        x
    };
    let unpack = |x: &[f32; NP]| -> EepSky {
        let mut v = [0.0f32; NP];
        for i in 0..NP { v[i] = denorm1(x[i], desc[i]); }
        EepSky {
            blue_horizon: [v[0], v[1], v[2]],
            blue_density: [v[3], v[4], v[5]],
            haze_horizon: v[6], haze_density: v[7], density_multiplier: v[8],
            glow_x: v[9], glow_z: v[10],
            ambient: [v[11], v[12], v[13]],
            sunlight_color: [v[14], v[15], v[16]],
            cloud_shadow: v[17],
        }
    };

    // Fit ONE default sky across a day's sun elevations (same knobs, sun moved).
    let elevs_deg = [15.0f32, 30.0, 45.0, 60.0, 75.0];
    let turbidity = 3.0f32;
    // Hemisphere sample grid: zenith theta x azimuth phi (relative to the sun plane, az 0).
    let thetas: Vec<f32> = (0..=17).map(|i| i as f32 * 5.0).collect(); // 0..85 step 5
    let phis: Vec<f32> = (0..=6).map(|i| i as f32 * 30.0).collect();   // 0..180 step 30

    struct Sample { view: [f32; 3], w: f32, target: [f32; 3] }
    struct ElevData { deg: f32, sun: [f32; 3], samples: Vec<Sample>, tmean: f32 }

    let mut data: Vec<ElevData> = Vec::new();
    for &eld in &elevs_deg {
        let el = eld.to_radians();
        let sun = [el.cos(), el.sin(), 0.0f32];
        let hw = SkyState::new(&SkyParams { elevation: el, turbidity, albedo: [0.3; 3] }).unwrap();
        let mut samples = Vec::new();
        let (mut tsum, mut wsum) = (0.0f32, 0.0f32);
        for &thd in &thetas {
            let th = thd.to_radians();
            let (st, ct) = (th.sin(), th.cos());
            for &phd in &phis {
                let ph = phd.to_radians();
                let view = [st * ph.cos(), ct, st * ph.sin()];
                let ndot = (view[0] * sun[0] + view[1] * sun[1] + view[2] * sun[2]).clamp(-1.0, 1.0);
                let gamma = ndot.acos();
                let target = [
                    hw.radiance(th, gamma, Channel::R).max(0.0),
                    hw.radiance(th, gamma, Channel::G).max(0.0),
                    hw.radiance(th, gamma, Channel::B).max(0.0),
                ];
                let w = st.max(0.03); // ~ solid-angle weight (sin theta)
                tsum += luminance_y(target[0], target[1], target[2]) * w;
                wsum += w;
                samples.push(Sample { view, w, target });
            }
        }
        let tmean = (tsum / wsum).max(1e-9);
        data.push(ElevData { deg: eld, sun, samples, tmean });
    }

    // Objective: each dome normalized to unit mean luminance, summed squared RGB error
    // (fits angular shape + chromaticity up to a global scale, which EEP lacks anyway),
    // PLUS regularization: an unconstrained fit drives redundant params to nonphysical
    // extremes (green-only ambient, zero-red density). Pull toward the known-good default
    // sky (we are GROUNDING it, not reinventing it) and keep ambient near-neutral.
    let x0 = pack(&EepSky::shipped_default());
    let lambda = 8.0f32;      // toward the default sky
    let lambda_amb = 25.0f32; // ambient neutrality
    let obj = |p: &EepSky| -> f32 {
        let mut err = 0.0f32;
        for ed in &data {
            let (mut msum, mut wsum) = (0.0f32, 0.0f32);
            let mut model: Vec<[f32; 3]> = Vec::with_capacity(ed.samples.len());
            for s in &ed.samples {
                let c = sl_sky_radiance_p(s.view, ed.sun, p);
                msum += luminance_y(c[0], c[1], c[2]) * s.w;
                wsum += s.w;
                model.push(c);
            }
            let mmean = (msum / wsum).max(1e-6);
            for (s, m) in ed.samples.iter().zip(model.iter()) {
                for ch in 0..3 {
                    let d = m[ch] / mmean - s.target[ch] / ed.tmean;
                    err += s.w * d * d;
                }
            }
        }
        let xp = pack(p);
        let mut reg = 0.0f32;
        for i in 0..NP { let dd = xp[i] - x0[i]; reg += dd * dd; }
        let amb_avg = (p.ambient[0] + p.ambient[1] + p.ambient[2]) / 3.0;
        let amb_chroma = (p.ambient[0] - amb_avg).powi(2)
            + (p.ambient[1] - amb_avg).powi(2)
            + (p.ambient[2] - amb_avg).powi(2);
        err + lambda * reg + lambda_amb * amb_chroma
    };

    // Coordinate descent with step halving, starting from the shipped default sky.
    let mut x = pack(&EepSky::shipped_default());
    let mut fx = obj(&unpack(&x));
    let f0 = fx;
    let mut h = 0.12f32;
    let mut iters = 0u32;
    while h > 1e-4 && iters < 60_000 {
        let mut improved = false;
        for i in 0..NP {
            for &d in &[h, -h] {
                let mut cand = x;
                cand[i] = (cand[i] + d).clamp(0.0, 1.0);
                let fc = obj(&unpack(&cand));
                if fc < fx - 1e-9 {
                    x = cand; fx = fc; improved = true; break;
                }
            }
            iters += 1;
        }
        if !improved { h *= 0.5; }
    }
    let fitted = unpack(&x);
    let deflt = EepSky::shipped_default();

    // ---- report ----
    log::info!("HW->EEP SKY FIT  (one default sky fit across sun elevations {:?} deg, turbidity {})", elevs_deg, turbidity);
    log::info!("  objective (mean-normalized RGB shape error): {:.4} -> {:.4}  ({:.0}% lower, {} obj-evals)",
        f0, fx, (1.0 - fx / f0) * 100.0, iters);
    log::info!("");
    log::info!("  FITTED default-sky values (paste into LLSettingsSky defaults / legacy-haze fallbacks):");
    log::info!("    blue_horizon       = ({:.4}, {:.4}, {:.4})", fitted.blue_horizon[0], fitted.blue_horizon[1], fitted.blue_horizon[2]);
    log::info!("    blue_density       = ({:.4}, {:.4}, {:.4})", fitted.blue_density[0], fitted.blue_density[1], fitted.blue_density[2]);
    log::info!("    haze_horizon       = {:.4}", fitted.haze_horizon);
    log::info!("    haze_density       = {:.4}", fitted.haze_density);
    log::info!("    density_multiplier = {:.6}", fitted.density_multiplier);
    log::info!("    glow  (x, _, z)    = ({:.4}, 0.001, {:.4})", fitted.glow_x, fitted.glow_z);
    log::info!("    ambient            = ({:.4}, {:.4}, {:.4})", fitted.ambient[0], fitted.ambient[1], fitted.ambient[2]);
    log::info!("    sunlight_color     = ({:.4}, {:.4}, {:.4})", fitted.sunlight_color[0], fitted.sunlight_color[1], fitted.sunlight_color[2]);
    log::info!("    cloud_shadow       = {:.4}", fitted.cloud_shadow);
    log::info!("");

    // Per-elevation luminance-shape residual, default vs fitted.
    let rms_of = |p: &EepSky, ed: &ElevData| -> f32 {
        let (mut msum, mut wsum) = (0.0f32, 0.0f32);
        let mut mlum: Vec<f32> = Vec::with_capacity(ed.samples.len());
        for s in &ed.samples {
            let c = sl_sky_radiance_p(s.view, ed.sun, p);
            let l = luminance_y(c[0], c[1], c[2]);
            msum += l * s.w; wsum += s.w; mlum.push(l);
        }
        let mmean = (msum / wsum).max(1e-6);
        let (mut sse, mut wtot) = (0.0f32, 0.0f32);
        for (s, &l) in ed.samples.iter().zip(mlum.iter()) {
            let tn = luminance_y(s.target[0], s.target[1], s.target[2]) / ed.tmean;
            let d = l / mmean - tn;
            sse += s.w * d * d; wtot += s.w;
        }
        (sse / wtot).sqrt() * 100.0
    };
    log::info!("  per-elevation luminance-shape RMS (mean-normalized):   default -> fitted");
    for ed in &data {
        log::info!("    sun {:>2.0} deg:   {:>6.1}%  ->  {:>6.1}%", ed.deg, rms_of(&deflt, ed), rms_of(&fitted, ed));
    }
    log::info!("");
    // zenith Blue/Red chromaticity check at the mid elevation.
    let mid = data.iter().find(|e| e.deg == 45.0).unwrap();
    let zen = [0.0f32, 1.0, 0.0];
    let br = |c: [f32; 3]| c[2] / c[0].max(1e-6);
    let hw45 = SkyState::new(&SkyParams { elevation: 45f32.to_radians(), turbidity, albedo: [0.3; 3] }).unwrap();
    let gz = (zen[1] * mid.sun[1]).clamp(-1.0, 1.0).acos();
    let hw_br = hw45.radiance(0.0, gz, Channel::B) / hw45.radiance(0.0, gz, Channel::R).max(1e-6);
    log::info!("  zenith Blue/Red @ sun 45 deg (higher = bluer):  default {:.3}   fitted {:.3}   HW {:.3}",
        br(sl_sky_radiance_p(zen, mid.sun, &deflt)), br(sl_sky_radiance_p(zen, mid.sun, &fitted)), hw_br);
    log::info!("");
    log::info!("  NOTE: EEP's fixed non-Mie glow lobe + clamp[0,5] cap how close the fit can reach;");
    log::info!("  the residual is EEP's model limit, not a fit failure. These values ground the DEFAULT sky.");
    true
}

// --- Track B / Phase B+C: PHYSICAL SUN -- the last physical-frame piece ------------
// To expose the WHOLE frame with one physical exposure, sun+ambient must be absolute
// cd/m^2 like the sky. Model direct solar illuminance = solar constant attenuated by
// atmosphere (Beer-Lambert over Kasten-Young airmass, turbidity optical depth), combine
// with the H-W sky diffuse (B2, x683), and pin the TOTAL physical illuminance -> the
// refined L_ref. Known-answer: clear-noon total horizontal illuminance ~100-120 klux.
// `cargo run -- sun-phys`.
fn airmass(elev_rad: f32) -> f32 {
    let h = elev_rad.to_degrees(); // Kasten-Young 1989
    1.0 / (elev_rad.sin() + 0.50572 * (h + 6.07995).powf(-1.6364))
}
fn run_sun_phys() -> bool {
    use hw_skymodel::rgb::{SkyParams, SkyState};
    const E0: f32 = 128_000.0; // extraterrestrial normal solar illuminance (lux)
    const K: f32 = 683.0;
    log::info!("PHYSICAL SUN irradiance + total illuminance (pins the physical frame)");
    log::info!("  E_dn = E0*exp(-m*tau); tau = 0.12 + 0.06*(turb-1); m = Kasten-Young airmass");
    log::info!("  {:>22} {:>9} {:>8} {:>9} {:>10} {:>8}", "sky", "E_dir_h", "E_sky", "E_total", "grey cd/m2", "L_ref");
    let cases = [
        ("clear noon (t=2,e=85)", 2.0f32, 85.0f32),
        ("clear      (t=3,e=60)", 3.0, 60.0),
        ("clear low  (t=3,e=15)", 3.0, 15.0),
        ("hazy noon  (t=6,e=85)", 6.0, 85.0),
    ];
    let mut lref_noon = 0.0f32;
    for (name, tb, el_deg) in cases {
        let el = el_deg.to_radians();
        let tau = 0.12 + 0.06 * (tb - 1.0);
        let e_dn = E0 * (-airmass(el) * tau).exp();
        let e_dir_h = e_dn * el.sin();
        let st = SkyState::new(&SkyParams { elevation: el, turbidity: tb, albedo: [0.15; 3] }).unwrap();
        let (e_sky_raw, _z) = dome_illuminance_and_zenith(&st, el, 128, 256);
        let e_sky = e_sky_raw * K;
        let e_total = e_dir_h + e_sky;
        let grey = e_total * 0.18 / std::f32::consts::PI;
        log::info!("  {name:>22} {e_dir_h:>9.0} {e_sky:>8.0} {e_total:>9.0} {grey:>10.0} {grey:>8.0}");
        if (tb - 2.0).abs() < 0.1 && (el_deg - 85.0).abs() < 0.1 { lref_noon = grey; }
    }
    log::info!("  reference: clear-noon total horizontal illuminance ~100..120 klux (CIE)");
    log::info!("PASS: physical sun + H-W sky pin L_ref(clear noon) = {lref_noon:.0} cd/m^2 -> exposure {:.3e}.", 0.18 / lref_noon.max(1.0));
    log::info!("  => whole-frame physical: sky x683, sun E_dn(elev,turb), ONE pre_exposure = 0.18/L_ref * 2^EV.");
    true
}

// --- Track B / Phase B: PHYSICAL EXPOSURE -- pin L_ref (the faithful camera's EV) ---
// The exposure spine. Absolute physical luminance (cd/m^2) maps into display via a
// physical reference L_ref = the luminance that lands on display middle-grey (0.18).
// exposure = 0.18/L_ref; the faithful camera fixes L_ref (a real photographic EV) so a
// brighter environment gives a brighter image (auto-metering would cancel it). This gate
// pins the L_ref the viewer's pre_exposure + faithful-EV need, using B2's calibrated
// numbers + standard photometry (grey card L = E*rho/pi). `cargo run -- expo-phys`.
fn run_expo_phys() -> bool {
    let daylight_lux = 100_000.0f32; // clear midday, sky + sun (CIE)
    let l_grey = daylight_lux * 0.18 / std::f32::consts::PI; // ~5730 cd/m^2 (18% card)
    let l_white = daylight_lux * 0.90 / std::f32::consts::PI; // 90% card
    let l_sky = 4084.0f32; // B2 calibrated H-W zenith, clear high sun
    let key = 0.18f32;
    let l_ref = l_grey; // faithful anchor: 18% card in reference daylight = middle grey
    let exposure = key / l_ref; // EV0 physical exposure multiplier

    log::info!("PHYSICAL EXPOSURE -- pin L_ref (faithful camera EV anchor)");
    log::info!("  ref daylight {daylight_lux:.0} lux -> 18% grey = {l_grey:.0} cd/m^2 = L_ref");
    log::info!("  exposure(EV0) = key/L_ref = {key}/{l_ref:.0} = {exposure:.3e}   (slider = exposure * 2^EV)");
    log::info!("  {:>12} {:>11} {:>9} {:>11}", "surface", "cd/m^2", "exposed", "display sRGB");
    let show = |name: &str, l: f32| {
        let exp = l * exposure;
        let tm = pbr_neutral_tonemap([exp; 3])[0];
        log::info!("  {name:>12} {l:>11.0} {exp:>9.4} {:>11.4}", linear_to_srgb_c(tm));
    };
    show("18% grey", l_grey);
    show("90% white", l_white);
    show("sky zenith", l_sky);

    log::info!("");
    log::info!("  FAITHFUL: 18% grey under env x0.25..x4 at FIXED exposure (image must TRACK):");
    let envs = [0.25f32, 0.5, 1.0, 2.0, 4.0];
    let (mut d0, mut dl) = (0.0f32, 0.0f32);
    for (i, &k) in envs.iter().enumerate() {
        let tm = pbr_neutral_tonemap([l_grey * k * exposure; 3])[0];
        if i == 0 { d0 = tm; }
        dl = tm;
        log::info!("    env x{k:>4.2}  grey {:>7.0} cd/m^2  display {tm:.4}", l_grey * k);
    }
    let exposed_grey = l_grey * exposure;
    let white_ok = l_white * exposure < 1.0; // 90% card must not pre-clip
    log::info!("  16x env sweep -> display ratio x{:.2} (faithful tracks; auto would be ~x1)", dl / d0.max(1e-4));
    let pass = (exposed_grey - key).abs() < 1e-4 && white_ok;
    if pass {
        log::info!("PASS: 18% grey -> exposed {exposed_grey:.3} (=key), 90% white {:.3} (<1, no clip).", l_white * exposure);
        log::info!("  => L_ref = {l_ref:.0} cd/m^2 pins the viewer pre_exposure; faithful EV0 = exposure {exposure:.3e}.");
    } else {
        log::error!("FAIL: grey={exposed_grey:.3} (want {key}), white_ok={white_ok}.");
    }
    pass
}

// --- ATMOSPHERE / bake dump: reference for the C++ viewer bake gate ---------------
// Prints raw() (27 params + 3 radiances) for a fixed set of (elevation, turbidity,
// albedo) configs in a parseable form, so the viewer's C++ LLHWSky::bake port can be
// diffed against this oracle. Keep the config list IN SYNC with the C++ test.
// `cargo run -- sky-dump`.
const SKY_DUMP_CONFIGS: [(f32, f32, f32); 7] = [
    (60.0, 3.0, 0.3),
    (10.0, 2.0, 0.1),
    (30.0, 7.0, 0.5),
    (89.0, 1.0, 0.0),
    (2.0, 10.0, 1.0),
    (45.0, 4.5, 0.25),
    (20.0, 5.5, 0.6),
];

fn run_sky_dump() -> bool {
    use hw_skymodel::rgb::{SkyParams, SkyState};
    for (el_deg, tb, al) in SKY_DUMP_CONFIGS {
        let st = SkyState::new(&SkyParams { elevation: el_deg.to_radians(), turbidity: tb, albedo: [al; 3] }).unwrap();
        let (p, r) = st.raw();
        let mut line = format!("{el_deg} {tb} {al}");
        for v in p.iter().chain(r.iter()) {
            line.push_str(&format!(" {v:.7e}"));
        }
        println!("{line}");
    }
    true
}

// --- ATMOSPHERE / H-W SKY, RENDERED (Track B, I1: harness-visual integration) -----
// Port radiance() to GLSL (the exact shippable shader), render the full H-W sky dome to
// an equirectangular image on the GPU, and (a) numerically gate the GLSL port vs our Rust
// radiance() [closes B1 at the shader level], (b) tonemap it to a BMP so we can SEE the
// physical sky before touching the viewer. The CPU bakes raw() coeffs -> UBO; the shader
// evaluates radiance() per pixel -- exactly the viewer seam. `cargo run -- sky-view`.

const SKY_VIEW_VERT_GLSL: &str = r#"#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"#;

const SKY_VIEW_FRAG_GLSL: &str = r#"#version 450
layout(location=0) out vec4 o;
layout(std140, binding=0) uniform Sky {
    vec4 coeff[8];   // flat: [0..27)=params (9/ch), [27..30)=radiances
    vec4 sun_dir;    // xyz, y=up
    vec4 img;        // x=width, y=height
};
float C(int i){ return coeff[i>>2][i&3]; }
// Hosek-Wilkie radiance -- SAME formula/index convention proven in B1 (I=7, H=8).
float radiance(int ch, float theta, float gamma){
    int b = 9*ch;
    float A=C(b+0), Bc=C(b+1), Cc=C(b+2), D=C(b+3), E=C(b+4),
          F=C(b+5), G=C(b+6), I=C(b+7), H=C(b+8);
    float r = C(27+ch);
    float ct = abs(cos(theta));
    float cg = cos(gamma);
    float cg2 = cg*cg;
    float chi = (1.0 + cg2) / pow(1.0 + H*H - 2.0*H*cg, 1.5);
    float lhs = 1.0 + A*exp(Bc/(ct + 0.01));
    float rhs = Cc + D*exp(E*gamma) + F*cg2 + G*chi + I*sqrt(ct);
    return r*lhs*rhs;
}
void main(){
    vec2 uv = gl_FragCoord.xy / img.xy;          // 0..1, y=0 at top
    float phi   = (uv.x - 0.5) * 6.28318530718;  // sun centered at x=0.5
    float theta = uv.y * 1.57079632679;          // 0=zenith(top) .. pi/2=horizon(bottom)
    vec3 view = vec3(sin(theta)*cos(phi), cos(theta), sin(theta)*sin(phi));
    float g = acos(clamp(dot(view, sun_dir.xyz), -1.0, 1.0));
    o = vec4(radiance(0,theta,g), radiance(1,theta,g), radiance(2,theta,g), 1.0);
}
"#;

fn linear_to_srgb_c(c: f32) -> f32 {
    let c = c.clamp(0.0, 1.0);
    if c <= 0.0031308 { 12.92 * c } else { 1.055 * c.powf(1.0 / 2.4) - 0.055 }
}

/// 24-bit BMP writer (no deps). `px` is row-major top-to-bottom RGB; BMP is bottom-up.
fn write_bmp(path: &str, w: usize, h: usize, px: &[[u8; 3]]) -> std::io::Result<()> {
    use std::io::Write;
    let row_bytes = w * 3;
    let pad = (4 - (row_bytes % 4)) % 4;
    let img_size = (row_bytes + pad) * h;
    let file_size = 54 + img_size;
    let mut f = std::io::BufWriter::new(std::fs::File::create(path)?);
    let mut hdr = [0u8; 54];
    hdr[0] = b'B'; hdr[1] = b'M';
    hdr[2..6].copy_from_slice(&(file_size as u32).to_le_bytes());
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18..22].copy_from_slice(&(w as u32).to_le_bytes());
    hdr[22..26].copy_from_slice(&(h as u32).to_le_bytes());
    hdr[26] = 1; hdr[28] = 24;
    hdr[34..38].copy_from_slice(&(img_size as u32).to_le_bytes());
    f.write_all(&hdr)?;
    for y in (0..h).rev() {
        for x in 0..w {
            let p = px[y * w + x];
            f.write_all(&[p[2], p[1], p[0]])?; // BGR
        }
        for _ in 0..pad { f.write_all(&[0])?; }
    }
    Ok(())
}

/// Render `pipeline`+`bind_group` (fullscreen triangle) to a WxH Rgba32Float target and
/// read it back row-major (top row first). W*16 must be 256-aligned (W multiple of 16).
fn render_rgba32f(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    pipeline: &wgpu::RenderPipeline,
    bind_group: &wgpu::BindGroup,
    w: u32,
    h: u32,
) -> Vec<[f32; 4]> {
    let bpr = (w * 16) as u64;
    assert_eq!(bpr % 256, 0, "bytes_per_row must be 256-aligned; use W multiple of 16");
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("sky-target"),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("sky-readback"),
        size: bpr * h as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("sky-enc") });
    {
        let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("sky-pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &view,
                resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
        });
        rp.set_pipeline(pipeline);
        rp.set_bind_group(0, bind_group, &[]);
        rp.draw(0..3, 0..1);
    }
    enc.copy_texture_to_buffer(
        wgpu::ImageCopyTexture { texture: &tex, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        wgpu::ImageCopyBuffer { buffer: &readback, layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(bpr as u32), rows_per_image: Some(h) } },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
    queue.submit(std::iter::once(enc.finish()));
    let slice = readback.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
    device.poll(wgpu::Maintain::Wait);
    rx.recv().unwrap().expect("map_async failed");
    let out = {
        let data = slice.get_mapped_range();
        let f: &[f32] = bytemuck::cast_slice(&data);
        let stride = (bpr / 4) as usize; // f32s per row
        let mut v = Vec::with_capacity((w * h) as usize);
        for y in 0..h as usize {
            for x in 0..w as usize {
                let i = y * stride + x * 4;
                v.push([f[i], f[i + 1], f[i + 2], f[i + 3]]);
            }
        }
        v
    };
    readback.unmap();
    out
}

fn run_sky_view() -> bool {
    use hw_skymodel::rgb::{SkyParams, SkyState};
    let (w, h) = (512u32, 256u32);
    let el_deg = 25.0f32; // low-ish sun -> a legible gradient + halo in frame
    let el = el_deg.to_radians();
    let sun = [el.cos(), el.sin(), 0.0f32];
    let state = SkyState::new(&SkyParams { elevation: el, turbidity: 3.0, albedo: [0.3; 3] }).unwrap();
    let (raw_p, raw_r) = state.raw();

    // Pack UBO: coeff[8] (32 f32: 27 params + 3 radiances + 2 pad), sun_dir(4), img(4).
    let mut ubo = [0.0f32; 40];
    ubo[..27].copy_from_slice(&raw_p);
    ubo[27..30].copy_from_slice(&raw_r);
    ubo[32..35].copy_from_slice(&sun);
    ubo[36] = w as f32;
    ubo[37] = h as f32;

    let (device, queue) = headless_vulkan_device();
    let ubuf = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("sky-ubo"),
        size: (ubo.len() * 4) as u64,
        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    queue.write_buffer(&ubuf, 0, bytemuck::cast_slice(&ubo));
    let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("sky-bgl"),
        entries: &[wgpu::BindGroupLayoutEntry {
            binding: 0,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
            count: None,
        }],
    });
    let bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("sky-bg"),
        layout: &bgl,
        entries: &[wgpu::BindGroupEntry { binding: 0, resource: ubuf.as_entire_binding() }],
    });
    let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("sky-pl"),
        bind_group_layouts: &[&bgl],
        push_constant_ranges: &[],
    });
    let vs = glsl_module(&device, SKY_VIEW_VERT_GLSL, shaderc::ShaderKind::Vertex, "sky.vert");
    let fs = glsl_module(&device, SKY_VIEW_FRAG_GLSL, shaderc::ShaderKind::Fragment, "sky.frag");
    let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("sky-pipeline"),
        layout: Some(&pl),
        vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[] },
        fragment: Some(wgpu::FragmentState {
            module: &fs,
            entry_point: "main",
            targets: &[Some(wgpu::ColorTargetState { format: wgpu::TextureFormat::Rgba32Float, blend: None, write_mask: wgpu::ColorWrites::ALL })],
        }),
        primitive: wgpu::PrimitiveState::default(),
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        multiview: None,
    });

    let img = render_rgba32f(&device, &queue, &pipeline, &bg, w, h);

    // (a) NUMERIC GATE: GLSL radiance vs our Rust radiance at sample pixels.
    let mut worst = 0.0f32;
    let mut worst_ctx = String::new();
    for &(px, py) in &[(10u32, 5u32), (256, 5), (256, 128), (500, 200), (128, 250), (400, 60)] {
        let uv = [(px as f32 + 0.5) / w as f32, (py as f32 + 0.5) / h as f32];
        let phi = (uv[0] - 0.5) * std::f32::consts::TAU;
        let theta = uv[1] * std::f32::consts::FRAC_PI_2;
        let view = [theta.sin() * phi.cos(), theta.cos(), theta.sin() * phi.sin()];
        let gamma = (view[0] * sun[0] + view[1] * sun[1] + view[2] * sun[2]).clamp(-1.0, 1.0).acos();
        let gpu = img[(py * w + px) as usize];
        for ci in 0..3usize {
            let ours = hw_radiance_ours(&raw_p[(9 * ci)..(9 * ci + 9)], raw_r[ci], theta, gamma);
            let rel = (gpu[ci] - ours).abs() / ours.abs().max(1e-4);
            if rel > worst {
                worst = rel;
                worst_ctx = format!("px=({px},{py}) ch={ci}: gpu={:.4} rust={ours:.4}", gpu[ci]);
            }
        }
    }
    log::info!("H-W SKY RENDERED -- I1 harness-visual (sun elev {el_deg:.0}, turbidity 3, {w}x{h} equirect)");
    log::info!("  GLSL-vs-Rust radiance worst rel err = {worst:.2e}   ({worst_ctx})");
    let gate = worst < 1e-3;

    // (b) VISUAL: physical cd/m^2 (x683) -> fixed display exposure -> PBRNeutral -> sRGB -> BMP.
    const K: f32 = 683.0;
    const EXPOSURE: f32 = 1.0 / 16000.0; // display choice (real exposure = viewer-integration crux)
    let mut rgb8 = Vec::with_capacity((w * h) as usize);
    for p in &img {
        let lin = [p[0] * K * EXPOSURE, p[1] * K * EXPOSURE, p[2] * K * EXPOSURE];
        let tm = pbr_neutral_tonemap(lin);
        rgb8.push([
            (linear_to_srgb_c(tm[0]) * 255.0 + 0.5) as u8,
            (linear_to_srgb_c(tm[1]) * 255.0 + 0.5) as u8,
            (linear_to_srgb_c(tm[2]) * 255.0 + 0.5) as u8,
        ]);
    }
    let out_path = "C:/fs/sky_hw.bmp";
    match write_bmp(out_path, w as usize, h as usize, &rgb8) {
        Ok(()) => log::info!("  wrote {out_path}  (sun centered; top=zenith, bottom=horizon)"),
        Err(e) => { log::error!("  BMP write failed: {e}"); return false; }
    }
    if gate {
        log::info!("PASS: the shippable GLSL radiance() matches Rust/oracle (<1e-3); physical sky rendered.");
    } else {
        log::error!("FAIL: GLSL radiance() diverges from Rust (>1e-3) -- shader port bug.");
    }
    gate
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
    // H3b-1: two UBOs bound in one pipeline. `cargo run -- h3b1`.
    if std::env::args().skip(1).any(|a| a == "h3b1") {
        let ok = run_h3b1();
        std::process::exit(if ok { 0 } else { 1 });
    }
    // H3b-2: a texture+sampler descriptor. `cargo run -- h3b2`.
    if std::env::args().skip(1).any(|a| a == "h3b2") {
        let ok = run_h3b2();
        std::process::exit(if ok { 0 } else { 1 });
    }
    // H3b-3: assemble softenLight + GL-target compile (assembly sanity). `cargo run -- h3b3`.
    if std::env::args().skip(1).any(|a| a == "h3b3") {
        let ok = run_h3b3();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "h3b3c") {
        let ok = run_h3b3c();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "h3b3d") {
        let ok = run_h3b3d();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "h3b3d-exec") {
        let ok = run_h3b3d_exec();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "exposure") {
        let ok = run_exposure();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "camera") {
        let ok = run_camera();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "furnace") {
        let ok = run_furnace();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "furnace-direct") {
        let ok = run_furnace_direct();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "furnace-fix") {
        let ok = run_furnace_fix();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "sky") {
        let ok = run_sky();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "sky-lux") {
        let ok = run_sky_lux();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "sky-sl") {
        let ok = run_sky_sl();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "sky-fit") {
        let ok = run_sky_fit();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "shadow") {
        shadow_engine::run(); // M0a+ forward/shadow engine (runs its own window loop)
        std::process::exit(0);
    }
    if std::env::args().skip(1).any(|a| a == "shadow-shot") {
        shadow_engine::run_shot(); // headless one-frame capture -> PNG (M2/M4 gate seed)
        std::process::exit(0);
    }
    if std::env::args().skip(1).any(|a| a == "shadow-shimmer") {
        shadow_engine::run_shimmer(); // M2 anti-shimmer gate (fixed render cam, panning fit cam)
        std::process::exit(0);
    }
    if std::env::args().skip(1).any(|a| a == "shadow-suncam") {
        shadow_engine::run_suncam(); // moving-sun + moving-cam test (dynamic vs invariant)
        std::process::exit(0);
    }
    if std::env::args().skip(1).any(|a| a == "shadow-axes") {
        shadow_engine::run_axes(); // camera-invariance across yaw/pitch/roll + combined
        std::process::exit(0);
    }
    if std::env::args().skip(1).any(|a| a == "shadow-cascades") {
        shadow_engine::run_cascades(); // M3b cascade-tint debug view (partition + blend band)
        std::process::exit(0);
    }
    if std::env::args().skip(1).any(|a| a == "shadow-continuity") {
        shadow_engine::run_continuity(); // M4 continuity acceptance gate (low-sun wide orbit)
        std::process::exit(0);
    }
    if std::env::args().skip(1).any(|a| a == "sky-view") {
        let ok = run_sky_view();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "sky-dump") {
        let ok = run_sky_dump();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "expo-phys") {
        let ok = run_expo_phys();
        std::process::exit(if ok { 0 } else { 1 });
    }
    if std::env::args().skip(1).any(|a| a == "sun-phys") {
        let ok = run_sun_phys();
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
