//! Build script: generate the Grok C-API FFI (bindgen) + link grokj2k, so the engine can
//! host J2C decode itself (the renderer owns its texture inputs; the viewer only bridges the
//! compressed bytes). Paths default to this workstation's layout and can be overridden by env.
//!   grok.h      = $GROK_ROOT/src/lib/core/grok.h
//!   includes    = $GROK_ROOT/src/lib/core , $GROK_ROOT/build/src/lib/core
//!   import lib   = $GROK_ROOT/build/bin/grokj2k.lib   (grokj2k.dll ships in newview/Release)
//!   libclang    = $LIBCLANG_PATH or C:/Program Files/LLVM/bin

use std::path::PathBuf;

fn main() {
    let grok_root = std::env::var("GROK_ROOT").unwrap_or_else(|_| "C:/fs/grok".to_string());
    let header = format!("{grok_root}/src/lib/core/grok.h");
    let inc_core = format!("{grok_root}/src/lib/core");
    let inc_build = format!("{grok_root}/build/src/lib/core");
    let lib_dir = format!("{grok_root}/build/bin");

    // clang-sys (used by bindgen) reads LIBCLANG_PATH; default to the usual LLVM install.
    if std::env::var("LIBCLANG_PATH").is_err() {
        std::env::set_var("LIBCLANG_PATH", "C:/Program Files/LLVM/bin");
    }

    println!("cargo:rerun-if-changed={header}");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=GROK_ROOT");
    println!("cargo:rerun-if-env-changed=LIBCLANG_PATH");

    // Bindgen the whole header (allowlist_file, not a name pattern -- the GRK_*-cased types
    // embedded by value in grk_image must be generated or field access breaks). Skip layout
    // tests; the decode zero-inits POD structs like the C++ `= {}`.
    let bindings = bindgen::Builder::default()
        .header(&header)
        .allowlist_file(".*grok\\.h")
        .layout_tests(false)
        .clang_arg(format!("-I{inc_core}"))
        .clang_arg(format!("-I{inc_build}"))
        .generate()
        .expect("bindgen: failed to parse grok.h (check LIBCLANG_PATH + grok include dirs)");

    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("grok_sys.rs");
    bindings.write_to_file(&out).expect("bindgen: write grok_sys.rs");

    // Link grokj2k via its import lib; the DLL is resolved at runtime from newview/Release.
    println!("cargo:rustc-link-search=native={lib_dir}");
    println!("cargo:rustc-link-lib=dylib=grokj2k");
}
