//! Generate the Grok C-API FFI from grok.h (bindgen) when the `grok` feature is on. Consumed by
//! BOTH llrust (viewer; LLRust.cmake sets LLRUST_GROK_HEADER/INCLUDES + LIBCLANG_PATH) and
//! fs_render (engine; standalone cargo, so we default the paths from GROK_ROOT). The grokj2k LINK
//! is emitted by each FINAL artifact (viewer cmake / fs_render build.rs), NOT here -- so a staticlib
//! consumer (llrust) never emits a duplicate link directive.

fn main() {
    #[cfg(feature = "grok")]
    grok::generate();
}

#[cfg(feature = "grok")]
mod grok {
    pub fn generate() {
        let out_dir = std::env::var("OUT_DIR").expect("OUT_DIR");
        let grok_root = std::env::var("GROK_ROOT").unwrap_or_else(|_| "C:/fs/grok".to_string());
        // Header + include dirs: prefer cmake-provided env (viewer), else derive from GROK_ROOT.
        let header = std::env::var("LLRUST_GROK_HEADER")
            .unwrap_or_else(|_| format!("{grok_root}/src/lib/core/grok.h"));
        let includes = std::env::var("LLRUST_GROK_INCLUDES")
            .unwrap_or_else(|_| format!("{grok_root}/src/lib/core,{grok_root}/build/src/lib/core"));
        if std::env::var("LIBCLANG_PATH").is_err() {
            std::env::set_var("LIBCLANG_PATH", "C:/Program Files/LLVM/bin");
        }

        println!("cargo:rerun-if-changed={header}");
        println!("cargo:rerun-if-env-changed=LLRUST_GROK_HEADER");
        println!("cargo:rerun-if-env-changed=LLRUST_GROK_INCLUDES");
        println!("cargo:rerun-if-env-changed=GROK_ROOT");
        println!("cargo:rerun-if-env-changed=LIBCLANG_PATH");

        // allowlist_file (not a name pattern): the GRK_*-cased types embedded by value in grk_image
        // must be generated or field access breaks. layout_tests off (decode zero-inits POD like C++).
        let mut builder = bindgen::Builder::default()
            .header(&header)
            .allowlist_file(".*grok\\.h")
            .layout_tests(false)
            .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()));
        for inc in includes.split([';', ',']).filter(|s| !s.is_empty()) {
            builder = builder.clang_arg(format!("-I{inc}"));
        }

        let bindings = builder
            .generate()
            .expect("bindgen: failed to parse grok.h (check LIBCLANG_PATH + grok include dirs)");
        bindings
            .write_to_file(std::path::Path::new(&out_dir).join("grok_sys.rs"))
            .expect("bindgen: write grok_sys.rs");
    }
}
