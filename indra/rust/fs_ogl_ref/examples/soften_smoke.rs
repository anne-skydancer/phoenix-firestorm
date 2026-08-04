//! Smoke: build the REAL softenLight fragment shader module headless (assemble the viewer's way ->
//! UBO/sampler transform -> Vulkan SPIR-V -> wgpu module via passthrough). Proves fs_ogl_ref runs
//! the actual OGL atmospherics shader on this machine's Vulkan driver -- the foundation the
//! atmospherics pass (UBO + G-buffer/texture feed) builds on.
//!
//! Run:  SHADERC_LIB_DIR=C:/VulkanSDK/1.4.350.0/Lib cargo run --release --example soften_smoke

fn main() {
    let r = fs_ogl_ref::RefEngine::new(256, 256).expect("no Vulkan adapter");
    let (_module, members, samplers) = r.build_soften_module();
    println!("softenLight module built OK: {members} UBO members, {samplers} samplers");
}
