//! Engine shadow work — see SHADOW_PLAN.md.
//!
//! **M0a: forward geometry base** — cube + ground plane + orbit camera + depth buffer.
//! **M0b: single ortho shadow map** — the cube casts a real shadow onto the plane (and
//! self-shadows). One depth-only pass from the sun's viewpoint, then the main pass
//! projects each fragment into light space and compares.
//!
//! Shaders are GLSL (compiled via the harness's shaderc path). Shadow sampling uses
//! Vulkan-GLSL *separate* texture + sampler (`texture2D` + `samplerShadow`) because wgpu
//! has no combined-sampler type — this is also what a GL backport would split into.
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
const SHADOW_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth32Float;
const SHADOW_SIZE: u32 = 2048;

// The sun: direction the light TRAVELS (down, toward +x/+z). So the cast shadow lands on
// the +x/+z side of the cube — the hand-computable prediction we verify against.
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
    light_vp: [[f32; 4]; 4], // sun's ortho view-projection
    sun_dir: [f32; 4],       // xyz = direction light travels (normalized)
    base_color: [f32; 4],
}

/// Sun ortho view-projection: look from above the scene down the sun direction, ortho box
/// sized to cover the cube + where its shadow falls. (M2 replaces this with the stable
/// sphere-bounded, texel-snapped cascade.)
fn light_matrix() -> Mat4 {
    let dir = Vec3::from_array(SUN).normalize();
    let center = Vec3::new(0.0, 0.5, 0.0);
    let up = if dir.y.abs() > 0.99 { Vec3::Z } else { Vec3::Y };
    let eye = center - dir * 12.0; // back off toward the sun
    let view = Mat4::look_at_rh(eye, center, up);
    let proj = Mat4::orthographic_rh(-6.0, 6.0, -6.0, 6.0, 0.1, 30.0);
    proj * view
}

// ---- geometry -------------------------------------------------------------------

/// Unit cube: x,z in [-0.5,0.5], y in [0,1] (sits ON the ground plane at y=0).
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

// Shadow pass: depth only, from the sun's viewpoint.
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
// Separate texture + comparison sampler (wgpu has no combined sampler type).
layout(set = 1, binding = 0) uniform texture2D    shadowTex;
layout(set = 1, binding = 1) uniform samplerShadow shadowSamp;

// 1.0 = lit, 0.0 = in shadow. Fragments outside the sun's ortho box are lit.
float shadow_lit(vec3 wp) {
    vec4 lc = u.light_vp * vec4(wp, 1.0);
    vec3 p = lc.xyz / lc.w;
    vec2 uv = p.xy * vec2(0.5, -0.5) + 0.5; // NDC(y-up) -> texture(v-down)
    float ref = p.z;                         // wgpu clip z already in [0,1]
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ref > 1.0) return 1.0;
    return texture(sampler2DShadow(shadowTex, shadowSamp), vec3(uv, ref - 0.0015));
}
void main() {
    vec3 N = normalize(v_nrm);
    vec3 L = -normalize(u.sun_dir.xyz);        // toward the sun
    float ndl = max(dot(N, L), 0.0);
    float lit = shadow_lit(v_world);
    // Ambient fill stays; direct sun is gated by both facing (N.L) and occlusion (lit).
    vec3 col = u.base_color.rgb * (0.25 + 0.75 * ndl * lit);
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

fn make_depth(device: &wgpu::Device, w: u32, h: u32, format: wgpu::TextureFormat, label: &str) -> wgpu::Texture {
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

struct Scene {
    window: Arc<Window>,
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    depth: wgpu::TextureView,
    pipeline: wgpu::RenderPipeline,
    shadow_pipeline: wgpu::RenderPipeline,
    shadow_view: wgpu::TextureView,
    shadow_bind: wgpu::BindGroup,
    objs: Vec<Obj>,
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

            // group 0: per-object uniforms
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
            // group 1: shadow map + comparison sampler (shared)
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

            // shadow map texture + sampler
            let shadow_tex = make_depth(&device, SHADOW_SIZE, SHADOW_SIZE, SHADOW_FORMAT, "shadow-map");
            let shadow_view = shadow_tex.create_view(&wgpu::TextureViewDescriptor::default());
            let shadow_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some("shadow-cmp"),
                address_mode_u: wgpu::AddressMode::ClampToEdge,
                address_mode_v: wgpu::AddressMode::ClampToEdge,
                address_mode_w: wgpu::AddressMode::ClampToEdge,
                mag_filter: wgpu::FilterMode::Nearest, // M0b = hard shadow; M1 makes it PCF
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

            // shadow pipeline (depth only, from the sun)
            let shadow_vs = glsl_module(&device, SHADOW_VERT, shaderc::ShaderKind::Vertex, "shadow.vert");
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
                    // slope-scaled bias to keep the receiver off the caster (anti-acne)
                    bias: wgpu::DepthBiasState { constant: 2, slope_scale: 2.0, clamp: 0.0 },
                }),
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            });

            // main forward pipeline
            let vs = glsl_module(&device, VERT, shaderc::ShaderKind::Vertex, "m0.vert");
            let fs = glsl_module(&device, FRAG, shaderc::ShaderKind::Fragment, "m0.frag");
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
                        format: config.format,
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

            let depth = make_depth(&device, config.width, config.height, DEPTH_FORMAT, "depth")
                .create_view(&wgpu::TextureViewDescriptor::default());

            let mut objs = Vec::new();
            let (cv, ci) = cube();
            objs.push(Obj::new(&device, &obj_bgl, &cv, &ci, Mat4::IDENTITY, [0.85, 0.45, 0.20, 1.0], true));
            let (pv, pi) = plane(20.0);
            objs.push(Obj::new(&device, &obj_bgl, &pv, &pi, Mat4::IDENTITY, [0.55, 0.55, 0.58, 1.0], false));

            let aspect = config.width as f32 / config.height as f32;
            let proj = Mat4::perspective_rh(45f32.to_radians(), aspect, 0.1, 100.0);

            Scene {
                window, surface, device, queue, config, depth, pipeline, shadow_pipeline,
                shadow_view, shadow_bind, objs, proj, yaw: 0.7, pitch: 0.5, auto_rotate: false,
            }
        })
    }

    fn resize(&mut self, size: winit::dpi::PhysicalSize<u32>) {
        if size.width == 0 || size.height == 0 {
            return;
        }
        self.config.width = size.width;
        self.config.height = size.height;
        self.surface.configure(&self.device, &self.config);
        self.depth = make_depth(&self.device, size.width, size.height, DEPTH_FORMAT, "depth")
            .create_view(&wgpu::TextureViewDescriptor::default());
        let aspect = size.width as f32 / size.height as f32;
        self.proj = Mat4::perspective_rh(45f32.to_radians(), aspect, 0.1, 100.0);
    }

    fn render(&mut self) -> Result<(), wgpu::SurfaceError> {
        // Starts still (inspect the shadow); Space toggles a gentle auto-orbit, arrows steer.
        if self.auto_rotate {
            self.yaw += 0.0015;
        }
        let center = Vec3::new(0.0, 0.5, 0.0);
        let r = 5.0;
        let eye = center
            + Vec3::new(self.pitch.cos() * self.yaw.sin(), self.pitch.sin(), self.pitch.cos() * self.yaw.cos()) * r;
        let view = Mat4::look_at_rh(eye, center, Vec3::Y);
        let view_proj = self.proj * view;
        let light_vp = light_matrix();
        let sun = Vec3::from_array(SUN).normalize();

        for obj in &self.objs {
            let u = Uniforms {
                view_proj: view_proj.to_cols_array_2d(),
                model: obj.model.to_cols_array_2d(),
                light_vp: light_vp.to_cols_array_2d(),
                sun_dir: [sun.x, sun.y, sun.z, 0.0],
                base_color: obj.color,
            };
            self.queue.write_buffer(&obj.ubo, 0, bytemuck::bytes_of(&u));
        }

        let frame = self.surface.get_current_texture()?;
        let target = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        let mut enc = self.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("frame") });

        // --- shadow pass: casters -> sun depth ---
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

        // --- main pass: forward + shadow lookup ---
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
            rp.set_bind_group(1, &self.shadow_bind, &[]);
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
