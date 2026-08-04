//! PROBE GENERATION A/B: fs_render's convolution generators vs the REAL radianceGenF / irradianceGenF.
//! Both sample an identical direction-varying source cube with an identical per-pixel direction, so any
//! per-pixel divergence is a fragment-math / wgpu-transform bug in the port.
//!
//!   - radiance: swept across mip levels (roughness = mip/max_probe_lod -> sample count + LOD).
//!   - irradiance: the cosine convolution (32 samples, LOD bias +2, u_width hardcoded 64, fixed TBN).
//!
//! Run:  SHADERC_LIB_DIR=C:/VulkanSDK/1.4.350.0/Lib cargo run --release --example gen_ab

use fs_ogl_ref::gen_pass::GenKind;

fn diff(o: &[f64], v: &[f64]) -> (f64, f64) {
    let (mut max_d, mut sum_d) = (0f64, 0f64);
    for (a, b) in o.iter().zip(v.iter()) {
        let d = (a - b).abs();
        if d > max_d { max_d = d; }
        sum_d += d;
    }
    (max_d, sum_d / o.len().max(1) as f64)
}

fn main() {
    let r = fs_ogl_ref::RefEngine::new(128, 128).expect("no Vulkan adapter");
    let max_probe_lod = 6.0; // log2(128)-1

    println!("=== GEN A/B: radiance prefilter (radianceGenF), oracle = invariant ===");
    for &mip in &[0.0f32, 1.0, 3.0, 6.0] {
        let o = r.gen_frame(&format!("C:/fs/fsref_gen_rad_o_m{mip}.png"), GenKind::Radiance, true, mip, max_probe_lod);
        let ob = r.last_lum_image();
        let v = r.gen_frame(&format!("C:/fs/fsref_gen_rad_v_m{mip}.png"), GenKind::Radiance, false, mip, max_probe_lod);
        let vb = r.last_lum_image();
        let (max_d, mean_d) = diff(&ob, &vb);
        let ratio = if o > 1e-9 { v / o } else { f64::NAN };
        println!("  [mip {mip:>3}] oracle={o:.4} ours={v:.4} ratio={ratio:.4} | per-pixel Δ max={max_d:.4} mean={mean_d:.5}");
    }

    println!("\n=== GEN A/B: irradiance convolution (irradianceGenF), oracle = invariant ===");
    let o = r.gen_frame("C:/fs/fsref_gen_irr_o.png", GenKind::Irradiance, true, 0.0, max_probe_lod);
    let ob = r.last_lum_image();
    let v = r.gen_frame("C:/fs/fsref_gen_irr_v.png", GenKind::Irradiance, false, 0.0, max_probe_lod);
    let vb = r.last_lum_image();
    let (max_d, mean_d) = diff(&ob, &vb);
    let ratio = if o > 1e-9 { v / o } else { f64::NAN };
    println!("  [irrad ] oracle={o:.4} ours={v:.4} ratio={ratio:.4} | per-pixel Δ max={max_d:.4} mean={mean_d:.5}");
}
