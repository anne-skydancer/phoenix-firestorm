//! U0 UI A/B bench: the STOCK LLRender/LLUI fixed-function contract (the oracle) vs fs_render's
//! ENGINE-CURRENT UI state, on identical `fsr_ui_submit` fixtures, using the REAL ui.vert/ui.frag.
//! See fs_render/UI_STRATIGRAPHY.md.
//!
//! Each fixture isolates one stratum. The oracle side is ASSERTED against the analytic stock result
//! (the sRGB-space blend math on raw bytes -- computed here, not read from a shader), pinning "what
//! stock produces." The engine-current side is MEASURED against the oracle -- that Δ is the gap U1-U4
//! must close. This turns the stratigraphy's qualitative claims into hard numbers, login-free.
//!
//! Run:  cargo run --release --example ui_ab

use fs_ogl_ref::ui_pass::{self, Blend, Draw, Tex, UiState};

const W: u32 = 64;
const H: u32 = 64;

fn px(img: &[[u8; 4]], x: u32, y: u32) -> [u8; 4] { img[(y * W + x) as usize] }

/// Analytic STOCK straight-alpha over an opaque background, in sRGB byte space (raw, no linearize).
fn stock_alpha(src: u8, dst: u8, a: u8) -> u8 {
    let af = a as f64 / 255.0;
    (src as f64 * af + dst as f64 * (1.0 - af)).round().clamp(0.0, 255.0) as u8
}

fn full_quad(color: [u8; 4]) -> Vec<u8> { ui_pass::quad(-1.0, -1.0, 1.0, 1.0, color) }
const IDENTITY: [f32; 16] = [1.,0.,0.,0., 0.,1.,0.,0., 0.,0.,1.,0., 0.,0.,0.,1.];

fn main() {
    let r = fs_ogl_ref::RefEngine::new(W, H).expect("no Vulkan adapter");
    let (dev, q) = (r.device(), r.queue());
    let stock = UiState::stock();
    let eng = UiState::engine_current();
    let bg: [u8; 4] = [64, 64, 64, 255];

    println!("=== U0 UI A/B: STOCK (oracle) vs ENGINE-CURRENT, per stratum ===\n");
    let mut worst_ok = true;

    // ---- Stratum A (color space): translucent MID-GRAY panel over gray bg, white texture. ----
    // The dominant UI case: color carried by the VERTEX color, composited translucent. Stock blends
    // raw sRGB bytes in sRGB space; engine does the linear round-trip. Endpoints (0/255) hide this --
    // midtone + alpha exposes it.
    {
        let d = vec![Draw { mvp: IDENTITY, tex_id: 0, verts: full_quad([128, 128, 128, 128]), line: false, blend: Blend::Alpha, clip: None }];
        let so = ui_pass::render(dev, q, W, H, bg, stock, &[], &d);
        let eo = ui_pass::render(dev, q, W, H, bg, eng, &[], &d);
        let want = stock_alpha(128, 64, 128); // = 96
        let got = px(&so, W / 2, H / 2);
        let (md, mean) = ui_pass::diff_rgb(&so, &eo);
        let ok = (got[0] as i32 - want as i32).abs() <= 1;
        worst_ok &= ok;
        println!("A color-space: stock center={:?} (analytic {}) {} | engine center={:?} | Δ max={} mean={:.1}",
            got, want, if ok { "OK" } else { "FAIL" }, px(&eo, W / 2, H / 2), md, mean);
    }

    // ---- Stratum C (blend modes): ADDITIVE glow. Stock ONE/ONE = src+dst; engine forces alpha. ----
    {
        let d = vec![Draw { mvp: IDENTITY, tex_id: 0, verts: full_quad([128, 128, 128, 128]), line: false, blend: Blend::Add, clip: None }];
        let so = ui_pass::render(dev, q, W, H, bg, stock, &[], &d);
        let eo = ui_pass::render(dev, q, W, H, bg, eng, &[], &d);
        let want = (128u32 + 64).min(255) as u8; // ADD ignores alpha (ONE,ONE) = 192
        let got = px(&so, W / 2, H / 2);
        let (md, mean) = ui_pass::diff_rgb(&so, &eo);
        let ok = (got[0] as i32 - want as i32).abs() <= 1;
        worst_ok &= ok;
        println!("C blend-add:   stock center={:?} (analytic {}) {} | engine center={:?} | Δ max={} mean={:.1}",
            got, want, if ok { "OK" } else { "FAIL" }, px(&eo, W / 2, H / 2), md, mean);
    }

    // ---- Stratum D (filter): a 2-texel font atlas (transparent | opaque white), sampled across. ----
    // Stock samples fonts NEAREST (hard edge on ll_round'd origins); engine LINEAR (soft ramp).
    {
        // 2x1 RGBA: texel0 = white RGB, alpha 0 (transparent); texel1 = white, alpha 255.
        let atlas = Tex { id: 7, w: 2, h: 1, rgba: vec![255, 255, 255, 0, 255, 255, 255, 255], is_font: true };
        let d = vec![Draw { mvp: IDENTITY, tex_id: 7, verts: full_quad([255, 255, 255, 255]), line: false, blend: Blend::Alpha, clip: None }];
        let so = ui_pass::render(dev, q, W, H, bg, stock, std::slice::from_ref(&atlas), &d);
        let eo = ui_pass::render(dev, q, W, H, bg, eng, std::slice::from_ref(&atlas), &d);
        // Endpoints are unambiguous under clamp for BOTH filters: far-left = transparent -> bg 64,
        // far-right = opaque white -> 255.
        let left = px(&so, 1, H / 2);
        let right = px(&so, W - 2, H / 2);
        let (md, mean) = ui_pass::diff_rgb(&so, &eo);
        let ok = (left[0] as i32 - 64).abs() <= 1 && right[0] >= 254;
        worst_ok &= ok;
        println!("D filter:      stock left={:?} right={:?} (analytic 64 / 255) {} | Δ max={} mean={:.1} (soft-ramp vs hard-edge)",
            left, right, if ok { "OK" } else { "FAIL" }, md, mean);
    }

    // ---- Stratum B (scissor): a full-screen white panel clipped to the center rect. ----
    // Stock clips (outside = bg); engine draws everywhere.
    {
        let clip = Some([W / 4, H / 4, W / 2, H / 2]); // [16,16,32,32]
        let d = vec![Draw { mvp: IDENTITY, tex_id: 0, verts: full_quad([255, 255, 255, 255]), line: false, blend: Blend::Alpha, clip }];
        let so = ui_pass::render(dev, q, W, H, bg, stock, &[], &d);
        let eo = ui_pass::render(dev, q, W, H, bg, eng, &[], &d);
        let outside = px(&so, 2, 2); // corner, outside the clip
        let inside = px(&so, W / 2, H / 2); // center, inside
        let eng_outside = px(&eo, 2, 2);
        let (md, mean) = ui_pass::diff_rgb(&so, &eo);
        let ok = (outside[0] as i32 - 64).abs() <= 1 && inside[0] >= 254;
        worst_ok &= ok;
        println!("B scissor:     stock outside={:?} inside={:?} (analytic 64 / 255) {} | engine outside={:?} | Δ max={} mean={:.1}",
            outside, inside, if ok { "OK" } else { "FAIL" }, eng_outside, md, mean);
    }

    // ---- Stratum A (texture path): translucent MID-GRAY textured image (decode sub-case). ----
    // Opaque untinted images round-trip cleanly (decode+encode cancel); translucent/tinted does not.
    {
        let img = Tex { id: 9, w: 1, h: 1, rgba: vec![128, 128, 128, 255], is_font: false };
        let d = vec![Draw { mvp: IDENTITY, tex_id: 9, verts: full_quad([255, 255, 255, 128]), line: false, blend: Blend::Alpha, clip: None }];
        let so = ui_pass::render(dev, q, W, H, bg, stock, std::slice::from_ref(&img), &d);
        let eo = ui_pass::render(dev, q, W, H, bg, eng, std::slice::from_ref(&img), &d);
        let want = stock_alpha(128, 64, 128); // white*tex(128) = 128, alpha 128 over bg 64 = 96
        let got = px(&so, W / 2, H / 2);
        let (md, mean) = ui_pass::diff_rgb(&so, &eo);
        let ok = (got[0] as i32 - want as i32).abs() <= 1;
        worst_ok &= ok;
        println!("A tex-decode:  stock center={:?} (analytic {}) {} | engine center={:?} | Δ max={} mean={:.1}",
            got, want, if ok { "OK" } else { "FAIL" }, px(&eo, W / 2, H / 2), md, mean);
    }

    println!();
    assert!(worst_ok, "UI oracle diverged from the analytic stock contract -- the reference itself is wrong");
    println!("U0 oracle PINNED: stock contract matches analytic expectations. Engine gaps above are the U1-U4 targets.");
}
