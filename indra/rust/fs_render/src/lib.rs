//! fs_render — the live-bridge renderer DLL (PHASE3_PLAN.md P3a).
//!
//! C ABI consumed by the viewer in engine mode (FS_RENDER_ENGINE=1): the viewer hands
//! over its HWND, then streams per-frame draws through `fsr_submit` (the same data the
//! P2b scene dump captured — SoA-block vertex bytes, typemask, indices, program name,
//! matrices). P3a scope: init on the HWND + clear-color present + the unlit-textured
//! draw path (the replay renderer, live). Handle/POD/no-panic-across-boundary per the
//! llrust FFI house pattern.

use std::ffi::c_void;
use std::sync::Mutex;

use glam::Mat4;
use raw_window_handle::{RawDisplayHandle, RawWindowHandle, Win32WindowHandle, WindowsDisplayHandle};

struct Engine {
    _instance: wgpu::Instance,
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    frame_open: bool,
    frames: u64,
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
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            format,
            width: width.max(1),
            height: height.max(1),
            present_mode: caps.present_modes[0],
            alpha_mode: caps.alpha_modes[0],
            view_formats: vec![],
            desired_maximum_frame_latency: 2,
        };
        surface.configure(&device, &config);
        log::info!("fs_render: surface {}x{} {:?}", config.width, config.height, format);
        Some(Engine {
            _instance: instance,
            surface,
            device,
            queue,
            config,
            frame_open: false,
            frames: 0,
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

// keep glam/bytemuck linked ahead of the P3b draw path (matrices cross here next)
#[allow(dead_code)]
fn _phase3b_types(m: [f32; 16]) -> Mat4 {
    Mat4::from_cols_array(&m)
}
