//! j2c -- memory-safe Rust J2C (JPEG2000) texture decode.
//!
//! The untrusted-parse orchestration layer that will drive a C/C++ codec (Grok,
//! AGPL primary; OpenJPEG, permissive fallback) behind a bounds-checked, fail-
//! closed boundary. Lives as a module of `llrust` (not a separate staticlib --
//! two Rust staticlibs cannot co-link). The heavy Grok/OpenJPEG deps arrive in
//! Phase 1 feature-gated in llrust's Cargo.toml so the default build stays lean.
//! See indra/rust/llrust-j2c/PLAN.md.
//!
//! PHASE 0: stub. `ll_j2c_decode` always returns null ("not handled") so the C++
//! side falls through to the existing codec. Real decode (Grok via bindgen,
//! shadow-compared bit-exact) lands in Phase 1.

use std::os::raw::{c_char, c_void};

/// Version/identity for logging + a "the j2c bridge is linked" check.
#[no_mangle]
pub extern "C" fn ll_j2c_rust_version() -> *const c_char {
    concat!("llrust-j2c ", env!("CARGO_PKG_VERSION"), " (phase0-stub)\0").as_ptr() as *const c_char
}

/// A decoded-image view: a borrow into the handle-owned pixel buffer plus its
/// geometry. `#[repr(C)]` POD the C++ side reads directly.
#[repr(C)]
pub struct LlJ2cView {
    pub pixels: *const u8,  // borrowed; valid until ll_j2c_free(handle)
    pub len: usize,         // pixels length in bytes
    pub width: i32,
    pub height: i32,
    pub components: i32,    // 1..4
    pub discard_level: i32, // resolution level actually decoded
    pub decode_ok: i32,     // 0 = malformed/failed (fail-closed), 1 = ok
}

/// Decode a J2C codestream to a resolution/discard level.
///
/// PHASE 0 STUB: always returns null -> the C++ backend treats it as "not
/// handled" and uses the existing decoder.
///
/// # Safety
/// `data`/`len` describe an untrusted codestream owned by the caller, valid for
/// the call. A non-null return is a Rust-owned handle; release with `ll_j2c_free`.
#[no_mangle]
pub extern "C" fn ll_j2c_decode(
    _data: *const u8,
    _len: usize,
    _discard_level: i32,
) -> *mut c_void {
    std::ptr::null_mut()
}

/// Read the decoded-image view for a handle from `ll_j2c_decode`.
/// Returns 1 on success (fills `*out`), 0 if `handle`/`out` is null.
///
/// # Safety
/// `handle` must be a live handle from `ll_j2c_decode`; `out` a writable `LlJ2cView`.
#[no_mangle]
pub extern "C" fn ll_j2c_view(_handle: *const c_void, _out: *mut LlJ2cView) -> i32 {
    0 // Phase 0: no handles are ever produced.
}

/// Release a handle from `ll_j2c_decode` (null is a no-op).
///
/// # Safety
/// `handle` must have come from `ll_j2c_decode` and not been freed already.
#[no_mangle]
pub extern "C" fn ll_j2c_free(_handle: *mut c_void) {
    // Phase 0: no handles are ever produced.
}

// --- Grok C-API bridge (Phase 1a: de-risk the Rust -> Grok link) --------------
// bindgen is unavailable here (no libclang), so the Grok C API is declared BY
// HAND. Phase 1a needs only grk_version() -- no structs -- to prove the Rust ->
// Grok C symbol resolves at the final viewer link (grokj2k is already linked via
// llimagej2cgrok). The `grok` cargo feature is enabled by LLRust.cmake only for
// USE_GROK builds, so a non-Grok (e.g. OpenJPEG) build never references grk_*.
#[cfg(feature = "grok")]
extern "C" {
    fn grk_version() -> *const c_char;
}

/// Grok's own version string, obtained by calling into libgrok FROM RUST. Returns
/// null when built without the `grok` feature. Proves the Rust->Grok C-ABI bridge
/// links and executes.
#[no_mangle]
pub extern "C" fn ll_j2c_grok_version() -> *const c_char {
    #[cfg(feature = "grok")]
    {
        // Safety: grk_version() takes no args and returns a static C string owned
        // by libgrok; we only borrow it (never free).
        unsafe { grk_version() }
    }
    #[cfg(not(feature = "grok"))]
    {
        std::ptr::null()
    }
}
