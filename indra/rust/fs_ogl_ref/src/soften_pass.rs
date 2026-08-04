//! The real OGL softenLight (atmospherics/deferred-lighting) pass, headless. Feeds a fixture:
//! - a flat G-buffer (a known albedo + up-normal + HAS_ATMOS flag) so we're lighting a plain surface,
//! - NEUTRAL shadow/AO/probe inputs (shadow=lit, ao=full, probes=black) -- isolates the ATMOSPHERICS
//!   look, which is the divergence we're chasing,
//! - the SoftenFrameBlock UBO filled from a fixture EEP sky (std140.rs), + a zeroed ReflectionProbes UBO.
//! Renders into a linear-HDR scene target; the OGL present (tonemap/gamma) runs after (added next).
//!
//! Bindings (from the emitted transform): 0 = SoftenFrameBlock, 1..41 = 20 samplers, 35 = ReflectionProbes.

use crate::std140::Block;
use wgpu::util::DeviceExt;

/// A fixture EEP sky (a plain daytime WindLight sky, the LLSettingsSky defaults from the stratigraphy).
/// Fills the atmospherics members of SoftenFrameBlock; everything else stays zero/neutral.
pub fn fixture_sky_block() -> Block {
    let mut b = Block::new();
    // Atmospherics (WindLight defaults, llsettingssky.cpp:900-919,1227-1233):
    b.vec3("blue_horizon", [0.4954, 0.4954, 0.6399])
        .vec3("blue_density", [0.2447, 0.4487, 0.7599])
        .f32("haze_horizon", 0.19)
        .f32("haze_density", 0.7)
        .f32("cloud_shadow", 0.2699)
        .f32("density_multiplier", 0.0001)
        .f32("distance_multiplier", 0.8)
        .f32("max_y", 1605.0)
        .vec3("ambient_color", [0.25, 0.25, 0.25])
        .vec3("sunlight_color", [0.7342, 0.7815, 0.8999])
        .vec3("moonlight_color", [0.0, 0.0, 0.0])
        .vec3("glow", [5.0, 0.001, -0.4799])
        .f32("sun_moon_glow_factor", 1.0)
        // sun at ~45deg (lightnorm is OGL Y-up); sun_dir SL Z-up.
        .vec3("lightnorm", [0.0, 0.707, 0.707])
        .vec3("sun_dir", [0.0, 0.707, 0.707])
        .vec3("moon_dir", [0.0, -0.707, -0.707])
        .i32("sun_up_factor", 1)
        // HDR/classic regime: a legacy sky (no probe ambiance) -> classic_mode=1, sky_hdr_scale=1.
        .i32("classic_mode", 1)
        .f32("sky_hdr_scale", 1.0)
        .f32("sky_sunlight_scale", 1.5)
        .f32("sky_ambient_scale", 1.5)
        .f32("scene_light_strength", 2.0)
        // screen res (the fullscreen target).
        .vec2("screen_res", [256.0, 256.0])
        // neutral shadows/ssao (no darkening) in case sampled.
        .f32("shadow_bias", 0.0)
        .f32("ssao_irradiance_scale", 1.0)
        .f32("ssao_irradiance_max", 1.0);
    b
}

/// A 1x1 texture2D of a given format + RGBA8/float bytes, RENDER-neutral for the fixture.
fn tex_1x1(device: &wgpu::Device, queue: &wgpu::Queue, format: wgpu::TextureFormat, data: &[u8], label: &str) -> wgpu::TextureView {
    let t = device.create_texture_with_data(
        queue,
        &wgpu::TextureDescriptor {
            label: Some(label),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        },
        wgpu::util::TextureDataOrder::LayerMajor,
        data,
    );
    t.create_view(&wgpu::TextureViewDescriptor::default())
}

/// A 1x1 depth texture (for the shadow-comparison samplers). Cleared to 1.0 (far) = nothing occludes.
fn depth_1x1(device: &wgpu::Device, queue: &wgpu::Queue, label: &str) -> wgpu::TextureView {
    // Depth32Float can't be create_texture_with_data; render-clear it.
    let t = device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Depth32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let view = t.create_view(&wgpu::TextureViewDescriptor::default());
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("depth-clear") });
    enc.begin_render_pass(&wgpu::RenderPassDescriptor {
        label: Some("depth-clear"),
        color_attachments: &[],
        depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
            view: &view,
            depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Store }),
            stencil_ops: None,
        }),
        timestamp_writes: None,
        occlusion_query_set: None,
    });
    queue.submit([enc.finish()]);
    view
}

/// A 1x1 cube-array (6 faces, 1 cube) of black -- neutral reflection/irradiance probe.
fn cube_array_1x1(device: &wgpu::Device, queue: &wgpu::Queue, label: &str) -> wgpu::TextureView {
    let t = device.create_texture_with_data(
        queue,
        &wgpu::TextureDescriptor {
            label: Some(label),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 6 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba16Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        },
        wgpu::util::TextureDataOrder::LayerMajor,
        &[0u8; 2 * 4 * 6], // 6 faces x rgba16f (8 bytes) = but 1px each -> 8*6=48 bytes
    );
    t.create_view(&wgpu::TextureViewDescriptor {
        dimension: Some(wgpu::TextureViewDimension::CubeArray),
        ..Default::default()
    })
}

// f16 helpers for the Rgba16Float G-buffer/probe fixtures.
fn f16(x: f32) -> [u8; 2] {
    // minimal f32->f16 (round-to-nearest-even not needed for fixtures).
    let bits = x.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exp = ((bits >> 23) & 0xff) as i32 - 127 + 15;
    let mant = (bits >> 13) & 0x3ff;
    let h = if exp <= 0 { sign } else if exp >= 31 { sign | 0x7c00 } else { sign | ((exp as u16) << 10) | mant as u16 };
    h.to_le_bytes()
}
fn rgba16f(r: f32, g: f32, b: f32, a: f32) -> Vec<u8> {
    [f16(r), f16(g), f16(b), f16(a)].concat()
}

pub const SCENE_HDR: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;
