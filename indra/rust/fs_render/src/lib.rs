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

fn write_capture(device: &wgpu::Device, buf: &wgpu::Buffer, stride: u32, w: u32, h: u32, fmt: wgpu::TextureFormat, out: &str) {
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
    if let Ok(f) = std::fs::File::create(out) {
        let mut enc = png::Encoder::new(std::io::BufWriter::new(f), w, h);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        if let Ok(mut wr) = enc.write_header() {
            let _ = wr.write_image_data(&rgba);
        }
    }
    log::info!("fs_render: frame captured to {}", out);
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

/// P1 payload: the render-settings snapshot (`#[repr(C)]` = `scene::SettingsSnapshot`). Called
/// on any settings change (not per frame); the engine reads this instead of scraping GL state
/// (honoring mechanisms 1 & 2). Values are effective (LLFeatureManager already applied).
#[no_mangle]
pub extern "C" fn fsr_scene_set_settings(s: *const scene::SettingsSnapshot) -> i32 {
    if s.is_null() {
        return 0;
    }
    let snap = unsafe { *s };
    let mut g = ENGINE.lock().unwrap();
    if let Some(e) = g.as_mut() {
        e.scene.set_settings(&snap);
        1
    } else {
        0
    }
}

/// P2 payload: the EEP atmospherics/sky block (`#[repr(C)]` = `scene::EepSkyBlock`), per frame.
#[no_mangle]
pub extern "C" fn fsr_scene_set_sky(sky: *const scene::EepSkyBlock) -> i32 {
    if sky.is_null() {
        return 0;
    }
    let s = unsafe { *sky };
    let mut g = ENGINE.lock().unwrap();
    if let Some(e) = g.as_mut() {
        e.scene.set_sky(&s);
        1
    } else {
        0
    }
}

/// S3b: the sky-regime settings subset (`#[repr(C)]`, mirrored C++-side as `FsrRegimeSettings`).
/// A focused feed so the viewer need not mirror the full 120-field `SettingsSnapshot` yet: these
/// are exactly the `gSavedSettings` the regime derivation (`SceneFrame::sky_regime`) reads. The
/// values land in the engine's `SettingsSnapshot` regime fields; a future full settings feed (P4)
/// supersedes this with the same values.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RegimeSettings {
    pub sky_auto_adjust_legacy: u32,
    pub sky_sunlight_scale: f32,
    pub hdr_sky_sunlight_scale: f32,
    pub sky_ambient_scale: f32,
    pub sky_auto_adjust_ambient_scale: f32,
    pub sky_auto_adjust_hdr_scale: f32,
    pub sun_dynamic_range: f32,
    pub tonemap_mix: f32,
    pub tonemap_type: u32,
    pub exposure: f32,
    pub hdr_enabled: u32,
    pub reflection_probes_enabled: u32,
}

/// S3b: apply the sky-regime settings subset into the engine's settings snapshot. Called by the
/// viewer when these change (or per frame -- idempotent). Drives `sky_regime()` -> the tonemap.
#[no_mangle]
pub extern "C" fn fsr_scene_set_regime(rs: *const RegimeSettings) -> i32 {
    if rs.is_null() {
        return 0;
    }
    let r = unsafe { *rs };
    let mut g = ENGINE.lock().unwrap();
    let Some(e) = g.as_mut() else { return 0 };
    let s = &mut e.scene.settings;
    s.render_sky_auto_adjust_legacy = r.sky_auto_adjust_legacy;
    s.render_sky_sunlight_scale = r.sky_sunlight_scale;
    s.render_hdr_sky_sunlight_scale = r.hdr_sky_sunlight_scale;
    s.render_sky_ambient_scale = r.sky_ambient_scale;
    s.render_sky_auto_adjust_ambient_scale = r.sky_auto_adjust_ambient_scale;
    s.render_sky_auto_adjust_hdr_scale = r.sky_auto_adjust_hdr_scale;
    s.render_sun_dynamic_range = r.sun_dynamic_range;
    s.render_tonemap_mix = r.tonemap_mix;
    s.render_tonemap_type = r.tonemap_type;
    s.render_exposure = r.exposure;
    s.render_hdr_enabled = r.hdr_enabled;
    s.render_reflection_probes_enabled = r.reflection_probes_enabled;
    1
}

/// P2 payload: the EEP water block (`#[repr(C)]` = `scene::WaterBlock`), per frame.
#[no_mangle]
pub extern "C" fn fsr_scene_set_water(water: *const scene::WaterBlock) -> i32 {
    if water.is_null() {
        return 0;
    }
    let w = unsafe { *water };
    let mut g = ENGINE.lock().unwrap();
    if let Some(e) = g.as_mut() {
        e.scene.set_water(&w);
        1
    } else {
        0
    }
}

/// P3 payload: the local point/spot lights for this frame (`count` x `scene::Light`), replacing
/// the previous list. The viewer caps `count` at RenderLocalLightCount (from the settings snapshot).
#[no_mangle]
pub extern "C" fn fsr_scene_set_lights(count: u32, ptr: *const scene::Light) -> i32 {
    let mut g = ENGINE.lock().unwrap();
    let Some(e) = g.as_mut() else { return 0 };
    if count == 0 || ptr.is_null() {
        e.scene.set_lights(&[]);
        return 1;
    }
    let lights = unsafe { std::slice::from_raw_parts(ptr, count as usize) };
    e.scene.set_lights(lights);
    1
}

/// End the typed frame. Returns the number of typed draws contributed (0 through P3; the present
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

/// Phase A.3: reset the per-frame UI list. Call once at frame start, before any fsr_ui_submit.
#[no_mangle]
pub extern "C" fn fsr_ui_begin() -> i32 {
    if let Some(e) = ENGINE.lock().unwrap().as_mut() { e.live.ui_begin(); 1 } else { 0 }
}

/// Phase A.3: submit one LLRender::flush as a native-VK UI draw. `mvp` = 16 f32 (the viewer's OWN
/// projection*modelview, column-major), `verts` = `vtx_count` interleaved vertices of
/// {f32 x,y,z; f32 u,v; u8 r,g,b,a} (24 bytes each), `mode` = LLRender::eGeomModes, `tex_id` = the
/// bound GL texture id (0/unknown -> 1x1 white). No glGet: honest state only.
/// # Safety: `mvp` -> 16 f32; `verts` -> `vtx_count*24` bytes, both valid for the call.
#[no_mangle]
pub unsafe extern "C" fn fsr_ui_submit(mvp: *const f32, tex_id: u32, mode: u32, vtx_count: u32, verts: *const u8) -> i32 {
    if mvp.is_null() || verts.is_null() || vtx_count == 0 { return 0; }
    let mut m = [0f32; 16];
    m.copy_from_slice(std::slice::from_raw_parts(mvp, 16));
    let vbytes = std::slice::from_raw_parts(verts, vtx_count as usize * 24);
    let mut g = ENGINE.lock().unwrap();
    let Some(e) = g.as_mut() else { return 0 };
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        e.live.ui_submit(&m, tex_id, mode, vbytes);
    }));
    if r.is_err() { 0 } else { 1 }
}

/// End the frame: render every queued draw to the swapchain and present.
/// Returns the number of draws rendered (0 = nothing/failed).
#[no_mangle]
pub extern "C" fn fsr_end_frame() -> i32 {
    let mut g = ENGINE.lock().unwrap();
    let Some(e) = g.as_mut() else { return 0 };
    e.frame_open = false;
    // S3b: consume the derived sky regime -- drive the global tonemap's exposure/mix/gamma/
    // legacy-gamma from the SAME legacy-vs-advanced regime the viewer used for the sky. This is
    // the piece the reverted P3 consumption lacked (a tonemap at all); now the model + tonemap
    // exist (S1/S2/S3a) and the regime is oracle-tested, so this is safe. `None` (no typed sky
    // fed yet) leaves the post defaults; the sky's own sky_hdr_scale still comes via the tap aux.
    if let Some(r) = e.scene.sky_regime() {
        e.live.apply_sky_regime(&r);
    }
    // Phase A.2: the engine derives the fullscreen sky UBO from the typed camera + EEP sky it was
    // fed, and renders the sky itself (parallel-read) -- no tapped dome draw needed.
    if let Some(mut sky_ubo) = e.scene.fullscreen_sky_ubo() {
        // The viewer's setSceneCamera never populates viewport_w/h (memset 0), which made the
        // fullscreen sky's NDC divide by zero -> NaN view-ray -> a UNIFORM constant sky. The
        // authoritative viewport for this fullscreen pass IS the engine's render target (the
        // scene_hdr the triangle covers = swapchain size), so supply it here.
        sky_ubo[50] = e.config.width as f32;
        sky_ubo[51] = e.config.height as f32;
        // Cloud animation clock (a7.w = u[47], previously unused): monotonic seconds since the first
        // frame, driving the volumetric cloud march's wind scroll + evolution. Real Instant (engine
        // is native Rust); no viewer feed needed.
        {
            use std::sync::OnceLock;
            static START: OnceLock<std::time::Instant> = OnceLock::new();
            sky_ubo[47] = START.get_or_init(std::time::Instant::now).elapsed().as_secs_f32();
        }
        e.live.set_fullscreen_sky(&sky_ubo);
    }
    // P1 DIAG (temporary): pinpoint the white sky WITHOUT guessing. cam/sky = typed feed reached;
    // ubo = derived; sky_fs = live pass armed; msaa = resolve path (confirmed forced 1); clear =
    // viewer clear (what shows when sky_fs=false); exp/mix = tonemap; sun/amb/hdr/maxy = sanity of
    // the derived params (huge/NaN -> blown-out white even when armed).
    {
        static D: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        if D.fetch_add(1, std::sync::atomic::Ordering::Relaxed) % 120 == 0 {
            let cam = e.scene.has_camera();
            let sky = e.scene.has_sky();
            let ubo_opt = e.scene.fullscreen_sky_ubo();
            let ubo = ubo_opt.is_some();
            let (sun, amb, hdr, maxy, bh, sundir, sun_up, moondir) = match ubo_opt {
                // sun_color, ambient, sky_hdr_scale, max_y, blue_horizon, FULL sun_dir(xyz), sun_up_factor, FULL moon_dir(xyz)
                Some(u) => ((u[24], u[25], u[26]), (u[32], u[33], u[34]), u[48], u[19], (u[36], u[37], u[38]), (u[52], u[53], u[54]), u[23], (u[56], u[57], u[58])),
                None => ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 0.0, 0.0, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 0.0, (0.0, 0.0, 0.0)),
            };
            let fs = e.live.sky_fs_enabled();
            let ms = e.live.msaa_samples();
            let (exp, exps, mix) = e.live.post_diag();
            let c = e.clear_color;
            if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open("C:/fs/fsr_perf.log") {
                use std::io::Write;
                let _ = writeln!(f, "SKYDIAG cam={} sky={} ubo={} sky_fs={} msaa={} clear=({:.2},{:.2},{:.2}) exp={:.3} exp_scale={:.3} mix={:.2} sun=({:.2},{:.2},{:.2}) amb=({:.2},{:.2},{:.2}) bh=({:.2},{:.2},{:.2}) sundir=({:.3},{:.3},{:.3}) moondir=({:.3},{:.3},{:.3}) sun_up={:.1} hdr={:.2} maxy={:.1}",
                    cam, sky, ubo, fs, ms, c.r, c.g, c.b, exp, exps, mix, sun.0, sun.1, sun.2, amb.0, amb.1, amb.2, bh.0, bh.1, bh.2, sundir.0, sundir.1, sundir.2, moondir.0, moondir.1, moondir.2, sun_up, hdr, maxy);
            }
        }
    }
    // P3 (resolve consumption) still deferred to S4: the opaque lighting resolve is a stub;
    // reconstruct-from-depth + calcAtmosphericVars land there. The sky + tonemap are live now.
    // B1: an empty frame would present the raw clear over a good image -- the whole
    // teal-flash class (window resize swaps, startup states with no draws). Keep the
    // last presented frame instead. The very first present still goes through so the
    // window never shows stale desktop contents.
    if !e.live.has_content() && e.presented_once {
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
        // Flash detector (debug tool, FS_ENGINE_FLASHLOG set). With metered auto-exposure the SCENE
        // can no longer blow out (it measures + compensates the same frame), so a white flash lives
        // in the FINAL composite -- a UI announcement rendering opaque-white, or a transition frame.
        // Read back a middle ROW of the presented frame (a cheap proxy for a full-screen flash) and,
        // when it's near-white, log the frame number + the sky/exposure state so a transient becomes
        // a timestamped record with its cause attached. Env-gated (it stalls); zero cost when off.
        {
            static FLASH: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
            let flash_on = *FLASH.get_or_init(|| std::env::var("FS_ENGINE_FLASHLOG").is_ok());
            if flash_on && w >= 4 {
                let row_stride = ((w * 4 + 255) / 256) * 256;
                let fbuf = device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("flash-row"),
                    size: row_stride as u64,
                    usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                    mapped_at_creation: false,
                });
                let mut encf = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("flash") });
                encf.copy_texture_to_buffer(
                    wgpu::ImageCopyTexture { texture: &frame.texture, mip_level: 0, origin: wgpu::Origin3d { x: 0, y: h / 2, z: 0 }, aspect: wgpu::TextureAspect::All },
                    wgpu::ImageCopyBuffer { buffer: &fbuf, layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(row_stride), rows_per_image: Some(1) } },
                    wgpu::Extent3d { width: w, height: 1, depth_or_array_layers: 1 },
                );
                queue.submit([encf.finish()]);
                fbuf.slice(..).map_async(wgpu::MapMode::Read, |_| {});
                device.poll(wgpu::Maintain::Wait);
                let avg = {
                    let data = fbuf.slice(..).get_mapped_range();
                    let mut sum = 0u64;
                    for i in 0..(w as usize) {
                        let p = i * 4;
                        sum += data[p] as u64 + data[p + 1] as u64 + data[p + 2] as u64;
                    }
                    sum as f32 / (w as f32 * 3.0 * 255.0)
                };
                fbuf.unmap();
                // FLASH on a near-white row; ROWAVG baseline every 120 frames (confirms the readback
                // returns sane values -- a blue sky reads ~0.4 -- and gives a brightness timeline).
                if avg > 0.90 || e.frames % 120 == 0 {
                    let tag = if avg > 0.90 { "FLASH" } else { "ROWAVG" };
                    let fs = live.sky_fs_enabled();
                    let (exp, exps, mix) = live.post_diag();
                    let meas = live.read_exp_lum(device, queue); // the avg the tonemap metered (NaN? near-0?)
                    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open("C:/fs/fsr_perf.log") {
                        use std::io::Write;
                        let _ = writeln!(f, "{} frame {} row_avg={:.3} meas_lum={:.5} sky_fs={} exp={:.2} exp_scale={:.2} mix={:.2}", tag, e.frames, avg, meas, fs, exp, exps, mix);
                    }
                    // On a flash, dump the large UI draws to ID the full-screen leaker (LLRender::flush).
                    if avg > 0.90 {
                        let dump = live.dump_ui_large();
                        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open("C:/fs/fsr_perf.log") {
                            use std::io::Write;
                            let _ = write!(f, "FLASH-UI frame {} large draws:\n{}", e.frames, dump);
                        }
                    }
                    // Auto-save the FIRST flash frame (full grab) to a separate file so we can SEE
                    // exactly what is whitening the screen -- image beats inference.
                    static FLASH_SAVED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
                    if avg > 0.90 && !FLASH_SAVED.swap(true, std::sync::atomic::Ordering::Relaxed) {
                        let gstride = ((w * 4 + 255) / 256) * 256;
                        let gbuf = device.create_buffer(&wgpu::BufferDescriptor {
                            label: Some("flash-grab"),
                            size: (gstride * h) as u64,
                            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                            mapped_at_creation: false,
                        });
                        let mut encg = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("flash-grab") });
                        encg.copy_texture_to_buffer(
                            wgpu::ImageCopyTexture { texture: &frame.texture, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
                            wgpu::ImageCopyBuffer { buffer: &gbuf, layout: wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(gstride), rows_per_image: Some(h) } },
                            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                        );
                        queue.submit([encg.finish()]);
                        write_capture(device, &gbuf, gstride, w, h, fmt, "C:\\fs\\fsr_flash.png");
                    }
                }
            }
        }
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
            write_capture(device, &buf, stride, w, h, fmt, CAPTURE_OUT);
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

/// The REAL Vulkan-adapter capabilities the viewer's GPU detection needs, `#[repr(C)]` (mirrored
/// C++-side in llgl.cpp as `FsrGpuInfo`). The null-GL stub reports a FABRICATED identity
/// ("FSVulkan" / "fs_render null-GL bridge") -> the viewer classifies the GPU as unrecognized
/// "MISC" and resets graphics to medium-low. `LLGLManager::initGL` calls `fsr_query_gpu_info`
/// to replace that lie with the truth. Strings are NUL-terminated; `gl_vendor` is a
/// GL-recognizable name (e.g. "ATI Technologies Inc.") so the vendor classifier sets mIsAMD/etc.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FsrGpuInfo {
    pub gl_renderer: [u8; 128], // real adapter name, e.g. "AMD Radeon RX 9070 XT"
    pub gl_vendor: [u8; 64],    // GL-recognizable vendor from the PCI id
    pub gl_version: [u8; 64],
    pub vendor_id: u32,         // PCI vendor id (0x1002 AMD / 0x10DE NVIDIA / 0x8086 Intel)
    pub device_id: u32,
    pub device_type: u32,       // 0 Other / 1 Integrated / 2 Discrete / 3 Virtual / 4 Cpu
    pub max_texture_2d: u32,
    pub max_varying_vectors: u32,
}

fn cstr_into(dst: &mut [u8], s: &str) {
    let b = s.as_bytes();
    let n = b.len().min(dst.len().saturating_sub(1));
    dst[..n].copy_from_slice(&b[..n]);
    for x in dst[n..].iter_mut() {
        *x = 0;
    }
}

/// Enumerate the real Vulkan adapter and capture its capabilities. Standalone -- creates its own
/// transient instance/adapter (no surface, no device), so it works BEFORE `fsr_init` (the viewer
/// reads GPU caps before the engine's device is up). Result is cached (enumeration is ~ms).
fn query_gpu_info() -> FsrGpuInfo {
    let mut gi: FsrGpuInfo = unsafe { std::mem::zeroed() };
    let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
        backends: wgpu::Backends::VULKAN,
        ..Default::default()
    });
    if let Some(adapter) = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::HighPerformance,
        compatible_surface: None,
        force_fallback_adapter: false,
    })) {
        let info = adapter.get_info();
        let limits = adapter.limits();
        cstr_into(&mut gi.gl_renderer, &info.name);
        let vendor = match info.vendor {
            0x1002 => "ATI Technologies Inc.", // AMD/ATI -- classifier keys on "ATI " / "AMD"
            0x10DE => "NVIDIA Corporation",
            0x8086 => "Intel",
            0x13B5 => "ARM",
            0x5143 => "Qualcomm",
            _ => "Vulkan",
        };
        cstr_into(&mut gi.gl_vendor, vendor);
        cstr_into(&mut gi.gl_version, "4.6.0 (fs_render Vulkan)");
        gi.vendor_id = info.vendor;
        gi.device_id = info.device;
        gi.device_type = match info.device_type {
            wgpu::DeviceType::Other => 0,
            wgpu::DeviceType::IntegratedGpu => 1,
            wgpu::DeviceType::DiscreteGpu => 2,
            wgpu::DeviceType::VirtualGpu => 3,
            wgpu::DeviceType::Cpu => 4,
        };
        gi.max_texture_2d = limits.max_texture_dimension_2d;
        // GL_MAX_VARYING_VECTORS: wgpu doesn't expose the GL value; the viewer only cares that it
        // exceeds 16 (else it force-downgrades). Any Vulkan-capable GPU supports >= 32 varyings;
        // derive from the adapter's inter-stage limit, floored to a truthful hardware minimum.
        let from_limit = limits.max_inter_stage_shader_components / 4;
        gi.max_varying_vectors = from_limit.max(32);
    }
    gi
}

/// Fill `out` with the real Vulkan-adapter capabilities. Returns 1 on success, 0 if no adapter
/// (then the viewer keeps the stub's values). Cached after the first call.
/// # Safety: `out` must point to a valid `FsrGpuInfo`.
#[no_mangle]
pub unsafe extern "C" fn fsr_query_gpu_info(out: *mut FsrGpuInfo) -> i32 {
    if out.is_null() {
        return 0;
    }
    static GPU_INFO: std::sync::OnceLock<FsrGpuInfo> = std::sync::OnceLock::new();
    let gi = GPU_INFO.get_or_init(query_gpu_info);
    if gi.gl_renderer[0] == 0 {
        return 0; // enumeration failed -- no adapter
    }
    *out = *gi;
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
