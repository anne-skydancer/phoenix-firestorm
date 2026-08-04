//! PROBE A/B bench: oracle softenLightF+reflectionProbeF vs fs_render's resolve.frag, with a FILLED
//! reflection-probe fixture so the reflection-probe sampling actually contributes. Exercises the ported
//! probe sampling core -- preProbeSample, sampleProbeAmbient (ambient), sampleProbes (gloss), taps,
//! sphereWeight, auto/manual mix, applyGlossEnv/applyLegacyEnv (legacy) AND pbrIbl split-sum + brdfLut
//! (PBR: radiance -> iblSpec) -- against the REAL shader.
//!
//!   - LegacySpecular: ambient via sampleProbeAmbient (non-classic), gloss via applyGlossEnv.
//!   - PbrShiny (low roughness, high metallic): iblSpec DOMINATES -> stresses pbrIbl + the brdfLut.
//!
//! Run:  SHADERC_LIB_DIR=C:/VulkanSDK/1.4.350.0/Lib cargo run --release --example probe_ab

use fs_ogl_ref::soften_pass::Material;
use fs_ogl_ref::resolve_pass::ProbeFixture;

fn main() {
    let r = fs_ogl_ref::RefEngine::new(256, 256).expect("no Vulkan adapter");

    let single = ProbeFixture::default_probe([0.30, 0.40, 0.55], [0.18, 0.22, 0.30]);
    let dual = ProbeFixture::two_probe(
        [0.30, 0.40, 0.55], [0.18, 0.22, 0.30], // probe 0 (default, layer 0)
        [0.55, 0.30, 0.20], [0.32, 0.18, 0.12], // probe 1 (manual, layer 1)
    );

    let cases: [(&str, Material, &ProbeFixture); 3] = [
        ("legacy single", Material::LegacySpecular, &single),
        ("legacy dual walk+mix", Material::LegacySpecular, &dual),
        ("pbr-shiny single (iblSpec)", Material::PbrShiny, &single),
    ];

    for (name, m, probes) in cases {
        println!("=== PROBE A/B [{name}]: oracle softenLightF+reflectionProbeF = invariant ===");
        for &(classic, tag) in &[(true, "classic"), (false, "pbr")] {
            let o = r.soften_probe_frame(&format!("C:/fs/fsref_probe_{tag}_oracle.png"), classic, m, probes);
            let ob = r.last_lum_image();
            let v = r.resolve_probe_frame(&format!("C:/fs/fsref_probe_{tag}_resolve.png"), classic, m, probes);
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
        println!();
    }
}
