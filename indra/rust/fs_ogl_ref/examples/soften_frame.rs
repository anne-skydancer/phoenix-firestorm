//! Render the REAL OGL softenLight on a fixture (plain PBR ground under a default WindLight sky) and
//! write the linear-HDR result to a PNG -- the atmospherics ORACLE frame. Runs BOTH HDR regimes:
//! classic_mode=1 (legacy WindLight, auto-adjust off) and classic_mode=0 (PBR/EEP sky), since
//! softenLightF's lit branches diverge sharply between them and fs_render's resolve.frag must match
//! whichever regime the scene selects. The mean-luminance line is the magnitude signal.
//!
//! Run:  SHADERC_LIB_DIR=C:/VulkanSDK/1.4.350.0/Lib cargo run --release --example soften_frame

fn main() {
    let r = fs_ogl_ref::RefEngine::new(256, 256).expect("no Vulkan adapter");
    let lc = r.soften_frame("C:/fs/fsref_soften_classic.png", true);
    let lp = r.soften_frame("C:/fs/fsref_soften_pbr.png", false);
    println!("wrote C:/fs/fsref_soften_classic.png ({:.4}) and C:/fs/fsref_soften_pbr.png ({:.4})", lc, lp);
}
