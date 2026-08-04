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

pub mod shaders; // assemble+transform+compile the REAL OGL shaders (softenLight, ...) to SPIR-V
pub mod std140; // SoftenFrameBlock std140 layout builder (offsets computed, not hand-placed)
pub mod soften_pass; // the real softenLight atmospherics pass fixture (G-buffer/shadow/probe neutrals)
pub mod resolve_pass; // the TEST side: fs_render's resolve.frag on the equivalent fixture (A/B bench)

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
    last_center_lum: std::cell::Cell<f64>, // center-pixel linear luminance of the last readback_hdr
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
        // SPIRV_SHADER_PASSTHROUGH: the real OGL SPIR-V goes straight to the driver (naga PANICs on it,
        // and re-translation is exactly what we don't want -- the thesis is the shaders run on the real
        // Vulkan driver). Request it if the adapter has it (AMD does).
        // SPIR-V passthrough (run the real shaders) + float32-filterable (softenLight samples the R32F
        // depth/misc stand-ins as filterable Float, matching GL where depth textures filter freely).
        let want = wgpu::Features::SPIRV_SHADER_PASSTHROUGH | wgpu::Features::FLOAT32_FILTERABLE;
        let features = want & adapter.features();
        // softenLight binds 20 sampled textures + comparison/cube samplers in one fragment stage; the
        // default limit (16 sampled textures) rejects it. Take the adapter's own maxima -- the real GL
        // driver has no such 16-texture ceiling, so matching hardware limits is the faithful choice.
        let (device, queue) = pollster::block_on(adapter.request_device(
            &wgpu::DeviceDescriptor {
                label: Some("fs_ogl_ref"),
                required_features: features,
                required_limits: adapter.limits(),
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
            last_center_lum: std::cell::Cell::new(0.0),
        })
    }

    /// Center-pixel linear luminance from the last readback_hdr (head-on view -> isolates the
    /// view-vector-independent diffuse math from the off-center Fresnel/specular sweep).
    pub fn last_center_lum(&self) -> f64 { self.last_center_lum.get() }

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

    /// Build the REAL softenLight fragment shader module (assemble the viewer's way -> UBO/sampler
    /// transform -> Vulkan SPIR-V -> wgpu module via passthrough). Returns (module, n_ubo_members,
    /// n_samplers). This is the atmospherics pass's fragment; the vertex is a fullscreen triangle.
    /// The UBO/texture feed + pass wiring build on this. Panics on a shaderc error (compile is proven).
    pub fn build_soften_module(&self) -> (wgpu::ShaderModule, usize, usize) {
        let (spv, members, samplers) = shaders::build_softenlight_ubo_spirv()
            .unwrap_or_else(|e| panic!("softenLight assemble/transform/compile failed:\n{e}"));
        // Real engine SPIR-V -> passthrough (naga PANICs on it). Requires SPIRV_SHADER_PASSTHROUGH.
        let module = unsafe {
            self.device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV {
                label: Some("softenLightF"),
                source: std::borrow::Cow::Borrowed(&spv),
            })
        };
        log::info!(
            "fs_ogl_ref: softenLight module OK -- {} UBO members, {} samplers, {} SPIR-V words",
            members, samplers, spv.len()
        );
        (module, members, samplers)
    }

    /// Render the REAL softenLight on the fixture + read the linear-HDR scene back to a viewable PNG
    /// (clamp+sRGB). The atmospherics ORACLE frame; also logs the mean linear luminance (>1 pre-clamp
    /// means the lit ground would blow out under a classic tonemap -- the magnitude signal).
    pub fn soften_frame(&self, out: &str, classic: bool) -> f64 {
        let (module, _, _) = self.build_soften_module();
        let r = soften_pass::render(&self.device, &self.queue, &module, self.width, classic);
        let regime = if classic { "classic" } else { "pbr" };
        let mean_lum = self.readback_hdr(&r.target, out);
        println!("softenLight oracle [{regime}]: mean linear luminance = {:.4}", mean_lum);
        mean_lum
    }

    /// The TEST side: run fs_render's resolve.frag on the equivalent fixture, same regime, and report
    /// its mean linear luminance -> directly comparable to soften_frame() (the oracle).
    pub fn resolve_frame(&self, out: &str, classic: bool) -> f64 {
        let r = resolve_pass::render(&self.device, &self.queue, self.width, classic);
        let regime = if classic { "classic" } else { "pbr" };
        let mean_lum = self.readback_hdr(&r.target, out);
        println!("resolve.frag      [{regime}]: mean linear luminance = {:.4}", mean_lum);
        mean_lum
    }

    /// Read an Rgba16Float target back, write a clamp+sRGB PNG, and return mean LINEAR luminance
    /// (>1 pre-clamp = would blow out under a classic tonemap). Shared by both A/B sides so the
    /// magnitude signal is measured identically.
    fn readback_hdr(&self, target: &wgpu::Texture, out: &str) -> f64 {
        let (w, h) = (self.width, self.height);
        let stride = (((w * 8) + 255) / 256) * 256; // rgba16f = 8 bytes/px
        let buf = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("hdr-grab"),
            size: (stride * h) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc = self.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("hdr-grab") });
        enc.copy_texture_to_buffer(
            wgpu::ImageCopyTexture { texture: target, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            wgpu::ImageCopyBuffer { buffer: &buf, layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(stride), rows_per_image: Some(h) } },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        self.queue.submit([enc.finish()]);
        let slice = buf.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |x| { let _ = tx.send(x); });
        self.device.poll(wgpu::Maintain::Wait);
        if !matches!(rx.recv(), Ok(Ok(()))) { return -1.0; }
        let data = slice.get_mapped_range();
        let mut rgba = vec![0u8; (w * h * 4) as usize];
        let mut lum_sum = 0f64;
        let mut center_lum = 0f64;
        for y in 0..h {
            for x in 0..w {
                let o = (y * stride + x * 8) as usize;
                let lr = f16_to_f32(u16::from_le_bytes([data[o], data[o + 1]]));
                let lg = f16_to_f32(u16::from_le_bytes([data[o + 2], data[o + 3]]));
                let lb = f16_to_f32(u16::from_le_bytes([data[o + 4], data[o + 5]]));
                let l = (0.2126 * lr + 0.7152 * lg + 0.0722 * lb) as f64;
                lum_sum += l;
                if x == w / 2 && y == h / 2 { center_lum = l; } // head-on view: F~=f0, isolates diffuse math
                let d = ((y * w + x) * 4) as usize;
                rgba[d] = lin_to_srgb8(lr);
                rgba[d + 1] = lin_to_srgb8(lg);
                rgba[d + 2] = lin_to_srgb8(lb);
                rgba[d + 3] = 255;
            }
        }
        drop(data);
        buf.unmap();
        self.last_center_lum.set(center_lum);
        if let Ok(f) = std::fs::File::create(out) {
            let mut e = png::Encoder::new(std::io::BufWriter::new(f), w, h);
            e.set_color(png::ColorType::Rgba);
            e.set_depth(png::BitDepth::Eight);
            if let Ok(mut wr) = e.write_header() { let _ = wr.write_image_data(&rgba); }
        }
        lum_sum / (w * h) as f64
    }
}

fn f16_to_f32(h: u16) -> f32 {
    let sign = ((h >> 15) & 1) as u32;
    let exp = ((h >> 10) & 0x1f) as u32;
    let mant = (h & 0x3ff) as u32;
    let bits = if exp == 0 {
        if mant == 0 { sign << 31 } else {
            // subnormal
            let mut e = -1i32;
            let mut m = mant;
            while (m & 0x400) == 0 { m <<= 1; e -= 1; }
            m &= 0x3ff;
            (sign << 31) | (((e + 127 - 15) as u32) << 23) | (m << 13)
        }
    } else if exp == 0x1f {
        (sign << 31) | (0xff << 23) | (mant << 13)
    } else {
        (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13)
    };
    f32::from_bits(bits)
}

fn lin_to_srgb8(c: f32) -> u8 {
    let c = c.clamp(0.0, 1.0);
    let s = if c <= 0.0031308 { c * 12.92 } else { 1.055 * c.powf(1.0 / 2.4) - 0.055 };
    (s * 255.0 + 0.5) as u8
}
