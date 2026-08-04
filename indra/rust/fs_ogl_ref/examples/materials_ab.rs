//! Materials A/B bench + VIEW-VECTOR PROOF. Runs the softenLight ORACLE and fs_render's resolve.frag
//! on the SAME legacy specular material (HAS_ATMOS, spec color + glossiness), both HDR regimes.
//!
//! The point: the oracle computes v = -normalize(reconstructed eye pos) from the DEPTH buffer; resolve
//! computes v from the pixel's view RAY (inv_view_proj, no depth). The Blinn-Phong specular is brutally
//! v-sensitive (pow(nh, gloss^2*368)). If the two mean luminances match, the ray-derived v is the
//! geometric IDENTITY claimed (proven, not asserted); if they diverge, the "identity" leaked a
//! dependency and the view vector needs the full depth reconstruction instead.
//!
//! Run:  SHADERC_LIB_DIR=C:/VulkanSDK/1.4.350.0/Lib cargo run --release --example materials_ab

fn main() {
    let r = fs_ogl_ref::RefEngine::new(256, 256).expect("no Vulkan adapter");
    let m = fs_ogl_ref::soften_pass::Material::LegacySpecular;
    println!("=== materials A/B: legacy Blinn-Phong specular (oracle depth-v vs resolve ray-v) ===");
    println!("  match => the ray-derived view vector is the identity; diverge => needs full depth reconstruction\n");
    for &(classic, tag) in &[(true, "classic"), (false, "pbr")] {
        let o = r.soften_frame(&format!("C:/fs/fsref_mat_oracle_{tag}.png"), classic, m);
        let ob = r.last_lum_image();
        let v = r.resolve_frame(&format!("C:/fs/fsref_mat_resolve_{tag}.png"), classic, m);
        let vb = r.last_lum_image();
        let ratio = if o > 1e-6 { v / o } else { f64::NAN };
        // PER-PIXEL diff -- catches position errors (mirrored highlight) the mean hides.
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
