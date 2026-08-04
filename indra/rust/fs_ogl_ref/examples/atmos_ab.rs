//! Atmospherics A/B bench: run the softenLight ORACLE and fs_render's resolve.frag on the SAME
//! logical fixture (plain grey PBR ground under the fixture WindLight sky), in BOTH HDR regimes, and
//! print the per-regime mean-luminance divergence. This is the empirical measurement the resolve.frag
//! rewrite is judged against -- not a hand-calc, not a screenshot.
//!
//! Run:  SHADERC_LIB_DIR=C:/VulkanSDK/1.4.350.0/Lib cargo run --release --example atmos_ab

fn main() {
    let r = fs_ogl_ref::RefEngine::new(256, 256).expect("no Vulkan adapter");
    println!("=== atmospherics A/B (linear luminance; oracle = invariant) ===");
    println!("  mean = whole-frame (includes off-center Fresnel/specular sweep);");
    println!("  center = head-on pixel (view-vector-independent diffuse math -- the P2b faithfulness gate)\n");
    for &(classic, tag) in &[(true, "classic"), (false, "pbr")] {
        let o = r.soften_frame(&format!("C:/fs/fsref_oracle_{tag}.png"), classic);
        let oc = r.last_center_lum();
        let v = r.resolve_frame(&format!("C:/fs/fsref_resolve_{tag}.png"), classic);
        let vc = r.last_center_lum();
        let ratio = if o > 1e-6 { v / o } else { f64::NAN };
        let cratio = if oc > 1e-6 { vc / oc } else { f64::NAN };
        println!("  [{tag:>7}] mean:  oracle={o:.4}  resolve={v:.4}  ratio={ratio:.3}");
        println!("  [{tag:>7}] center:oracle={oc:.4}  resolve={vc:.4}  ratio={cratio:.3}");
    }
}
