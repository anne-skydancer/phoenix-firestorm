//! fs_render — the live-bridge renderer DLL (PHASE3_PLAN.md P3a).
//!
//! C ABI consumed by the viewer in engine mode (FS_RENDER_ENGINE=1): the viewer hands
//! over its HWND, then streams per-frame draws through `fsr_submit` (the same data the
//! P2b scene dump captured — SoA-block vertex bytes, typemask, indices, program name,
//! matrices). P3a scope: init on the HWND + clear-color present + the unlit-textured
//! draw path (the replay renderer, live). Handle/POD/no-panic-across-boundary per the
//! llrust FFI house pattern.

mod live;

use std::ffi::c_void;
use live::{DrawDesc, LiveRenderer};
use std::sync::Mutex;

use glam::Mat4;
use raw_window_handle::{RawDisplayHandle, RawWindowHandle, Win32WindowHandle, WindowsDisplayHandle};

struct Engine {
    _instance: wgpu::Instance,
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    frames: u64,
    live: LiveRenderer,
    frame_open: bool,
    adapter_desc: String,
}

static ENGINE: Mutex<Option<Engine>> = Mutex::new(None);

fn init_logging() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        let _ = env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"))
            .try_init();
    });
}

/// Initialize the engine on the viewer's window. Returns 1 on success, 0 on failure.
/// # Safety: `hwnd` must be a valid Win32 window handle owned by the caller for the
/// lifetime of the engine (until fsr_shutdown).
#[no_mangle]
pub unsafe extern "C" fn fsr_init(hwnd: *mut c_void, width: u32, height: u32) -> i32 {
    init_logging();
    let result = std::panic::catch_unwind(|| -> Option<Engine> {
        let hwnd_nz = std::num::NonZeroIsize::new(hwnd as isize)?;
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN,
            ..Default::default()
        });
        let mut wh = Win32WindowHandle::new(hwnd_nz);
        wh.hinstance = None;
        let surface = instance
            .create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
                raw_display_handle: RawDisplayHandle::Windows(WindowsDisplayHandle::new()),
                raw_window_handle: RawWindowHandle::Win32(wh),
            })
            .ok()?;
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: Some(&surface),
            force_fallback_adapter: false,
        }))?;
        let info = adapter.get_info();
        log::info!(
            "fs_render: adapter {} | {:?} | {} {}",
            info.name, info.backend, info.driver, info.driver_info
        );
        let (device, queue) = pollster::block_on(adapter.request_device(
            &wgpu::DeviceDescriptor {
                label: Some("fs_render"),
                required_features: wgpu::Features::empty(),
                required_limits: wgpu::Limits::default(),
            },
            None,
        ))
        .ok()?;
        let caps = surface.get_capabilities(&adapter);
        let format = caps
            .formats
            .iter()
            .copied()
            .find(|f| f.is_srgb())
            .unwrap_or(caps.formats[0]);
        let config = wgpu::SurfaceConfiguration {
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC, // frame grabber
            format,
            width: width.max(1),
            height: height.max(1),
            // FIFO = vsync. The first-listed mode is often IMMEDIATE: the viewer's main
            // loop then spins flat-out (~120fps of no-op-GL scene traversal), saturating
            // cores and starving other apps + its own decode threads -- the residual
            // stutter after the upload fix. Fifo is guaranteed present on Vulkan.
            present_mode: wgpu::PresentMode::Fifo,
            alpha_mode: caps.alpha_modes[0],
            view_formats: vec![],
            desired_maximum_frame_latency: 2,
        };
        surface.configure(&device, &config);
        log::info!("fs_render: surface {}x{} {:?}", config.width, config.height, format);
        let live = LiveRenderer::new(&device, &queue, config.format);
        Some(Engine {
            _instance: instance,
            surface,
            device,
            queue,
            config,
            frames: 0,
            live,
            frame_open: false,
            adapter_desc: format!("{} ({} {})", info.name, info.driver, info.driver_info),
        })
    });
    match result {
        Ok(Some(e)) => {
            *ENGINE.lock().unwrap() = Some(e);
            1
        }
        _ => {
            log::error!("fsr_init failed");
            0
        }
    }
}

/// Resize the swapchain (viewer window resized).
#[no_mangle]
pub extern "C" fn fsr_resize(width: u32, height: u32) {
    if let Some(e) = ENGINE.lock().unwrap().as_mut() {
        if width > 0 && height > 0 {
            e.config.width = width;
            e.config.height = height;
            e.surface.configure(&e.device, &e.config);
        }
    }
}

/// Present one frame: P3a = clear color (deep teal so engine mode is unmistakable on
/// screen) + frame counter. The live draw path lands here next (P3b/P3c).
#[no_mangle]
pub extern "C" fn fsr_frame() -> i32 {
    let mut guard = ENGINE.lock().unwrap();
    let Some(e) = guard.as_mut() else { return 0 };
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let frame = match e.surface.get_current_texture() {
            Ok(f) => f,
            Err(wgpu::SurfaceError::Lost | wgpu::SurfaceError::Outdated) => {
                e.surface.configure(&e.device, &e.config);
                match e.surface.get_current_texture() {
                    Ok(f) => f,
                    Err(_) => return 0,
                }
            }
            Err(_) => return 0,
        };
        let view = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        let mut enc = e
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("fsr") });
        {
            let t = (e.frames % 240) as f64 / 240.0;
            let pulse = 0.08 + 0.04 * (t * std::f64::consts::TAU).sin();
            let _rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("fsr-clear"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color {
                            r: pulse,
                            g: 0.35,
                            b: 0.4,
                            a: 1.0,
                        }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
        }
        e.queue.submit([enc.finish()]);
        frame.present();
        e.frames += 1;
        1
    }));
    result.unwrap_or(0)
}

const CAPTURE_REQ: &str = "C:\\fs\\fsr_capture.req";
const CAPTURE_OUT: &str = "C:\\fs\\fsr_frame.png";

fn write_capture(device: &wgpu::Device, buf: &wgpu::Buffer, stride: u32, w: u32, h: u32, fmt: wgpu::TextureFormat) {
    let slice = buf.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| {
        let _ = tx.send(r);
    });
    device.poll(wgpu::Maintain::Wait);
    if !matches!(rx.recv(), Ok(Ok(()))) {
        return;
    }
    let data = slice.get_mapped_range();
    let bgra = matches!(
        fmt,
        wgpu::TextureFormat::Bgra8Unorm | wgpu::TextureFormat::Bgra8UnormSrgb
    );
    let mut rgba = vec![0u8; (w * h * 4) as usize];
    for row in 0..h {
        let src = &data[(row * stride) as usize..(row * stride + w * 4) as usize];
        let dst = &mut rgba[(row * w * 4) as usize..((row + 1) * w * 4) as usize];
        if bgra {
            for px in 0..w as usize {
                dst[px * 4] = src[px * 4 + 2];
                dst[px * 4 + 1] = src[px * 4 + 1];
                dst[px * 4 + 2] = src[px * 4];
                dst[px * 4 + 3] = 255;
            }
        } else {
            dst.copy_from_slice(src);
        }
    }
    drop(data);
    buf.unmap();
    if let Ok(f) = std::fs::File::create(CAPTURE_OUT) {
        let mut enc = png::Encoder::new(std::io::BufWriter::new(f), w, h);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        if let Ok(mut wr) = enc.write_header() {
            let _ = wr.write_image_data(&rgba);
        }
    }
    log::info!("fs_render: frame captured to {}", CAPTURE_OUT);
}

/// Frames presented so far (test/telemetry).
#[no_mangle]
pub extern "C" fn fsr_frame_count() -> u64 {
    ENGINE.lock().unwrap().as_ref().map(|e| e.frames).unwrap_or(0)
}

/// Tear down the engine and release the surface.
#[no_mangle]
pub extern "C" fn fsr_shutdown() {
    *ENGINE.lock().unwrap() = None;
    log::info!("fs_render: shutdown");
}

// ==========================  P3c: the live draw stream  =========================

/// Begin a frame: clears the queue. Viewer calls this once per frame before draws.
#[no_mangle]
pub extern "C" fn fsr_begin_frame() -> i32 {
    let mut g = ENGINE.lock().unwrap();
    let Some(e) = g.as_mut() else { return 0 };
    e.live.begin();
    e.frame_open = true;
    1
}

/// Submit one draw. `desc` is a DrawDesc; `vtx`/`idx` point at the viewer's CPU shadow
/// (copied here -- the viewer reuses those buffers next frame).
/// # Safety: pointers must be valid for the byte counts named in `desc`.
#[no_mangle]
pub unsafe extern "C" fn fsr_submit(desc: *const DrawDesc, vtx: *const u8, idx: *const u8) -> i32 {
    if desc.is_null() {
        return 0;
    }
    let d = *desc;
    let mut g = ENGINE.lock().unwrap();
    let Some(e) = g.as_mut() else { return 0 };
    if !e.frame_open {
        return 0;
    }
    let vslice = if vtx.is_null() || d.vtx_bytes == 0 { &[][..] } else { std::slice::from_raw_parts(vtx, d.vtx_bytes as usize) };
    let islice = if idx.is_null() || d.idx_bytes == 0 { &[][..] } else { std::slice::from_raw_parts(idx, d.idx_bytes as usize) };
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let Engine { device, queue, live, .. } = e;
        live.submit(device, queue, &d, vslice, islice);
    }));
    if r.is_err() { 0 } else { 1 }
}

/// Mirror one decoded texture into the engine (viewer calls at upload time).
/// # Safety: `rgba` must hold at least w*h*4 bytes.
#[no_mangle]
pub unsafe extern "C" fn fsr_texture_upload(id: u32, w: u32, h: u32, rgba: *const u8) -> i32 {
    if rgba.is_null() || w == 0 || h == 0 {
        return 0;
    }
    let bytes = std::slice::from_raw_parts(rgba, (w as usize) * (h as usize) * 4);
    let mut g = ENGINE.lock().unwrap();
    let Some(e) = g.as_mut() else { return 0 };
    let t0 = std::time::Instant::now();
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let Engine { device, queue, live, .. } = e;
        live.upload_texture(device, queue, id, w, h, bytes);
    }));
    let ms = t0.elapsed().as_millis();
    if ms > 5 {
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open("C:/fs/fsr_perf.log") {
            use std::io::Write;
            let _ = writeln!(f, "SLOWTEX id={} {}x{} {}ms", id, w, h, ms);
        }
    }
    if r.is_err() { 0 } else { 1 }
}

/// End the frame: render every queued draw to the swapchain and present.
/// Returns the number of draws rendered (0 = nothing/failed).
#[no_mangle]
pub extern "C" fn fsr_end_frame() -> i32 {
    let mut g = ENGINE.lock().unwrap();
    let Some(e) = g.as_mut() else { return 0 };
    e.frame_open = false;
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let frame = match e.surface.get_current_texture() {
            Ok(f) => f,
            Err(err) => {
                // alt-tab/occlusion diagnostics: name every surface hiccup in the perf log
                if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open("C:/fs/fsr_perf.log") {
                    use std::io::Write;
                    let _ = writeln!(f, "SURFACE frame {} err {:?}", e.frames, err);
                }
                match err {
                    wgpu::SurfaceError::Lost | wgpu::SurfaceError::Outdated => {
                        e.surface.configure(&e.device, &e.config);
                        match e.surface.get_current_texture() {
                            Ok(f) => f,
                            Err(_) => return 0,
                        }
                    }
                    _ => return 0,
                }
            }
        };
        let view = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        let (w, h) = (e.config.width, e.config.height);
        let fmt = e.config.format;
        let Engine { device, queue, live, .. } = e;
        let t0 = std::time::Instant::now();
        let n = live.flush(device, queue, &view, w, h);
        let flush_us = t0.elapsed().as_micros() as u64;
        // Frame grabber: when the trigger file exists, copy this frame out and write a
        // PNG the assistant can Read -- a direct view of exactly what the engine
        // presents, independent of OS screen-capture APIs. Trigger checked every 15
        // frames (one fs metadata call), consumed on capture.
        let cap = e.frames % 15 == 0 && std::path::Path::new(CAPTURE_REQ).exists();
        let cap_buf = if cap {
            let stride = ((w * 4 + 255) / 256) * 256;
            let buf = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("grab"),
                size: (stride * h) as u64,
                usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                mapped_at_creation: false,
            });
            let mut enc2 = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("grab") });
            enc2.copy_texture_to_buffer(
                wgpu::ImageCopyTexture {
                    texture: &frame.texture,
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
            queue.submit([enc2.finish()]);
            Some((buf, stride))
        } else {
            None
        };
        frame.present();
        if let Some((buf, stride)) = cap_buf {
            write_capture(device, &buf, stride, w, h, fmt);
            let _ = std::fs::remove_file(CAPTURE_REQ);
        }
        e.frames += 1;
        {
            static FLUSH_ACC: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
            FLUSH_ACC.fetch_add(flush_us, std::sync::atomic::Ordering::Relaxed);
            if e.frames % 300 == 1 {
                let acc = FLUSH_ACC.swap(0, std::sync::atomic::Ordering::Relaxed);
                // Firestorm.log never sees Rust stderr -- write perf where it can be read.
                if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open("C:/fs/fsr_perf.log") {
                    use std::io::Write;
                    let _ = writeln!(f, "frame {} draws {} flush_avg_ms {:.2}", e.frames, n, acc as f64 / 300.0 / 1000.0);
                }
            }
        }
        n as i32
    }));
    r.unwrap_or(0)
}

/// The viewer wrote to a CPU-shadow buffer: drop the engine cached GPU copy so it
/// re-uploads once, instead of re-uploading every buffer every frame.
#[no_mangle]
pub extern "C" fn fsr_buffer_dirty(ptr: usize) -> i32 {
    if let Some(e) = ENGINE.lock().unwrap().as_mut() {
        e.live.invalidate(ptr);
        1
    } else {
        0
    }
}

/// Patch a sub-rect of an existing texture (glTexSubImage2D path).
/// # Safety: `rgba` must hold at least w*h*4 bytes.
#[no_mangle]
pub unsafe extern "C" fn fsr_texture_subupload(id: u32, x: u32, y: u32, w: u32, h: u32, full_w: u32, full_h: u32, rgba: *const u8) -> i32 {
    if rgba.is_null() || w == 0 || h == 0 {
        return 0;
    }
    let bytes = std::slice::from_raw_parts(rgba, (w as usize) * (h as usize) * 4);
    let mut g = ENGINE.lock().unwrap();
    let Some(e) = g.as_mut() else { return 0 };
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let Engine { device, queue, live, .. } = e;
        live.upload_subtexture(device, queue, id, x, y, w, h, full_w, full_h, bytes)
    }));
    match r { Ok(true) => 1, _ => 0 }
}

/// Set the current diffuse colour (called by the null-GL stub when the viewer sets its
/// "color" uniform -- that is where UI/text colour lives).
#[no_mangle]
pub extern "C" fn fsr_set_color(r: f32, g: f32, b: f32, a: f32) -> i32 {
    if let Some(e) = ENGINE.lock().unwrap().as_mut() {
        e.live.cur_color = [r, g, b, a];
        1
    } else {
        0
    }
}

/// Honest adapter identity for Help > About (NOT a spoofed GL string).
/// # Safety: `out` must have room for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn fsr_adapter_info(out: *mut u8, cap: u32) -> i32 {
    if out.is_null() || cap == 0 {
        return 0;
    }
    let g = ENGINE.lock().unwrap();
    let Some(e) = g.as_ref() else { return 0 };
    let s = format!("Vulkan (fs_render) -- {}", e.adapter_desc);
    let b = s.as_bytes();
    let n = b.len().min(cap as usize - 1);
    std::ptr::copy_nonoverlapping(b.as_ptr(), out, n);
    *out.add(n) = 0;
    n as i32
}
