//! fs_render — the live-bridge renderer DLL (PHASE3_PLAN.md P3a).
//!
//! C ABI consumed by the viewer in engine mode (FS_RENDER_ENGINE=1): the viewer hands
//! over its HWND, then streams per-frame draws through `fsr_submit` (the same data the
//! P2b scene dump captured — SoA-block vertex bytes, typemask, indices, program name,
//! matrices). P3a scope: init on the HWND + clear-color present + the unlit-textured
//! draw path (the replay renderer, live). Handle/POD/no-panic-across-boundary per the
//! llrust FFI house pattern.

pub mod live;
pub mod scene; // P0: typed scene-bridge scaffold (GROUNDUP_VULKAN_ENGINE_PLAN.md)

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
    /// P0: the typed scene-bridge frame, accumulated beside the tap. Empty until a migration
    /// phase fills it; the tap renders the whole scene until then.
    scene: scene::SceneFrame,
    frame_open: bool,
    adapter_desc: String,
    /// B4: retained copy of the last presented frame (readback for snapshots and the
    /// saved login backdrop -- stub glReadPixels was zero-filling screen_last.png).
    last_frame: Option<(wgpu::Texture, u32, u32)>,
    last_read_frame: u64,
    read_cache: Option<(u64, u32, u32, Vec<u8>)>, // (frame, w, h, GL-ordered tight RGBA)
    /// B5: the viewer's glClearColor, forwarded by the stub; teal only in debug.
    clear_color: wgpu::Color,
    debug_teal: bool,
    presented_once: bool,
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
            scene: scene::SceneFrame::new(),
            frame_open: false,
            adapter_desc: format!("{} ({} {})", info.name, info.driver, info.driver_info),
            last_frame: None,
            last_read_frame: 0,
            read_cache: None,
            clear_color: wgpu::Color { r: 0.0, g: 0.0, b: 0.0, a: 1.0 },
            debug_teal: std::env::var("FS_ENGINE_DEBUG").map(|v| v == "1").unwrap_or(false),
            presented_once: false,
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

/// #4: push the per-frame sun/atmospherics env (12 floats: eye-space sun_dir, sunlight,
/// ambient -- each a vec4). The tap calls this once per frame before submitting draws; the
/// deferred resolve reads it. No-op until the viewer wires it (engine uses its default).
#[no_mangle]
pub extern "C" fn fsr_set_frame_env(ptr: *const f32) -> i32 {
    if ptr.is_null() {
        return 0;
    }
    let env = unsafe { std::slice::from_raw_parts(ptr, 12) };
    let mut g = ENGINE.lock().unwrap();
    if let Some(e) = g.as_mut() {
        e.live.set_frame_env(env);
        1
    } else {
        0
    }
}

// ---- P0: typed scene-bridge lifecycle (GROUNDUP_VULKAN_ENGINE_PLAN.md) ----
// Co-exists with the GL-draw tap in the same frame:
//   fsr_begin_frame -> [fsr_scene_* typed feeders] + [fsr_submit tap draws] -> fsr_end_frame
// Until a migration phase fills the typed frame it contributes NOTHING and the tap renders
// the whole scene unchanged. The viewer wiring lands with P1 (its first real payload).

/// Begin the typed frame: reset last frame's accumulated typed data.
#[no_mangle]
pub extern "C" fn fsr_scene_begin() -> i32 {
    let mut g = ENGINE.lock().unwrap();
    if let Some(e) = g.as_mut() {
        e.scene.begin();
        1
    } else {
        0
    }
}

/// P1 payload: the frame camera (view/proj/origin/near/far/fov/aspect/viewport), `#[repr(C)]`
/// = `scene::CameraBlock`. The engine derives its own reverse-Z projection from near/far.
#[no_mangle]
pub extern "C" fn fsr_scene_set_camera(cam: *const scene::CameraBlock) -> i32 {
    if cam.is_null() {
        return 0;
    }
    let c = unsafe { *cam };
    let mut g = ENGINE.lock().unwrap();
    if let Some(e) = g.as_mut() {
        e.scene.set_camera(&c);
        1
    } else {
        0
    }
}

/// End the typed frame. Returns the number of typed draws contributed (0 in P0; the present
/// still happens in fsr_end_frame, which will render the typed frame once a phase fills it).
#[no_mangle]
pub extern "C" fn fsr_scene_end() -> i32 {
    0
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
    {
        let Engine { device, queue, live, .. } = e;
        live.drain_pending_uploads(device, queue);
    }
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
    // B1: an empty frame would present the raw clear over a good image -- the whole
    // teal-flash class (window resize swaps, startup states with no draws). Keep the
    // last presented frame instead. The very first present still goes through so the
    // window never shows stale desktop contents.
    if e.live.queued_len() == 0 && e.presented_once {
        return 0;
    }
    let t_acq = std::time::Instant::now();
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
        let clear = if e.debug_teal {
            wgpu::Color { r: 0.08, g: 0.35, b: 0.4, a: 1.0 }
        } else {
            e.clear_color
        };
        let Engine { device, queue, live, .. } = e;
        let t0 = std::time::Instant::now();
        let n = live.flush_clear(device, queue, &view, w, h, clear);
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
        // B4 + wave-5: retain only when a readback consumer is active (or on the
        // every-4th-frame heartbeat) -- an every-present extra queue.submit caused
        // FIFO deadline misses (the 60<->30 movement chop).
        if (e.frames.wrapping_sub(e.last_read_frame) < 300) || (e.frames % 4 == 0) {
            let need_new = match &e.last_frame {
                Some((_, lw, lh)) => *lw != w || *lh != h,
                None => true,
            };
            if need_new {
                e.last_frame = Some((
                    device.create_texture(&wgpu::TextureDescriptor {
                        label: Some("last-frame"),
                        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                        mip_level_count: 1,
                        sample_count: 1,
                        dimension: wgpu::TextureDimension::D2,
                        format: fmt,
                        usage: wgpu::TextureUsages::COPY_DST | wgpu::TextureUsages::COPY_SRC,
                        view_formats: &[],
                    }),
                    w,
                    h,
                ));
            }
            if let Some((lt, _, _)) = &e.last_frame {
                let mut enc3 = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("retain") });
                enc3.copy_texture_to_texture(
                    wgpu::ImageCopyTexture { texture: &frame.texture, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
                    wgpu::ImageCopyTexture { texture: lt, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
                    wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                );
                queue.submit([enc3.finish()]);
            }
        }
        frame.present();
        e.presented_once = true;
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
                    let (ntex, ngeo, npal) = e.live.stats();
                let (tb, drops, pend) = e.live.mem_stats();
                let sky = e.live.sky_draws; let wat = e.live.water_draws;
                e.live.sky_draws = 0; e.live.water_draws = 0;
                let _ = writeln!(
                    f,
                    "frame {} draws {} flush_avg_ms {:.2} acquire_ms {:.2} tex {} geo {} pal {} texMB {} drops {} pend {} SKYdraws {} WATERdraws {}",
                    e.frames, n, acc as f64 / 300.0 / 1000.0,
                    t_acq.elapsed().as_micros() as f64 / 1000.0,
                    ntex, ngeo, npal, tb / (1024 * 1024), drops, pend, sky, wat
                );
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

/// Antialiasing: the viewer's RenderFSAASamples (1/2/4/8). Rebuilds pipelines + targets.
#[no_mangle]
pub extern "C" fn fsr_set_msaa(samples: u32) -> i32 {
    if let Some(e) = ENGINE.lock().unwrap().as_mut() {
        e.live.set_msaa(samples);
        1
    } else {
        0
    }
}

/// wave-6: forwarded glDeleteTextures -- without this every texture ever uploaded
/// lived forever (monotonic VRAM growth, WDDM paging, desktop-wide starvation).
#[no_mangle]
pub extern "C" fn fsr_texture_delete(id: u32) -> i32 {
    if id == 0 {
        return 0;
    }
    if let Some(e) = ENGINE.lock().unwrap().as_mut() {
        e.live.delete_texture(id);
        1
    } else {
        0
    }
}

/// C: register/update a rigged-mesh joint palette (viewer mGLMp layout: joint_count
/// x 12 floats, world-space). Draws reference it via DrawDesc.skin_id.
/// # Safety: `floats` must hold joint_count*12 f32s.
#[no_mangle]
pub unsafe extern "C" fn fsr_set_matrix_palette(skin_id: u32, joint_count: u32, floats: *const f32) -> i32 {
    if floats.is_null() || skin_id == 0 || joint_count == 0 {
        return 0;
    }
    let n = (joint_count as usize).min(110) * 12;
    let slice = std::slice::from_raw_parts(floats, n);
    let mut g = ENGINE.lock().unwrap();
    let Some(e) = g.as_mut() else { return 0 };
    e.live.set_palette(skin_id, joint_count.min(110), slice);
    1
}

/// B5: the viewer's clear color (stub forwards glClearColor).
#[no_mangle]
pub extern "C" fn fsr_set_clear_color(r: f32, g: f32, b: f32, a: f32) -> i32 {
    if let Some(e) = ENGINE.lock().unwrap().as_mut() {
        e.clear_color = wgpu::Color { r: r as f64, g: g as f64, b: b as f64, a: a as f64 };
        1
    } else {
        0
    }
}

/// B4: read back a region of the LAST PRESENTED frame as tightly-packed RGBA8 with
/// GL semantics (y=0 = bottom row). Returns 1 on success.
/// # Safety: `out` must hold w*h*4 bytes.
#[no_mangle]
pub unsafe extern "C" fn fsr_read_pixels(x: u32, y: u32, w: u32, h: u32, out: *mut u8) -> i32 {
    if out.is_null() || w == 0 || h == 0 {
        return 0;
    }
    let mut g = ENGINE.lock().unwrap();
    let Some(e) = g.as_mut() else { return 0 };
    e.last_read_frame = e.frames;
    let (fw, fh) = match &e.last_frame {
        Some((_, a, b)) => (*a, *b),
        None => return 0,
    };
    if x + w > fw || y + h > fh {
        return 0;
    }
    // Per-frame full-frame cache: rawSnapshot issues ONE glReadPixels PER SCANLINE
    // (llviewerwindow.cpp:6362 loop) -- ~1080 full device drains per snapshot without
    // this. One readback sync per frame, all rects served from the cache.
    let cache_ok = matches!(&e.read_cache, Some((cf, cw, ch, _)) if *cf == e.frames && *cw == fw && *ch == fh);
    if !cache_ok {
        let Some((lt, _, _)) = &e.last_frame else { return 0 };
        let stride = ((fw * 4 + 255) / 256) * 256;
        let buf = e.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("readpx"),
            size: (stride * fh) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc = e.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("readpx") });
        enc.copy_texture_to_buffer(
            wgpu::ImageCopyTexture { texture: lt, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            wgpu::ImageCopyBuffer {
                buffer: &buf,
                layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(stride), rows_per_image: Some(fh) },
            },
            wgpu::Extent3d { width: fw, height: fh, depth_or_array_layers: 1 },
        );
        e.queue.submit([enc.finish()]);
        let slice = buf.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        e.device.poll(wgpu::Maintain::Wait);
        if !matches!(rx.recv(), Ok(Ok(()))) {
            return 0;
        }
        let data = slice.get_mapped_range();
        let bgra = matches!(e.config.format, wgpu::TextureFormat::Bgra8Unorm | wgpu::TextureFormat::Bgra8UnormSrgb);
        let mut cache = vec![0u8; (fw * fh * 4) as usize];
        for row in 0..fh {
            let src = &data[((fh - 1 - row) * stride) as usize..((fh - 1 - row) * stride + fw * 4) as usize];
            let dst = &mut cache[(row * fw * 4) as usize..((row + 1) * fw * 4) as usize];
            if bgra {
                for px in 0..fw as usize {
                    dst[px * 4] = src[px * 4 + 2];
                    dst[px * 4 + 1] = src[px * 4 + 1];
                    dst[px * 4 + 2] = src[px * 4];
                    dst[px * 4 + 3] = src[px * 4 + 3];
                }
            } else {
                dst.copy_from_slice(src);
            }
        }
        drop(data);
        buf.unmap();
        e.read_cache = Some((e.frames, fw, fh, cache));
    }
    if let Some((_, _, _, cache)) = &e.read_cache {
        for row in 0..h {
            let src_row = (y + row) as usize;
            let src = &cache[(src_row * fw as usize + x as usize) * 4..][..(w * 4) as usize];
            let dst = std::slice::from_raw_parts_mut(out.add((row * w * 4) as usize), (w * 4) as usize);
            dst.copy_from_slice(src);
        }
    }
    1
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
