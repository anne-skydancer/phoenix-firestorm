//! Reference-harness task (a) smoke test: drive `fs_render` HEADLESS (no window, no SL login),
//! render a few frames into the offscreen present target, and grab the result to a PNG. Proves the
//! headless render + readback path works end-to-end.
//!
//! Run:  cargo run --release --example headless_smoke
//! (grokj2k.dll must be on PATH; fs_render links it even though this test never decodes J2C.)

use std::ffi::CString;

fn main() {
    unsafe {
        // hwnd == null -> headless.
        assert_eq!(
            fs_render::fsr_init(std::ptr::null_mut(), 256, 256),
            1,
            "fsr_init(headless) failed -- no Vulkan adapter?"
        );
        // A distinctive clear so the readback proves the color path (not merely a black frame).
        fs_render::fsr_set_clear_color(0.20, 0.50, 0.80, 1.0);
        for _ in 0..3 {
            fs_render::fsr_begin_frame();
            fs_render::fsr_end_frame();
        }
        let out = CString::new("C:/fs/fsr_headless.png").unwrap();
        assert_eq!(fs_render::fsr_grab(out.as_ptr()), 1, "fsr_grab failed");
        fs_render::fsr_shutdown();
    }
    let meta = std::fs::metadata("C:/fs/fsr_headless.png").expect("no output PNG written");
    println!("headless smoke OK: C:/fs/fsr_headless.png ({} bytes)", meta.len());
}
