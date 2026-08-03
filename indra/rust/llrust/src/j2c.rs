//! j2c -- the viewer's C ABI over the SHARED `j2c` decoder crate (single source, also used by the
//! Vulkan engine `fs_render`). This file is now thin FFI glue: the decode + validation + Grok
//! bindgen live in the crate; here we only expose the `ll_j2c_*` symbols the C++ viewer links and
//! marshal to/from the crate's Rust API.
//!
//! Init ownership: the viewer's C++ calls grk_initialize itself, so `ll_j2c_decode` uses the crate's
//! assume-init `decode()` (it does NOT re-init). The engine side owns init via `j2c::ensure_init`.

use std::os::raw::{c_char, c_void};

/// Version/identity for logging + a "the j2c bridge is linked" check.
#[no_mangle]
pub extern "C" fn ll_j2c_rust_version() -> *const c_char {
    concat!("llrust-j2c ", env!("CARGO_PKG_VERSION"), " (shared-crate)\0").as_ptr() as *const c_char
}

/// A decoded-image view: a borrow into the handle-owned pixel buffer plus geometry. `#[repr(C)]`
/// POD the C++ side reads directly. Unchanged ABI from the pre-refactor decoder.
#[repr(C)]
pub struct LlJ2cView {
    pub pixels: *const u8,  // borrowed; valid until ll_j2c_free(handle)
    pub len: usize,
    pub width: i32,
    pub height: i32,
    pub components: i32,
    pub discard_level: i32,
    pub decode_ok: i32,
}

/// Owns the decoded image behind the opaque handle returned by `ll_j2c_decode`.
struct Handle {
    img: j2c::DecodedJ2c,
    discard_level: i32,
}

/// Decode a J2C codestream (untrusted, caller-owned) via the shared crate. Returns an opaque handle
/// (release with `ll_j2c_free`) or null on any failure (fail-closed). Null without the `grok`
/// feature (the crate's decode() returns None), so the C++ backend stays authoritative.
///
/// # Safety
/// `data`/`len` describe a codestream owned by the caller, valid for the call.
#[no_mangle]
pub extern "C" fn ll_j2c_decode(data: *const u8, len: usize, discard_level: i32, first_channel: i32, max_channel_count: i32) -> *mut c_void {
    if data.is_null() || len == 0 {
        return std::ptr::null_mut();
    }
    let bytes = unsafe { std::slice::from_raw_parts(data, len) };
    // The viewer's C++ owns grk_initialize; the crate's decode() assumes init (does not re-init).
    match j2c::decode(bytes, discard_level, first_channel, max_channel_count) {
        Some(img) => Box::into_raw(Box::new(Handle { img, discard_level })) as *mut c_void,
        None => std::ptr::null_mut(),
    }
}

/// Read the decoded-image view for a handle. Returns 1 on success, 0 if `handle`/`out` is null.
///
/// # Safety
/// `handle` must be a live handle from `ll_j2c_decode`; `out` a writable `LlJ2cView`. The `pixels`
/// pointer borrows the handle -- valid until `ll_j2c_free`.
#[no_mangle]
pub extern "C" fn ll_j2c_view(handle: *const c_void, out: *mut LlJ2cView) -> i32 {
    if handle.is_null() || out.is_null() {
        return 0;
    }
    unsafe {
        let h = &*(handle as *const Handle);
        (*out).pixels = h.img.pixels.as_ptr();
        (*out).len = h.img.pixels.len();
        (*out).width = h.img.width as i32;
        (*out).height = h.img.height as i32;
        (*out).components = h.img.components as i32;
        (*out).discard_level = h.discard_level;
        (*out).decode_ok = 1;
    }
    1
}

/// Release a handle from `ll_j2c_decode` (null is a no-op).
///
/// # Safety
/// `handle` must have come from `ll_j2c_decode` and not been freed already.
#[no_mangle]
pub extern "C" fn ll_j2c_free(handle: *mut c_void) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle as *mut Handle)) };
    }
}

/// Verdict from `ll_j2c_validate`. `#[repr(C)]` POD the C++ side reads directly.
#[repr(C)]
pub struct LlJ2cVerdict {
    pub accept: i32,
    pub reason: i32,
    pub width: i32,
    pub height: i32,
    pub components: i32,
}

/// Ingress gate: validate an untrusted J2C codestream against the SL profile WITHOUT decoding.
/// Fills `*out`, returns 1 (accept) / 0 (reject). Null-safe. Pure safe Rust (crate side).
///
/// # Safety
/// `data`/`len` describe a buffer owned by the caller, valid for the call; `out` a writable verdict.
#[no_mangle]
pub extern "C" fn ll_j2c_validate(data: *const u8, len: usize, out: *mut LlJ2cVerdict) -> i32 {
    if out.is_null() {
        return 0;
    }
    let v = if data.is_null() || len == 0 {
        j2c::Verdict { reason: j2c::LL_J2C_REJECT_TOO_SHORT, width: 0, height: 0, components: 0 }
    } else {
        let slice = unsafe { std::slice::from_raw_parts(data, len) };
        j2c::validate(slice)
    };
    let accept = if v.reason == j2c::LL_J2C_OK { 1 } else { 0 };
    unsafe {
        (*out).accept = accept;
        (*out).reason = v.reason;
        (*out).width = v.width;
        (*out).height = v.height;
        (*out).components = v.components;
    }
    accept
}

/// Grok's version string via the shared crate. Null without the `grok` feature.
#[no_mangle]
pub extern "C" fn ll_j2c_grok_version() -> *const c_char {
    j2c::grok_version()
}
