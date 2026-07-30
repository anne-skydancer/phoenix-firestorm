//! Engine shadow work — see SHADOW_PLAN.md.
//!
//! **M0a: forward geometry base** — cube + ground plane + orbit camera + depth buffer.
//! **M0b: single ortho shadow map** — the cube casts a real shadow onto the plane.
//! Shared `Renderer` drives both the windowed view (`shadow`) and a headless one-frame
//! capture (`shadow-shot` -> PNG) so the capture tests exactly what ships. The capture is
//! the seed of the M2 (anti-shimmer) / M4 (continuity) gates, where motion artifacts are
//! invisible in stills and eyeballing does not suffice.
//!
//! Shaders are GLSL (shaderc), shadow sampling via Vulkan-GLSL *separate* texture+sampler
//! (`texture2D` + `samplerShadow`) — wgpu has no combined-sampler type, and that is exactly
//! what a GL backport would split into.
//!
//! `cargo run --release -- shadow`        (interactive window)
//! `cargo run --release -- shadow-shot`   (one headless frame -> PNG, path logged)

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
const SHADOW_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth32Float;
const SHADOW_SIZE: u32 = 2048;

// The sun: direction the light TRAVELS (down, toward +x/+z; ~59deg elevation). So the cast
// shadow lands on the +x/+z side of the cube — the hand-computable prediction we verify.
const SUN: [f32; 3] = [0.4, -0.9, 0.35];

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
    light_vp: [[f32; 4]; 4],
    sun_dir: [f32; 4],
    base_color: [f32; 4],
}

/// Sun ortho view-projection. (M2 replaces this with the stable sphere-bounded,
/// texel-snapped cascade.)
fn light_matrix() -> Mat4 {
    let dir = Vec3::from_array(SUN).normalize();
    let center = Vec3::new(0.0, 0.5, 0.0);
    let up = if dir.y.abs() > 0.99 { Vec3::Z } else { Vec3::Y };
    let eye = center - dir * 12.0;
    let view = Mat4::look_at_rh(eye, center, up);
    let proj = Mat4::orthographic_rh(-6.0, 6.0, -6.0, 6.0, 0.1, 30.0);
    proj * view
}

/// Orbit camera -> view-projection.
fn camera(proj: Mat4, yaw: f32, pitch: f32, radius: f32) -> Mat4 {
    let center = Vec3::new(0.0, 0.5, 0.0);
    let eye = center + Vec3::new(pitch.cos() * yaw.sin(), pitch.sin(), pitch.cos() * yaw.cos()) * radius;
    proj * Mat4::look_at_rh(eye, center, Vec3::Y)
}

// ---- geometry -------------------------------------------------------------------

fn cube() -> (Vec<Vtx>, Vec<u16>) {
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

// ---- shaders --------------------------------------------------------------------

const SHADOW_VERT: &str = r#"
#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(set = 0, binding = 0) uniform U {
    mat4 view_proj; mat4 model; mat4 light_vp; vec4 sun_dir; vec4 base_color;
} u;
void main() { gl_Position = u.light_vp * u.model * vec4(pos, 1.0); }
"#;

const VERT: &str = r#"
#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(set = 0, binding = 0) uniform U {
    mat4 view_proj; mat4 model; mat4 light_vp; vec4 sun_dir; vec4 base_color;
} u;
layout(location = 0) out vec3 v_nrm;
layout(location = 1) out vec3 v_world;
void main() {
    vec4 wp = u.model * vec4(pos, 1.0);
    v_world = wp.xyz;
    v_nrm = mat3(u.model) * nrm;
    gl_Position = u.view_proj * wp;
}
"#;

const FRAG: &str = r#"
#version 450
layout(location = 0) in vec3 v_nrm;
layout(location = 1) in vec3 v_world;
layout(location = 0) out vec4 frag;
layout(set = 0, binding = 0) uniform U {
    mat4 view_proj; mat4 model; mat4 light_vp; vec4 sun_dir; vec4 base_color;
} u;
layout(set = 1, binding = 0) uniform texture2D    shadowTex;
layout(set = 1, binding = 1) uniform samplerShadow shadowSamp;

float shadow_lit(vec3 wp) {
    vec4 lc = u.light_vp * vec4(wp, 1.0);
    vec3 p = lc.xyz / lc.w;
    vec2 uv = p.xy * vec2(0.5, -0.5) + 0.5;
    float ref = p.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ref > 1.0) return 1.0;
    return texture(sampler2DShadow(shadowTex, shadowSamp), vec3(uv, ref - 0.0015));
}
void main() {
    vec3 N = normalize(v_nrm);
    vec3 L = -normalize(u.sun_dir.xyz);
    float ndl = max(dot(N, L), 0.0);
    float lit = shadow_lit(v_world);
    vec3 col = u.base_color.rgb * (0.25 + 0.75 * ndl * lit);
    frag = vec4(col, 1.0);
}
"#;

// ---- object ---------------------------------------------------------------------

struct Obj {
    vbuf: wgpu::Buffer,
    ibuf: wgpu::Buffer,
    nidx: u32,
    ubo: wgpu::Buffer,
    bind: wgpu::BindGroup,
    model: Mat4,
    color: [f32; 4],
    caster: bool,
}

impl Obj {
    fn new(
        device: &wgpu::Device,
        bgl: &wgpu::BindGroupLayout,
        verts: &[Vtx],
        idx: &[u16],
        model: Mat4,
        color: [f32; 4],
        caster: bool,
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
            label: Some("obj-bind"),
            layout: bgl,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: ubo.as_entire_binding() }],
        });
        Obj { vbuf, ibuf, nidx: idx.len() as u32, ubo, bind, model, color, caster }
    }
}

fn depth_texture(device: &wgpu::Device, w: u32, h: u32, format: wgpu::TextureFormat, label: &str) -> wgpu::Texture {
    device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    })
}

// ---- shared renderer (windowed + headless use the SAME code) ---------------------

struct Renderer {
    pipeline: wgpu::RenderPipeline,
    shadow_pipeline: wgpu::RenderPipeline,
    shadow_view: wgpu::TextureView,
    shadow_bind: wgpu::BindGroup,
    objs: Vec<Obj>,
}

impl Renderer {
    fn new(device: &wgpu::Device, color_format: wgpu::TextureFormat) -> Renderer {
        let obj_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
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
        let shadow_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("shadow-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Comparison),
                    count: None,
                },
            ],
        });

        let shadow_tex = depth_texture(device, SHADOW_SIZE, SHADOW_SIZE, SHADOW_FORMAT, "shadow-map");
        let shadow_view = shadow_tex.create_view(&wgpu::TextureViewDescriptor::default());
        let shadow_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("shadow-cmp"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Nearest, // M0b hard shadow; M1 -> PCF
            min_filter: wgpu::FilterMode::Nearest,
            mipmap_filter: wgpu::FilterMode::Nearest,
            compare: Some(wgpu::CompareFunction::LessEqual),
            ..Default::default()
        });
        let shadow_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("shadow-bind"),
            layout: &shadow_bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&shadow_view) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&shadow_sampler) },
            ],
        });

        let shadow_vs = glsl_module(device, SHADOW_VERT, shaderc::ShaderKind::Vertex, "shadow.vert");
        let shadow_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("shadow-pl"),
            bind_group_layouts: &[&obj_bgl],
            push_constant_ranges: &[],
        });
        let shadow_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("shadow"),
            layout: Some(&shadow_pl),
            vertex: wgpu::VertexState { module: &shadow_vs, entry_point: "main", buffers: &[Vtx::LAYOUT] },
            fragment: None,
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: SHADOW_FORMAT,
                depth_write_enabled: true,
                depth_compare: wgpu::CompareFunction::Less,
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState { constant: 2, slope_scale: 2.0, clamp: 0.0 },
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });

        let vs = glsl_module(device, VERT, shaderc::ShaderKind::Vertex, "m0.vert");
        let fs = glsl_module(device, FRAG, shaderc::ShaderKind::Fragment, "m0.frag");
        let main_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("main-pl"),
            bind_group_layouts: &[&obj_bgl, &shadow_bgl],
            push_constant_ranges: &[],
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("forward"),
            layout: Some(&main_pl),
            vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[Vtx::LAYOUT] },
            fragment: Some(wgpu::FragmentState {
                module: &fs,
                entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: color_format,
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
                format: DEPTH_FORMAT,
                depth_write_enabled: true,
                depth_compare: wgpu::CompareFunction::Less,
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });

        let mut objs = Vec::new();
        let (cv, ci) = cube();
        objs.push(Obj::new(device, &obj_bgl, &cv, &ci, Mat4::IDENTITY, [0.85, 0.45, 0.20, 1.0], true));
        let (pv, pi) = plane(20.0);
        objs.push(Obj::new(device, &obj_bgl, &pv, &pi, Mat4::IDENTITY, [0.55, 0.55, 0.58, 1.0], false));

        Renderer { pipeline, shadow_pipeline, shadow_view, shadow_bind, objs }
    }

    fn update(&self, queue: &wgpu::Queue, view_proj: Mat4, light_vp: Mat4, sun: Vec3) {
        for obj in &self.objs {
            let u = Uniforms {
                view_proj: view_proj.to_cols_array_2d(),
                model: obj.model.to_cols_array_2d(),
                light_vp: light_vp.to_cols_array_2d(),
                sun_dir: [sun.x, sun.y, sun.z, 0.0],
                base_color: obj.color,
            };
            queue.write_buffer(&obj.ubo, 0, bytemuck::bytes_of(&u));
        }
    }

    /// Shadow pass (casters -> sun depth) then main forward pass into `color`+`depth`.
    fn encode(&self, enc: &mut wgpu::CommandEncoder, color: &wgpu::TextureView, depth: &wgpu::TextureView) {
        {
            let mut sp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("shadow"),
                color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &self.shadow_view,
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Store }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            sp.set_pipeline(&self.shadow_pipeline);
            for obj in &self.objs {
                if !obj.caster {
                    continue;
                }
                sp.set_bind_group(0, &obj.bind, &[]);
                sp.set_vertex_buffer(0, obj.vbuf.slice(..));
                sp.set_index_buffer(obj.ibuf.slice(..), wgpu::IndexFormat::Uint16);
                sp.draw_indexed(0..obj.nidx, 0, 0..1);
            }
        }
        {
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("forward"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: color,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color { r: 0.50, g: 0.62, b: 0.78, a: 1.0 }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: depth,
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Store }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            rp.set_pipeline(&self.pipeline);
            rp.set_bind_group(1, &self.shadow_bind, &[]);
            for obj in &self.objs {
                rp.set_bind_group(0, &obj.bind, &[]);
                rp.set_vertex_buffer(0, obj.vbuf.slice(..));
                rp.set_index_buffer(obj.ibuf.slice(..), wgpu::IndexFormat::Uint16);
                rp.draw_indexed(0..obj.nidx, 0, 0..1);
            }
        }
    }
}

// ---- windowed view --------------------------------------------------------------

struct Scene {
    window: Arc<Window>,
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    depth: wgpu::TextureView,
    renderer: Renderer,
    proj: Mat4,
    yaw: f32,
    pitch: f32,
    auto_rotate: bool,
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

            let renderer = Renderer::new(&device, config.format);
            let depth = depth_texture(&device, config.width, config.height, DEPTH_FORMAT, "depth")
                .create_view(&wgpu::TextureViewDescriptor::default());
            let aspect = config.width as f32 / config.height as f32;
            let proj = Mat4::perspective_rh(45f32.to_radians(), aspect, 0.1, 100.0);

            Scene { window, surface, device, queue, config, depth, renderer, proj, yaw: 0.7, pitch: 0.5, auto_rotate: false }
        })
    }

    fn resize(&mut self, size: winit::dpi::PhysicalSize<u32>) {
        if size.width == 0 || size.height == 0 {
            return;
        }
        self.config.width = size.width;
        self.config.height = size.height;
        self.surface.configure(&self.device, &self.config);
        self.depth = depth_texture(&self.device, size.width, size.height, DEPTH_FORMAT, "depth")
            .create_view(&wgpu::TextureViewDescriptor::default());
        let aspect = size.width as f32 / size.height as f32;
        self.proj = Mat4::perspective_rh(45f32.to_radians(), aspect, 0.1, 100.0);
    }

    fn render(&mut self) -> Result<(), wgpu::SurfaceError> {
        if self.auto_rotate {
            self.yaw += 0.0015;
        }
        let view_proj = camera(self.proj, self.yaw, self.pitch, 5.0);
        let light_vp = light_matrix();
        let sun = Vec3::from_array(SUN).normalize();
        self.renderer.update(&self.queue, view_proj, light_vp, sun);

        let frame = self.surface.get_current_texture()?;
        let target = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        let mut enc = self.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("frame") });
        self.renderer.encode(&mut enc, &target, &self.depth);
        self.queue.submit([enc.finish()]);
        frame.present();
        Ok(())
    }
}

pub fn run() {
    let event_loop = EventLoop::new().expect("event loop");
    let window = Arc::new(
        WindowBuilder::new()
            .with_title("vkharness -- engine shadows M0b (cast shadow)")
            .with_inner_size(winit::dpi::LogicalSize::new(1280.0, 800.0))
            .build(&event_loop)
            .expect("window"),
    );
    let mut scene = Scene::new(window.clone());
    log::info!("M0b: cube casts a shadow onto the plane (+x/+z side). Starts still. Arrows orbit, Space auto-spin, Esc quits.");
    event_loop
        .run(move |event, elwt| {
            elwt.set_control_flow(winit::event_loop::ControlFlow::Poll);
            match event {
                Event::WindowEvent { event, window_id } if window_id == scene.window.id() => match event {
                    WindowEvent::CloseRequested => elwt.exit(),
                    WindowEvent::KeyboardInput {
                        event: KeyEvent { physical_key: PhysicalKey::Code(code), state: ElementState::Pressed, .. },
                        ..
                    } => {
                        let step = 0.06;
                        match code {
                            KeyCode::Escape => elwt.exit(),
                            KeyCode::Space => scene.auto_rotate = !scene.auto_rotate,
                            KeyCode::ArrowLeft => scene.yaw -= step,
                            KeyCode::ArrowRight => scene.yaw += step,
                            KeyCode::ArrowUp => scene.pitch = (scene.pitch + step).min(1.5),
                            KeyCode::ArrowDown => scene.pitch = (scene.pitch - step).max(-0.2),
                            _ => {}
                        }
                    }
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

// ---- headless one-frame capture (M2/M4 gate seed) -------------------------------

/// Render one frame headless (no window) to a PNG. `yaw_deg`/`pitch_deg` pick the camera.
/// Returns the path written. Width is a multiple of 64 so the readback row stride needs
/// no 256-byte padding.
pub fn shot(out: &std::path::Path, w: u32, h: u32, yaw_deg: f32, pitch_deg: f32) {
    const FMT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8UnormSrgb;
    let (device, queue) = crate::headless_vulkan_device();
    let renderer = Renderer::new(&device, FMT);

    let color = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("shot-color"),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: FMT,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let color_view = color.create_view(&wgpu::TextureViewDescriptor::default());
    let depth = depth_texture(&device, w, h, DEPTH_FORMAT, "shot-depth")
        .create_view(&wgpu::TextureViewDescriptor::default());

    let proj = Mat4::perspective_rh(45f32.to_radians(), w as f32 / h as f32, 0.1, 100.0);
    let view_proj = camera(proj, yaw_deg.to_radians(), pitch_deg.to_radians(), 5.5);
    renderer.update(&queue, view_proj, light_matrix(), Vec3::from_array(SUN).normalize());

    let bpr = w * 4; // Rgba8 = 4 bytes; w multiple of 64 -> bpr multiple of 256 (no padding)
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("shot-readback"),
        size: (bpr * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });

    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("shot") });
    renderer.encode(&mut enc, &color_view, &depth);
    enc.copy_texture_to_buffer(
        wgpu::ImageCopyTexture { texture: &color, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        wgpu::ImageCopyBuffer {
            buffer: &readback,
            layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(bpr), rows_per_image: Some(h) },
        },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
    queue.submit([enc.finish()]);

    let slice = readback.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| tx.send(r).unwrap());
    device.poll(wgpu::Maintain::Wait);
    rx.recv().unwrap().unwrap();
    let pixels = slice.get_mapped_range().to_vec();
    readback.unmap();

    let file = std::fs::File::create(out).expect("create png");
    let mut encoder = png::Encoder::new(std::io::BufWriter::new(file), w, h);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    encoder.write_header().expect("png header").write_image_data(&pixels).expect("png data");
    log::info!("shadow-shot: wrote {}x{} -> {}", w, h, out.display());
}

pub fn run_shot() {
    let out = std::env::temp_dir().join("shadow_shot.png");
    shot(&out, 1280, 800, 40.0, 32.0);
}
