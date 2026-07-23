//! llrust: memory-safe Rust components linked into the Firestorm viewer through
//! a C ABI (`extern "C"` + cbindgen-generated header).
//!
//! Strategy: introduce Rust behind a narrow FFI seam, one self-contained module
//! at a time, starting with the untrusted **SL mesh asset decode** path
//! (`LLVolume::unpackVolumeFaces`) -- attacker-controlled bytes from the asset
//! CDN, exactly where memory safety earns its keep.
//!
//! This first commit is the toolchain bridge: `ll_rust_version()` proves that
//! cargo -> staticlib -> cbindgen -> CMake -> link into firestorm-bin works
//! end-to-end on FreeBSD, before any decoder logic depends on it.
//!
//! FFI rules for everything in this crate:
//!  - `#[no_mangle] pub extern "C"` on every exported symbol.
//!  - Never panic across the boundary (crate is built with panic = "abort").
//!  - Treat every incoming pointer/length as untrusted: bounds-check before use.

use std::os::raw::c_char;

/// Version/self-test probe. Returns a pointer to a static, NUL-terminated C
/// string owned by this crate (the caller must not free it). Proves the FFI
/// bridge links and runs.
#[no_mangle]
pub extern "C" fn ll_rust_version() -> *const c_char {
    // Static, NUL-terminated, 'static lifetime -> safe to hand to C forever.
    concat!("llrust ", env!("CARGO_PKG_VERSION"), "\0").as_ptr() as *const c_char
}

/// Cheap arithmetic self-test the C++ side can assert on at startup to confirm
/// the linked Rust code actually executes (not just links).
#[no_mangle]
pub extern "C" fn ll_rust_selftest(a: i32, b: i32) -> i32 {
    a.wrapping_add(b)
}
