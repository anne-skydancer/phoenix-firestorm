//! PBR A/B bench: oracle softenLightF vs fs_render's resolve.frag on a SHINY PBR material (low
//! roughness + high metallic), where the GGX punctual specular DOMINATES -- so this stresses the
//! full pbrPunctual F/G/D + the pbrBaseLight combination, not just the diffuse. Both HDR regimes.
//!
//! Run:  SHADERC_LIB_DIR=C:/VulkanSDK/1.4.350.0/Lib cargo run --release --example pbr_ab

fn main() {
    let r = fs_ogl_ref::RefEngine::new(256, 256).expect("no Vulkan adapter");
    let m = fs_ogl_ref::soften_pass::Material::PbrShiny;
    println!("=== PBR A/B: shiny metal (GGX specular dominates), oracle = invariant ===\n");
    for &(classic, tag) in &[(true, "classic"), (false, "pbr")] {
        let o = r.soften_frame(&format!("C:/fs/fsref_pbr_oracle_{tag}.png"), classic, m);
        let ob = r.last_lum_image();
        let v = r.resolve_frame(&format!("C:/fs/fsref_pbr_resolve_{tag}.png"), classic, m);
        let vb = r.last_lum_image();
        let ratio = if o > 1e-6 { v / o } else { f64::NAN };
        let (mut max_d, mut sum_d) = (0f64, 0f64);
        for (a, b) in ob.iter().zip(vb.iter()) {
            let d = (a - b).abs();
            if d > max_d { max_d = d; }
            sum_d += d;
        }
        let mean_d = sum_d / ob.len().max(1) as f64;
        println!("  [{tag:>7}] mean: oracle={o:.4} resolve={v:.4} ratio={ratio:.3} | per-pixel Δ max={max_d:.3} mean={mean_d:.4}");
    }
}
