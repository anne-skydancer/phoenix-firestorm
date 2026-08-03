//! Build script: link grokj2k so this cdylib resolves the Grok C-API symbols the shared `j2c`
//! crate references (the crate does the bindgen; the FINAL artifact does the link). grokj2k.dll
//! ships in newview/Release for runtime resolution. Path defaults to this box's layout (override
//! via GROK_ROOT).

fn main() {
    let grok_root = std::env::var("GROK_ROOT").unwrap_or_else(|_| "C:/fs/grok".to_string());
    let lib_dir = format!("{grok_root}/build/bin");
    println!("cargo:rerun-if-env-changed=GROK_ROOT");
    println!("cargo:rustc-link-search=native={lib_dir}");
    println!("cargo:rustc-link-lib=dylib=grokj2k");
}
