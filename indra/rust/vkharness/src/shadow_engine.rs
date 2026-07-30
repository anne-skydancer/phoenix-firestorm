//! Engine shadow work — see SHADOW_PLAN.md.
//!
//! M0a geometry, M0b cast shadow, M1 soft PCF, M2 stable single cascade (sphere-bound +
//! texel-snap). **M3a: N cascades with the UNIFIED PARTITION** — one `splits[]` array drives
//! BOTH which cascade a receiver samples AND which slice each cascade's casters are fit to,
//! so selection and caster-inclusion cannot drift apart (that drift was the legacy dropout's
//! second root cause). Shadow map is a depth texture ARRAY (one layer per cascade).
//!
//! Gates: `shadow-shimmer` (anti-crawl), `shadow-suncam` (dynamic-to-sun / invariant-to-cam),
//! `shadow-axes` (invariance on yaw/pitch/roll). Windowed: `shadow`. Capture: `shadow-shot`.

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
const N_CASCADES: usize = 4;
const FOV: f32 = std::f32::consts::PI / 4.0; // 45 deg
const CASC_NEAR: f32 = 1.0;
const CASC_FAR: f32 = 30.0;
const FOCUS: Vec3 = Vec3::new(0.0, 0.5, 0.0);
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

// Per-object UBO. Frame-wide data (matrices, splits, sun) is duplicated per object -- cheap
// (a handful of objects) and keeps the binding model to two groups.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Uniforms {
    view_proj: [[f32; 4]; 4],
    sel_view: [[f32; 4]; 4], // view used for cascade SELECTION (depth)
    model: [[f32; 4]; 4],
    light_vp: [[[f32; 4]; 4]; N_CASCADES],
    splits: [f32; 4],  // cascade far distances (sel-view depth); [x,y,z,w] = cascade 0..3 far
    sun_dir: [f32; 4],
    base_color: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct CascIdx {
    idx: u32,
    _pad: [u32; 3],
}

/// Everything a frame needs: raster view-proj, selection view, the N cascade matrices, the
/// split distances, and the sun. Selection view can differ from raster view (the invariance
/// gates render from a fixed cam while the cascades follow a moving fit cam).
struct Frame {
    view_proj: Mat4,
    sel_view: Mat4,
    cascades: [Mat4; N_CASCADES],
    splits: [f32; N_CASCADES],
    sun: Vec3,
}

fn orbit_eye(yaw: f32, pitch: f32, radius: f32) -> Vec3 {
    FOCUS + Vec3::new(pitch.cos() * yaw.sin(), pitch.sin(), pitch.cos() * yaw.cos()) * radius
}

/// Sun travel direction from elevation + azimuth (degrees). Lower elevation = longer shadow.
fn sun_dir(elev_deg: f32, azim_deg: f32) -> Vec3 {
    let (el, az) = (elev_deg.to_radians(), azim_deg.to_radians());
    Vec3::new(el.cos() * az.cos(), -el.sin(), el.cos() * az.sin()).normalize()
}

/// **M2 core.** Camera-following sun ortho, stable by construction: fit to the bounding
/// SPHERE of the [near,far] view-frustum slice (radius orientation-independent -> no sweep on
/// rotate) and TEXEL-SNAPPED (-> no crawl on translate). `snap=false` reproduces classic
/// shimmer for the gate.
fn stable_light_matrix(
    fit_eye: Vec3, fit_fwd: Vec3, fit_up: Vec3, fov: f32, aspect: f32, near: f32, far: f32,
    sun: Vec3, snap: bool,
) -> Mat4 {
    let tan = (fov * 0.5).tan();
    let right = fit_fwd.cross(fit_up).normalize();
    let up = right.cross(fit_fwd).normalize();
    let mut corners = [Vec3::ZERO; 8];
    let mut i = 0;
    for &d in &[near, far] {
        let (hh, hw) = (d * tan, d * tan * aspect);
        let c = fit_eye + fit_fwd * d;
        for &sy in &[1.0f32, -1.0] {
            for &sx in &[1.0f32, -1.0] {
                corners[i] = c + up * (hh * sy) + right * (hw * sx);
                i += 1;
            }
        }
    }
    let center = corners.iter().copied().fold(Vec3::ZERO, |a, b| a + b) / 8.0;
    let radius = corners.iter().map(|c| (*c - center).length()).fold(0.0f32, f32::max);

    let ldir = sun.normalize();
    let lup = if ldir.y.abs() > 0.99 { Vec3::Z } else { Vec3::Y };
    let back = 100.0f32;
    let leye = -ldir * back;
    let view = Mat4::look_at_rh(leye, leye + ldir, lup);

    let mut c = view.transform_point3(center);
    if snap {
        let units_per_texel = 2.0 * radius / SHADOW_SIZE as f32;
        c.x = (c.x / units_per_texel).floor() * units_per_texel;
        c.y = (c.y / units_per_texel).floor() * units_per_texel;
    }
    let near_z = back - radius - 40.0;
    let far_z = back + radius + 40.0;
    let proj = Mat4::orthographic_rh(c.x - radius, c.x + radius, c.y - radius, c.y + radius, near_z, far_z);
    proj * view
}

/// Practical-split cascade far distances (blend of uniform + logarithmic).
fn cascade_splits(near: f32, far: f32, lambda: f32) -> [f32; N_CASCADES] {
    let mut s = [0f32; N_CASCADES];
    for i in 0..N_CASCADES {
        let p = (i + 1) as f32 / N_CASCADES as f32;
        let log = near * (far / near).powf(p);
        let uni = near + (far - near) * p;
        s[i] = lambda * log + (1.0 - lambda) * uni;
    }
    s
}

/// **The unified partition.** One `splits[]` array. Cascade i is stably fit to the slice
/// [prev, splits[i]]; the SAME splits drive selection in the shader. Selection and
/// caster-inclusion therefore reference the identical partition -> they cannot drift.
fn compute_cascades(
    fit_eye: Vec3, fit_fwd: Vec3, fit_up: Vec3, aspect: f32, sun: Vec3, snap: bool,
) -> ([Mat4; N_CASCADES], [f32; N_CASCADES]) {
    let splits = cascade_splits(CASC_NEAR, CASC_FAR, 0.5);
    let mut mats = [Mat4::IDENTITY; N_CASCADES];
    let mut prev = CASC_NEAR;
    for i in 0..N_CASCADES {
        mats[i] = stable_light_matrix(fit_eye, fit_fwd, fit_up, FOV, aspect, prev, splits[i], sun, snap);
        prev = splits[i];
    }
    (mats, splits)
}

/// Build a Frame with raster (render) camera possibly distinct from the fit camera driving
/// the cascades (the invariance gates exploit that).
fn make_frame(render_vp: Mat4, fit_eye: Vec3, fit_fwd: Vec3, fit_up: Vec3, aspect: f32, sun: Vec3, snap: bool) -> Frame {
    let sel_view = Mat4::look_at_rh(fit_eye, fit_eye + fit_fwd, fit_up);
    let (cascades, splits) = compute_cascades(fit_eye, fit_fwd, fit_up, aspect, sun, snap);
    Frame { view_proj: render_vp, sel_view, cascades, splits, sun }
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

const UBO_DECL: &str = r#"
layout(set = 0, binding = 0) uniform U {
    mat4 view_proj;
    mat4 sel_view;
    mat4 model;
    mat4 light_vp[4];
    vec4 splits;
    vec4 sun_dir;
    vec4 base_color;
} u;
"#;

fn shadow_vert() -> String {
    format!(r#"#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
{UBO_DECL}
layout(set = 1, binding = 0) uniform C {{ uint idx; }} c;
void main() {{ gl_Position = u.light_vp[c.idx] * u.model * vec4(pos, 1.0); }}
"#)
}

fn main_vert() -> String {
    format!(r#"#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
{UBO_DECL}
layout(location = 0) out vec3 v_nrm;
layout(location = 1) out vec3 v_world;
layout(location = 2) out float v_viewdepth;
void main() {{
    vec4 wp = u.model * vec4(pos, 1.0);
    v_world = wp.xyz;
    v_nrm = mat3(u.model) * nrm;
    v_viewdepth = -(u.sel_view * wp).z;   // distance along the selection-view forward
    gl_Position = u.view_proj * wp;
}}
"#)
}

fn main_frag() -> String {
    format!(r#"#version 450
layout(location = 0) in vec3 v_nrm;
layout(location = 1) in vec3 v_world;
layout(location = 2) in float v_viewdepth;
layout(location = 0) out vec4 frag;
{UBO_DECL}
layout(set = 1, binding = 0) uniform texture2DArray shadowTex;
layout(set = 1, binding = 1) uniform samplerShadow  shadowSamp;

int pick_cascade(float vd) {{
    if (vd <= u.splits.x) return 0;
    if (vd <= u.splits.y) return 1;
    if (vd <= u.splits.z) return 2;
    return 3;
}}
float shadow_lit(vec3 wp, vec3 N, int ci) {{
    vec3 wpo = wp + N * 0.02;              // normal-offset (anti-acne, no peter-panning)
    vec4 lc = u.light_vp[ci] * vec4(wpo, 1.0);
    vec3 p = lc.xyz / lc.w;
    vec2 uv = p.xy * vec2(0.5, -0.5) + 0.5;
    float ref = p.z - 0.0006;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ref > 1.0) return 1.0;
    float texel = 1.0 / {size}.0;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(sampler2DArrayShadow(shadowTex, shadowSamp), vec4(uv + vec2(x, y) * texel, float(ci), ref));
    return sum / 9.0;
}}
void main() {{
    vec3 N = normalize(v_nrm);
    vec3 L = -normalize(u.sun_dir.xyz);
    float ndl = max(dot(N, L), 0.0);
    int ci = pick_cascade(v_viewdepth);
    float lit = shadow_lit(v_world, N, ci);
    vec3 col = u.base_color.rgb * (0.25 + 0.75 * ndl * lit);
    frag = vec4(col, 1.0);
}}
"#, size = SHADOW_SIZE)
}

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
        device: &wgpu::Device, bgl: &wgpu::BindGroupLayout, verts: &[Vtx], idx: &[u16],
        model: Mat4, color: [f32; 4], caster: bool,
    ) -> Obj {
        let vbuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("vb"), contents: bytemuck::cast_slice(verts), usage: wgpu::BufferUsages::VERTEX,
        });
        let ibuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("ib"), contents: bytemuck::cast_slice(idx), usage: wgpu::BufferUsages::INDEX,
        });
        let ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("ubo"), size: std::mem::size_of::<Uniforms>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST, mapped_at_creation: false,
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("obj-bind"), layout: bgl,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: ubo.as_entire_binding() }],
        });
        Obj { vbuf, ibuf, nidx: idx.len() as u32, ubo, bind, model, color, caster }
    }
}

fn depth_texture(device: &wgpu::Device, w: u32, h: u32, layers: u32, format: wgpu::TextureFormat, label: &str) -> wgpu::Texture {
    device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: layers.max(1) },
        mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
    })
}

// ---- shared renderer ------------------------------------------------------------

struct Renderer {
    pipeline: wgpu::RenderPipeline,
    shadow_pipeline: wgpu::RenderPipeline,
    shadow_layer_views: Vec<wgpu::TextureView>, // one per cascade (render target)
    shadow_bind: wgpu::BindGroup,               // array view + comparison sampler (sample)
    cascidx_binds: Vec<wgpu::BindGroup>,         // static per-cascade index UBOs
    objs: Vec<Obj>,
}

impl Renderer {
    fn new(device: &wgpu::Device, color_format: wgpu::TextureFormat) -> Renderer {
        let obj_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("obj-bgl"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0, visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                count: None,
            }],
        });
        let cascidx_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("cascidx-bgl"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0, visibility: wgpu::ShaderStages::VERTEX,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                count: None,
            }],
        });
        let shadow_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("shadow-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2Array, multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Comparison), count: None,
                },
            ],
        });

        // shadow map array + views
        let shadow_tex = depth_texture(device, SHADOW_SIZE, SHADOW_SIZE, N_CASCADES as u32, SHADOW_FORMAT, "shadow-array");
        let shadow_layer_views: Vec<_> = (0..N_CASCADES as u32)
            .map(|i| shadow_tex.create_view(&wgpu::TextureViewDescriptor {
                label: Some("shadow-layer"), dimension: Some(wgpu::TextureViewDimension::D2),
                base_array_layer: i, array_layer_count: Some(1), ..Default::default()
            }))
            .collect();
        let shadow_array_view = shadow_tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("shadow-array-view"), dimension: Some(wgpu::TextureViewDimension::D2Array), ..Default::default()
        });
        let shadow_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("shadow-cmp"),
            address_mode_u: wgpu::AddressMode::ClampToEdge, address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear, min_filter: wgpu::FilterMode::Linear, mipmap_filter: wgpu::FilterMode::Nearest,
            compare: Some(wgpu::CompareFunction::LessEqual), ..Default::default()
        });
        let shadow_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("shadow-bind"), layout: &shadow_bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&shadow_array_view) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&shadow_sampler) },
            ],
        });

        // static per-cascade index UBOs (0..N)
        let cascidx_binds: Vec<_> = (0..N_CASCADES as u32)
            .map(|i| {
                let ubo = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some("cascidx"),
                    contents: bytemuck::bytes_of(&CascIdx { idx: i, _pad: [0; 3] }),
                    usage: wgpu::BufferUsages::UNIFORM,
                });
                device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("cascidx-bind"), layout: &cascidx_bgl,
                    entries: &[wgpu::BindGroupEntry { binding: 0, resource: ubo.as_entire_binding() }],
                })
            })
            .collect();

        // pipelines
        let shadow_vs = glsl_module(device, &shadow_vert(), shaderc::ShaderKind::Vertex, "shadow.vert");
        let shadow_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("shadow-pl"), bind_group_layouts: &[&obj_bgl, &cascidx_bgl], push_constant_ranges: &[],
        });
        let shadow_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("shadow"), layout: Some(&shadow_pl),
            vertex: wgpu::VertexState { module: &shadow_vs, entry_point: "main", buffers: &[Vtx::LAYOUT] },
            fragment: None,
            primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: SHADOW_FORMAT, depth_write_enabled: true, depth_compare: wgpu::CompareFunction::Less,
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState { constant: 2, slope_scale: 2.0, clamp: 0.0 },
            }),
            multisample: wgpu::MultisampleState::default(), multiview: None,
        });

        let vs = glsl_module(device, &main_vert(), shaderc::ShaderKind::Vertex, "m.vert");
        let fs = glsl_module(device, &main_frag(), shaderc::ShaderKind::Fragment, "m.frag");
        let main_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("main-pl"), bind_group_layouts: &[&obj_bgl, &shadow_bgl], push_constant_ranges: &[],
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("forward"), layout: Some(&main_pl),
            vertex: wgpu::VertexState { module: &vs, entry_point: "main", buffers: &[Vtx::LAYOUT] },
            fragment: Some(wgpu::FragmentState {
                module: &fs, entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState { format: color_format, blend: Some(wgpu::BlendState::REPLACE), write_mask: wgpu::ColorWrites::ALL })],
            }),
            primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH_FORMAT, depth_write_enabled: true, depth_compare: wgpu::CompareFunction::Less,
                stencil: wgpu::StencilState::default(), bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(), multiview: None,
        });

        // scene: casters at increasing depth (span the cascades) + a big ground plane.
        let mut objs = Vec::new();
        let (cv, ci) = cube();
        for &z in &[0.0f32, 9.0, 18.0] {
            objs.push(Obj::new(device, &obj_bgl, &cv, &ci, Mat4::from_translation(Vec3::new(0.0, 0.0, z)), [0.85, 0.45, 0.20, 1.0], true));
        }
        let (pv, pi) = plane(40.0);
        objs.push(Obj::new(device, &obj_bgl, &pv, &pi, Mat4::IDENTITY, [0.55, 0.55, 0.58, 1.0], false));

        Renderer { pipeline, shadow_pipeline, shadow_layer_views, shadow_bind, cascidx_binds, objs }
    }

    fn update(&self, queue: &wgpu::Queue, f: &Frame) {
        let mut light_vp = [[[0f32; 4]; 4]; N_CASCADES];
        for i in 0..N_CASCADES {
            light_vp[i] = f.cascades[i].to_cols_array_2d();
        }
        let splits = [f.splits[0], f.splits[1], f.splits[2], f.splits[3]];
        for obj in &self.objs {
            let u = Uniforms {
                view_proj: f.view_proj.to_cols_array_2d(),
                sel_view: f.sel_view.to_cols_array_2d(),
                model: obj.model.to_cols_array_2d(),
                light_vp,
                splits,
                sun_dir: [f.sun.x, f.sun.y, f.sun.z, 0.0],
                base_color: obj.color,
            };
            queue.write_buffer(&obj.ubo, 0, bytemuck::bytes_of(&u));
        }
    }

    fn encode(&self, enc: &mut wgpu::CommandEncoder, color: &wgpu::TextureView, depth: &wgpu::TextureView) {
        // N shadow sub-passes, one per cascade layer.
        for ci in 0..N_CASCADES {
            let mut sp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("shadow"),
                color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &self.shadow_layer_views[ci],
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Store }),
                    stencil_ops: None,
                }),
                timestamp_writes: None, occlusion_query_set: None,
            });
            sp.set_pipeline(&self.shadow_pipeline);
            sp.set_bind_group(1, &self.cascidx_binds[ci], &[]);
            for obj in &self.objs {
                if !obj.caster { continue; }
                sp.set_bind_group(0, &obj.bind, &[]);
                sp.set_vertex_buffer(0, obj.vbuf.slice(..));
                sp.set_index_buffer(obj.ibuf.slice(..), wgpu::IndexFormat::Uint16);
                sp.draw_indexed(0..obj.nidx, 0, 0..1);
            }
        }
        // main forward pass
        let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("forward"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: color, resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color { r: 0.50, g: 0.62, b: 0.78, a: 1.0 }), store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                view: depth,
                depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Store }),
                stencil_ops: None,
            }),
            timestamp_writes: None, occlusion_query_set: None,
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
    radius: f32,
    auto_rotate: bool,
    snap: bool,
}

impl Scene {
    fn new(window: Arc<Window>) -> Scene {
        pollster::block_on(async {
            let instance = wgpu::Instance::new(wgpu::InstanceDescriptor { backends: wgpu::Backends::VULKAN, ..Default::default() });
            let surface = instance.create_surface(window.clone()).expect("surface");
            let adapter = instance.request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::HighPerformance, compatible_surface: Some(&surface), force_fallback_adapter: false,
            }).await.expect("no Vulkan adapter");
            let info = adapter.get_info();
            log::info!("adapter: {} | backend={:?} | {} {}", info.name, info.backend, info.driver, info.driver_info);
            let (device, queue) = adapter.request_device(&wgpu::DeviceDescriptor {
                label: Some("shadow-engine"), required_features: wgpu::Features::empty(), required_limits: wgpu::Limits::default(),
            }, None).await.expect("request_device");

            let size = window.inner_size();
            let caps = surface.get_capabilities(&adapter);
            let format = caps.formats.iter().copied().find(|f| f.is_srgb()).unwrap_or(caps.formats[0]);
            let config = wgpu::SurfaceConfiguration {
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT, format,
                width: size.width.max(1), height: size.height.max(1),
                present_mode: caps.present_modes[0], alpha_mode: caps.alpha_modes[0],
                view_formats: vec![], desired_maximum_frame_latency: 2,
            };
            surface.configure(&device, &config);

            let renderer = Renderer::new(&device, config.format);
            let depth = depth_texture(&device, config.width, config.height, 1, DEPTH_FORMAT, "depth")
                .create_view(&wgpu::TextureViewDescriptor::default());
            let aspect = config.width as f32 / config.height as f32;
            let proj = Mat4::perspective_rh(FOV, aspect, 0.1, 100.0);

            Scene { window, surface, device, queue, config, depth, renderer, proj, yaw: 0.7, pitch: 0.55, radius: 16.0, auto_rotate: false, snap: true }
        })
    }

    fn resize(&mut self, size: winit::dpi::PhysicalSize<u32>) {
        if size.width == 0 || size.height == 0 { return; }
        self.config.width = size.width;
        self.config.height = size.height;
        self.surface.configure(&self.device, &self.config);
        self.depth = depth_texture(&self.device, size.width, size.height, 1, DEPTH_FORMAT, "depth")
            .create_view(&wgpu::TextureViewDescriptor::default());
        self.proj = Mat4::perspective_rh(FOV, size.width as f32 / size.height as f32, 0.1, 100.0);
    }

    fn render(&mut self) -> Result<(), wgpu::SurfaceError> {
        if self.auto_rotate { self.yaw += 0.0015; }
        let eye = orbit_eye(self.yaw, self.pitch, self.radius);
        let view_proj = self.proj * Mat4::look_at_rh(eye, FOCUS, Vec3::Y);
        let sun = Vec3::from_array(SUN).normalize();
        let fwd = (FOCUS - eye).normalize();
        let aspect = self.config.width as f32 / self.config.height as f32;
        let frame = make_frame(view_proj, eye, fwd, Vec3::Y, aspect, sun, self.snap);
        self.renderer.update(&self.queue, &frame);

        let sframe = self.surface.get_current_texture()?;
        let target = sframe.texture.create_view(&wgpu::TextureViewDescriptor::default());
        let mut enc = self.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("frame") });
        self.renderer.encode(&mut enc, &target, &self.depth);
        self.queue.submit([enc.finish()]);
        sframe.present();
        Ok(())
    }
}

pub fn run() {
    let event_loop = EventLoop::new().expect("event loop");
    let window = Arc::new(WindowBuilder::new()
        .with_title("vkharness -- engine shadows M3 (N cascades, unified partition)")
        .with_inner_size(winit::dpi::LogicalSize::new(1280.0, 800.0))
        .build(&event_loop).expect("window"));
    let mut scene = Scene::new(window.clone());
    log::info!("M3: {} cascades, unified partition. N toggles snap. Space auto-spin, arrows orbit, Esc quits.", N_CASCADES);
    event_loop.run(move |event, elwt| {
        elwt.set_control_flow(winit::event_loop::ControlFlow::Poll);
        match event {
            Event::WindowEvent { event, window_id } if window_id == scene.window.id() => match event {
                WindowEvent::CloseRequested => elwt.exit(),
                WindowEvent::KeyboardInput { event: KeyEvent { physical_key: PhysicalKey::Code(code), state: ElementState::Pressed, .. }, .. } => {
                    let step = 0.06;
                    match code {
                        KeyCode::Escape => elwt.exit(),
                        KeyCode::Space => scene.auto_rotate = !scene.auto_rotate,
                        KeyCode::KeyN => { scene.snap = !scene.snap; log::info!("texel snap: {}", if scene.snap { "ON" } else { "OFF" }); }
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
                        Err(wgpu::SurfaceError::Lost) => { let s = scene.window.inner_size(); scene.resize(s); }
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
    }).expect("event loop");
}

// ---- headless capture + gates ---------------------------------------------------

const SHOT_FMT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8UnormSrgb;

/// Render one frame headless -> tight Rgba8 pixels (w multiple of 64 -> 256-aligned stride).
fn render_to_pixels(device: &wgpu::Device, queue: &wgpu::Queue, renderer: &Renderer, frame: &Frame, w: u32, h: u32) -> Vec<u8> {
    let color = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("shot-color"), size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format: SHOT_FMT,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC, view_formats: &[],
    });
    let color_view = color.create_view(&wgpu::TextureViewDescriptor::default());
    let depth = depth_texture(device, w, h, 1, DEPTH_FORMAT, "shot-depth").create_view(&wgpu::TextureViewDescriptor::default());
    renderer.update(queue, frame);

    let bpr = w * 4;
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("shot-readback"), size: (bpr * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ, mapped_at_creation: false,
    });
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("shot") });
    renderer.encode(&mut enc, &color_view, &depth);
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
    let px = slice.get_mapped_range().to_vec();
    readback.unmap();
    px
}

fn write_png(out: &std::path::Path, px: &[u8], w: u32, h: u32) {
    let file = std::fs::File::create(out).expect("create png");
    let mut e = png::Encoder::new(std::io::BufWriter::new(file), w, h);
    e.set_color(png::ColorType::Rgba);
    e.set_depth(png::BitDepth::Eight);
    e.write_header().expect("png header").write_image_data(px).expect("png data");
    log::info!("wrote {}x{} -> {}", w, h, out.display());
}

fn pixel_diff(a: &[u8], b: &[u8]) -> u64 {
    let mut n = 0u64;
    let mut i = 0;
    while i + 2 < a.len() && i + 2 < b.len() {
        if a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] { n += 1; }
        i += 4;
    }
    n
}

/// Camera view-proj + eye + fwd for an orbit shot.
fn orbit_cam(w: u32, h: u32, yaw_deg: f32, pitch_deg: f32, radius: f32) -> (Mat4, Vec3, Vec3) {
    let proj = Mat4::perspective_rh(FOV, w as f32 / h as f32, 0.1, 100.0);
    let eye = orbit_eye(yaw_deg.to_radians(), pitch_deg.to_radians(), radius);
    let vp = proj * Mat4::look_at_rh(eye, FOCUS, Vec3::Y);
    (vp, eye, (FOCUS - eye).normalize())
}

pub fn run_shot() {
    let (device, queue) = crate::headless_vulkan_device();
    let renderer = Renderer::new(&device, SHOT_FMT);
    let (w, h) = (1280u32, 800u32);
    let aspect = w as f32 / h as f32;
    let dir = std::env::temp_dir();
    // A camera looking across the receding cubes so the cascade depth-range is visible.
    let (vp, eye, fwd) = orbit_cam(w, h, 35.0, 30.0, 22.0);
    for &(name, elev) in &[("high", 55.0f32), ("low", 20.0f32)] {
        let sun = sun_dir(elev, 41.0);
        let frame = make_frame(vp, eye, fwd, Vec3::Y, aspect, sun, true);
        let px = render_to_pixels(&device, &queue, &renderer, &frame, w, h);
        write_png(&dir.join(format!("shadow_{}.png", name)), &px, w, h);
    }
}

pub fn run_shimmer() {
    let (device, queue) = crate::headless_vulkan_device();
    let renderer = Renderer::new(&device, SHOT_FMT);
    let (w, h) = (1280u32, 800u32);
    let aspect = w as f32 / h as f32;
    let sun = sun_dir(35.0, 41.0);
    let (rvp, _, _) = orbit_cam(w, h, 40.0, 55.0, 16.0); // FIXED render cam
    let dir = std::env::temp_dir();
    log::info!("M2/M3 shimmer gate: fixed render cam, fit cam pans 0.4deg/frame x16.");
    for &snap in &[false, true] {
        let mut prev: Option<Vec<u8>> = None;
        let (mut total, mut peak) = (0u64, 0u64);
        let steps = 16;
        let (mut first, mut last) = (None, None);
        for k in 0..steps {
            let fit_yaw = (30.0 + k as f32 * 0.4).to_radians();
            let feye = orbit_eye(fit_yaw, 0.5, 5.0);
            let ffwd = (FOCUS - feye).normalize();
            let frame = make_frame(rvp, feye, ffwd, Vec3::Y, aspect, sun, snap);
            let px = render_to_pixels(&device, &queue, &renderer, &frame, w, h);
            if let Some(p) = &prev { let d = pixel_diff(&px, p); total += d; peak = peak.max(d); }
            if k == 0 { first = Some(px.clone()); }
            if k == steps - 1 { last = Some(px.clone()); }
            prev = Some(px);
        }
        let tag = if snap { "on" } else { "off" };
        log::info!("  snap={:<3}: {:>8} px changed total, {:>6} peak/frame (of {})", tag, total, peak, w * h);
        if let (Some(f), Some(l)) = (first, last) {
            write_png(&dir.join(format!("shimmer_{}_first.png", tag)), &f, w, h);
            write_png(&dir.join(format!("shimmer_{}_last.png", tag)), &l, w, h);
        }
    }
    log::info!("GATE PASS if snap=on total << snap=off total.");
}

pub fn run_suncam() {
    let (device, queue) = crate::headless_vulkan_device();
    let renderer = Renderer::new(&device, SHOT_FMT);
    let (w, h) = (1280u32, 800u32);
    let aspect = w as f32 / h as f32;
    let dir = std::env::temp_dir();
    let steps = 16;
    let (mvp, _, _) = orbit_cam(w, h, 25.0, 65.0, 12.0); // FIXED top-down measurement cam

    // A: camera (fit) orbits, sun FIXED.
    let sun_fixed = sun_dir(45.0, 41.0);
    let (mut prev, mut a_total) = (None::<Vec<u8>>, 0u64);
    for k in 0..steps {
        let feye = orbit_eye((k as f32 * 4.0).to_radians(), 0.5, 5.0);
        let ffwd = (FOCUS - feye).normalize();
        let frame = make_frame(mvp, feye, ffwd, Vec3::Y, aspect, sun_fixed, true);
        let px = render_to_pixels(&device, &queue, &renderer, &frame, w, h);
        if let Some(p) = &prev { a_total += pixel_diff(&px, p); }
        prev = Some(px);
    }
    // B: sun sweeps, camera (fit) FIXED.
    let feye = orbit_eye(30f32.to_radians(), 0.5, 5.0);
    let ffwd = (FOCUS - feye).normalize();
    let (mut prev, mut b_total) = (None::<Vec<u8>>, 0u64);
    for k in 0..steps {
        let sun_k = sun_dir(58.0 - k as f32 * 3.0, 41.0);
        let frame = make_frame(mvp, feye, ffwd, Vec3::Y, aspect, sun_k, true);
        let px = render_to_pixels(&device, &queue, &renderer, &frame, w, h);
        if let Some(p) = &prev { b_total += pixel_diff(&px, p); }
        if k % 5 == 0 { write_png(&dir.join(format!("suncam_B_sun{:02}.png", k)), &px, w, h); }
        prev = Some(px);
    }
    log::info!("moving-sun / moving-cam (fixed top-down measurement cam, {} frames each):", steps);
    log::info!("  A) camera orbits, sun FIXED : {:>8} px changed  -> want ~0    (shadow IGNORES camera)", a_total);
    log::info!("  B) sun sweeps, camera FIXED : {:>8} px changed  -> want LARGE (shadow FOLLOWS sun)", b_total);
    log::info!("  PASS if A << B.");
}

#[allow(clippy::too_many_arguments)]
fn axis_sweep(device: &wgpu::Device, queue: &wgpu::Queue, renderer: &Renderer, mvp: Mat4, sun: Vec3, w: u32, h: u32, aspect: f32, steps: usize, gen: impl Fn(usize) -> (f32, f32, f32)) -> (u64, u64) {
    let mut prev: Option<Vec<u8>> = None;
    let (mut total, mut peak) = (0u64, 0u64);
    for k in 0..steps {
        let (yaw, pitch, roll) = gen(k);
        let eye = orbit_eye(yaw, pitch, 5.0);
        let fwd = (FOCUS - eye).normalize();
        let up = glam::Quat::from_axis_angle(fwd, roll) * Vec3::Y;
        let frame = make_frame(mvp, eye, fwd, up, aspect, sun, true);
        let px = render_to_pixels(device, queue, renderer, &frame, w, h);
        if let Some(p) = &prev { let d = pixel_diff(&px, p); total += d; peak = peak.max(d); }
        prev = Some(px);
    }
    (total, peak)
}

pub fn run_axes() {
    let (device, queue) = crate::headless_vulkan_device();
    let renderer = Renderer::new(&device, SHOT_FMT);
    let (w, h) = (1280u32, 800u32);
    let aspect = w as f32 / h as f32;
    let sun = sun_dir(45.0, 41.0);
    let (mvp, _, _) = orbit_cam(w, h, 25.0, 65.0, 12.0);
    let steps = 16;
    let d = |x: f32| x.to_radians();
    let sweep = |label: &str, gen: &dyn Fn(usize) -> (f32, f32, f32)| -> u64 {
        let (total, peak) = axis_sweep(&device, &queue, &renderer, mvp, sun, w, h, aspect, steps, gen);
        log::info!("  {:<15}: {:>7} px total, {:>5} peak/frame  (want ~0)", label, total, peak);
        total
    };
    log::info!("CAMERA INVARIANCE across axes (fixed sun, fixed measurement cam, {} frames each):", steps);
    let yaw = sweep("yaw (L<->R)", &|k| (d(k as f32 * 4.0), d(30.0), 0.0));
    let pitch = sweep("pitch (D<->U)", &|k| (d(30.0), d(15.0 + k as f32 * 3.0), 0.0));
    let roll = sweep("roll (tilt)", &|k| (d(30.0), d(30.0), d(k as f32 * 4.0)));
    let combo = sweep("yaw+pitch+roll", &|k| (d(k as f32 * 3.0), d(20.0 + k as f32 * 2.0), d(k as f32 * 3.0)));
    let worst = yaw.max(pitch).max(roll).max(combo);
    log::info!("worst axis: {} px over {} frames. Sun-response baseline (suncam B): ~large. PASS if every axis << that.", worst, steps);
}
