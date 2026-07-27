//! llrust-j2c -- memory-safe Rust J2C (JPEG2000) texture decode for Firestorm.
//!
//! The untrusted-parse orchestration + validation layer that will drive a C/C++
//! codec (Grok, AGPL primary; OpenJPEG, permissive fallback) behind a bounds-
//! checked, fail-closed boundary. See `indra/rust/llrust-j2c/PLAN.md`.
//!
//! FFI discipline (matches the `llrust` crate):
//!  - `#[no_mangle] pub extern "C"` on every exported symbol.
//!  - `panic = "abort"` (Cargo.toml): never unwind across the C ABI.
//!  - Decode returns an opaque handle owning the pixel buffer; C++ reads a
//!    `LlJ2cView` into it, then calls `ll_j2c_free`. No per-field copy across FFI.
//!
//! PHASE 0: this is a stub. `ll_j2c_decode` always returns null ("not handled")
//! so the C++ `llimagej2crust` backend falls through to the existing codec. The
//! real decode (Grok/OpenJPEG via bindgen, shadow-compared bit-exact) lands in
//! Phase 1.

use std::os::raw::{c_char, c_void};

/// Version/identity string for logging + a trivial "the bridge is linked" check.
#[no_mangle]
pub extern "C" fn ll_j2c_rust_version() -> *const c_char {
    // Static NUL-terminated -- valid for the life of the process, never freed.
    b"llrust-j2c 0.1.0 (phase0-stub)\0".as_ptr() as *const c_char
}

/// A decoded-image view: a borrow into the handle-owned pixel buffer plus its
/// geometry. `#[repr(C)]` so the C++ side reads it directly (std140-free POD).
#[repr(C)]
pub struct LlJ2cView {
    pub pixels: *const u8, // borrowed; valid until ll_j2c_free(handle)
    pub len: usize,        // pixels length in bytes
    pub width: i32,
    pub height: i32,
    pub components: i32,   // 1..4
    pub discard_level: i32, // resolution level actually decoded
    pub decode_ok: i32,    // 0 = malformed/failed (fail-closed), 1 = ok
}

/// Decode a J2C codestream to a resolution/discard level.
///
/// PHASE 0 STUB: always returns null -> the C++ backend treats it as "not
/// handled" and uses the existing decoder. Real decode arrives in Phase 1.
///
/// # Safety
/// `data`/`len` describe an untrusted codestream buffer owned by the caller and
/// valid for the call. The returned handle (when non-null) is owned by Rust and
/// must be released with `ll_j2c_free`.
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
/// `handle` must be a live handle from `ll_j2c_decode`; `out` must point to a
/// writable `LlJ2cView`.
#[no_mangle]
pub extern "C" fn ll_j2c_view(_handle: *const c_void, _out: *mut LlJ2cView) -> i32 {
    // Phase 0: no handles are ever produced, so nothing to view.
    0
}

/// Release a handle from `ll_j2c_decode` (null is a no-op).
///
/// # Safety
/// `handle` must have come from `ll_j2c_decode` and not been freed already.
#[no_mangle]
pub extern "C" fn ll_j2c_free(_handle: *mut c_void) {
    // Phase 0: no handles are ever produced.
}
