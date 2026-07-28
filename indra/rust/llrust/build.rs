//! Build script for llrust.
//!
//! When the `grok` feature is on, generate the Grok C API FFI from grok.h using
//! bindgen (libclang), so struct layouts are compiler-verified against the same
//! header the C++ side compiles -- no hand-written offsets. LLRust.cmake passes
//! the header path, include dirs, and LIBCLANG_PATH via env (it knows GROK_ROOT).
//! Output: $OUT_DIR/grok_sys.rs, included by src/j2c.rs.
//!
//! Without the `grok` feature this does nothing (and bindgen isn't even a dep).

fn main() {
    #[cfg(feature = "grok")]
    grok::generate();
}

#[cfg(feature = "grok")]
mod grok {
    pub fn generate() {
        let out_dir = std::env::var("OUT_DIR").expect("OUT_DIR");
        let header = std::env::var("LLRUST_GROK_HEADER").expect(
            "LLRUST_GROK_HEADER not set -- LLRust.cmake passes the grok.h path for \
             USE_GROK + LL_RUSTJ2C builds",
        );
        let includes = std::env::var("LLRUST_GROK_INCLUDES").unwrap_or_default();

        println!("cargo:rerun-if-changed={header}");
        println!("cargo:rerun-if-env-changed=LLRUST_GROK_HEADER");
        println!("cargo:rerun-if-env-changed=LLRUST_GROK_INCLUDES");

        let mut builder = bindgen::Builder::default()
            .header(&header)
            // Generate everything declared in grok.h (functions, structs, enums,
            // consts) + their dependent types. A name-pattern allowlist ("grk_.*")
            // MISSES the GRK_*-cased types embedded BY VALUE in grk_image
            // (GRK_COLOR_SPACE, GRK_SUPPORTED_FILE_FMT, ...), which makes bindgen
            // opaque grk_image and breaks field access. allowlist_file avoids that.
            .allowlist_file(".*grok\\.h")
            // FFI POD structs -- skip generated layout-assert tests (noise) and let
            // the decode zero-init with mem::zeroed() (C++ uses `= {}`).
            .layout_tests(false)
            // Header is unchanging system-ish input; don't rebuild on every touch.
            .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()));

        for inc in includes.split([';', ',']).filter(|s| !s.is_empty()) {
            builder = builder.clang_arg(format!("-I{inc}"));
        }

        let bindings = builder
            .generate()
            .expect("bindgen: failed to parse grok.h (check LIBCLANG_PATH + include dirs)");

        let out = std::path::Path::new(&out_dir).join("grok_sys.rs");
        bindings
            .write_to_file(&out)
            .expect("bindgen: failed to write grok_sys.rs");
    }
}
