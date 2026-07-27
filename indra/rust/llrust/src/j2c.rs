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

/// Owns the decoded interleaved pixel buffer + geometry behind the opaque handle
/// returned by `ll_j2c_decode`. Freed by `ll_j2c_free`.
struct J2cDecoded {
    pixels: Vec<u8>,
    width: i32,
    height: i32,
    components: i32,
    discard_level: i32,
}

// grk_image is bindgen-OPAQUE (forward-declared at grok.h:683, so bindgen won't
// fill it in). We read it through a #[repr(C)] view with field offsets MEASURED
// from grok.h via clang, VERIFIED at compile time below -- if Grok ever changes
// the layout, the asserts fail the build loudly (never silent corruption).
//   sizeof(grk_image)=208, x1@16, y1@20, numcomps@24, comps@200 (see build notes).
#[cfg(feature = "grok")]
#[repr(C)]
struct GrkImageView {
    _pad_obj_x0_y0: [u8; 16], // grk_object obj (8) + x0 (4) + y0 (4)
    x1: u32,                  // @16
    y1: u32,                  // @20
    numcomps: u16,            // @24
    _pad_to_comps: [u8; 200 - 26],
    comps: *mut grok_sys::grk_image_comp, // @200
}

#[cfg(feature = "grok")]
const _: () = {
    assert!(core::mem::size_of::<GrkImageView>() == 208);
    assert!(core::mem::offset_of!(GrkImageView, x1) == 16);
    assert!(core::mem::offset_of!(GrkImageView, y1) == 20);
    assert!(core::mem::offset_of!(GrkImageView, numcomps) == 24);
    assert!(core::mem::offset_of!(GrkImageView, comps) == 200);
};

// grk_header_info embeds grk_image BY VALUE at offset 0, so bindgen's struct is
// undersized (opaque grk_image = 1 byte). We pass a full-size, 8-aligned buffer to
// grk_decompress_read_header (else Grok overruns it) and read header_image at 0.
// Measured: sizeof(grk_header_info)=12632, header_image@0, align 8.
#[cfg(feature = "grok")]
const GRK_HEADER_INFO_SIZE: usize = 12632;

/// SL clamps the reduce (discard) factor to this (llimage.h MAX_DISCARD_LEVEL).
#[cfg(feature = "grok")]
const MAX_DISCARD_LEVEL: i32 = 5;

/// Decode a J2C codestream via Grok into an interleaved, vertically-flipped 8-bit
/// buffer, matching `LLImageJ2CGrok::decodeImpl` exactly (for bit-exact shadow-
/// compare). Returns an opaque handle (release with `ll_j2c_free`) or null on any
/// failure (fail-closed) so the C++ backend stays authoritative. Null without the
/// `grok` feature.
///
/// # Safety
/// `data`/`len` describe an untrusted codestream owned by the caller, valid for
/// the call.
#[no_mangle]
pub extern "C" fn ll_j2c_decode(
    data: *const u8,
    len: usize,
    discard_level: i32,
    first_channel: i32,
    max_channel_count: i32,
) -> *mut c_void {
    #[cfg(feature = "grok")]
    {
        unsafe { grok_decode(data, len, discard_level, first_channel, max_channel_count) }
    }
    #[cfg(not(feature = "grok"))]
    {
        let _ = (data, len, discard_level, first_channel, max_channel_count);
        std::ptr::null_mut()
    }
}

#[cfg(feature = "grok")]
unsafe fn grok_decode(
    data: *const u8,
    len: usize,
    discard_level: i32,
    first_channel: i32,
    max_channel_count: i32,
) -> *mut c_void {
    if data.is_null() || len == 0 {
        return std::ptr::null_mut();
    }

    // Read from the caller's untrusted buffer. Grok only reads it (is_read_stream);
    // the C API takes *mut, so cast away const (Grok never writes here).
    let mut stream_params: grok_sys::grk_stream_params = std::mem::zeroed();
    stream_params.buf = data as *mut u8;
    stream_params.buf_len = len;
    stream_params.is_read_stream = true;

    let mut params: grok_sys::grk_decompress_parameters = std::mem::zeroed();
    params.core.reduce = discard_level.clamp(0, MAX_DISCARD_LEVEL) as u8;
    params.num_threads = 1; // one worker per decode (matches the C++ concurrency model)

    let codec = grok_sys::grk_decompress_init(&mut stream_params, &mut params);
    if codec.is_null() {
        return std::ptr::null_mut();
    }
    // Release the codec on EVERY exit path (fail-closed).
    struct CodecGuard(*mut grok_sys::grk_object);
    impl Drop for CodecGuard {
        fn drop(&mut self) {
            unsafe { grok_sys::grk_object_unref(self.0) };
        }
    }
    let _guard = CodecGuard(codec);

    // Full-size, 8-aligned scratch for grk_header_info (see GRK_HEADER_INFO_SIZE).
    let mut hdr = vec![0u64; GRK_HEADER_INFO_SIZE.div_ceil(8)];
    if !grok_sys::grk_decompress_read_header(codec, hdr.as_mut_ptr() as *mut grok_sys::grk_header_info) {
        return std::ptr::null_mut();
    }
    let header_image = &*(hdr.as_ptr() as *const GrkImageView); // header_image @ offset 0
    let numcomps = header_image.numcomps as i32;

    let channels = (numcomps - first_channel).min(max_channel_count).max(0);
    if channels <= 0 {
        return std::ptr::null_mut();
    }

    if !grok_sys::grk_decompress(codec, std::ptr::null_mut()) {
        return std::ptr::null_mut();
    }

    let image_ptr = grok_sys::grk_decompress_get_image(codec);
    if image_ptr.is_null() {
        return std::ptr::null_mut();
    }
    let image = &*(image_ptr as *const GrkImageView);
    if image.numcomps == 0 || image.comps.is_null() {
        return std::ptr::null_mut();
    }

    // Full-image decompress: comps[0].(w,h) is the decoded rectangle.
    let width = (*image.comps).w;
    let height = (*image.comps).h;
    if width == 0 || height == 0 {
        return std::ptr::null_mut();
    }

    let out_len = (width as usize) * (height as usize) * (channels as usize);
    let mut pixels = vec![0u8; out_len];

    // Interleaved output, rows copied BOTTOM-UP (J2C top-down -> LL raw bottom-up),
    // per-component GRK_INT_16 vs GRK_INT_32 sample width. Identical to decodeImpl.
    for dest in 0..channels {
        let c = &*image.comps.offset((first_channel + dest) as isize);
        if c.data.is_null() {
            return std::ptr::null_mut();
        }
        let stride = c.stride as usize;
        let mut offset = dest as usize;
        if c.data_type == grok_sys::_grk_data_type_GRK_INT_16 {
            let src = c.data as *const i16;
            for y in (0..height).rev() {
                let row = (y as usize) * stride;
                for x in 0..width as usize {
                    pixels[offset] = *src.add(row + x) as u8;
                    offset += channels as usize;
                }
            }
        } else {
            let src = c.data as *const i32;
            for y in (0..height).rev() {
                let row = (y as usize) * stride;
                for x in 0..width as usize {
                    pixels[offset] = *src.add(row + x) as u8;
                    offset += channels as usize;
                }
            }
        }
    }

    Box::into_raw(Box::new(J2cDecoded {
        pixels,
        width: width as i32,
        height: height as i32,
        components: channels,
        discard_level,
    })) as *mut c_void
}

/// Read the decoded-image view for a handle from `ll_j2c_decode`.
/// Returns 1 on success (fills `*out`), 0 if `handle`/`out` is null.
///
/// # Safety
/// `handle` must be a live handle from `ll_j2c_decode`; `out` a writable `LlJ2cView`.
/// The `pixels` pointer in `*out` borrows the handle -- valid until `ll_j2c_free`.
#[no_mangle]
pub extern "C" fn ll_j2c_view(handle: *const c_void, out: *mut LlJ2cView) -> i32 {
    if handle.is_null() || out.is_null() {
        return 0;
    }
    unsafe {
        let d = &*(handle as *const J2cDecoded);
        (*out).pixels = d.pixels.as_ptr();
        (*out).len = d.pixels.len();
        (*out).width = d.width;
        (*out).height = d.height;
        (*out).components = d.components;
        (*out).discard_level = d.discard_level;
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
        unsafe { drop(Box::from_raw(handle as *mut J2cDecoded)) };
    }
}

// --- Grok C-API bridge -------------------------------------------------------
// The Grok C API FFI is bindgen-generated from grok.h at build time (see build.rs)
// -- compiler-verified struct layouts, no hand-written offsets. Present only under
// the `grok` feature (enabled by LLRust.cmake for USE_GROK builds); a non-Grok
// build never references grk_*. grokj2k resolves the symbols at the final viewer
// link (it is already linked via llimagej2cgrok).
#[cfg(feature = "grok")]
#[allow(non_upper_case_globals, non_camel_case_types, non_snake_case, dead_code, unused_imports)]
mod grok_sys {
    include!(concat!(env!("OUT_DIR"), "/grok_sys.rs"));
}

/// Grok's own version string, obtained by calling into libgrok FROM RUST via the
/// bindgen-generated binding. Null when built without the `grok` feature. Proves
/// the Rust->Grok C-ABI bridge links and executes.
#[no_mangle]
pub extern "C" fn ll_j2c_grok_version() -> *const c_char {
    #[cfg(feature = "grok")]
    {
        // Safety: grk_version() takes no args, returns a static C string owned by
        // libgrok; we only borrow it (never free).
        unsafe { grok_sys::grk_version() }
    }
    #[cfg(not(feature = "grok"))]
    {
        std::ptr::null()
    }
}
