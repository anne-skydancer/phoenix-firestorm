//! Engine shadow work — see SHADOW_PLAN.md.
//!
//! **M0a: forward geometry base.** Cube + ground plane + orbit camera + a real depth
//! buffer + simple directional (sun) N·L shading. No shadows yet — M0b bolts on the
//! shadow map. Shaders are GLSL (compiled via the harness's shaderc path) so the proven
//! design stays portable for the eventual GL backport.
//!
//! `cargo run --release -- shadow`

use std::sync::Arc;

use glam::{Mat4, Vec3};
use wgpu::util::DeviceExt;
use winit::{
    event::{ElementState, Event, KeyEvent, WindowEvent},
    event_loop::EventLoop,
    keyboard::{KeyCode, PhysicalKey},
    window::{Window, WindowBuilder},
};

use crate::glsl_module;

const DEPTH_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth32Float;

// ---- vertex + uniforms ----------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Vtx {
    pos: [f32; 3],
    nrm: [f32; 3],
}

impl Vtx {
    const LAYOUT: wgpu::VertexBufferLayout<'static> = wgpu::VertexBufferLayout {
        array_stride: std::mem::size_of::<Vtx>() as wgpu::BufferAddress,
        step_mode: wgpu::VertexStepMode::Vertex,
        attributes: &[
            wgpu::VertexAttribute { offset: 0, shader_location: 0, format: wgpu::VertexFormat::Float32x3 },
            wgpu::VertexAttribute { offset: 12, shader_location: 1, format: wgpu::VertexFormat::Float32x3 },
        ],
    };
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Uniforms {
    view_proj: [[f32; 4]; 4],
    model: [[f32; 4]; 4],
    sun_dir: [f32; 4],    // xyz = direction light travels (normalized); w unused
    base_color: [f32; 4], // rgb; a unused
}

// ---- geometry -------------------------------------------------------------------

/// Unit cube: x,z in [-0.5,0.5], y in [0,1] (sits ON the ground plane at y=0).
/// Per-face normals so the lighting reads as a cube.
fn cube() -> (Vec<Vtx>, Vec<u16>) {
    // (normal, 4 CCW corners)
    let faces: [([f32; 3], [[f32; 3]; 4]); 6] = [
        ([ 1., 0.,  0.], [[ 0.5, 0., -0.5], [ 0.5, 1., -0.5], [ 0.5, 1.,  0.5], [ 0.5, 0.,  0.5]]),
        ([-1., 0.,  0.], [[-0.5, 0.,  0.5], [-0.5, 1.,  0.5], [-0.5, 1., -0.5], [-0.5, 0., -0.5]]),
        ([ 0., 1.,  0.], [[-0.5, 1., -0.5], [-0.5, 1.,  0.5], [ 0.5, 1.,  0.5], [ 0.5, 1., -0.5]]),
        ([ 0.,-1.,  0.], [[-0.5, 0.,  0.5], [-0.5, 0., -0.5], [ 0.5, 0., -0.5], [ 0.5, 0.,  0.5]]),
        ([ 0., 0.,  1.], [[ 0.5, 0.,  0.5], [ 0.5, 1.,  0.5], [-0.5, 1.,  0.5], [-0.5, 0.,  0.5]]),
        ([ 0., 0., -1.], [[-0.5, 0., -0.5], [-0.5, 1., -0.5], [ 0.5, 1., -0.5], [ 0.5, 0., -0.5]]),
    ];
    let mut v = Vec::new();
    let mut idx = Vec::new();
    for (n, quad) in faces.iter() {
        let base = v.len() as u16;
        for p in quad.iter() {
            v.push(Vtx { pos: *p, nrm: *n });
        }
        idx.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
    }
    (v, idx)
}

/// Ground plane: a `2*half` quad at y=0, normal up.
fn plane(half: f32) -> (Vec<Vtx>, Vec<u16>) {
    let n = [0., 1., 0.];
    let v = vec![
        Vtx { pos: [-half, 0., -half], nrm: n },
        Vtx { pos: [-half, 0.,  half], nrm: n },
        Vtx { pos: [ half, 0.,  half], nrm: n },
        Vtx { pos: [ half, 0., -half], nrm: n },
    ];
    (v, vec![0u16, 1, 2, 0, 2, 3])
}

// ---- shaders (GLSL -> shaderc -> SPIR-V; GL-portable) ----------------------------

const VERT: &str = r#"
#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(set = 0, binding = 0) uniform U {
    mat4 view_proj;
    mat4 model;
    vec4 sun_dir;
    vec4 base_color;
} u;
layout(location = 0) out vec3 v_nrm;
void main() {
    gl_Position = u.view_proj * u.model * vec4(pos, 1.0);
    v_nrm = mat3(u.model) * nrm;
}
"#;

const FRAG: &str = r#"
#version 450
layout(location = 0) in vec3 v_nrm;
layout(location = 0) out vec4 frag;
layout(set = 0, binding = 0) uniform U {
    mat4 view_proj;
    mat4 model;
    vec4 sun_dir;
    vec4 base_color;
} u;
void main() {
    vec3 N = normalize(v_nrm);
    vec3 L = -normalize(u.sun_dir.xyz);      // toward the sun
    float ndl = max(dot(N, L), 0.0);
    vec3 col = u.base_color.rgb * (0.25 + 0.75 * ndl); // ambient + diffuse
    frag = vec4(col, 1.0);
}
"#;

// ---- object + scene -------------------------------------------------------------

struct Obj {
    vbuf: wgpu::Buffer,
    ibuf: wgpu::Buffer,
    nidx: u32,
    ubo: wgpu::Buffer,
    bind: wgpu::BindGroup,
    model: Mat4,
    color: [f32; 4],
}

impl Obj {
    fn new(
        device: &wgpu::Device,
        bgl: &wgpu::BindGroupLayout,
        verts: &[Vtx],
        idx: &[u16],
        model: Mat4,
        color: [f32; 4],
    ) -> Obj {
        let vbuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("vb"),
            contents: bytemuck::cast_slice(verts),
            usage: wgpu::BufferUsages::VERTEX,
        });
        let ibuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("ib"),
            contents: bytemuck::cast_slice(idx),
            usage: wgpu::BufferUsages::INDEX,
        });
        let ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("ubo"),
            size: std::mem::size_of::<Uniforms>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("bind"),
            layout: bgl,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: ubo.as_entire_binding() }],
        });
        Obj { vbuf, ibuf, nidx: idx.len() as u32, ubo, bind, model, color }
    }
}

fn make_depth(device: &wgpu::Device, w: u32, h: u32) -> wgpu::TextureView {
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("depth"),
        size: wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: DEPTH_FORMAT,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
        view_formats: &[],
    });
    tex.create_view(&wgpu::TextureViewDescriptor::default())
}

struct Scene {
    window: Arc<Window>,
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    depth: wgpu::TextureView,
    pipeline: wgpu::RenderPipeline,
    objs: Vec<Obj>,
    proj: Mat4,
    yaw: f32,
    pitch: f32,
}

impl Scene {
    fn new(window: Arc<Window>) -> Scene {
        pollster::block_on(async {
            let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
                backends: wgpu::Backends::VULKAN,
                ..Default::default()
            });
            let surface = instance.create_surface(window.clone()).expect("surface");
            let adapter = instance
                .request_adapter(&wgpu::RequestAdapterOptions {
                    power_preference: wgpu::PowerPreference::HighPerformance,
                    compatible_surface: Some(&surface),
                    force_fallback_adapter: false,
                })
                .await
                .expect("no Vulkan adapter");
            let info = adapter.get_info();
            log::info!("adapter: {} | backend={:?} | {} {}", info.name, info.backend, info.driver, info.driver_info);
            let (device, queue) = adapter
                .request_device(
                    &wgpu::DeviceDescriptor {
                        label: Some("shadow-engine"),
                        required_features: wgpu::Features::empty(),
                        required_limits: wgpu::Limits::default(),
                    },
                    None,
                )
                .await
                .expect("request_device");

            let size = window.inner_size();
            let caps = surface.get_capabilities(&adapter);
            let format = caps.formats.iter().copied().find(|f| f.is_srgb()).unwrap_or(caps.formats[0]);
            let config = wgpu::SurfaceConfiguration {
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                format,
                width: size.width.max(1),
                height: size.height.max(1),
                present_mode: caps.present_modes[0],
                alpha_mode: caps.alpha_modes[0],
                view_formats: vec![],
                desired_maximum_frame_latency: 2,
            };
            surface.configure(&device, &config);

            let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("obj-bgl"),
                entries: &[wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                }],
            });
            let pl_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("obj-pl"),
                bind_group_layouts: &[&bgl],
                push_constant_ranges: &[],
            });
            let vs = glsl_module(&device, VERT, shaderc::ShaderKind::Vertex, "m0.vert");
            let fs = glsl_module(&device, FRAG, shaderc::ShaderKind::Fragment, "m0.frag");
            let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("forward"),
                layout: Some(&pl_layout),
                vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[Vtx::LAYOUT] },
                fragment: Some(wgpu::FragmentState {
                    module: &fs,
                    entry_point: "main",
                    targets: &[Some(wgpu::ColorTargetState {
                        format: config.format,
                        blend: Some(wgpu::BlendState::REPLACE),
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode: None, // M0a: don't fight winding; revisit when shadows land
                    ..Default::default()
                },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: DEPTH_FORMAT,
                    depth_write_enabled: true,
                    depth_compare: wgpu::CompareFunction::Less,
                    stencil: wgpu::StencilState::default(),
                    bias: wgpu::DepthBiasState::default(),
                }),
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            });

            let depth = make_depth(&device, config.width, config.height);

            let mut objs = Vec::new();
            let (cv, ci) = cube();
            objs.push(Obj::new(&device, &bgl, &cv, &ci, Mat4::IDENTITY, [0.85, 0.45, 0.20, 1.0]));
            let (pv, pi) = plane(20.0);
            objs.push(Obj::new(&device, &bgl, &pv, &pi, Mat4::IDENTITY, [0.55, 0.55, 0.58, 1.0]));

            let aspect = config.width as f32 / config.height as f32;
            let proj = Mat4::perspective_rh(45f32.to_radians(), aspect, 0.1, 100.0);

            Scene { window, surface, device, queue, config, depth, pipeline, objs, proj, yaw: 0.7, pitch: 0.5 }
        })
    }

    fn resize(&mut self, size: winit::dpi::PhysicalSize<u32>) {
        if size.width == 0 || size.height == 0 {
            return;
        }
        self.config.width = size.width;
        self.config.height = size.height;
        self.surface.configure(&self.device, &self.config);
        self.depth = make_depth(&self.device, size.width, size.height);
        let aspect = size.width as f32 / size.height as f32;
        self.proj = Mat4::perspective_rh(45f32.to_radians(), aspect, 0.1, 100.0);
    }

    fn render(&mut self) -> Result<(), wgpu::SurfaceError> {
        // Slow auto-orbit so depth ordering + shading are visible from all sides.
        self.yaw += 0.005;
        let center = Vec3::new(0.0, 0.5, 0.0);
        let r = 5.0;
        let eye = center
            + Vec3::new(self.pitch.cos() * self.yaw.sin(), self.pitch.sin(), self.pitch.cos() * self.yaw.cos()) * r;
        let view = Mat4::look_at_rh(eye, center, Vec3::Y);
        let view_proj = self.proj * view;
        let sun = Vec3::new(0.4, -0.9, 0.35).normalize();

        for obj in &self.objs {
            let u = Uniforms {
                view_proj: view_proj.to_cols_array_2d(),
                model: obj.model.to_cols_array_2d(),
                sun_dir: [sun.x, sun.y, sun.z, 0.0],
                base_color: obj.color,
            };
            self.queue.write_buffer(&obj.ubo, 0, bytemuck::bytes_of(&u));
        }

        let frame = self.surface.get_current_texture()?;
        let target = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        let mut enc = self.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("frame") });
        {
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("forward"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &target,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color { r: 0.50, g: 0.62, b: 0.78, a: 1.0 }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &self.depth,
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Store }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            rp.set_pipeline(&self.pipeline);
            for obj in &self.objs {
                rp.set_bind_group(0, &obj.bind, &[]);
                rp.set_vertex_buffer(0, obj.vbuf.slice(..));
                rp.set_index_buffer(obj.ibuf.slice(..), wgpu::IndexFormat::Uint16);
                rp.draw_indexed(0..obj.nidx, 0, 0..1);
            }
        }
        self.queue.submit([enc.finish()]);
        frame.present();
        Ok(())
    }
}

pub fn run() {
    let event_loop = EventLoop::new().expect("event loop");
    let window = Arc::new(
        WindowBuilder::new()
            .with_title("vkharness -- engine shadows M0a (forward geometry base)")
            .with_inner_size(winit::dpi::LogicalSize::new(1280.0, 800.0))
            .build(&event_loop)
            .expect("window"),
    );
    let mut scene = Scene::new(window.clone());
    log::info!("M0a: forward geometry (cube + plane + depth), auto-orbit. Esc quits.");
    event_loop
        .run(move |event, elwt| {
            elwt.set_control_flow(winit::event_loop::ControlFlow::Poll);
            match event {
                Event::WindowEvent { event, window_id } if window_id == scene.window.id() => match event {
                    WindowEvent::CloseRequested => elwt.exit(),
                    WindowEvent::KeyboardInput {
                        event: KeyEvent { physical_key: PhysicalKey::Code(KeyCode::Escape), state: ElementState::Pressed, .. },
                        ..
                    } => elwt.exit(),
                    WindowEvent::Resized(sz) => scene.resize(sz),
                    WindowEvent::RedrawRequested => {
                        match scene.render() {
                            Ok(()) => {}
                            Err(wgpu::SurfaceError::Lost) => {
                                let s = scene.window.inner_size();
                                scene.resize(s);
                            }
                            Err(wgpu::SurfaceError::OutOfMemory) => elwt.exit(),
                            Err(e) => log::warn!("render: {e:?}"),
                        }
                        scene.window.request_redraw();
                    }
                    _ => {}
                },
                Event::AboutToWait => scene.window.request_redraw(),
                _ => {}
            }
        })
        .expect("event loop");
}
