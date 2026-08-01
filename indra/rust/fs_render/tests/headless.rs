//! Headless diagnostics for the live-bridge engine.
//!
//! Drives the REAL `LiveRenderer` on an offscreen (surfaceless) Vulkan device and reads
//! pixels back, so each deferred-pipeline increment can be verified deterministically
//! without launching the viewer. The viewer is reserved for human eyeball sign-off at the
//! visible milestones.
//!
//! Requires a Vulkan adapter; if none is present the tests no-op (skip) rather than fail,
//! so CI on a headless box without Vulkan does not spuriously break.

use fs_render::live::{DrawDesc, LiveRenderer};

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

/// sRGB OETF (linear -> encoded), matching what the swapchain ROP applies on write.
fn srgb_encode(u: f64) -> u8 {
    let e = if u <= 0.0031308 { 12.92 * u } else { 1.055 * u.powf(1.0 / 2.4) - 0.055 };
    (e * 255.0).round().clamp(0.0, 255.0) as u8
}

/// Render one empty frame through flush_clear into an offscreen sRGB target and read back
/// the centre pixel. Increment 1a routes the clear through scene_hdr (linear) and composites
/// it out via the texelFetch copy; the result must equal sRGB(clear) -- proving the two-pass
/// restructure is a pixel-identity and did not disturb the presented image.
#[test]
fn scene_hdr_copy_is_identity() {
    let Some((device, queue)) = headless() else {
        eprintln!("no Vulkan adapter; skipping headless test");
        return;
    };
    let fmt = wgpu::TextureFormat::Bgra8UnormSrgb; // the swapchain format the engine sees
    let mut live = LiveRenderer::new(&device, &queue, fmt);
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

    let clear = wgpu::Color { r: 0.09, g: 0.35, b: 0.40, a: 1.0 };
    live.begin();
    let n = live.flush_clear(&device, &queue, &view, w, h, clear);
    assert_eq!(n, 0, "no draws were submitted");

    // Read the target back.
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
            layout: wgpu::ImageDataLayout {
                offset: 0,
                bytes_per_row: Some(stride),
                rows_per_image: Some(h),
            },
        },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
    queue.submit([enc.finish()]);

    let slice = buf.slice(..);
    slice.map_async(wgpu::MapMode::Read, |_| {});
    device.poll(wgpu::Maintain::Wait);
    let data = slice.get_mapped_range();

    let (cx, cy) = (w / 2, h / 2);
    let off = (cy * stride + cx * 4) as usize;
    // Bgra8UnormSrgb byte order: B, G, R, A
    let (b8, g8, r8) = (data[off], data[off + 1], data[off + 2]);

    let (er, eg, eb) = (srgb_encode(0.09), srgb_encode(0.35), srgb_encode(0.40));
    // +-2 for float->8bit rounding across the two encode paths.
    assert!((r8 as i32 - er as i32).abs() <= 2, "R {} vs expected {}", r8, er);
    assert!((g8 as i32 - eg as i32).abs() <= 2, "G {} vs expected {}", g8, eg);
    assert!((b8 as i32 - eb as i32).abs() <= 2, "B {} vs expected {}", b8, eb);

    drop(data);
    buf.unmap();
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
