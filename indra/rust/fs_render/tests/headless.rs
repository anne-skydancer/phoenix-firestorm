//! Headless diagnostics for the live-bridge engine.
//!
//! Drives the REAL `LiveRenderer` on an offscreen (surfaceless) Vulkan device and reads
//! pixels back, so each deferred-pipeline increment can be verified deterministically
//! without launching the viewer. The viewer is reserved for human eyeball sign-off at the
//! visible milestones.
//!
//! Requires a Vulkan adapter; if none is present the tests no-op (skip) rather than fail,
//! so CI on a headless box without Vulkan does not spuriously break.

use fs_render::live::{DrawDesc, LiveRenderer, CLASS_SKY_DOME};
use fs_render::scene::{CameraBlock, EepSkyBlock, SceneFrame, SettingsSnapshot, SkyRegime};

fn headless() -> Option<(wgpu::Device, wgpu::Queue)> {
    let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
        backends: wgpu::Backends::VULKAN,
        ..Default::default()
    });
    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::HighPerformance,
        compatible_surface: None,
        force_fallback_adapter: false,
    }))?;
    let dq = pollster::block_on(adapter.request_device(
        &wgpu::DeviceDescriptor {
            label: Some("headless"),
            required_features: wgpu::Features::empty(),
            required_limits: wgpu::Limits::default(),
        },
        None,
    ))
    .ok()?;
    Some(dq)
}

// ===== CPU mirrors of shaders/post_tonemap.frag (the S1 ORACLE) =====
// Each is transcribed op-for-op from the shader; the harness computes the expected display
// pixel, the GPU runs the SPIR-V. The shader is the authority; these mirror it. A shared
// transcription slip is guarded by the independent anti-blowout check at the end.

/// linear -> sRGB (the shader's linear_to_srgb; also the sRGB ROP's encode).
fn lin_to_srgb(u: f64) -> f64 {
    let u = u.clamp(0.0, 1.0);
    if u < 0.0031308 { 12.92 * u } else { 1.055 * u.powf(1.0 / 2.4) - 0.055 }
}
fn to_u8(x: f64) -> u8 { (x.clamp(0.0, 1.0) * 255.0).round() as u8 }

/// PBRNeutralToneMapping (Khronos).
fn pbr_neutral(mut c: [f64; 3]) -> [f64; 3] {
    let start_compression = 0.8 - 0.04; // 0.76
    let desaturation = 0.15;
    let x = c[0].min(c[1]).min(c[2]);
    let offset = if x < 0.08 { x - 6.25 * x * x } else { 0.04 };
    c = [c[0] - offset, c[1] - offset, c[2] - offset];
    let peak = c[0].max(c[1]).max(c[2]);
    if peak < start_compression {
        return c;
    }
    let d = 1.0 - start_compression; // 0.24
    let new_peak = 1.0 - d * d / (peak + d - start_compression);
    let s = new_peak / peak;
    c = [c[0] * s, c[1] * s, c[2] * s];
    let g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    [
        c[0] * (1.0 - g) + new_peak * g,
        c[1] * (1.0 - g) + new_peak * g,
        c[2] * (1.0 - g) + new_peak * g,
    ]
}

/// ACES Hill fit (matches the mat3 column-major multiplies in the shader).
fn aces_hill(v: [f64; 3]) -> [f64; 3] {
    let inp = [
        0.59719 * v[0] + 0.35458 * v[1] + 0.04823 * v[2],
        0.07600 * v[0] + 0.90834 * v[1] + 0.01566 * v[2],
        0.02840 * v[0] + 0.13383 * v[1] + 0.83777 * v[2],
    ];
    let mut fit = [0.0; 3];
    for i in 0..3 {
        let a = inp[i] * (inp[i] + 0.0245786) - 0.000090537;
        let b = inp[i] * (0.983729 * inp[i] + 0.4329510) + 0.238081;
        fit[i] = a / b;
    }
    let out = [
        1.60475 * fit[0] - 0.53108 * fit[1] - 0.07367 * fit[2],
        -0.10208 * fit[0] + 1.10813 * fit[1] - 0.00605 * fit[2],
        -0.00327 * fit[0] - 0.07276 * fit[1] + 1.07602 * fit[2],
    ];
    [out[0].clamp(0.0, 1.0), out[1].clamp(0.0, 1.0), out[2].clamp(0.0, 1.0)]
}

/// legacyGamma soft-clip on the sRGB-encoded value.
fn legacy_gamma(c: [f64; 3], gamma: f64) -> [f64; 3] {
    let mut o = [0.0; 3];
    for i in 0..3 {
        let t = 1.0 - c[i].clamp(0.0, 1.0);
        o[i] = 1.0 - t.powf(gamma);
    }
    o
}

/// Post-pass parameters (mirror of the Post UBO).
#[derive(Clone, Copy)]
struct Post {
    exposure: f64,
    exp_scale: f64,
    tonemap_mix: f64,
    gamma: f64,
    tonemap_type: i32,
    legacy_gamma: i32,
}

/// The full CPU oracle: HDR clear -> the exact 8-bit display pixel the GPU pass must produce.
fn expected_disp(clear: [f64; 3], p: Post) -> [u8; 3] {
    let fe = p.exposure * p.exp_scale;
    let exposed = [clear[0] * fe, clear[1] * fe, clear[2] * fe];
    let tonemapped = if p.tonemap_type == 1 { aces_hill(exposed) } else { pbr_neutral(exposed) };
    let mut tm = [0.0; 3];
    for i in 0..3 {
        tm[i] = (exposed[i] * (1.0 - p.tonemap_mix) + tonemapped[i] * p.tonemap_mix).clamp(0.0, 1.0);
    }
    let mut disp = [lin_to_srgb(tm[0]), lin_to_srgb(tm[1]), lin_to_srgb(tm[2])];
    if p.legacy_gamma != 0 {
        disp = legacy_gamma(disp, p.gamma);
    }
    [to_u8(disp[0]), to_u8(disp[1]), to_u8(disp[2])]
}

/// Render one empty frame with the given HDR clear (routed through scene_hdr, linear, so
/// values > 1.0 survive the Rgba16Float target) under the given post params, and read back the
/// centre pixel as (R,G,B). Exercises the real tonemap SPIR-V end to end (incl. the sRGB ROP).
fn tonemap_center(device: &wgpu::Device, queue: &wgpu::Queue, clear: [f64; 3], p: Post) -> [u8; 3] {
    let fmt = wgpu::TextureFormat::Bgra8UnormSrgb; // the swapchain format the engine sees
    let mut live = LiveRenderer::new(device, queue, fmt);
    live.set_post_params(p.exposure as f32, p.exp_scale as f32, p.tonemap_mix as f32,
        p.gamma as f32, p.tonemap_type, p.legacy_gamma);
    render_empty_center(device, queue, &mut live, clear)
}

/// Render one empty frame (clear -> scene_hdr -> the pre-configured tonemap pass) and read the
/// centre pixel. The caller sets the LiveRenderer's post params first (directly or via
/// apply_sky_regime), so this isolates "whatever tonemap the renderer is configured with".
fn render_empty_center(device: &wgpu::Device, queue: &wgpu::Queue, live: &mut LiveRenderer, clear: [f64; 3]) -> [u8; 3] {
    let fmt = wgpu::TextureFormat::Bgra8UnormSrgb;
    let (w, h) = (64u32, 48u32);
    let target = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("headless-target"),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: fmt,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = target.create_view(&wgpu::TextureViewDescriptor::default());

    live.begin();
    let n = live.flush_clear(device, queue, &view, w, h,
        wgpu::Color { r: clear[0], g: clear[1], b: clear[2], a: 1.0 });
    assert_eq!(n, 0, "no draws were submitted");

    let stride = ((w * 4 + 255) / 256) * 256;
    let buf = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("readback"),
        size: (stride * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("rb") });
    enc.copy_texture_to_buffer(
        wgpu::ImageCopyTexture {
            texture: &target,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        wgpu::ImageCopyBuffer {
            buffer: &buf,
            layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(stride), rows_per_image: Some(h) },
        },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
    queue.submit([enc.finish()]);

    let slice = buf.slice(..);
    slice.map_async(wgpu::MapMode::Read, |_| {});
    device.poll(wgpu::Maintain::Wait);
    let data = slice.get_mapped_range();
    let off = ((h / 2) * stride + (w / 2) * 4) as usize;
    // Bgra8UnormSrgb byte order: B, G, R, A
    let out = [data[off + 2], data[off + 1], data[off]];
    drop(data);
    buf.unmap();
    out
}

/// S1 ORACLE: the mandatory tonemap pass (post_tonemap.frag) faithfully reproduces stock LL's
/// toneMap() + legacyGamma across the full parameter matrix -- both operators, mix in {0, 0.7},
/// exposure/exp_scale scaling, and the legacyGamma soft-clip. Plus an operator-independent
/// anti-blowout check (the flat-white bug). Must hold before sky/resolve feed real HDR.
#[test]
fn tonemap_pass_is_faithful() {
    let Some((device, queue)) = headless() else {
        eprintln!("no Vulkan adapter; skipping headless tonemap test");
        return;
    };

    // (clear, Post). Spans: ACES default (type1/mix0.7), PBRNeutral full-mix (type0/mix1),
    // legacy classic (mix0 + legacyGamma), gamma=1 legacyGamma no-op, exposure & exp_scale.
    let cases: [([f64; 3], Post); 9] = [
        // ACES Hill, default mix -- the advanced-sky path.
        ([0.30, 0.30, 0.30], Post { exposure: 1.0, exp_scale: 1.0, tonemap_mix: 0.7, gamma: 1.0, tonemap_type: 1, legacy_gamma: 0 }),
        ([0.50, 0.375, 0.25], Post { exposure: 1.0, exp_scale: 1.0, tonemap_mix: 0.7, gamma: 1.0, tonemap_type: 1, legacy_gamma: 0 }),
        // PBRNeutral, full mix -- the 1a operator, exercised at full strength.
        ([2.0, 2.0, 2.0], Post { exposure: 1.0, exp_scale: 1.0, tonemap_mix: 1.0, gamma: 1.0, tonemap_type: 0, legacy_gamma: 0 }),
        ([1.5, 0.5, 0.25], Post { exposure: 1.0, exp_scale: 1.0, tonemap_mix: 1.0, gamma: 1.0, tonemap_type: 0, legacy_gamma: 0 }),
        // Legacy classic: curve bypassed (mix=0), legacyGamma soft-clip engaged (gamma=2.2).
        ([0.50, 0.50, 0.50], Post { exposure: 1.0, exp_scale: 1.0, tonemap_mix: 0.0, gamma: 2.2, tonemap_type: 0, legacy_gamma: 1 }),
        ([1.20, 0.80, 0.40], Post { exposure: 1.0, exp_scale: 1.0, tonemap_mix: 0.0, gamma: 2.2, tonemap_type: 1, legacy_gamma: 1 }),
        // Legacy with gamma=1.0 -> legacyGamma is the identity branch.
        ([0.40, 0.40, 0.40], Post { exposure: 1.0, exp_scale: 1.0, tonemap_mix: 0.0, gamma: 1.0, tonemap_type: 0, legacy_gamma: 1 }),
        // Exposure > 1 pushes a dim scene up; exp_scale < 1 pulls a bright scene down.
        ([0.25, 0.25, 0.25], Post { exposure: 2.0, exp_scale: 1.0, tonemap_mix: 1.0, gamma: 1.0, tonemap_type: 0, legacy_gamma: 0 }),
        ([1.00, 1.00, 1.00], Post { exposure: 1.0, exp_scale: 0.5, tonemap_mix: 1.0, gamma: 1.0, tonemap_type: 0, legacy_gamma: 0 }),
    ];
    for (clear, p) in cases {
        let got = tonemap_center(&device, &queue, clear, p);
        let exp = expected_disp(clear, p);
        for ch in 0..3 {
            assert!(
                (got[ch] as i32 - exp[ch] as i32).abs() <= 2,
                "clear {:?} type{} mix{} legacy{} g{} ch{}: got {} vs expected {}",
                clear, p.tonemap_type, p.tonemap_mix, p.legacy_gamma, p.gamma, ch, got[ch], exp[ch]
            );
        }
    }

    // Anti-blowout, operator-independent: under PBRNeutral at full mix, three increasingly
    // bright grays map to DISTINCT, strictly-increasing, sub-255 outputs. The old identity-copy
    // hard clamp saturated all of them to 255 -- this would catch that regression.
    let pbr = Post { exposure: 1.0, exp_scale: 1.0, tonemap_mix: 1.0, gamma: 1.0, tonemap_type: 0, legacy_gamma: 0 };
    let g05 = tonemap_center(&device, &queue, [0.5, 0.5, 0.5], pbr)[0] as i32;
    let g20 = tonemap_center(&device, &queue, [2.0, 2.0, 2.0], pbr)[0] as i32;
    let g80 = tonemap_center(&device, &queue, [8.0, 8.0, 8.0], pbr)[0] as i32;
    assert!(g80 < 255, "brightest HDR gray must compress below saturation, got {}", g80);
    assert!(g80 > g20 && g20 > g05,
        "tonemap must preserve a gradient across HDR brightness: 0.5->{} 2.0->{} 8.0->{}", g05, g20, g80);
}

// ===== S2 sky-dome oracle =====

/// sRGB -> linear (inverse of lin_to_srgb; matches the shader's srgb_to_linear).
fn srgb_to_lin(u: f64) -> f64 {
    if u <= 0.04045 { u / 12.92 } else { ((u + 0.055) / 1.055).powf(2.4) }
}

/// The WindLight sky params, in f64. Constructed from the exact f32 aux the GPU reads, so the
/// only CPU/GPU difference is f32-vs-f64 arithmetic (absorbed by the byte tolerance).
struct SkyParams {
    cam_pos: [f64; 3], max_y: f64,
    lightnorm: [f64; 3], sun_up_factor: f64,
    sunlight_color: [f64; 3], density_multiplier: f64,
    moonlight_color: [f64; 3], sun_moon_glow_factor: f64,
    ambient_color: [f64; 3], haze_density: f64,
    blue_horizon: [f64; 3], haze_horizon: f64,
    blue_density: [f64; 3], cloud_shadow: f64,
    glow: [f64; 3],
    sky_hdr_scale: f64,
}
fn f3(a: &[f32; 48], i: usize) -> [f64; 3] { [a[i] as f64, a[i + 1] as f64, a[i + 2] as f64] }
impl SkyParams {
    fn from_aux(a: &[f32; 48]) -> SkyParams {
        SkyParams {
            cam_pos: f3(a, 0), max_y: a[3] as f64,
            lightnorm: f3(a, 4), sun_up_factor: a[7] as f64,
            sunlight_color: f3(a, 8), density_multiplier: a[11] as f64,
            moonlight_color: f3(a, 12), sun_moon_glow_factor: a[15] as f64,
            ambient_color: f3(a, 16), haze_density: a[19] as f64,
            blue_horizon: f3(a, 20), haze_horizon: a[23] as f64,
            blue_density: f3(a, 24), cloud_shadow: a[27] as f64,
            glow: f3(a, 28),
            sky_hdr_scale: a[32] as f64,
        }
    }
}

/// CPU port of sky.vert (skyV.glsl): pos -> vary_HazeColor. Transcribed op-for-op, including
/// the canonical DOUBLE-sqrt in the below-cloud blend (skyV.glsl:159-162).
fn sky_v_haze(pos: [f64; 3], p: &SkyParams) -> [f64; 3] {
    let mut rel = [pos[0] - p.cam_pos[0], pos[1] - p.cam_pos[1] + 50.0, pos[2] - p.cam_pos[2]];
    if rel[1] > 0.0 { let s = p.max_y / rel[1]; rel = [rel[0] * s, rel[1] * s, rel[2] * s]; }
    if rel[1] < 0.0 { let s = -32000.0 / rel[1]; rel = [rel[0] * s, rel[1] * s, rel[2] * s]; }
    let len = (rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]).sqrt();
    let n = [rel[0] / len, rel[1] / len, rel[2] / len];
    let rel_dot = n[0] * p.lightnorm[0] + n[1] * p.lightnorm[1] + n[2] * p.lightnorm[2];
    let mut sunlight = if p.sun_up_factor == 1.0 {
        p.sunlight_color
    } else {
        [p.moonlight_color[0] * 0.7, p.moonlight_color[1] * 0.7, p.moonlight_color[2] * 0.7]
    };
    let atten_scale = p.density_multiplier * p.max_y;
    let light_atten = [
        (p.blue_density[0] + p.haze_density * 0.25) * atten_scale,
        (p.blue_density[1] + p.haze_density * 0.25) * atten_scale,
        (p.blue_density[2] + p.haze_density * 0.25) * atten_scale,
    ];
    let mut combined = [
        (p.blue_density[0].abs() + p.haze_density.abs()).max(1e-6),
        (p.blue_density[1].abs() + p.haze_density.abs()).max(1e-6),
        (p.blue_density[2].abs() + p.haze_density.abs()).max(1e-6),
    ];
    let blue_weight = [p.blue_density[0] / combined[0], p.blue_density[1] / combined[1], p.blue_density[2] / combined[2]];
    let haze_weight = [p.haze_density / combined[0], p.haze_density / combined[1], p.haze_density / combined[2]];
    let off_axis = 1.0 / (1e-6f64).max(n[1].max(0.0) + p.lightnorm[1]);
    for i in 0..3 { sunlight[i] *= (-light_atten[i] * off_axis).exp(); }
    let density_dist = len * p.density_multiplier;
    for i in 0..3 { combined[i] = (-combined[i] * density_dist).exp(); }
    let mut haze_glow = (1.0 - rel_dot).max(0.001);
    haze_glow *= p.glow[0];
    haze_glow = haze_glow.powf(p.glow[2]);
    haze_glow = if p.sun_moon_glow_factor < 1.0 { 0.0 } else { p.sun_moon_glow_factor * (haze_glow + 0.25) };
    let mut color = [0.0; 3];
    for i in 0..3 {
        color[i] = p.blue_horizon[i] * blue_weight[i] * (sunlight[i] + p.ambient_color[i])
            + (p.haze_horizon * haze_weight[i]) * (sunlight[i] * haze_glow + p.ambient_color[i]);
    }
    for i in 0..3 { color[i] *= 1.0 - combined[i]; }
    let ambient = [
        p.ambient_color[0] + (1.0 - p.ambient_color[0]).max(0.0) * p.cloud_shadow * 0.5,
        p.ambient_color[1] + (1.0 - p.ambient_color[1]).max(0.0) * p.cloud_shadow * 0.5,
        p.ambient_color[2] + (1.0 - p.ambient_color[2]).max(0.0) * p.cloud_shadow * 0.5,
    ];
    let sc = (1.0 - p.cloud_shadow).max(0.0);
    for i in 0..3 { sunlight[i] *= sc; }
    let mut add_below = [0.0; 3];
    for i in 0..3 {
        add_below[i] = p.blue_horizon[i] * blue_weight[i] * (sunlight[i] + ambient[i])
            + (p.haze_horizon * haze_weight[i]) * (sunlight[i] * haze_glow + ambient[i]);
    }
    for i in 0..3 { combined[i] = combined[i].sqrt(); }
    for i in 0..3 { color[i] += (add_below[i] - color[i]) * (1.0 - combined[i].sqrt()); }
    color
}

/// 2D barycentric weights of p wrt triangle (a,b,c).
fn bary(p: [f64; 2], a: [f64; 2], b: [f64; 2], c: [f64; 2]) -> [f64; 3] {
    let d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
    let l0 = ((b[1] - c[1]) * (p[0] - c[0]) + (c[0] - b[0]) * (p[1] - c[1])) / d;
    let l1 = ((c[1] - a[1]) * (p[0] - c[0]) + (a[0] - c[0]) * (p[1] - c[1])) / d;
    [l0, l1, 1.0 - l0 - l1]
}

/// S2 ORACLE: the WindLight sky dome (sky.vert skyV port -> sky.frag SKIP_ATMOS chain ->
/// S1 tonemap) renders the faithful default-WL sky color. Submits a screen-covering sky
/// triangle with default WL params + a chosen lightnorm; the mvp is identity (screen xy =
/// pos.xy; depth_fix only touches z, and pos.z=0 -> depth 0.5 passes the reverse-Z test).
/// The read pixel's color is the barycentric interpolation of the 3 vertices' v_haze
/// (w=1 -> linear), pushed through the exact sky.frag + tonemap chain on the CPU.
#[test]
fn sky_dome_matches_windlight() {
    let Some((device, queue)) = headless() else {
        eprintln!("no Vulkan adapter; skipping headless sky test");
        return;
    };
    let fmt = wgpu::TextureFormat::Bgra8UnormSrgb;
    let mut live = LiveRenderer::new(&device, &queue, fmt);
    // Legacy classic regime (D1): tonemap curve bypassed (mix=0), legacyGamma with sky
    // gamma=1.0 (identity) so the expected = srgb(clamp(sky_hdr_color)). Exercises the sky
    // COLOR end to end; the tonemap operator variations are S1's job.
    let post = Post { exposure: 1.0, exp_scale: 1.0, tonemap_mix: 0.0, gamma: 1.0, tonemap_type: 0, legacy_gamma: 1 };
    live.set_post_params(post.exposure as f32, post.exp_scale as f32, post.tonemap_mix as f32,
        post.gamma as f32, post.tonemap_type, post.legacy_gamma);

    // Default WindLight params (llsettingssky/llsettingsvo defaults) + a mid-high sun.
    let ln = {
        let v = [0.30f64, 0.90, 0.15];
        let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
        [(v[0] / l) as f32, (v[1] / l) as f32, (v[2] / l) as f32]
    };
    let mut aux = [0.0f32; 48];
    aux[0] = 0.0; aux[1] = 0.0; aux[2] = 0.0; aux[3] = 1605.0;        // camPosLocal, max_y
    aux[4] = ln[0]; aux[5] = ln[1]; aux[6] = ln[2]; aux[7] = 1.0;      // lightnorm, sun_up=1
    aux[8] = 0.7342; aux[9] = 0.7815; aux[10] = 0.8999; aux[11] = 0.0001; // sunlight, density_mult
    aux[12] = 0.7342; aux[13] = 0.7815; aux[14] = 0.8999; aux[15] = 1.0;  // moonlight (unused), glow_factor=1
    aux[16] = 0.25; aux[17] = 0.25; aux[18] = 0.25; aux[19] = 0.7;     // ambient, haze_density
    aux[20] = 0.4954; aux[21] = 0.4954; aux[22] = 0.6399; aux[23] = 0.19; // blue_horizon, haze_horizon
    aux[24] = 0.2447; aux[25] = 0.4487; aux[26] = 0.7599; aux[27] = 0.4;  // blue_density, cloud_shadow
    aux[28] = 5.0; aux[29] = 0.001; aux[30] = -0.4799; aux[31] = 0.0;  // glow (x,y,z)
    aux[32] = 1.0;                                                     // a8.x = sky_hdr_scale (legacy=1.0)
    let sp = SkyParams::from_aux(&aux);

    // Screen-covering triangle (NDC == pos.xy since mvp=identity). Vertices intentionally
    // outside [-1,1] so the read pixel is comfortably interior.
    let verts: [[f32; 4]; 3] = [[0.0, 1.4, 0.0, 1.0], [-1.4, -1.0, 0.0, 1.0], [1.4, -1.0, 0.0, 1.0]];
    let vtx: Vec<u8> = bytemuck::cast_slice(&verts).to_vec();

    let mut d: DrawDesc = unsafe { std::mem::zeroed() };
    d.mode = 0; d.count = 3; d.typemask = 1; d.num_verts = 3;
    d.vtx_bytes = vtx.len() as u32;
    d.depth_test = 1; d.depth_write = 0;
    d.draw_class = CLASS_SKY_DOME;
    d.mvp = glam::Mat4::IDENTITY.to_cols_array();
    d.modelview = glam::Mat4::IDENTITY.to_cols_array();
    d.aux = aux;
    d.min_alpha = -1.0;

    let (w, h) = (64u32, 48u32);
    let target = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("sky-target"),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format: fmt,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = target.create_view(&wgpu::TextureViewDescriptor::default());

    device.push_error_scope(wgpu::ErrorFilter::Validation);
    live.begin();
    live.submit(&device, &queue, &d, &vtx, &[]);
    live.flush_clear(&device, &queue, &view, w, h, wgpu::Color { r: 0.0, g: 0.0, b: 0.0, a: 1.0 });
    let err = pollster::block_on(device.pop_error_scope());
    assert!(err.is_none(), "wgpu validation error in sky path: {:?}", err);

    // Read the centre pixel.
    let (cx, cy) = (w / 2, h / 2);
    let stride = ((w * 4 + 255) / 256) * 256;
    let buf = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("rb"), size: (stride * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ, mapped_at_creation: false,
    });
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
    enc.copy_texture_to_buffer(
        wgpu::ImageCopyTexture { texture: &target, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        wgpu::ImageCopyBuffer { buffer: &buf, layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(stride), rows_per_image: Some(h) } },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 });
    queue.submit([enc.finish()]);
    let slice = buf.slice(..);
    slice.map_async(wgpu::MapMode::Read, |_| {});
    device.poll(wgpu::Maintain::Wait);
    let data = slice.get_mapped_range();
    let off = (cy * stride + cx * 4) as usize;
    let got = [data[off + 2], data[off + 1], data[off]]; // R,G,B from BGRA
    drop(data);
    buf.unmap();

    // Expected: barycentric-interpolate v_haze (linear, w=1), then sky.frag chain + tonemap.
    let p_ndc = [2.0 * (cx as f64 + 0.5) / w as f64 - 1.0, 1.0 - 2.0 * (cy as f64 + 0.5) / h as f64];
    let vp: [[f64; 2]; 3] = [
        [verts[0][0] as f64, verts[0][1] as f64],
        [verts[1][0] as f64, verts[1][1] as f64],
        [verts[2][0] as f64, verts[2][1] as f64],
    ];
    let b = bary(p_ndc, vp[0], vp[1], vp[2]);
    let hz: [[f64; 3]; 3] = [
        sky_v_haze([verts[0][0] as f64, verts[0][1] as f64, 0.0], &sp),
        sky_v_haze([verts[1][0] as f64, verts[1][1] as f64, 0.0], &sp),
        sky_v_haze([verts[2][0] as f64, verts[2][1] as f64, 0.0], &sp),
    ];
    let mut v_haze = [0.0; 3];
    for i in 0..3 { v_haze[i] = b[0] * hz[0][i] + b[1] * hz[1][i] + b[2] * hz[2][i]; }
    // sky.frag: min(v_haze*2,5) -> srgb_to_linear -> * sky_hdr_scale (LINEAR HDR).
    let mut sky_hdr = [0.0; 3];
    for i in 0..3 { sky_hdr[i] = srgb_to_lin((v_haze[i] * 2.0).min(5.0)) * sp.sky_hdr_scale; }
    let exp = expected_disp(sky_hdr, post);

    // Sanity: the dome must actually render (not the black clear).
    let bright = got[0] as u32 + got[1] as u32 + got[2] as u32;
    assert!(bright > 30, "sky dome did not render (got {:?}); check the sky pass wiring", got);
    for ch in 0..3 {
        assert!(
            (got[ch] as i32 - exp[ch] as i32).abs() <= 3,
            "sky ch{}: got {} vs expected {} (v_haze {:?} sky_hdr {:?})",
            ch, got[ch], exp[ch], v_haze, sky_hdr
        );
    }
}

/// PHASE A.1 ORACLE: the fullscreen procedural sky (sky_fullscreen.frag) -- parallel-read from a
/// typed camera + EepSkyBlock, NO tap, NO dome mesh -- reproduces the WL sky. Reconstructs the
/// centre-pixel world view-ray exactly as the shader does, runs the S2 CPU skyV port on it, pushes
/// through the sky.frag chain + S1 tonemap, and asserts the rendered pixel matches. This is the
/// first "parallel reader drives the engine" proof (native-Vulkan Phase A).
#[test]
fn fullscreen_sky_matches_windlight() {
    let Some((device, queue)) = headless() else {
        eprintln!("no Vulkan adapter; skipping fullscreen-sky test");
        return;
    };
    let (w, h) = (64u32, 48u32);

    // Default WindLight params (same as the S2 dome oracle) + a mid-high sun.
    let ln = {
        let v = [0.30f64, 0.90, 0.15];
        let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
        [(v[0] / l) as f32, (v[1] / l) as f32, (v[2] / l) as f32]
    };
    let mut aux = [0.0f32; 48];
    aux[0] = 0.0; aux[1] = 0.0; aux[2] = 0.0; aux[3] = 1605.0;         // camPosLocal(=eye), max_y
    aux[4] = ln[0]; aux[5] = ln[1]; aux[6] = ln[2]; aux[7] = 1.0;       // lightnorm, sun_up=1
    aux[8] = 0.7342; aux[9] = 0.7815; aux[10] = 0.8999; aux[11] = 0.0001;
    aux[12] = 0.7342; aux[13] = 0.7815; aux[14] = 0.8999; aux[15] = 1.0;
    aux[16] = 0.25; aux[17] = 0.25; aux[18] = 0.25; aux[19] = 0.7;
    aux[20] = 0.4954; aux[21] = 0.4954; aux[22] = 0.6399; aux[23] = 0.19;
    aux[24] = 0.2447; aux[25] = 0.4487; aux[26] = 0.7599; aux[27] = 0.4;
    aux[28] = 5.0; aux[29] = 0.001; aux[30] = -0.4799; aux[31] = 0.0;
    aux[32] = 1.0;                                                     // a8.x = sky_hdr_scale (legacy)
    aux[34] = w as f32; aux[35] = h as f32;                            // a8.zw = viewport
    let sp = SkyParams::from_aux(&aux);

    // Camera looking up-and-forward so the centre pixel sees the sky. eye at origin == camPosLocal.
    let eye = glam::Vec3::ZERO;
    let dir = glam::Vec3::new(0.2, 0.7, 0.3).normalize();
    let view = glam::Mat4::look_at_rh(eye, eye + dir, glam::Vec3::Y);
    let proj = glam::Mat4::perspective_rh(60f32.to_radians(), w as f32 / h as f32, 0.1, 1.0e4);
    let inv_vp = (proj * view).inverse();

    // Pack the 52-float UBO: inv_view_proj(16, column-major) + a0..a8 (aux[0..36]).
    let mut ubo = [0.0f32; 52];
    ubo[0..16].copy_from_slice(&inv_vp.to_cols_array());
    ubo[16..52].copy_from_slice(&aux[0..36]);

    // Legacy-classic tonemap (mix=0, exposure=1, legacyGamma at gamma=1 => identity).
    let post = Post { exposure: 1.0, exp_scale: 1.0, tonemap_mix: 0.0, gamma: 1.0, tonemap_type: 0, legacy_gamma: 1 };
    let mut live = LiveRenderer::new(&device, &queue, wgpu::TextureFormat::Bgra8UnormSrgb);
    live.set_post_params(post.exposure as f32, post.exp_scale as f32, post.tonemap_mix as f32,
        post.gamma as f32, post.tonemap_type, post.legacy_gamma);
    live.set_fullscreen_sky(&ubo);
    let got = render_empty_center(&device, &queue, &mut live, [0.0, 0.0, 0.0]);

    // CPU: reconstruct the centre-pixel ray exactly as the shader, run skyV, sky.frag chain, tonemap.
    let (cx, cy) = (w / 2, h / 2);
    let ndc_x = (cx as f32 + 0.5) / w as f32 * 2.0 - 1.0;
    let ndc_y = 1.0 - (cy as f32 + 0.5) / h as f32 * 2.0;
    let wh = inv_vp * glam::Vec4::new(ndc_x, ndc_y, 0.5, 1.0);
    let wp = wh.truncate() / wh.w;
    let d = (wp - eye).normalize();
    let world_pos = eye + d * 1.0e6;
    let v_haze = sky_v_haze([world_pos.x as f64, world_pos.y as f64, world_pos.z as f64], &sp);
    let mut sky_hdr = [0.0; 3];
    for i in 0..3 { sky_hdr[i] = srgb_to_lin((v_haze[i] * 2.0).min(5.0)) * sp.sky_hdr_scale; }
    let exp = expected_disp(sky_hdr, post);

    let bright = got[0] as u32 + got[1] as u32 + got[2] as u32;
    assert!(bright > 30, "fullscreen sky did not render (got {:?})", got);
    for ch in 0..3 {
        assert!(
            (got[ch] as i32 - exp[ch] as i32).abs() <= 4,
            "fullscreen sky ch{}: got {} vs expected {} (v_haze {:?} sky_hdr {:?})",
            ch, got[ch], exp[ch], v_haze, sky_hdr
        );
    }
}

/// PHASE A.2 ORACLE: the engine derives the fullscreen-sky UBO from the TYPED scene
/// (CameraBlock + full EepSkyBlock + regime settings) via SceneFrame::fullscreen_sky_ubo(),
/// and renders a correct WL sky from it -- no hand-packed UBO, no tap. This proves the
/// "sky just works from the typed scene" path that A.2 wires in fsr_end_frame.
#[test]
fn engine_derives_fullscreen_sky_from_typed_scene() {
    let Some((device, queue)) = headless() else {
        eprintln!("no Vulkan adapter; skipping engine-derives-sky test");
        return;
    };
    let (w, h) = (64u32, 48u32);

    // Typed scene: legacy-classic regime settings + camera + FULL WindLight sky.
    let set = SettingsSnapshot {
        render_sky_sunlight_scale: 1.5,
        render_hdr_sky_sunlight_scale: 1.5,
        render_sky_ambient_scale: 1.5,
        render_sky_auto_adjust_hdr_scale: 2.0,
        render_sun_dynamic_range: 1.0,
        render_tonemap_mix: 0.7,
        render_tonemap_type: 0,
        render_exposure: 1.0,
        ..Default::default()
    };
    let eye = glam::Vec3::ZERO;
    let dir = glam::Vec3::new(0.2, 0.7, 0.3).normalize();
    let view = glam::Mat4::look_at_rh(eye, eye + dir, glam::Vec3::Y);
    let cam = CameraBlock {
        view: view.to_cols_array(),
        origin: [0.0, 0.0, 0.0],
        near: 0.1, far: 1.0e4, fov_y: 60f32.to_radians(), aspect: w as f32 / h as f32,
        viewport_w: w as f32, viewport_h: h as f32,
        ..Default::default()
    };
    let ln = {
        let v = glam::Vec3::new(0.3, 0.9, 0.15).normalize();
        [v.x, v.y, v.z]
    };
    let sky = EepSkyBlock {
        max_y: 1605.0,
        lightnorm: ln, sun_up_factor: 1.0,
        sun_color: [0.7342, 0.7815, 0.8999], density_multiplier: 0.0001,
        moon_color: [0.7342, 0.7815, 0.8999], sun_moon_glow_factor: 1.0,
        ambient: [0.25, 0.25, 0.25], haze_density: 0.7,
        blue_horizon: [0.4954, 0.4954, 0.6399], haze_horizon: 0.19,
        blue_density: [0.2447, 0.4487, 0.7599], cloud_shadow: 0.4,
        glow: [5.0, 0.001, -0.4799],
        gamma: 1.0, can_auto_adjust: 1, reflection_probe_ambiance: 0.0,
        ..Default::default()
    };
    let mut scene = SceneFrame::new();
    scene.set_settings(&set);
    scene.set_camera(&cam);
    scene.set_sky(&sky);
    let ubo = scene.fullscreen_sky_ubo().expect("camera + sky present -> UBO");
    let r = scene.sky_regime().expect("regime");

    // Render via the derived UBO + derived regime (exactly what fsr_end_frame does).
    let mut live = LiveRenderer::new(&device, &queue, wgpu::TextureFormat::Bgra8UnormSrgb);
    live.apply_sky_regime(&r);
    live.set_fullscreen_sky(&ubo);
    let got = render_empty_center(&device, &queue, &mut live, [0.0, 0.0, 0.0]);

    // CPU: use the SAME inv_view_proj + WL params the engine packed. aux[0..36] = ubo a0..a8.
    let mut aux = [0.0f32; 48];
    aux[0..36].copy_from_slice(&ubo[16..52]);
    let sp = SkyParams::from_aux(&aux);
    let inv_arr: [f32; 16] = ubo[0..16].try_into().unwrap();
    let inv_vp = glam::Mat4::from_cols_array(&inv_arr);
    let eye = glam::Vec3::new(ubo[16], ubo[17], ubo[18]);
    let (cx, cy) = (w / 2, h / 2);
    let ndc_x = (cx as f32 + 0.5) / w as f32 * 2.0 - 1.0;
    let ndc_y = 1.0 - (cy as f32 + 0.5) / h as f32 * 2.0;
    let wh = inv_vp * glam::Vec4::new(ndc_x, ndc_y, 0.5, 1.0);
    let wp = wh.truncate() / wh.w;
    let d = (wp - eye).normalize();
    let world_pos = eye + d * 1.0e6;
    let v_haze = sky_v_haze([world_pos.x as f64, world_pos.y as f64, world_pos.z as f64], &sp);
    let post = Post {
        exposure: r.exposure as f64, exp_scale: 1.0, tonemap_mix: r.tonemap_mix as f64,
        gamma: r.gamma as f64, tonemap_type: r.tonemap_type as i32, legacy_gamma: r.legacy_gamma as i32,
    };
    let mut sky_hdr = [0.0; 3];
    for i in 0..3 { sky_hdr[i] = srgb_to_lin((v_haze[i] * 2.0).min(5.0)) * sp.sky_hdr_scale; }
    let exp = expected_disp(sky_hdr, post);

    let bright = got[0] as u32 + got[1] as u32 + got[2] as u32;
    assert!(bright > 30, "engine-derived sky did not render (got {:?})", got);
    for ch in 0..3 {
        assert!(
            (got[ch] as i32 - exp[ch] as i32).abs() <= 4,
            "engine-derived sky ch{}: got {} vs expected {} (v_haze {:?})",
            ch, got[ch], exp[ch], v_haze
        );
    }
}

/// S3b CONSUMPTION ORACLE: the engine derives the sky regime (S3a) and drives the global
/// tonemap from it (apply_sky_regime -> set_post_params -> S1 tonemap). A legacy sky must
/// produce the legacy tonemap (curve bypassed, exposure 1.0, legacyGamma) end to end -- this
/// catches a wiring slip (wrong arg order into set_post_params) that the unit tests can't.
#[test]
fn apply_sky_regime_drives_tonemap() {
    let Some((device, queue)) = headless() else {
        eprintln!("no Vulkan adapter; skipping regime-consumption test");
        return;
    };
    // Legacy classic sky (no probe-ambiance key) + stock regime settings.
    let mut scene = SceneFrame::new();
    let set = SettingsSnapshot {
        render_sky_sunlight_scale: 1.5,
        render_hdr_sky_sunlight_scale: 1.5,
        render_sky_ambient_scale: 1.5,
        render_sky_auto_adjust_hdr_scale: 2.0,
        render_sun_dynamic_range: 1.0,
        render_tonemap_mix: 0.7,
        render_tonemap_type: 1,
        render_exposure: 1.0,
        ..Default::default()
    };
    scene.set_settings(&set);
    scene.set_sky(&EepSkyBlock {
        gamma: 1.0, sun_dir: [0.0, 0.0, 0.7], can_auto_adjust: 1, reflection_probe_ambiance: 0.0,
        ..Default::default()
    });
    let r = scene.sky_regime().expect("regime derived");
    assert_eq!(r.classic_mode, 1);
    assert_eq!(r.tonemap_mix, 0.0, "legacy bypasses the curve");
    assert_eq!(r.legacy_gamma, 1);

    let post = Post {
        exposure: r.exposure as f64, exp_scale: 1.0, tonemap_mix: r.tonemap_mix as f64,
        gamma: r.gamma as f64, tonemap_type: r.tonemap_type as i32, legacy_gamma: r.legacy_gamma as i32,
    };
    // Configure the renderer via apply_sky_regime (NOT set_post_params) and render a bright HDR
    // frame: with legacy mix=0 the tonemap is clamp -> sRGB (+ identity legacyGamma at gamma=1).
    let mut live = LiveRenderer::new(&device, &queue, wgpu::TextureFormat::Bgra8UnormSrgb);
    live.apply_sky_regime(&r);
    let clear = [2.0, 0.5, 0.1];
    let got = render_empty_center(&device, &queue, &mut live, clear);
    let exp = expected_disp(clear, post);
    for ch in 0..3 {
        assert!(
            (got[ch] as i32 - exp[ch] as i32).abs() <= 2,
            "regime-driven tonemap ch{}: got {} vs expected {}", ch, got[ch], exp[ch]
        );
    }
}

/// HDR S5: the dynamic exposure end-to-end (meter -> exposureF curve -> tonemap) matches stock
/// exposureF.glsl. Non-classic regime (sky_hdr_scale=2 -> bounds [0.5, 2.0]); a DIM scene must adapt
/// UP toward exp_max. Guards the fix of the inverted mix (old code mixed exp_min->exp_max, brightening
/// bright scenes, then damped exp_max to hide the blowout). CPU model: L=clamp(avg/0.175,0,1)^2,
/// dyn=mix(exp_max, exp_min, L); fold dyn into exposure and reuse the verified tonemap oracle.
#[test]
fn dynamic_exposure_matches_stock_exposureF() {
    let Some((device, queue)) = headless() else {
        eprintln!("no Vulkan adapter; skipping dynamic-exposure test");
        return;
    };
    let r = SkyRegime {
        classic_mode: 0, sky_hdr_scale: 2.0, sky_sunlight_scale: 1.5, sky_ambient_scale: 1.5,
        scene_light_strength: 2.0, tonemap_mix: 0.7, legacy_gamma: 0, exposure: 1.0,
        tonemap_type: 0, gamma: 2.2,
    };
    let clear = [0.05, 0.05, 0.05]; // dim -> avg_lum << 0.175 -> adapt toward exp_max
    let mut live = LiveRenderer::new(&device, &queue, wgpu::TextureFormat::Bgra8UnormSrgb);
    live.apply_sky_regime(&r);
    let got = render_empty_center(&device, &queue, &mut live, clear);

    let avg = 0.2126 * clear[0] + 0.7152 * clear[1] + 0.0722 * clear[2];
    let (exp_min, exp_max) = (0.5f64, 2.0f64);
    let ln = (avg / 0.175).clamp(0.0, 1.0);
    let ln = ln * ln;
    let dyn_exp = exp_max * (1.0 - ln) + exp_min * ln; // mix(exp_max, exp_min, ln)
    assert!(dyn_exp > 1.2, "a dim non-classic scene should adapt UP toward exp_max, dyn_exp={}", dyn_exp);
    // fold dyn_exp into the exposure (exp_scale=1, FS_ENGINE_EXPOSURE unset) and reuse the tonemap oracle.
    let post = Post { exposure: dyn_exp, exp_scale: 1.0, tonemap_mix: 0.7, gamma: 2.2, tonemap_type: 0, legacy_gamma: 0 };
    let exp = expected_disp(clear, post);
    for ch in 0..3 {
        assert!(
            (got[ch] as i32 - exp[ch] as i32).abs() <= 4,
            "dynamic exposure ch{}: got {} vs expected {} (dyn_exp {:.3})", ch, got[ch], exp[ch], dyn_exp
        );
    }
}

/// BLOCKER #3 draw-level verification: submit a real triangle through the generic pipeline
/// and confirm it renders WITHOUT a wgpu validation error. Uses typemask = VERTEX only, so
/// uv, color, AND the new normal all take their fallbacks -- this exercises exactly the
/// slot-3 normal_up binding + the 512-byte UBO (normal_mat) added this stage. A malformed
/// vertex layout (e.g. the normal attribute not matching) would raise a validation error
/// (caught by the scope) or fail to draw.
#[test]
fn generic_draw_exercises_normal_binding() {
    let Some((device, queue)) = headless() else {
        eprintln!("no Vulkan adapter; skipping headless draw test");
        return;
    };
    let fmt = wgpu::TextureFormat::Bgra8UnormSrgb;
    let mut live = LiveRenderer::new(&device, &queue, fmt);
    let (w, h) = (64u32, 64u32);
    let target = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("t"),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
        format: fmt,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = target.create_view(&wgpu::TextureViewDescriptor::default());

    // Opaque white triangle covering the centre. VERTEX-only SoA (3 x vec4 positions).
    let verts: [[f32; 4]; 3] = [[-0.5, -0.5, 0.0, 1.0], [0.5, -0.5, 0.0, 1.0], [0.0, 0.5, 0.0, 1.0]];
    let vtx: Vec<u8> = bytemuck::cast_slice(&verts).to_vec();

    let mut d: DrawDesc = unsafe { std::mem::zeroed() };
    d.mode = 0;            // TRIANGLES
    d.count = 3;
    d.typemask = 1;        // VERTEX only -> uv/color/normal all fall back
    d.num_verts = 3;
    d.vtx_bytes = vtx.len() as u32;
    d.depth_test = 1;
    d.depth_write = 1;
    let id = glam::Mat4::IDENTITY.to_cols_array();
    d.mvp = id;
    d.modelview = id;
    d.color = [1.0, 1.0, 1.0, 1.0];
    d.min_alpha = -1.0;    // MASK disabled

    device.push_error_scope(wgpu::ErrorFilter::Validation);
    live.begin();
    live.submit(&device, &queue, &d, &vtx, &[]);
    let n = live.flush_clear(&device, &queue, &view, w, h,
        wgpu::Color { r: 0.09, g: 0.35, b: 0.40, a: 1.0 });
    let err = pollster::block_on(device.pop_error_scope());
    assert!(err.is_none(), "wgpu validation error during generic draw: {:?}", err);
    assert_eq!(n, 1, "the triangle was queued and flushed");

    // Centre pixel is inside the triangle -> white, not the teal clear.
    let stride = ((w * 4 + 255) / 256) * 256;
    let buf = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("rb"), size: (stride * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
    enc.copy_texture_to_buffer(
        wgpu::ImageCopyTexture { texture: &target, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        wgpu::ImageCopyBuffer { buffer: &buf, layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(stride), rows_per_image: Some(h) } },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 });
    queue.submit([enc.finish()]);
    let slice = buf.slice(..);
    slice.map_async(wgpu::MapMode::Read, |_| {});
    device.poll(wgpu::Maintain::Wait);
    let data = slice.get_mapped_range();
    let off = ((h / 2) * stride + (w / 2) * 4) as usize;
    let (b8, g8, r8) = (data[off], data[off + 1], data[off + 2]);
    assert!(r8 > 200 && g8 > 200 && b8 > 200,
        "centre should be the white triangle, got BGR ({},{},{})", b8, g8, r8);
    drop(data);
    buf.unmap();
}

/// BLOCKER #4 lighting verification: submit an opaque generic quad WITH a normal (so it
/// takes the deferred G-buffer -> resolve path) and confirm the resolve applies N.L sun
/// shading. Same albedo, two normals: one facing the sun (bright), one facing away (ambient
/// only). Proves the whole chain -- normal encode via modelview, G-buffer fill, resolve
/// lighting -- end to end.
#[test]
fn deferred_resolve_applies_ndl_shading() {
    let Some((device, queue)) = headless() else {
        eprintln!("no Vulkan adapter; skipping deferred lighting test");
        return;
    };
    let fmt = wgpu::TextureFormat::Bgra8UnormSrgb;
    let mut live = LiveRenderer::new(&device, &queue, fmt);
    // Feed the resolve via the SHARED 60-float sky UBO (the P2b path the deferred resolve actually
    // binds) -- NOT the dead env_ubo. World sun_dir = +z so the sun-facing quad (world normal +z via
    // identity modelview) gets full N.L; white sun, dim ambient, non-classic (classic_mode=0). The
    // view vector (from inv_view_proj) is unused here: the fill writes glossiness 0 -> no specular.
    let mut sky_ubo = [0.0f32; 60];
    sky_ubo[0] = 1.0; sky_ubo[5] = 1.0; sky_ubo[10] = 1.0; sky_ubo[15] = 1.0; // inv_view_proj = identity
    sky_ubo[19] = 1605.0;                                       // max_y
    sky_ubo[21] = 1.0; sky_ubo[23] = 1.0;                       // lightnorm.y=1 (above_horizon=1), sun_up=1
    sky_ubo[24] = 1.0; sky_ubo[25] = 1.0; sky_ubo[26] = 1.0;    // sun_color white (density_mult 0 -> sunlight=sun_color)
    sky_ubo[32] = 0.1; sky_ubo[33] = 0.1; sky_ubo[34] = 0.1;    // ambient
    sky_ubo[47] = 1.0;                                          // sky_sunlight_scale
    sky_ubo[48] = 1.0; sky_ubo[49] = 1.0;                       // sky_hdr_scale, sky_ambient_scale
    sky_ubo[50] = 64.0; sky_ubo[51] = 64.0;                     // viewport (the 64x64 render)
    sky_ubo[54] = 1.0;                                          // sun_dir = (0,0,1) world
    live.set_fullscreen_sky(&sky_ubo);

    // SoA: 3 positions (vec4) then 3 normals (vec4-padded, read as vec3). Both buffers kept
    // alive so their distinct pointers don't alias in the geometry cache.
    let pos: [[f32; 4]; 3] = [[-0.6, -0.6, 0.0, 1.0], [0.6, -0.6, 0.0, 1.0], [0.0, 0.6, 0.0, 1.0]];
    let mk_vtx = |n: [f32; 4]| -> Vec<u8> {
        let nrm: [[f32; 4]; 3] = [n, n, n];
        let mut v: Vec<u8> = bytemuck::cast_slice(&pos).to_vec();
        v.extend_from_slice(bytemuck::cast_slice(&nrm));
        v
    };
    let bright_vtx = mk_vtx([0.0, 0.0, 1.0, 0.0]);  // faces the sun
    let dark_vtx = mk_vtx([0.0, 0.0, -1.0, 0.0]);   // faces away

    let mut render = |live: &mut LiveRenderer, vtx: &[u8]| -> [u8; 3] {
        let (w, h) = (64u32, 64u32);
        let target = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("t"),
            size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format: fmt,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = target.create_view(&wgpu::TextureViewDescriptor::default());
        let mut d: DrawDesc = unsafe { std::mem::zeroed() };
        d.mode = 0;
        d.count = 3;
        d.typemask = 1 | 2; // VERTEX | NORMAL -> is_gb (deferred)
        d.num_verts = 3;
        d.vtx_bytes = vtx.len() as u32;
        d.depth_test = 1;
        d.depth_write = 1;
        let id = glam::Mat4::IDENTITY.to_cols_array();
        d.mvp = id;
        d.modelview = id;
        d.color = [1.0, 1.0, 1.0, 1.0];
        d.min_alpha = -1.0;

        device.push_error_scope(wgpu::ErrorFilter::Validation);
        live.begin();
        live.submit(&device, &queue, &d, vtx, &[]);
        let n = live.flush_clear(&device, &queue, &view, w, h, wgpu::Color { r: 0.0, g: 0.0, b: 0.0, a: 1.0 });
        let err = pollster::block_on(device.pop_error_scope());
        assert!(err.is_none(), "wgpu validation error in deferred path: {:?}", err);
        assert_eq!(n, 1, "the quad was queued and flushed");

        let stride = ((w * 4 + 255) / 256) * 256;
        let buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("rb"), size: (stride * h) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ, mapped_at_creation: false,
        });
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        enc.copy_texture_to_buffer(
            wgpu::ImageCopyTexture { texture: &target, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            wgpu::ImageCopyBuffer { buffer: &buf, layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(stride), rows_per_image: Some(h) } },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 });
        queue.submit([enc.finish()]);
        let slice = buf.slice(..);
        slice.map_async(wgpu::MapMode::Read, |_| {});
        device.poll(wgpu::Maintain::Wait);
        let data = slice.get_mapped_range();
        let off = ((h / 2) * stride + (w / 2) * 4) as usize;
        let out = [data[off + 2], data[off + 1], data[off]]; // R,G,B from BGRA
        drop(data);
        buf.unmap();
        out
    };

    let bright = render(&mut live, &bright_vtx);
    let dark = render(&mut live, &dark_vtx);

    assert!(bright[0] > 180, "sun-facing quad should be bright, got {:?}", bright);
    assert!(dark[0] < 130, "away-facing quad should fall to ambient, got {:?}", dark);
    assert!(
        bright[0] as i32 > dark[0] as i32 + 60,
        "N.L must brighten the lit side vs the dark side: bright {:?} dark {:?}", bright, dark
    );
}

// ===== A.3 native-VK UI oracle =====

/// Pack one interleaved UI vertex: {f32 x,y,z; f32 u,v; u8 r,g,b,a} = 24 bytes (the fsr_ui_submit
/// wire format, matching shaders/ui.vert's vertex layout).
fn ui_vtx(out: &mut Vec<u8>, x: f32, y: f32, u: f32, v: f32, c: [u8; 4]) {
    out.extend_from_slice(&x.to_le_bytes());
    out.extend_from_slice(&y.to_le_bytes());
    out.extend_from_slice(&0f32.to_le_bytes());
    out.extend_from_slice(&u.to_le_bytes());
    out.extend_from_slice(&v.to_le_bytes());
    out.extend_from_slice(&c);
}

/// A.3 ORACLE: the native-VK UI pass renders honest LLRender::flush draws over the tonemapped
/// swapchain -- a TEXTURED quad (bound texture id, white vertex color -> the texture's color) and
/// a SOLID quad (tex_id 0 -> the engine's 1x1 white fallback -> the vertex color). Proves the full
/// UI path end to end: the {pos,uv,color} vertex layout, the per-draw dynamic-mvp UBO, one
/// bind-group per texture id, straight-alpha blend, and painter's order -- with ZERO glGet, pure
/// typed submit. This is the mechanism that makes native-vulkan a usable viewer (real menus/text).
#[test]
fn ui_pass_renders_textured_and_solid_quads() {
    let Some((device, queue)) = headless() else {
        eprintln!("no Vulkan adapter; skipping headless UI test");
        return;
    };
    let fmt = wgpu::TextureFormat::Bgra8UnormSrgb;
    let mut live = LiveRenderer::new(&device, &queue, fmt);

    // A 2x2 solid-red texture (id 42); straight RGBA bytes.
    let red_tex = [255u8, 0, 0, 255].repeat(4);
    live.upload_texture(&device, &queue, 42, 2, 2, &red_tex);

    // Identity mvp -> positions are already NDC. Left half [-1,0] = textured (red), right half
    // [0,1] = solid green. TRIANGLES (mode 0), two tris each. Textured quad uses WHITE vertex
    // color so out = texture (red); solid quad uses GREEN with tex_id 0 (white fallback -> green).
    let identity: [f32; 16] = [1.,0.,0.,0., 0.,1.,0.,0., 0.,0.,1.,0., 0.,0.,0.,1.];
    let white = [255u8, 255, 255, 255];
    let green = [0u8, 255, 0, 255];
    let mut left = Vec::new();
    for &(x, y, u, v) in &[(-1f32,-1f32,0f32,1f32), (0.,-1.,1.,1.), (0.,1.,1.,0.),
                           (-1.,-1.,0.,1.), (0.,1.,1.,0.), (-1.,1.,0.,0.)] {
        ui_vtx(&mut left, x, y, u, v, white);
    }
    let mut right = Vec::new();
    for &(x, y, u, v) in &[(0f32,-1f32,0f32,1f32), (1.,-1.,1.,1.), (1.,1.,1.,0.),
                           (0.,-1.,0.,1.), (1.,1.,1.,0.), (0.,1.,0.,0.)] {
        ui_vtx(&mut right, x, y, u, v, green);
    }

    live.begin();
    live.ui_begin();
    live.ui_submit(&identity, 42, 0, &left);  // textured red (left)
    live.ui_submit(&identity, 0, 0, &right);  // solid green (right, white fallback)

    let (w, h) = (64u32, 48u32);
    let target = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("ui-target"),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: fmt,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = target.create_view(&wgpu::TextureViewDescriptor::default());
    live.flush_clear(&device, &queue, &view, w, h, wgpu::Color { r: 0., g: 0., b: 0., a: 1. });

    let stride = ((w * 4 + 255) / 256) * 256;
    let buf = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("ui-rb"), size: (stride * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ, mapped_at_creation: false,
    });
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("ui-rb") });
    enc.copy_texture_to_buffer(
        wgpu::ImageCopyTexture { texture: &target, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        wgpu::ImageCopyBuffer { buffer: &buf, layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(stride), rows_per_image: Some(h) } },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
    queue.submit([enc.finish()]);
    let slice = buf.slice(..);
    slice.map_async(wgpu::MapMode::Read, |_| {});
    device.poll(wgpu::Maintain::Wait);
    let data = slice.get_mapped_range();
    // Bgra8UnormSrgb byte order: B, G, R, A. Sample left-quarter and right-quarter, mid-height.
    let px = |cx: u32, cy: u32| -> [u8; 3] {
        let off = (cy * stride + cx * 4) as usize;
        [data[off + 2], data[off + 1], data[off]] // R, G, B
    };
    let lft = px(w / 4, h / 2);
    let rgt = px(3 * w / 4, h / 2);
    drop(data);
    buf.unmap();

    assert!(lft[0] > 200 && lft[1] < 60 && lft[2] < 60,
        "textured-quad pixel should be red (texture color through white vertex), got {:?}", lft);
    assert!(rgt[1] > 200 && rgt[0] < 60 && rgt[2] < 60,
        "solid-quad pixel should be green (vertex color through white-texture fallback), got {:?}", rgt);
}
