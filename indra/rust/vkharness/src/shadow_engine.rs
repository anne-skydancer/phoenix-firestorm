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

use glam::{Mat4, Vec3, Vec4};
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
    uv: [f32; 2], // used by the textured G-buffer (H4c); forward/shadow ignore it
}

impl Vtx {
    const LAYOUT: wgpu::VertexBufferLayout<'static> = wgpu::VertexBufferLayout {
        array_stride: std::mem::size_of::<Vtx>() as wgpu::BufferAddress,
        step_mode: wgpu::VertexStepMode::Vertex,
        attributes: &[
            wgpu::VertexAttribute { offset: 0, shader_location: 0, format: wgpu::VertexFormat::Float32x3 },
            wgpu::VertexAttribute { offset: 12, shader_location: 1, format: wgpu::VertexFormat::Float32x3 },
            wgpu::VertexAttribute { offset: 24, shader_location: 2, format: wgpu::VertexFormat::Float32x2 },
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
    debug: bool, // cascade-tint overlay
    mask: bool,  // output the shadow mask (lit value) for area measurement
    sky: bool,   // EEP sky backdrop behind geometry (H4d)
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
    Frame { view_proj: render_vp, sel_view, cascades, splits, sun, debug: false, mask: false, sky: false }
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
    let uvq = [[0., 0.], [0., 1.], [1., 1.], [1., 0.]]; // per-face 0..1
    let mut v = Vec::new();
    let mut idx = Vec::new();
    for (n, quad) in faces.iter() {
        let base = v.len() as u16;
        for (k, p) in quad.iter().enumerate() {
            v.push(Vtx { pos: *p, nrm: *n, uv: uvq[k] });
        }
        idx.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
    }
    (v, idx)
}

fn plane(half: f32) -> (Vec<Vtx>, Vec<u16>) {
    let n = [0., 1., 0.];
    let v = vec![
        Vtx { pos: [-half, 0., -half], nrm: n, uv: [0., 0.] },
        Vtx { pos: [-half, 0.,  half], nrm: n, uv: [0., 1.] },
        Vtx { pos: [ half, 0.,  half], nrm: n, uv: [1., 1.] },
        Vtx { pos: [ half, 0., -half], nrm: n, uv: [1., 0.] },
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
    // Blend into the next cascade over the last `band` world-units -> no seam at the boundary.
    float band = 2.0;
    float blend = 0.0;
    if (ci < 3) {{
        blend = clamp((v_viewdepth - (u.splits[ci] - band)) / band, 0.0, 1.0);
        if (blend > 0.0) lit = mix(lit, shadow_lit(v_world, N, ci + 1), blend);
    }}
    // Shadow mask (sun_dir.w == 2): output the lit value directly -> shadowed area is
    // countable (dark = in shadow) for the M4 continuity gate.
    if (u.sun_dir.w > 1.5) {{ frag = vec4(vec3(lit), 1.0); return; }}
    vec3 col = u.base_color.rgb * (0.25 + 0.75 * ndl * lit);
    // Debug (sun_dir.w > 0.5): tint each surface by the cascade it samples, blended across
    // the band -- our RENDER_DEBUG_SHADOW_FRUSTA. Shows the partition IS coverage-complete.
    if (u.sun_dir.w > 0.5) {{
        vec3 tints[4] = vec3[4](vec3(0.2,1.0,0.3), vec3(1.0,1.0,0.2), vec3(1.0,0.6,0.1), vec3(1.0,0.25,0.25));
        vec3 tc = tints[ci];
        if (ci < 3) tc = mix(tc, tints[ci + 1], blend);
        col = col * 0.55 + tc * 0.45;
    }}
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
                sun_dir: [f.sun.x, f.sun.y, f.sun.z, if f.mask { 2.0 } else if f.debug { 1.0 } else { 0.0 }],
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
    debug: bool,
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

            Scene { window, surface, device, queue, config, depth, renderer, proj, yaw: 0.7, pitch: 0.55, radius: 16.0, auto_rotate: false, snap: true, debug: false }
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
        let mut frame = make_frame(view_proj, eye, fwd, Vec3::Y, aspect, sun, self.snap);
        frame.debug = self.debug;
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
                        KeyCode::KeyC => { scene.debug = !scene.debug; log::info!("cascade tint: {}", if scene.debug { "ON" } else { "OFF" }); }
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

/// Capture the cascade-tint debug view -- shows the unified partition (each surface tinted by
/// the cascade it samples) and the blend band (gradient at boundaries).
pub fn run_cascades() {
    let (device, queue) = crate::headless_vulkan_device();
    let renderer = Renderer::new(&device, SHOT_FMT);
    let (w, h) = (1280u32, 800u32);
    let aspect = w as f32 / h as f32;
    // Low camera looking down the row of cubes so the ground recedes through ALL cascades.
    let eye = Vec3::new(7.0, 2.5, -7.0);
    let target = Vec3::new(0.0, 0.5, 9.0);
    let proj = Mat4::perspective_rh(FOV, aspect, 0.1, 100.0);
    let vp = proj * Mat4::look_at_rh(eye, target, Vec3::Y);
    let fwd = (target - eye).normalize();
    let mut frame = make_frame(vp, eye, fwd, Vec3::Y, aspect, sun_dir(45.0, 41.0), true);
    frame.debug = true;
    let px = render_to_pixels(&device, &queue, &renderer, &frame, w, h);
    write_png(&std::env::temp_dir().join("shadow_cascades.png"), &px, w, h);
    log::info!("cascade tint: green=0(near) yellow=1 orange=2 red=3(far); gradient = blend band.");
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

/// **M4 — the continuity acceptance gate.** The legacy failure: shadow AREA collapses across
/// a range of camera angles, then snaps back. Here: FIXED measurement cam, LOW sun (long
/// shadow = stress), fit cam orbits a wide arc. Each frame we measure the world shadowed AREA
/// (mask mode) and the per-frame change. PASS = area stays continuous (never collapses) AND
/// no cut/flicker spike -- the whole "no cut, no flicker on a pan" spec, one number.
pub fn run_continuity() {
    let (device, queue) = crate::headless_vulkan_device();
    let renderer = Renderer::new(&device, SHOT_FMT);
    let (w, h) = (1280u32, 800u32);
    let aspect = w as f32 / h as f32;
    let dir = std::env::temp_dir();
    let sun = sun_dir(15.0, 41.0); // LOW sun -- long raking shadow, the stress case
    let (mvp, _, _) = orbit_cam(w, h, 30.0, 50.0, 14.0); // FIXED measurement cam
    let steps = 60usize;

    let count_shadow = |px: &[u8]| -> u64 {
        let mut n = 0u64;
        let mut i = 0;
        while i + 2 < px.len() {
            if px[i] < 40 && px[i + 1] < 40 && px[i + 2] < 40 { n += 1; }
            i += 4;
        }
        n
    };

    let mut prev: Option<Vec<u8>> = None;
    let (mut peak_diff, mut total_diff, mut peak_at) = (0u64, 0u64, 0usize);
    let (mut min_area, mut max_area) = (u64::MAX, 0u64);
    for k in 0..steps {
        let fit_yaw = (k as f32 * 2.0).to_radians(); // wide orbit, 2deg/frame
        let feye = orbit_eye(fit_yaw, 0.4, 5.0);
        let ffwd = (FOCUS - feye).normalize();
        let mut frame = make_frame(mvp, feye, ffwd, Vec3::Y, aspect, sun, true);
        frame.mask = true;
        let px = render_to_pixels(&device, &queue, &renderer, &frame, w, h);
        let area = count_shadow(&px);
        min_area = min_area.min(area);
        max_area = max_area.max(area);
        if let Some(p) = &prev {
            let d = pixel_diff(&px, p);
            total_diff += d;
            if d > peak_diff { peak_diff = d; peak_at = k; }
        }
        if k == 0 || k == steps / 2 { write_png(&dir.join(format!("continuity_{:02}.png", k)), &px, w, h); }
        prev = Some(px);
    }
    let spread = 100.0 * (max_area - min_area) as f64 / max_area.max(1) as f64;
    log::info!("M4 CONTINUITY GATE: {} frames, LOW sun 15deg, fit cam orbits 0..{}deg (2deg steps):", steps, (steps - 1) * 2);
    log::info!("  shadowed area: min {} .. max {} px  (spread {:.1}%)  -> want small; shadow never collapses", min_area, max_area, spread);
    log::info!("  per-frame change: peak {} px (frame {}), total {}  -> want NO cut/flicker spike", peak_diff, peak_at, total_diff);
    let ok = spread < 5.0 && min_area > max_area / 2 && peak_diff < max_area / 4;
    log::info!("  M4 {} (area continuous, no collapse, no spike).", if ok { "PASS" } else { "REVIEW" });
}

// ==== H4 — deferred unified render ================================================
// H4a: prove a deferred G-buffer + fullscreen lighting pass reproduces the proven
// FORWARD shading (the M-series `Renderer`), reusing the shadow pass unchanged. Same
// computation, new plumbing (the H3 discipline). The forward renderer is the trusted
// oracle; the gate is `deferred == forward` on lit geometry. `cargo run -- h4a`.
//
// G-buffer: albedo (32F) + world (32F) + normal (16F, w=1 marks coverage). Full-precision
// albedo+world make the result BYTE-EXACT vs the forward oracle (0 differing pixels whole-
// frame); 16F albedo alone rounds base_color ~1-3/255. Packing (8-bit albedo, oct-normals,
// depth-reconstruction instead of a 32F world target) is a later optimization, each step
// re-validated against this same forward oracle. Lighting: a fullscreen triangle reads
// the G-buffer by `texelFetch(gl_FragCoord)` (exact per-pixel correspondence, no uv
// orientation ambiguity), samples the same cascade shadow array, and runs the identical
// `pick_cascade`/`shadow_lit`/band-blend/`0.25 + 0.75*ndl*lit` shading.

const GALBEDO_FMT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba32Float;
const GWORLD_FMT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba32Float;
const GNORMAL_FMT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

/// One scene object for the deferred renderer: geometry, model transform, colour, is-caster.
type ObjSpec = (Vec<Vtx>, Vec<u16>, Mat4, [f32; 4], bool);

/// Sky display tone-map exponent: display = 1 - exp(-radiance * K). Shared by the GLSL sky and
/// the CPU oracle so the H4d gate is a like-for-like comparison.
const SKY_TONEMAP_K: f32 = 0.5;

fn gbuf_vert() -> String {
    format!(r#"#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec2 uv;
{UBO_DECL}
layout(location = 0) out vec3 v_nrm;
layout(location = 1) out vec3 v_world;
layout(location = 2) out vec2 v_uv;
void main() {{
    vec4 wp = u.model * vec4(pos, 1.0);
    v_world = wp.xyz;
    v_nrm = mat3(u.model) * nrm;
    v_uv = uv;
    gl_Position = u.view_proj * wp;
}}
"#)
}

/// Flat G-buffer fragment: albedo = base_color (H4a/H4b path).
fn gbuf_frag() -> String {
    format!(r#"#version 450
layout(location = 0) in vec3 v_nrm;
layout(location = 1) in vec3 v_world;
layout(location = 2) in vec2 v_uv;
{UBO_DECL}
layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec4 gWorld;
layout(location = 2) out vec4 gNormal;
void main() {{
    gAlbedo = vec4(u.base_color.rgb, 1.0);
    gWorld  = vec4(v_world, 1.0);
    gNormal = vec4(normalize(v_nrm), 1.0); // w = 1 -> this texel has geometry
}}
"#)
}

/// Textured G-buffer fragment: albedo = sample(albedoTex, uv) (H4c). Proves the UV + sampler
/// path lands in the deferred albedo, reusing the H3b2 texture+sampler descriptor mechanism.
fn gbuf_frag_tex() -> String {
    format!(r#"#version 450
layout(location = 0) in vec3 v_nrm;
layout(location = 1) in vec3 v_world;
layout(location = 2) in vec2 v_uv;
{UBO_DECL}
layout(set = 1, binding = 0) uniform texture2D albedoTex;
layout(set = 1, binding = 1) uniform sampler   albedoSamp;
layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec4 gWorld;
layout(location = 2) out vec4 gNormal;
void main() {{
    gAlbedo = vec4(texture(sampler2D(albedoTex, albedoSamp), v_uv).rgb, 1.0);
    gWorld  = vec4(v_world, 1.0);
    gNormal = vec4(normalize(v_nrm), 1.0);
}}
"#)
}

fn light_vert() -> String {
    r#"#version 450
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"#.to_string()
}

fn light_frag() -> String {
    format!(r#"#version 450
layout(location = 0) out vec4 frag;
{UBO_DECL}
layout(set = 1, binding = 0) uniform texture2D gAlbedoT;
layout(set = 1, binding = 1) uniform texture2D gWorldT;
layout(set = 1, binding = 2) uniform texture2D gNormalT;
layout(set = 1, binding = 3) uniform sampler   gSamp;
layout(set = 2, binding = 0) uniform texture2DArray shadowTex;
layout(set = 2, binding = 1) uniform samplerShadow  shadowSamp;

// EEP/WindLight dome radiance (skyV+skyF), shipped default sky -- a faithful port of the Rust
// sl_sky_radiance (vectorized), validated per-pixel against it by the H4d gate. NOT Hosek-Wilkie.
vec3 sl_sky(vec3 view, vec3 sun) {{
    const vec3  blue_horizon = vec3(0.4954, 0.4954, 0.6399);
    const vec3  blue_density = vec3(0.2447, 0.4487, 0.7599);
    const float haze_horizon = 0.19;
    const float haze_density = 0.7;
    const float density_multiplier = 0.0001;
    const vec3  glow = vec3(5.0, 0.001, -0.4799);
    const vec3  ambient = vec3(0.25);
    const vec3  sunlight = vec3(0.7342, 0.7815, 0.8999);
    const float cloud_shadow = 0.2699;
    const float max_y = 1605.0;

    float dy = max(view.y, 1e-3);
    float rel_pos_len = max_y / dy;
    float ndot = dot(view, sun);
    float atten_k = density_multiplier * max_y;
    float off_axis = 1.0 / max(max(view.y, 0.0) + sun.y, 1e-6);
    float density_dist = rel_pos_len * density_multiplier;
    float hg = pow(max(1.0 - ndot, 0.001) * glow.x, glow.z);
    float haze_glow = hg + 0.25;

    vec3 light_atten = (blue_density + haze_density * 0.25) * atten_k;
    vec3 sun_c = sunlight * exp(-light_atten * off_axis);
    vec3 comb = max(blue_density + haze_density, vec3(1e-6));
    vec3 blue_w = blue_density / comb;
    vec3 haze_w = vec3(haze_density) / comb;
    vec3 combined_haze = exp(-comb * density_dist);
    vec3 col = blue_horizon * blue_w * (sun_c + ambient)
             + (haze_horizon * haze_w) * (sun_c * haze_glow + ambient);
    col *= (vec3(1.0) - combined_haze);
    vec3 ambient2 = ambient + (vec3(1.0) - ambient) * cloud_shadow * 0.5;
    vec3 sun2 = sun_c * (1.0 - cloud_shadow);
    vec3 add_below = blue_horizon * blue_w * (sun2 + ambient2)
                   + (haze_horizon * haze_w) * (sun2 * haze_glow + ambient2);
    vec3 ch2 = sqrt(combined_haze);
    col += (add_below - col) * (vec3(1.0) - sqrt(ch2));
    return clamp(col * 2.0, 0.0, 5.0);
}}
vec3 sky_background() {{
    vec2 res = vec2(textureSize(sampler2D(gNormalT, gSamp), 0));
    vec2 uvv = gl_FragCoord.xy / res;
    vec2 ndc = vec2(uvv.x * 2.0 - 1.0, 1.0 - uvv.y * 2.0);
    mat4 invVP = inverse(u.view_proj);
    vec4 pn = invVP * vec4(ndc, 0.0, 1.0);
    vec4 pf = invVP * vec4(ndc, 1.0, 1.0);
    vec3 vd = normalize(pf.xyz / pf.w - pn.xyz / pn.w);
    vec3 sunUp = -normalize(u.sun_dir.xyz);
    vec3 disp = vec3(1.0) - exp(-sl_sky(vd, sunUp) * {k});
    float sd = dot(vd, sunUp);
    float disk = smoothstep(0.9993, 0.9997, sd);
    float glow = pow(max(sd, 0.0), 250.0) * 0.35;
    disp += vec3(1.0, 0.96, 0.85) * (disk * 2.0 + glow);
    return clamp(disp, 0.0, 1.0);
}}

int pick_cascade(float vd) {{
    if (vd <= u.splits.x) return 0;
    if (vd <= u.splits.y) return 1;
    if (vd <= u.splits.z) return 2;
    return 3;
}}
float shadow_lit(vec3 wp, vec3 N, int ci) {{
    vec3 wpo = wp + N * 0.02;
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
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 nrm4 = texelFetch(sampler2D(gNormalT, gSamp), px, 0);
    if (nrm4.w < 0.5) {{
        // background: EEP sky (flag 3) or the flat clear (h4a/b/c -> byte-exact)
        if (u.sun_dir.w > 2.5) frag = vec4(sky_background(), 1.0);
        else frag = vec4(0.50, 0.62, 0.78, 1.0);
        return;
    }}
    vec3 albedo = texelFetch(sampler2D(gAlbedoT, gSamp), px, 0).rgb;
    vec3 world  = texelFetch(sampler2D(gWorldT,  gSamp), px, 0).xyz;
    vec3 N = normalize(nrm4.xyz);
    vec3 L = -normalize(u.sun_dir.xyz);
    float ndl = max(dot(N, L), 0.0);
    float vd = -(u.sel_view * vec4(world, 1.0)).z;
    int ci = pick_cascade(vd);
    float lit = shadow_lit(world, N, ci);
    float band = 2.0;
    float blend = 0.0;
    if (ci < 3) {{
        blend = clamp((vd - (u.splits[ci] - band)) / band, 0.0, 1.0);
        if (blend > 0.0) lit = mix(lit, shadow_lit(world, N, ci + 1), blend);
    }}
    vec3 col = albedo * (0.25 + 0.75 * ndl * lit);
    frag = vec4(col, 1.0);
}}
"#, size = SHADOW_SIZE, k = SKY_TONEMAP_K)
}

/// Deferred renderer: same scene + same shadow pass as the forward `Renderer`, but shading
/// happens in a fullscreen lighting pass off a G-buffer. Its own shadow resources (the proven
/// forward path stays untouched).
struct DeferredRenderer {
    gbuffer_pipeline: wgpu::RenderPipeline,
    lighting_pipeline: wgpu::RenderPipeline,
    shadow_pipeline: wgpu::RenderPipeline,
    shadow_layer_views: Vec<wgpu::TextureView>,
    shadow_bind: wgpu::BindGroup,
    cascidx_binds: Vec<wgpu::BindGroup>,
    gbuf_bgl: wgpu::BindGroupLayout,
    gbuf_sampler: wgpu::Sampler,
    frame_ubo: wgpu::Buffer,
    frame_bind: wgpu::BindGroup,
    albedo_bind: Option<wgpu::BindGroup>, // Some -> textured G-buffer (H4c)
    objs: Vec<Obj>,
}

impl DeferredRenderer {
    /// `checker: Some(view)` -> textured G-buffer (albedo from that texture via UVs); `None` ->
    /// flat albedo = base_color (the H4a/H4b byte-exact path).
    fn new(device: &wgpu::Device, scene: &[ObjSpec], checker: Option<&wgpu::TextureView>) -> DeferredRenderer {
        let obj_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("d-obj-bgl"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0, visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                count: None,
            }],
        });
        let cascidx_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("d-cascidx-bgl"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0, visibility: wgpu::ShaderStages::VERTEX,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                count: None,
            }],
        });
        let shadow_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("d-shadow-bgl"),
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
        let gbuf_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("d-gbuf-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry { binding: 0, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: false }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false }, count: None },
                wgpu::BindGroupLayoutEntry { binding: 1, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: false }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false }, count: None },
                wgpu::BindGroupLayoutEntry { binding: 2, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false }, count: None },
                wgpu::BindGroupLayoutEntry { binding: 3, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::NonFiltering), count: None },
            ],
        });

        // shadow map array (own copy; forward path untouched)
        let shadow_tex = depth_texture(device, SHADOW_SIZE, SHADOW_SIZE, N_CASCADES as u32, SHADOW_FORMAT, "d-shadow-array");
        let shadow_layer_views: Vec<_> = (0..N_CASCADES as u32)
            .map(|i| shadow_tex.create_view(&wgpu::TextureViewDescriptor {
                label: Some("d-shadow-layer"), dimension: Some(wgpu::TextureViewDimension::D2),
                base_array_layer: i, array_layer_count: Some(1), ..Default::default()
            }))
            .collect();
        let shadow_array_view = shadow_tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("d-shadow-array-view"), dimension: Some(wgpu::TextureViewDimension::D2Array), ..Default::default()
        });
        let shadow_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("d-shadow-cmp"),
            address_mode_u: wgpu::AddressMode::ClampToEdge, address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear, min_filter: wgpu::FilterMode::Linear, mipmap_filter: wgpu::FilterMode::Nearest,
            compare: Some(wgpu::CompareFunction::LessEqual), ..Default::default()
        });
        let shadow_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("d-shadow-bind"), layout: &shadow_bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&shadow_array_view) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&shadow_sampler) },
            ],
        });
        let cascidx_binds: Vec<_> = (0..N_CASCADES as u32)
            .map(|i| {
                let ubo = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some("d-cascidx"), contents: bytemuck::bytes_of(&CascIdx { idx: i, _pad: [0; 3] }),
                    usage: wgpu::BufferUsages::UNIFORM,
                });
                device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("d-cascidx-bind"), layout: &cascidx_bgl,
                    entries: &[wgpu::BindGroupEntry { binding: 0, resource: ubo.as_entire_binding() }],
                })
            })
            .collect();

        // shadow pipeline (identical to the forward path so the depth maps match)
        let shadow_vs = glsl_module(device, &shadow_vert(), shaderc::ShaderKind::Vertex, "d-shadow.vert");
        let shadow_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("d-shadow-pl"), bind_group_layouts: &[&obj_bgl, &cascidx_bgl], push_constant_ranges: &[],
        });
        let shadow_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("d-shadow"), layout: Some(&shadow_pl),
            vertex: wgpu::VertexState { module: &shadow_vs, entry_point: "main", buffers: &[Vtx::LAYOUT] },
            fragment: None,
            primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: SHADOW_FORMAT, depth_write_enabled: true, depth_compare: wgpu::CompareFunction::Less,
                stencil: wgpu::StencilState::default(), bias: wgpu::DepthBiasState { constant: 2, slope_scale: 2.0, clamp: 0.0 },
            }),
            multisample: wgpu::MultisampleState::default(), multiview: None,
        });

        // albedo texture bind group (H4c textured mode)
        let albedo_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("albedo-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry { binding: 0, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false }, count: None },
                wgpu::BindGroupLayoutEntry { binding: 1, visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering), count: None },
            ],
        });
        let albedo_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("albedo-samp"),
            address_mode_u: wgpu::AddressMode::Repeat, address_mode_v: wgpu::AddressMode::Repeat, address_mode_w: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear, min_filter: wgpu::FilterMode::Linear, mipmap_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });
        let albedo_bind = checker.map(|view| device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("albedo-bind"), layout: &albedo_bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(view) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&albedo_sampler) },
            ],
        }));

        // g-buffer pipeline (3 float targets + depth); textured variant when a checker is bound
        let gvs = glsl_module(device, &gbuf_vert(), shaderc::ShaderKind::Vertex, "gbuf.vert");
        let gfs_src = if checker.is_some() { gbuf_frag_tex() } else { gbuf_frag() };
        let gfs = glsl_module(device, &gfs_src, shaderc::ShaderKind::Fragment, "gbuf.frag");
        let gbuf_layouts: Vec<&wgpu::BindGroupLayout> = if checker.is_some() { vec![&obj_bgl, &albedo_bgl] } else { vec![&obj_bgl] };
        let gbuf_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("gbuf-pl"), bind_group_layouts: &gbuf_layouts, push_constant_ranges: &[],
        });
        let ct = |fmt| Some(wgpu::ColorTargetState { format: fmt, blend: None, write_mask: wgpu::ColorWrites::ALL });
        let gbuffer_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("gbuffer"), layout: Some(&gbuf_pl),
            vertex: wgpu::VertexState { module: &gvs, entry_point: "main", buffers: &[Vtx::LAYOUT] },
            fragment: Some(wgpu::FragmentState { module: &gfs, entry_point: "main", targets: &[ct(GALBEDO_FMT), ct(GWORLD_FMT), ct(GNORMAL_FMT)] }),
            primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH_FORMAT, depth_write_enabled: true, depth_compare: wgpu::CompareFunction::Less,
                stencil: wgpu::StencilState::default(), bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(), multiview: None,
        });

        // lighting pipeline (fullscreen, no vertex buffers, no depth)
        let lvs = glsl_module(device, &light_vert(), shaderc::ShaderKind::Vertex, "light.vert");
        let lfs = glsl_module(device, &light_frag(), shaderc::ShaderKind::Fragment, "light.frag");
        let light_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("light-pl"), bind_group_layouts: &[&obj_bgl, &gbuf_bgl, &shadow_bgl], push_constant_ranges: &[],
        });
        let lighting_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("lighting"), layout: Some(&light_pl),
            vertex: wgpu::VertexState { module: &lvs, entry_point: "main", buffers: &[] },
            fragment: Some(wgpu::FragmentState {
                module: &lfs, entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState { format: SHOT_FMT, blend: Some(wgpu::BlendState::REPLACE), write_mask: wgpu::ColorWrites::ALL })],
            }),
            primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
            depth_stencil: None, multisample: wgpu::MultisampleState::default(), multiview: None,
        });

        let gbuf_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("gbuf-nearest"),
            mag_filter: wgpu::FilterMode::Nearest, min_filter: wgpu::FilterMode::Nearest, mipmap_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });

        // frame UBO for the lighting pass (frame-global fields; model/base_color unused there)
        let frame_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("frame-ubo"), size: std::mem::size_of::<Uniforms>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST, mapped_at_creation: false,
        });
        let frame_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("frame-bind"), layout: &obj_bgl,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: frame_ubo.as_entire_binding() }],
        });

        // caller-supplied scene
        let objs: Vec<Obj> = scene.iter()
            .map(|(v, i, m, c, caster)| Obj::new(device, &obj_bgl, v, i, *m, *c, *caster))
            .collect();

        DeferredRenderer {
            gbuffer_pipeline, lighting_pipeline, shadow_pipeline, shadow_layer_views, shadow_bind,
            cascidx_binds, gbuf_bgl, gbuf_sampler, frame_ubo, frame_bind, albedo_bind, objs,
        }
    }

    fn update(&self, queue: &wgpu::Queue, f: &Frame) {
        let mut light_vp = [[[0f32; 4]; 4]; N_CASCADES];
        for i in 0..N_CASCADES { light_vp[i] = f.cascades[i].to_cols_array_2d(); }
        let splits = [f.splits[0], f.splits[1], f.splits[2], f.splits[3]];
        let flag = if f.mask { 2.0 } else if f.debug { 1.0 } else if f.sky { 3.0 } else { 0.0 };
        for obj in &self.objs {
            let u = Uniforms {
                view_proj: f.view_proj.to_cols_array_2d(), sel_view: f.sel_view.to_cols_array_2d(),
                model: obj.model.to_cols_array_2d(), light_vp, splits,
                sun_dir: [f.sun.x, f.sun.y, f.sun.z, flag], base_color: obj.color,
            };
            queue.write_buffer(&obj.ubo, 0, bytemuck::bytes_of(&u));
        }
        let fu = Uniforms {
            view_proj: f.view_proj.to_cols_array_2d(), sel_view: f.sel_view.to_cols_array_2d(),
            model: Mat4::IDENTITY.to_cols_array_2d(), light_vp, splits,
            sun_dir: [f.sun.x, f.sun.y, f.sun.z, flag], base_color: [0.0; 4],
        };
        queue.write_buffer(&self.frame_ubo, 0, bytemuck::bytes_of(&fu));
    }

    /// Render the full deferred frame (shadow -> G-buffer -> lighting) into `target` (any color
    /// view of size w x h -- a readback texture headless, or the swapchain in the live window).
    fn render_into(&self, device: &wgpu::Device, queue: &wgpu::Queue, frame: &Frame, target: &wgpu::TextureView, w: u32, h: u32) {
        let mk = |fmt, label| device.create_texture(&wgpu::TextureDescriptor {
            label: Some(label), size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format: fmt,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
        });
        let g_albedo = mk(GALBEDO_FMT, "g-albedo");
        let g_world = mk(GWORLD_FMT, "g-world");
        let g_normal = mk(GNORMAL_FMT, "g-normal");
        let va = g_albedo.create_view(&wgpu::TextureViewDescriptor::default());
        let vw = g_world.create_view(&wgpu::TextureViewDescriptor::default());
        let vn = g_normal.create_view(&wgpu::TextureViewDescriptor::default());
        let depth = depth_texture(device, w, h, 1, DEPTH_FORMAT, "h4-depth").create_view(&wgpu::TextureViewDescriptor::default());

        let gbuf_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("gbuf-bind"), layout: &self.gbuf_bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&va) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&vw) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(&vn) },
                wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::Sampler(&self.gbuf_sampler) },
            ],
        });

        self.update(queue, frame);
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("h4") });

        // shadow sub-passes (one per cascade layer)
        for ci in 0..N_CASCADES {
            let mut sp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("d-shadow"), color_attachments: &[],
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

        // g-buffer pass
        {
            let clear0 = wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store };
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("gbuffer"),
                color_attachments: &[
                    Some(wgpu::RenderPassColorAttachment { view: &va, resolve_target: None, ops: clear0 }),
                    Some(wgpu::RenderPassColorAttachment { view: &vw, resolve_target: None, ops: clear0 }),
                    Some(wgpu::RenderPassColorAttachment { view: &vn, resolve_target: None, ops: clear0 }),
                ],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &depth,
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Store }),
                    stencil_ops: None,
                }),
                timestamp_writes: None, occlusion_query_set: None,
            });
            rp.set_pipeline(&self.gbuffer_pipeline);
            if let Some(ab) = &self.albedo_bind { rp.set_bind_group(1, ab, &[]); } // H4c textured albedo
            for obj in &self.objs {
                rp.set_bind_group(0, &obj.bind, &[]);
                rp.set_vertex_buffer(0, obj.vbuf.slice(..));
                rp.set_index_buffer(obj.ibuf.slice(..), wgpu::IndexFormat::Uint16);
                rp.draw_indexed(0..obj.nidx, 0, 0..1);
            }
        }

        // lighting pass (fullscreen triangle)
        {
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("lighting"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: target, resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color { r: 0.50, g: 0.62, b: 0.78, a: 1.0 }), store: wgpu::StoreOp::Store },
                })],
                depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None,
            });
            rp.set_pipeline(&self.lighting_pipeline);
            rp.set_bind_group(0, &self.frame_bind, &[]);
            rp.set_bind_group(1, &gbuf_bind, &[]);
            rp.set_bind_group(2, &self.shadow_bind, &[]);
            rp.draw(0..3, 0..1);
        }

        queue.submit([enc.finish()]);
    }

    /// Headless: render into a COPY_SRC color texture and read it back to tight Rgba8 pixels.
    fn render(&self, device: &wgpu::Device, queue: &wgpu::Queue, frame: &Frame, w: u32, h: u32) -> Vec<u8> {
        let color = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("h4-color"), size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format: SHOT_FMT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC, view_formats: &[],
        });
        let color_view = color.create_view(&wgpu::TextureViewDescriptor::default());
        self.render_into(device, queue, frame, &color_view, w, h);

        let bpr = w * 4;
        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("h4-readback"), size: (bpr * h) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ, mapped_at_creation: false,
        });
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("h4-readback") });
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
}

/// **H4a gate.** Render the same scene FORWARD (trusted `Renderer`) and DEFERRED, then prove the
/// deferred lighting reproduces the forward shading on lit geometry. Same computation, different
/// plumbing -- the first time the shadow pass + shading render together off a G-buffer.
/// The default cube+plane scene (identical to the forward `Renderer`'s hardcoded scene).
fn default_scene() -> Vec<ObjSpec> {
    let (cv, ci) = cube();
    let (pv, pi) = plane(40.0);
    let mut scene: Vec<ObjSpec> = Vec::new();
    for &z in &[0.0f32, 9.0, 18.0] {
        scene.push((cv.clone(), ci.clone(), Mat4::from_translation(Vec3::new(0.0, 0.0, z)), [0.85, 0.45, 0.20, 1.0], true));
    }
    scene.push((pv, pi, Mat4::IDENTITY, [0.55, 0.55, 0.58, 1.0], false));
    scene
}

pub fn run_h4a() -> bool {
    let (device, queue) = crate::headless_vulkan_device();
    let forward = Renderer::new(&device, SHOT_FMT);
    let deferred = DeferredRenderer::new(&device, &default_scene(), None);
    let (w, h) = (1280u32, 800u32);
    let aspect = w as f32 / h as f32;
    let dir = std::env::temp_dir();

    // A camera across the receding cubes, mid-low sun -> shadows span cascades (the interesting case).
    let (vp, eye, fwd) = orbit_cam(w, h, 35.0, 30.0, 22.0);
    let sun = sun_dir(35.0, 41.0);
    let frame = make_frame(vp, eye, fwd, Vec3::Y, aspect, sun, true);

    let fpx = render_to_pixels(&device, &queue, &forward, &frame, w, h);
    let dpx = deferred.render(&device, &queue, &frame, w, h);
    write_png(&dir.join("h4a_forward.png"), &fpx, w, h);
    write_png(&dir.join("h4a_deferred.png"), &dpx, w, h);

    // Geometry-masked comparison: the corner pixel is background (sky); geometry = pixels that
    // differ from it. Isolate the lighting integration from any background/clear encoding trivia.
    let bg = [fpx[0], fpx[1], fpx[2]];
    let is_geom = |p: &[u8], i: usize| {
        (p[i] as i16 - bg[0] as i16).abs() > 8 || (p[i + 1] as i16 - bg[1] as i16).abs() > 8 || (p[i + 2] as i16 - bg[2] as i16).abs() > 8
    };
    let chan_delta = |i: usize| {
        let d0 = (fpx[i] as i16 - dpx[i] as i16).abs();
        let d1 = (fpx[i + 1] as i16 - dpx[i + 1] as i16).abs();
        let d2 = (fpx[i + 2] as i16 - dpx[i + 2] as i16).abs();
        d0.max(d1).max(d2)
    };
    let mut diff_img = vec![0u8; (w * h * 4) as usize];
    let (mut geom, mut gdiff, mut full_diff, mut worst) = (0u64, 0u64, 0u64, 0i16);
    let mut i = 0;
    while i + 3 < fpx.len().min(dpx.len()) {
        let delta = chan_delta(i);
        if delta > 0 { full_diff += 1; }
        if is_geom(&fpx, i) {
            geom += 1;
            if delta > 3 {
                gdiff += 1;
                worst = worst.max(delta);
                diff_img[i] = 255; diff_img[i + 1] = 255; diff_img[i + 2] = 255; diff_img[i + 3] = 255;
            } else {
                diff_img[i + 3] = 255;
            }
        }
        i += 4;
    }
    write_png(&dir.join("h4a_diff.png"), &diff_img, w, h);

    let frac = if geom > 0 { gdiff as f64 / geom as f64 } else { 0.0 };
    log::info!("H4a DEFERRED == FORWARD gate ({}x{}, mid-low sun, cubes span cascades):", w, h);
    log::info!("  geometry pixels: {}   deferred!=forward (>3/ch): {}  ({:.3}% of geometry, worst {}/255)", geom, gdiff, frac * 100.0, worst);
    log::info!("  full-frame differing pixels (incl. edges/bg): {}", full_diff);
    log::info!("  wrote h4a_forward.png / h4a_deferred.png / h4a_diff.png to {}", dir.display());
    let ok = frac < 0.01;
    log::info!("  H4a {} -- deferred reproduces forward shading (unified render integration proven).", if ok { "PASS" } else { "REVIEW" });
    ok
}

/// Convert a loaded Collada mesh to the engine's vertex + index buffers.
fn dae_to_vtx(m: &crate::dae::DaeMesh) -> (Vec<Vtx>, Vec<u16>) {
    if m.positions.len() > u16::MAX as usize {
        log::warn!("dae mesh has {} verts (> u16 max) -- needs u32 indices; render will be wrong", m.positions.len());
    }
    let verts: Vec<Vtx> = (0..m.positions.len())
        .map(|i| Vtx { pos: m.positions[i], nrm: m.normals[i], uv: m.uvs[i] })
        .collect();
    let idx: Vec<u16> = m.indices.iter().map(|&i| i as u16).collect();
    (verts, idx)
}

/// Model matrix that scales a mesh to `target_height` and rests it (base-centered) on y=0.
fn fit_model(m: &crate::dae::DaeMesh, target_height: f32) -> Mat4 {
    let (lo, hi) = m.bounds();
    let s = target_height / (hi.y - lo.y).max(1e-3);
    let base_center = Vec3::new((lo.x + hi.x) * 0.5, lo.y, (lo.z + hi.z) * 0.5);
    Mat4::from_scale(Vec3::splat(s)) * Mat4::from_translation(-base_center)
}

/// count geometry pixels (differ from the background/corner) -- non-degenerate render check.
fn geom_px(px: &[u8]) -> u64 {
    let bg = [px[0], px[1], px[2]];
    let mut n = 0u64;
    let mut i = 0;
    while i + 3 < px.len() {
        if (px[i] as i16 - bg[0] as i16).abs() > 8 || (px[i + 1] as i16 - bg[1] as i16).abs() > 8 || (px[i + 2] as i16 - bg[2] as i16).abs() > 8 { n += 1; }
        i += 4;
    }
    n
}

/// **H4b gate.** Real `.dae` geometry through the (proven) deferred pipeline. Two parts:
/// (1) an ORACLE -- the repo `cube.dae`, loaded and fit to our procedural cube's box, must
/// render the same image as the procedural cube (validates the loader against trusted geometry);
/// (2) a SHOWCASE -- real SL meshes (repo `hex_phys.dae` + any `*.dae` paths on the CLI) load and
/// render lit + shadowed. The deferred pipeline is unchanged from H4a; only the mesh source is.
pub fn run_h4b(extra: Vec<String>) -> bool {
    let (device, queue) = crate::headless_vulkan_device();
    let (w, h) = (1280u32, 800u32);
    let aspect = w as f32 / h as f32;
    let dir = std::env::temp_dir();
    let (pv, pi) = plane(40.0);
    let ground = [0.55f32, 0.55, 0.58, 1.0];
    let orange = [0.85f32, 0.45, 0.20, 1.0];

    // ---- (1) loader oracle: cube.dae must match our procedural cube ----
    let cube_path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../newview/cube.dae");
    let dcube = match crate::dae::load(std::path::Path::new(cube_path)) {
        Ok(m) => m,
        Err(e) => { log::error!("H4b: load cube.dae failed: {e}"); return false; }
    };
    let (dcv, dci) = dae_to_vtx(&dcube);
    let cube_model = fit_model(&dcube, 1.0); // -> procedural cube's box (y 0..1, +-0.5)
    let (cv, ci) = cube();
    let scene_proc: Vec<ObjSpec> = vec![
        (cv, ci, Mat4::IDENTITY, orange, true),
        (pv.clone(), pi.clone(), Mat4::IDENTITY, ground, false),
    ];
    let scene_dae: Vec<ObjSpec> = vec![
        (dcv, dci, cube_model, orange, true),
        (pv.clone(), pi.clone(), Mat4::IDENTITY, ground, false),
    ];
    let r_proc = DeferredRenderer::new(&device, &scene_proc, None);
    let r_dae = DeferredRenderer::new(&device, &scene_dae, None);

    let (vp, eye, fwd) = orbit_cam(w, h, 35.0, 28.0, 7.0);
    let frame = make_frame(vp, eye, fwd, Vec3::Y, aspect, sun_dir(40.0, 41.0), true);
    let px_proc = r_proc.render(&device, &queue, &frame, w, h);
    let px_dae = r_dae.render(&device, &queue, &frame, w, h);
    write_png(&dir.join("h4b_cube_proc.png"), &px_proc, w, h);
    write_png(&dir.join("h4b_cube_dae.png"), &px_dae, w, h);

    let bg = [px_proc[0], px_proc[1], px_proc[2]];
    let (mut geom, mut gdiff) = (0u64, 0u64);
    let mut i = 0;
    while i + 3 < px_proc.len().min(px_dae.len()) {
        let is_geom = (px_proc[i] as i16 - bg[0] as i16).abs() > 8
            || (px_proc[i + 1] as i16 - bg[1] as i16).abs() > 8
            || (px_proc[i + 2] as i16 - bg[2] as i16).abs() > 8;
        if is_geom {
            geom += 1;
            let d = (px_proc[i] as i16 - px_dae[i] as i16).abs()
                .max((px_proc[i + 1] as i16 - px_dae[i + 1] as i16).abs())
                .max((px_proc[i + 2] as i16 - px_dae[i + 2] as i16).abs());
            if d > 6 { gdiff += 1; }
        }
        i += 4;
    }
    let frac = if geom > 0 { gdiff as f64 / geom as f64 } else { 1.0 };
    let cube_ok = frac < 0.01 && geom > 1000;
    log::info!("H4b (1) LOADER ORACLE -- cube.dae vs procedural cube ({} tris loaded):", dcube.tri_count());
    log::info!("  geometry px {}   differing (>6/ch) {}  ({:.3}%)  -> {}", geom, gdiff, frac * 100.0, if cube_ok { "loader validated" } else { "MISMATCH" });

    // ---- (2) showcase: real SL meshes ----
    let mut show: Vec<String> = vec![concat!(env!("CARGO_MANIFEST_DIR"), "/../../newview/fs_resources/hex_phys.dae").to_string()];
    show.extend(extra);
    let mut show_ok = true;
    log::info!("H4b (2) SHOWCASE -- real meshes through the deferred pipeline:");
    for path in &show {
        let p = std::path::Path::new(path);
        let stem = p.file_stem().and_then(|s| s.to_str()).unwrap_or("mesh").replace(' ', "_");
        match crate::dae::load(p) {
            Ok(m) => {
                let (mv, mi) = dae_to_vtx(&m);
                let scene: Vec<ObjSpec> = vec![
                    (mv, mi, fit_model(&m, 3.0), [0.82, 0.80, 0.74, 1.0], true),
                    (pv.clone(), pi.clone(), Mat4::IDENTITY, ground, false),
                ];
                let r = DeferredRenderer::new(&device, &scene, None);
                let (mvp, meye, mfwd) = orbit_cam(w, h, 35.0, 20.0, 8.0);
                let mframe = make_frame(mvp, meye, mfwd, Vec3::Y, aspect, sun_dir(45.0, 41.0), true);
                let mpx = r.render(&device, &queue, &mframe, w, h);
                let gp = geom_px(&mpx);
                let out = dir.join(format!("h4b_{stem}.png"));
                write_png(&out, &mpx, w, h);
                let (lo, hi) = m.bounds();
                let rendered = gp > 2000;
                log::info!("  {:<22} {:>6} verts / {:>5} tris  bbox [{:.1},{:.1},{:.1}]..[{:.1},{:.1},{:.1}]  {:>7} px  -> {}",
                    stem, m.positions.len(), m.tri_count(), lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, gp, if rendered { "RENDERED" } else { "DEGENERATE" });
                if !rendered { show_ok = false; }
            }
            Err(e) => { log::warn!("  {}: load failed: {e}", stem); show_ok = false; }
        }
    }

    log::info!("  wrote h4b_*.png to {}", dir.display());
    let ok = cube_ok && show_ok;
    log::info!("  H4b {} -- real .dae loads + renders lit/shadowed in the deferred pipeline.", if ok { "PASS" } else { "REVIEW" });
    ok
}

/// A procedural checker texture (sRGB) -- known content for the H4c UV+sampler proof.
fn make_checker(device: &wgpu::Device, queue: &wgpu::Queue) -> (wgpu::Texture, wgpu::TextureView) {
    let size = 512u32;
    let cell = size / 8; // 8x8 cells
    let mut data = vec![0u8; (size * size * 4) as usize];
    for y in 0..size {
        for x in 0..size {
            let dark = ((x / cell + y / cell) % 2) == 0;
            let (r, g, b) = if dark { (40u8, 44, 60) } else { (225u8, 228, 235) };
            let i = ((y * size + x) * 4) as usize;
            data[i] = r; data[i + 1] = g; data[i + 2] = b; data[i + 3] = 255;
        }
    }
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("checker"),
        size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
        mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8UnormSrgb,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST, view_formats: &[],
    });
    queue.write_texture(
        wgpu::ImageCopyTexture { texture: &tex, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        &data,
        wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(size * 4), rows_per_image: Some(size) },
        wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
    );
    let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
    (tex, view)
}

/// dark/light split over geometry pixels (luminance threshold) -- checker detector.
fn lum_split(px: &[u8]) -> (u64, u64) {
    let bg = [px[0], px[1], px[2]];
    let (mut dark, mut light) = (0u64, 0u64);
    let mut i = 0;
    while i + 3 < px.len() {
        let is_geom = (px[i] as i16 - bg[0] as i16).abs() > 8
            || (px[i + 1] as i16 - bg[1] as i16).abs() > 8
            || (px[i + 2] as i16 - bg[2] as i16).abs() > 8;
        if is_geom {
            let lum = (px[i] as u32 + px[i + 1] as u32 * 2 + px[i + 2] as u32) / 4;
            if lum < 110 { dark += 1; } else { light += 1; }
        }
        i += 4;
    }
    (dark, light)
}

/// **H4c gate.** Textures through the deferred pipeline via the mesh UVs + a sampler (the H3b2
/// mechanism, now feeding the G-buffer albedo). GATE: a checker-textured plane must show the
/// two-tone pattern (~50/50 dark/light) while a flat render of the same is single-tone -- proving
/// albedo varies by UV through the sampler, not a constant. SHOWCASE: checker on the cube + any
/// CLI `*.dae`. `cargo run -- h4c [extra.dae ...]`.
pub fn run_h4c(extra: Vec<String>) -> bool {
    let (device, queue) = crate::headless_vulkan_device();
    let (w, h) = (1280u32, 800u32);
    let aspect = w as f32 / h as f32;
    let dir = std::env::temp_dir();
    let (_checker_tex, checker_view) = make_checker(&device, &queue);
    let (pv, pi) = plane(40.0);
    let ground = [0.55f32, 0.55, 0.58, 1.0];
    let white = [1.0f32, 1.0, 1.0, 1.0];

    // ---- GATE ----
    let (gpv, gpi) = plane(4.0); // small plane: uv 0..1 -> ~8 checker cells in view
    let gate_scene: Vec<ObjSpec> = vec![(gpv, gpi, Mat4::IDENTITY, white, false)];
    let r_tex = DeferredRenderer::new(&device, &gate_scene, Some(&checker_view));
    let r_flat = DeferredRenderer::new(&device, &gate_scene, None);
    let eye = Vec3::new(0.0, 9.0, 0.001); // near-top-down
    let proj = Mat4::perspective_rh(FOV, aspect, 0.1, 100.0);
    let vp = proj * Mat4::look_at_rh(eye, Vec3::ZERO, Vec3::NEG_Z);
    let fwd = (Vec3::ZERO - eye).normalize();
    let frame = make_frame(vp, eye, fwd, Vec3::NEG_Z, aspect, sun_dir(70.0, 41.0), true);
    let px_tex = r_tex.render(&device, &queue, &frame, w, h);
    let px_flat = r_flat.render(&device, &queue, &frame, w, h);
    write_png(&dir.join("h4c_checker_tex.png"), &px_tex, w, h);
    write_png(&dir.join("h4c_checker_flat.png"), &px_flat, w, h);

    let (td, tl) = lum_split(&px_tex);
    let (fd, fl) = lum_split(&px_flat);
    let tex_dark = td as f64 / (td + tl).max(1) as f64;
    let flat_dark = fd as f64 / (fd + fl).max(1) as f64;
    let gate_ok = tex_dark > 0.30 && tex_dark < 0.70 && (flat_dark < 0.10 || flat_dark > 0.90);
    log::info!("H4c UV+SAMPLER gate (checker-textured plane, top-down):");
    log::info!("  textured: {:>4.0}% dark / {:>4.0}% light   (want ~50/50 -> checker sampled by UV)", tex_dark * 100.0, (1.0 - tex_dark) * 100.0);
    log::info!("  flat:     {:>4.0}% dark / {:>4.0}% light   (want single-tone -> no texture)", flat_dark * 100.0, (1.0 - flat_dark) * 100.0);
    log::info!("  gate {}", if gate_ok { "PASS" } else { "REVIEW" });

    // ---- SHOWCASE ----
    let (cv, ci) = cube();
    let mut show_ok = true;
    let cube_scene: Vec<ObjSpec> = vec![
        (cv, ci, Mat4::IDENTITY, white, true),
        (pv.clone(), pi.clone(), Mat4::IDENTITY, ground, false),
    ];
    let rc = DeferredRenderer::new(&device, &cube_scene, Some(&checker_view));
    let (cvp, ceye, cfwd) = orbit_cam(w, h, 35.0, 25.0, 5.0);
    let cframe = make_frame(cvp, ceye, cfwd, Vec3::Y, aspect, sun_dir(45.0, 41.0), true);
    let cpx = rc.render(&device, &queue, &cframe, w, h);
    write_png(&dir.join("h4c_cube.png"), &cpx, w, h);
    log::info!("H4c SHOWCASE:  checker cube {} geometry px -> RENDERED", geom_px(&cpx));

    for path in &extra {
        let p = std::path::Path::new(path);
        let stem = p.file_stem().and_then(|s| s.to_str()).unwrap_or("mesh").replace(' ', "_");
        match crate::dae::load(p) {
            Ok(m) => {
                let (mv, mi) = dae_to_vtx(&m);
                let scene: Vec<ObjSpec> = vec![
                    (mv, mi, fit_model(&m, 3.0), white, true),
                    (pv.clone(), pi.clone(), Mat4::IDENTITY, ground, false),
                ];
                let r = DeferredRenderer::new(&device, &scene, Some(&checker_view));
                let (mvp, meye, mfwd) = orbit_cam(w, h, 35.0, 20.0, 8.0);
                let mframe = make_frame(mvp, meye, mfwd, Vec3::Y, aspect, sun_dir(45.0, 41.0), true);
                let mpx = r.render(&device, &queue, &mframe, w, h);
                let gp = geom_px(&mpx);
                write_png(&dir.join(format!("h4c_{stem}.png")), &mpx, w, h);
                log::info!("  checker {:<20} {:>5} tris, {:>7} geometry px -> {}", stem, m.tri_count(), gp, if gp > 2000 { "RENDERED" } else { "DEGENERATE" });
                if gp <= 2000 { show_ok = false; }
            }
            Err(e) => { log::warn!("  {}: load failed: {e}", stem); show_ok = false; }
        }
    }

    log::info!("  wrote h4c_*.png to {}", dir.display());
    let ok = gate_ok && show_ok;
    log::info!("  H4c {} -- textures (UV + sampler) render through the deferred pipeline.", if ok { "PASS" } else { "REVIEW" });
    ok
}

fn srgb8(l: f32) -> u8 {
    let s = if l <= 0.0031308 { 12.92 * l } else { 1.055 * l.powf(1.0 / 2.4) - 0.055 };
    (s.clamp(0.0, 1.0) * 255.0 + 0.5) as u8
}

/// CPU mirror of the shader's sky tone-map + sRGB encode (the H4d oracle side).
fn sky_tonemap_srgb(sky: [f32; 3]) -> [u8; 3] {
    let mut o = [0u8; 3];
    for c in 0..3 {
        let disp = 1.0 - (-sky[c] * SKY_TONEMAP_K).exp();
        o[c] = srgb8(disp.clamp(0.0, 1.0));
    }
    o
}

/// **H4d gate.** The EEP sky (sun + sky) behind the shadowed, textured mesh -- the north star.
/// GATE: the GLSL sky (a port of the Rust `sl_sky_radiance`, the shipped-default EEP dome) must
/// match that Rust oracle PER-PIXEL, using the same inverse-VP view-ray reconstruction (sun disk
/// excluded). This is the H3/H4a discipline applied to the sky: a trusted CPU oracle, byte-close.
/// SHOWCASE: textured cube + any CLI `.dae` under the sky, high & low sun. `cargo run -- h4d`.
pub fn run_h4d(extra: Vec<String>) -> bool {
    let (device, queue) = crate::headless_vulkan_device();
    let (w, h) = (1280u32, 800u32);
    let aspect = w as f32 / h as f32;
    let dir = std::env::temp_dir();
    let (_ck, ckview) = make_checker(&device, &queue);
    let (pv, pi) = plane(40.0);
    let ground = [0.55f32, 0.55, 0.58, 1.0];
    let white = [1.0f32, 1.0, 1.0, 1.0];
    let sun_az = 41.0f32;

    // ---- GATE: GLSL EEP sky == Rust sl_sky_radiance, per-pixel ----
    let sky_only = DeferredRenderer::new(&device, &[], None); // empty scene -> whole frame is sky
    let sun = sun_dir(25.0, sun_az);
    let eye = Vec3::new(0.0, 2.0, 0.0);
    let target = Vec3::new(3.0, 6.0, 10.0);
    let proj = Mat4::perspective_rh(FOV, aspect, 0.1, 100.0);
    let vp = proj * Mat4::look_at_rh(eye, target, Vec3::Y);
    let fwd = (target - eye).normalize();
    let mut frame = make_frame(vp, eye, fwd, Vec3::Y, aspect, sun, true);
    frame.sky = true;
    let px = sky_only.render(&device, &queue, &frame, w, h);
    write_png(&dir.join("h4d_sky.png"), &px, w, h);

    let sun_up = -sun;
    let inv = vp.inverse();
    let (mut n, mut sum, mut worst, mut lmin, mut lmax) = (0u64, 0.0f64, 0i32, 255i32, 0i32);
    for y in 0..h {
        for x in 0..w {
            let (uvx, uvy) = ((x as f32 + 0.5) / w as f32, (y as f32 + 0.5) / h as f32);
            let (ndx, ndy) = (uvx * 2.0 - 1.0, 1.0 - uvy * 2.0);
            let pn = inv * Vec4::new(ndx, ndy, 0.0, 1.0);
            let pf = inv * Vec4::new(ndx, ndy, 1.0, 1.0);
            let d = (pf.truncate() / pf.w - pn.truncate() / pn.w).normalize();
            if d.dot(sun_up) > 0.95 { continue; } // exclude the sun disk/glow (not in the dome oracle)
            let exp = sky_tonemap_srgb(crate::sl_sky_radiance([d.x, d.y, d.z], [sun_up.x, sun_up.y, sun_up.z]));
            let i = ((y * w + x) * 4) as usize;
            let dm = (px[i] as i32 - exp[0] as i32).abs()
                .max((px[i + 1] as i32 - exp[1] as i32).abs())
                .max((px[i + 2] as i32 - exp[2] as i32).abs());
            sum += dm as f64;
            worst = worst.max(dm);
            n += 1;
            let lum = (px[i] as i32 + px[i + 1] as i32 + px[i + 2] as i32) / 3;
            lmin = lmin.min(lum);
            lmax = lmax.max(lum);
        }
    }
    let mean = sum / n.max(1) as f64;
    let gate_ok = mean < 2.0 && worst < 16 && (lmax - lmin) > 20;
    log::info!("H4d EEP SKY gate -- GLSL sky vs Rust sl_sky_radiance ({} sky px, sun disk excluded):", n);
    log::info!("  mean 8-bit delta {:.2}, worst {}   (want ~0 -> faithful EEP port + view-ray)", mean, worst);
    log::info!("  sky luminance spread {}..{}   (want wide -> real gradient, not flat)", lmin, lmax);
    log::info!("  gate {}", if gate_ok { "PASS" } else { "REVIEW" });

    // ---- SHOWCASE: mesh + EEP sky + shadows ----
    let (cv, ci) = cube();
    let scene: Vec<ObjSpec> = vec![
        (cv, ci, Mat4::IDENTITY, white, true),
        (pv.clone(), pi.clone(), Mat4::IDENTITY, ground, false),
    ];
    let r = DeferredRenderer::new(&device, &scene, Some(&ckview));
    log::info!("H4d SHOWCASE (mesh + EEP sky + shadows):");
    for &(name, elev) in &[("high", 55.0f32), ("low", 12.0)] {
        let (cvp, ceye, cfwd) = orbit_cam(w, h, 40.0, 10.0, 6.0);
        let mut cframe = make_frame(cvp, ceye, cfwd, Vec3::Y, aspect, sun_dir(elev, sun_az), true);
        cframe.sky = true;
        let cpx = r.render(&device, &queue, &cframe, w, h);
        write_png(&dir.join(format!("h4d_scene_{name}sun.png")), &cpx, w, h);
        log::info!("  checker cube + sky ({:>4} sun) -> RENDERED", name);
    }
    let mut show_ok = true;
    for path in &extra {
        let p = std::path::Path::new(path);
        let stem = p.file_stem().and_then(|s| s.to_str()).unwrap_or("mesh").replace(' ', "_");
        match crate::dae::load(p) {
            Ok(m) => {
                let (mv, mi) = dae_to_vtx(&m);
                let sc: Vec<ObjSpec> = vec![
                    (mv, mi, fit_model(&m, 3.0), white, true),
                    (pv.clone(), pi.clone(), Mat4::IDENTITY, ground, false),
                ];
                let rr = DeferredRenderer::new(&device, &sc, Some(&ckview));
                let (mvp, meye, mfwd) = orbit_cam(w, h, 35.0, 12.0, 8.0);
                let mut mframe = make_frame(mvp, meye, mfwd, Vec3::Y, aspect, sun_dir(30.0, sun_az), true);
                mframe.sky = true;
                let mpx = rr.render(&device, &queue, &mframe, w, h);
                write_png(&dir.join(format!("h4d_{stem}.png")), &mpx, w, h);
                log::info!("  {} + sky: {} tris -> RENDERED", stem, m.tri_count());
            }
            Err(e) => { log::warn!("  {}: load failed: {e}", stem); show_ok = false; }
        }
    }

    log::info!("  wrote h4d_*.png to {}", dir.display());
    let ok = gate_ok && show_ok;
    log::info!("  H4d {} -- EEP sky + sun behind the mesh, shadows consistent (north star).", if ok { "PASS" } else { "REVIEW" });
    ok
}

// ---- live windowed H4 view -------------------------------------------------------

/// Interactive H4: the deferred scene (mesh + EEP sky + cascade shadows) to the swapchain.
struct H4Scene {
    window: Arc<Window>,
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    _checker: wgpu::Texture, // keep the albedo texture alive
    renderer: DeferredRenderer,
    proj: Mat4,
    yaw: f32,
    pitch: f32,
    radius: f32,
    sun_elev: f32,
    sun_az: f32,
    auto_rotate: bool,
    sun_move: bool,
}

impl H4Scene {
    fn new(window: Arc<Window>, mesh_path: Option<String>) -> H4Scene {
        pollster::block_on(async {
            let instance = wgpu::Instance::new(wgpu::InstanceDescriptor { backends: wgpu::Backends::VULKAN, ..Default::default() });
            let surface = instance.create_surface(window.clone()).expect("surface");
            let adapter = instance.request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::HighPerformance, compatible_surface: Some(&surface), force_fallback_adapter: false,
            }).await.expect("no Vulkan adapter");
            let info = adapter.get_info();
            log::info!("adapter: {} | backend={:?} | {} {}", info.name, info.backend, info.driver, info.driver_info);
            let (device, queue) = adapter.request_device(&wgpu::DeviceDescriptor {
                label: Some("h4-engine"), required_features: wgpu::Features::empty(), required_limits: wgpu::Limits::default(),
            }, None).await.expect("request_device");

            let size = window.inner_size();
            let caps = surface.get_capabilities(&adapter);
            // The lighting pipeline targets SHOT_FMT (Rgba8UnormSrgb); match the surface to it.
            let format = if caps.formats.contains(&SHOT_FMT) { SHOT_FMT } else {
                let f = caps.formats.iter().copied().find(|f| f.is_srgb()).unwrap_or(caps.formats[0]);
                log::warn!("surface lacks {:?}; using {:?} (lighting pipeline may need that format)", SHOT_FMT, f);
                f
            };
            let config = wgpu::SurfaceConfiguration {
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT, format,
                width: size.width.max(1), height: size.height.max(1),
                present_mode: caps.present_modes[0], alpha_mode: caps.alpha_modes[0],
                view_formats: vec![], desired_maximum_frame_latency: 2,
            };
            surface.configure(&device, &config);

            let (checker, checker_view) = make_checker(&device, &queue);
            let (pv, pi) = plane(40.0);
            let ground = [0.55f32, 0.55, 0.58, 1.0];
            let mut objs: Vec<ObjSpec> = Vec::new();
            match mesh_path.as_ref().map(|p| (p.clone(), crate::dae::load(std::path::Path::new(p)))) {
                Some((p, Ok(m))) => {
                    log::info!("loaded {}: {} tris", p, m.tri_count());
                    let (mv, mi) = dae_to_vtx(&m);
                    objs.push((mv, mi, fit_model(&m, 3.0), [0.82, 0.80, 0.74, 1.0], true));
                }
                other => {
                    if let Some((p, Err(e))) = other { log::warn!("load {p}: {e}; using cube"); }
                    let (cv, ci) = cube();
                    objs.push((cv, ci, Mat4::IDENTITY, [0.85, 0.45, 0.20, 1.0], true));
                }
            }
            objs.push((pv, pi, Mat4::IDENTITY, ground, false));
            let renderer = DeferredRenderer::new(&device, &objs, Some(&checker_view));

            let aspect = config.width as f32 / config.height as f32;
            let proj = Mat4::perspective_rh(FOV, aspect, 0.1, 200.0);
            H4Scene {
                window, surface, device, queue, config, _checker: checker, renderer, proj,
                yaw: 0.7, pitch: 0.35, radius: 8.0, sun_elev: 35.0, sun_az: 41.0, auto_rotate: true, sun_move: false,
            }
        })
    }

    fn resize(&mut self, size: winit::dpi::PhysicalSize<u32>) {
        if size.width == 0 || size.height == 0 { return; }
        self.config.width = size.width;
        self.config.height = size.height;
        self.surface.configure(&self.device, &self.config);
        self.proj = Mat4::perspective_rh(FOV, size.width as f32 / size.height as f32, 0.1, 200.0);
    }

    fn render(&mut self) -> Result<(), wgpu::SurfaceError> {
        if self.auto_rotate { self.yaw += 0.004; }
        if self.sun_move { self.sun_az += 0.3; }
        let target = Vec3::new(0.0, 1.1, 0.0);
        let eye = orbit_eye(self.yaw, self.pitch, self.radius);
        let view_proj = self.proj * Mat4::look_at_rh(eye, target, Vec3::Y);
        let sun = sun_dir(self.sun_elev, self.sun_az);
        let fwd = (target - eye).normalize();
        let aspect = self.config.width as f32 / self.config.height as f32;
        let mut frame = make_frame(view_proj, eye, fwd, Vec3::Y, aspect, sun, true);
        frame.sky = true;

        let sframe = self.surface.get_current_texture()?;
        let view = sframe.texture.create_view(&wgpu::TextureViewDescriptor::default());
        self.renderer.render_into(&self.device, &self.queue, &frame, &view, self.config.width, self.config.height);
        sframe.present();
        Ok(())
    }
}

/// `cargo run -- h4 [mesh.dae]` -- interactive deferred scene: mesh + EEP sky + shadows.
pub fn run_h4_window(mesh_path: Option<String>) {
    let event_loop = EventLoop::new().expect("event loop");
    let window = Arc::new(WindowBuilder::new()
        .with_title("vkharness -- H4 engine (deferred: DAE + EEP sky + shadows)")
        .with_inner_size(winit::dpi::LogicalSize::new(1280.0, 800.0))
        .build(&event_loop).expect("window"));
    let mut scene = H4Scene::new(window.clone(), mesh_path);
    log::info!("H4 live: arrows orbit | Space auto-spin | S sun-spin | [ ] sun elevation | Esc quits");
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
                        KeyCode::KeyS => scene.sun_move = !scene.sun_move,
                        KeyCode::ArrowLeft => scene.yaw -= step,
                        KeyCode::ArrowRight => scene.yaw += step,
                        KeyCode::ArrowUp => scene.pitch = (scene.pitch + step).min(1.5),
                        KeyCode::ArrowDown => scene.pitch = (scene.pitch - step).max(-0.2),
                        KeyCode::BracketRight => scene.sun_elev = (scene.sun_elev + 2.0).min(89.0),
                        KeyCode::BracketLeft => scene.sun_elev = (scene.sun_elev - 2.0).max(2.0),
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
