//! fs_harness -- apples-to-apples OGL<->VLK verify driver.
//!
//! Feeds the SAME canned fixture to `fs_render` (the port, headless) and `fs_ogl_ref` (the oracle),
//! grabs both frames to PNG, and diffs them per-pixel. Step 1 = the skeleton baseline: both engines
//! clear an identical offscreen target to the same color; the diff must be bit-exact. As real passes
//! are absorbed (atmospherics, HDR, ...), the same driver measures each step's divergence.
//!
//! Run:  cargo run --release --bin fs_harness   (grokj2k.dll on PATH; LIBCLANG_PATH+GROK_ROOT to build)

use std::ffi::CString;

const W: u32 = 256;
const H: u32 = 256;
const PORT_PNG: &str = "C:/fs/fsr_ab_render.png"; // fs_render (the port)
const REF_PNG: &str = "C:/fs/fsr_ab_ref.png"; // fs_ogl_ref (the oracle)
const DIFF_PNG: &str = "C:/fs/fsr_ab_diff.png"; // amplified delta image

fn main() {
    // --- Skeleton fixture: size + the initial framebuffer clear color. ---
    let (cr, cg, cb, ca) = (0.20_f64, 0.50, 0.80, 1.0);

    // --- fs_render (the port), headless ---
    unsafe {
        assert_eq!(fs_render::fsr_init(std::ptr::null_mut(), W, H), 1, "fs_render headless init failed");
        fs_render::fsr_set_clear_color(cr as f32, cg as f32, cb as f32, ca as f32);
        for _ in 0..2 {
            fs_render::fsr_begin_frame();
            fs_render::fsr_end_frame();
        }
        let p = CString::new(PORT_PNG).unwrap();
        assert_eq!(fs_render::fsr_grab(p.as_ptr()), 1, "fs_render grab failed");
        fs_render::fsr_shutdown();
    }

    // --- fs_ogl_ref (the oracle) ---
    {
        let mut r = fs_ogl_ref::RefEngine::new(W, H).expect("fs_ogl_ref init failed");
        r.set_clear_color(cr, cg, cb, ca);
        r.frame();
        r.grab(REF_PNG);
    }

    // --- diff ---
    let (aw, ah, a) = load_rgba(PORT_PNG);
    let (bw, bh, b) = load_rgba(REF_PNG);
    assert_eq!((aw, ah), (bw, bh), "A/B size mismatch: {aw}x{ah} vs {bw}x{bh}");
    println!("port(fs_render) px0={:?}  ref(fs_ogl_ref) px0={:?}", &a[0..4], &b[0..4]);
    let (max_d, mism, psnr) = diff_rgba(&a, &b, aw, ah, DIFF_PNG);
    let total = (aw * ah) as u64;
    println!(
        "STEP 1 skeleton A/B  {aw}x{ah}  max_delta={max_d}  mismatched_px={mism}/{total}  psnr={}",
        if psnr.is_infinite() { "inf".to_string() } else { format!("{psnr:.2}dB") }
    );
    if max_d == 0 {
        println!("VERIFIED IDENTICAL (bit-exact) -- port skeleton == oracle skeleton.");
    } else {
        println!("DIVERGENCE -- amplified diff image at {DIFF_PNG}");
        std::process::exit(1);
    }
}

/// Load a PNG as tight RGBA8 (both engines write RGBA8).
fn load_rgba(path: &str) -> (u32, u32, Vec<u8>) {
    let dec = png::Decoder::new(std::fs::File::open(path).unwrap_or_else(|_| panic!("open {path}")));
    let mut reader = dec.read_info().expect("png info");
    let mut buf = vec![0u8; reader.output_buffer_size()];
    let info = reader.next_frame(&mut buf).expect("png frame");
    assert_eq!(info.color_type, png::ColorType::Rgba, "expected RGBA8 PNG");
    buf.truncate((info.width * info.height * 4) as usize);
    (info.width, info.height, buf)
}

/// Per-pixel diff: max channel delta, mismatched-pixel count, PSNR; writes an amplified delta image.
fn diff_rgba(a: &[u8], b: &[u8], w: u32, h: u32, diff_out: &str) -> (u8, u64, f64) {
    let px_count = (w * h) as usize;
    let mut max_d = 0u8;
    let mut mism = 0u64;
    let mut sse = 0f64;
    let mut diff_img = vec![0u8; px_count * 4];
    for px in 0..px_count {
        let mut pix_mism = false;
        for c in 0..4 {
            let i = px * 4 + c;
            let d = (a[i] as i32 - b[i] as i32).unsigned_abs() as u8;
            if d > max_d {
                max_d = d;
            }
            sse += (d as f64) * (d as f64);
            if d != 0 {
                pix_mism = true;
            }
            diff_img[i] = if c < 3 {
                // amplify so any nonzero delta is visible; a hit floors at 32.
                if d > 0 { d.saturating_mul(8).max(32) } else { 0 }
            } else {
                255
            };
        }
        if pix_mism {
            mism += 1;
        }
    }
    let mse = sse / (px_count as f64 * 4.0);
    let psnr = if mse == 0.0 { f64::INFINITY } else { 10.0 * (255.0_f64 * 255.0 / mse).log10() };
    if let Ok(f) = std::fs::File::create(diff_out) {
        let mut enc = png::Encoder::new(std::io::BufWriter::new(f), w, h);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        if let Ok(mut wr) = enc.write_header() {
            let _ = wr.write_image_data(&diff_img);
        }
    }
    (max_d, mism, psnr)
}
