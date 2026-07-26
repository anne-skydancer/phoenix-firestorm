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
}

impl State {
    async fn new(window: Arc<Window>) -> State {
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
            // H2: GLSL-sourced fullscreen background (no vertex buffer), then
            // the H1 WGSL triangle on top -- both shader front-ends, one frame.
            rpass.set_pipeline(&self.bg_pipeline);
            rpass.draw(0..3, 0..1);

            rpass.set_pipeline(&self.pipeline);
            rpass.set_vertex_buffer(0, self.vertex_buffer.slice(..));
            rpass.draw(0..VERTICES.len() as u32, 0..1);
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

    let event_loop = EventLoop::new().expect("event loop");
    let window = Arc::new(
        WindowBuilder::new()
            .with_title("vkharness -- Firestorm-FSVulkan (Vulkan)")
            .with_inner_size(winit::dpi::LogicalSize::new(1280.0, 720.0))
            .build(&event_loop)
            .expect("window"),
    );

    let mut state = pollster::block_on(State::new(window.clone()));

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
