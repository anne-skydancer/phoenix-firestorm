//! fs_ogl_ref -- the reference ORACLE for the apples-to-apples OGL<->VLK verify harness.
//!
//! A headless wgpu engine that faithfully REPRODUCES the SL OpenGL pipeline. It runs the REAL OGL
//! shaders wired into the real pass structure (from RENDER_STRATIGRAPHY.md), so a pixel diff against
//! `fs_render` (the clean port) is ground truth, not a reinterpretation. Both engines are wgpu ->
//! same rasterizer / float behavior -> any difference is a real port divergence.
//!
//! Step 1 (skeleton): headless init + an offscreen present target + a clear pass + PNG readback,
//! deliberately mirroring `fs_render`'s headless shape so the trivial baseline diffs identical. Real
//! passes (atmospherics, HDR, sky/clouds/water, ...) are absorbed into `frame()` step by step.

/// Present format -- matches `fs_render`'s headless target exactly (apples-to-apples): sRGB so the
/// tonemap output encodes identically, and readback/PNG-friendly.
pub const FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8UnormSrgb;

pub struct RefEngine {
    device: wgpu::Device,
    queue: wgpu::Queue,
    target: wgpu::Texture,
    width: u32,
    height: u32,
    clear: wgpu::Color,
}

impl RefEngine {
    /// Headless init: a Vulkan device with NO surface + a persistent offscreen present target.
    /// Returns `None` if no Vulkan adapter/device is available.
    pub fn new(width: u32, height: u32) -> Option<RefEngine> {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN,
            ..Default::default()
        });
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: None, // headless
            force_fallback_adapter: false,
        }))?;
        let info = adapter.get_info();
        log::info!("fs_ogl_ref: adapter {} | {:?} | {} {}", info.name, info.backend, info.driver, info.driver_info);
        let (device, queue) = pollster::block_on(adapter.request_device(
            &wgpu::DeviceDescriptor {
                label: Some("fs_ogl_ref"),
                required_features: wgpu::Features::empty(),
                required_limits: wgpu::Limits::default(),
            },
            None,
        ))
        .ok()?;
        let (width, height) = (width.max(1), height.max(1));
        let target = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("fs_ogl_ref-target"),
            size: wgpu::Extent3d { width, height, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        Some(RefEngine {
            device,
            queue,
            target,
            width,
            height,
            clear: wgpu::Color { r: 0.0, g: 0.0, b: 0.0, a: 1.0 },
        })
    }

    /// The initial framebuffer clear (the OGL pipeline clears the deferred G-buffer / screen each
    /// frame). Set to match `fs_render`'s clear color for the skeleton baseline.
    pub fn set_clear_color(&mut self, r: f64, g: f64, b: f64, a: f64) {
        self.clear = wgpu::Color { r, g, b, a };
    }

    /// Render one frame. Step 1 (skeleton): a clear pass into the offscreen target. Real OGL passes
    /// (sky dome, softenLight, tonemap, ...) get added here per step, in the stratigraphy's order.
    pub fn frame(&mut self) {
        let view = self.target.create_view(&wgpu::TextureViewDescriptor::default());
        let mut enc = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("fs_ogl_ref") });
        {
            let _rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("ref-clear"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Clear(self.clear), store: wgpu::StoreOp::Store },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
        }
        self.queue.submit([enc.finish()]);
    }

    /// Read the offscreen target back to a PNG. Encoding mirrors `fs_render::write_capture` so the
    /// two engines' PNGs are directly comparable.
    pub fn grab(&self, out: &str) {
        let (w, h) = (self.width, self.height);
        let stride = ((w * 4 + 255) / 256) * 256;
        let buf = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("ref-grab"),
            size: (stride * h) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("ref-grab") });
        enc.copy_texture_to_buffer(
            wgpu::ImageCopyTexture { texture: &self.target, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            wgpu::ImageCopyBuffer {
                buffer: &buf,
                layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(stride), rows_per_image: Some(h) },
            },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        self.queue.submit([enc.finish()]);
        // Map + un-pad the row stride -> tight RGBA8 -> PNG (FORMAT is RGBA, no BGRA swap needed).
        let slice = buf.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
        self.device.poll(wgpu::Maintain::Wait);
        if !matches!(rx.recv(), Ok(Ok(()))) {
            return;
        }
        let data = slice.get_mapped_range();
        let mut rgba = vec![0u8; (w * h * 4) as usize];
        for row in 0..h {
            let src = &data[(row * stride) as usize..(row * stride + w * 4) as usize];
            let dst = &mut rgba[(row * w * 4) as usize..((row + 1) * w * 4) as usize];
            dst.copy_from_slice(src);
        }
        drop(data);
        buf.unmap();
        if let Ok(f) = std::fs::File::create(out) {
            let mut penc = png::Encoder::new(std::io::BufWriter::new(f), w, h);
            penc.set_color(png::ColorType::Rgba);
            penc.set_depth(png::BitDepth::Eight);
            if let Ok(mut wr) = penc.write_header() {
                let _ = wr.write_image_data(&rgba);
            }
        }
        log::info!("fs_ogl_ref: frame captured to {}", out);
    }
}
