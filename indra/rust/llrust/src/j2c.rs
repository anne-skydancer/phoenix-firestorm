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
    let out_w = (*image.comps).w as usize;
    let out_h = (*image.comps).h as usize;
    if out_w == 0 || out_h == 0 {
        return std::ptr::null_mut();
    }

    // out_len = out_w * out_h * channels, with overflow guards so a hostile
    // dimension DISCARDS (null) instead of wrapping to a short buffer.
    let out_len = match out_w
        .checked_mul(out_h)
        .and_then(|a| a.checked_mul(channels as usize))
    {
        Some(n) => n,
        None => return std::ptr::null_mut(),
    };
    let mut pixels = vec![0u8; out_len];

    // Interleaved output, rows copied BOTTOM-UP (J2C top-down -> LL raw bottom-up),
    // per-component GRK_INT_16 vs GRK_INT_32 sample width. Byte-identical to
    // LLImageJ2CGrok::decodeImpl for well-formed images, but SELF-VALIDATING
    // against Grok's reported geometry (see copy_plane): a malformed/degenerate
    // image is discarded, never read out of bounds.
    for dest in 0..channels {
        let c = &*image.comps.offset((first_channel + dest) as isize);
        if c.data.is_null() {
            return std::ptr::null_mut();
        }
        // Uniform-geometry policy: SL textures are RGB/RGBA with every component
        // at the image rectangle. A subsampled/degenerate component (different
        // w/h, or stride < width) is not something we interleave -- discard.
        if c.w as usize != out_w || c.h as usize != out_h || (c.stride as usize) < out_w {
            return std::ptr::null_mut();
        }
        let stride = c.stride as usize;
        let ok = if c.data_type == grok_sys::_grk_data_type_GRK_INT_16 {
            copy_plane(c.data as *const i16, stride, out_w, out_h,
                       &mut pixels, dest as usize, channels as usize, |s| s as u8)
        } else {
            copy_plane(c.data as *const i32, stride, out_w, out_h,
                       &mut pixels, dest as usize, channels as usize, |s| s as u8)
        };
        if !ok {
            return std::ptr::null_mut();
        }
    }

    Box::into_raw(Box::new(J2cDecoded {
        pixels,
        width: out_w as i32,
        height: out_h as i32,
        components: channels,
        discard_level,
    })) as *mut c_void
}

/// Copy one decoded component plane into the interleaved output, bottom-up,
/// narrowing each sample to 8 bits. SELF-VALIDATING: `data` is viewed as a slice
/// of EXACTLY `stride * out_h` samples (Grok's per-component allocation contract),
/// and every source read goes through a bounds-checked per-row sub-slice -- so
/// bogus geometry from Grok yields `false` (caller discards), never an OOB read.
/// FAST: one bounds check per row, then a plain iterator over the row (no
/// per-sample checks), which the optimizer lowers to a tight copy.
///
/// # Safety
/// `data` must point to at least `stride * out_h` readable `T` (Grok's contract
/// for a component buffer). Caller guarantees `out_w <= stride`.
#[cfg(feature = "grok")]
unsafe fn copy_plane<T: Copy>(
    data: *const T,
    stride: usize,
    out_w: usize,
    out_h: usize,
    pixels: &mut [u8],
    dest: usize,
    channels: usize,
    to_u8: impl Fn(T) -> u8,
) -> bool {
    // Component buffer length per Grok's allocation contract. Overflow -> reject.
    let buf_len = match stride.checked_mul(out_h) {
        Some(n) => n,
        None => return false,
    };
    let samples = std::slice::from_raw_parts(data, buf_len);
    let mut offset = dest;
    for y in (0..out_h).rev() {
        let row = y * stride; // y < out_h and stride*out_h fits usize => no overflow
        let end = match row.checked_add(out_w) {
            Some(e) => e,
            None => return false,
        };
        // ONE bounds check for the whole row; OOB (bad geometry) -> discard.
        let src_row = match samples.get(row..end) {
            Some(s) => s,
            None => return false,
        };
        for &sample in src_row {
            // Write into our own out_len-sized buffer; offset is provably
            // < pixels.len() for consistent geometry (checked_mul above).
            pixels[offset] = to_u8(sample);
            offset += channels;
        }
    }
    true
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

// --- Ingress gate: pre-decode codestream validation --------------------------
// A pure, memory-safe (NO `unsafe`) walk of the J2C main header that refuses
// hostile/exotic streams BEFORE Grok's C++ parser is ever invoked. It pins the
// input to the narrow profile SL actually uses -- Part-1 codestream, 1..4
// components, 8-bit, NO subsampling, sane dimensions -- so decompression bombs
// and the least-battle-tested Grok code paths (HTJ2K, subsampled/high-depth) are
// discarded at the door. This does NOT (cannot) validate the entropy-coded packet
// data; it is the ingress half of a two-gate design (the egress half is the
// self-validating copy in grok_decode). See indra/rust/llrust-j2c/PLAN.md.

/// Rejection reason codes (0 = accepted). Kept stable for log analysis.
pub const LL_J2C_OK: i32 = 0;
pub const LL_J2C_REJECT_TOO_SHORT: i32 = 1; // truncated before the header parses
pub const LL_J2C_REJECT_NO_SOC: i32 = 2; // missing SOC (0xFF4F) start marker
pub const LL_J2C_REJECT_NO_SIZ: i32 = 3; // SIZ (0xFF51) not immediately after SOC
pub const LL_J2C_REJECT_BAD_SIZ_LEN: i32 = 4; // Lsiz != 38 + 3*Csiz (malformed)
pub const LL_J2C_REJECT_BAD_DIMS: i32 = 5; // zero/negative image rectangle
pub const LL_J2C_REJECT_DIMS_TOO_LARGE: i32 = 6; // decompression-bomb dimensions
pub const LL_J2C_REJECT_BAD_COMPONENTS: i32 = 7; // Csiz 0 or > MAX_COMPONENTS
pub const LL_J2C_REJECT_SUBSAMPLED: i32 = 8; // XRsiz/YRsiz != 1 on some component
pub const LL_J2C_REJECT_DEPTH: i32 = 9; // bit depth outside policy

/// Policy caps. Deliberately GENEROUS on first ship so the shadow (log-only)
/// pass measures a real false-positive rate before we ever enforce; tighten from
/// data. MAX_DIM=4096 kills the 65535-square bomb while clearing any legit SL
/// texture (upload max is 1024) with wide margin.
const LL_J2C_MAX_DIM: u32 = 4096;
// Components: a generous sanity ceiling, NOT the SL norm. Real SL textures are
// 1/3/4 channel, but 5-component codestreams occur in the wild and Grok decodes
// them fine (the viewer takes only the channels it needs) -- so the real bomb
// guard is the sample BUDGET below (dims x components), not a tight comp cap.
// (Shadow-log measured: a MAX_COMPONENTS=4 cap false-rejected real 5-comp textures.)
const LL_J2C_MAX_COMPONENTS: u16 = 64;
const LL_J2C_MAX_BITDEPTH: u8 = 8;
// Total declared samples (width * height * components) Grok would allocate for.
// 4096*4096*8 clears any real texture (<=4096 per dim, <=8 comps at full res)
// while the 65535-square / high-component bombs blow past it by ~100x.
const LL_J2C_MAX_SAMPLES: u64 = 4096 * 4096 * 8;

/// Verdict from `ll_j2c_validate`. `#[repr(C)]` POD the C++ side reads directly.
#[repr(C)]
pub struct LlJ2cVerdict {
    pub accept: i32, // 1 = passes policy, 0 = reject
    pub reason: i32, // one of LL_J2C_* above (0 when accepted)
    pub width: i32,  // parsed image width (0 if not parsed that far)
    pub height: i32,
    pub components: i32,
}

#[inline]
fn be_u16(d: &[u8], off: usize) -> Option<u16> {
    let b = d.get(off..off + 2)?;
    Some(u16::from_be_bytes([b[0], b[1]]))
}

#[inline]
fn be_u32(d: &[u8], off: usize) -> Option<u32> {
    let b = d.get(off..off + 4)?;
    Some(u32::from_be_bytes([b[0], b[1], b[2], b[3]]))
}

/// Pure validator. Returns (reason, width, height, components); reason==LL_J2C_OK
/// means accept. All reads are bounds-checked -- a truncated/hostile buffer can
/// only produce a reject, never a panic or out-of-bounds access.
fn validate_j2c(data: &[u8]) -> (i32, i32, i32, i32) {
    // SOC then SIZ, back to back, at the very start of a raw codestream.
    match be_u16(data, 0) {
        Some(0xFF4F) => {}
        Some(_) => return (LL_J2C_REJECT_NO_SOC, 0, 0, 0),
        None => return (LL_J2C_REJECT_TOO_SHORT, 0, 0, 0),
    }
    match be_u16(data, 2) {
        Some(0xFF51) => {}
        Some(_) => return (LL_J2C_REJECT_NO_SIZ, 0, 0, 0),
        None => return (LL_J2C_REJECT_TOO_SHORT, 0, 0, 0),
    }
    // SIZ layout: Lsiz@4 Rsiz@6 Xsiz@8 Ysiz@12 XOsiz@16 YOsiz@20 XTsiz@24
    // YTsiz@28 XTOsiz@32 YTOsiz@36 Csiz@40, then Csiz*(Ssiz,XRsiz,YRsiz) from @42.
    let lsiz = match be_u16(data, 4) {
        Some(v) => v,
        None => return (LL_J2C_REJECT_TOO_SHORT, 0, 0, 0),
    };
    let (xsiz, ysiz, xosiz, yosiz, csiz) = match (
        be_u32(data, 8),
        be_u32(data, 12),
        be_u32(data, 16),
        be_u32(data, 20),
        be_u16(data, 40),
    ) {
        (Some(a), Some(b), Some(c), Some(d), Some(e)) => (a, b, c, d, e),
        _ => return (LL_J2C_REJECT_TOO_SHORT, 0, 0, 0),
    };

    // Compute the image rectangle up front so EVERY reject carries real dims for
    // log analysis (saturating so a malformed offset can't underflow).
    let width = xsiz.saturating_sub(xosiz);
    let height = ysiz.saturating_sub(yosiz);
    let w_i = width.min(i32::MAX as u32) as i32;
    let h_i = height.min(i32::MAX as u32) as i32;
    let c_i = csiz as i32;

    if csiz < 1 || csiz > LL_J2C_MAX_COMPONENTS {
        return (LL_J2C_REJECT_BAD_COMPONENTS, w_i, h_i, c_i);
    }
    // Lsiz must exactly account for the fixed fields + per-component triples.
    if lsiz as usize != 38 + 3 * csiz as usize {
        return (LL_J2C_REJECT_BAD_SIZ_LEN, w_i, h_i, c_i);
    }
    // Image rectangle = (Xsiz-XOsiz) x (Ysiz-YOsiz); must be strictly positive.
    if xsiz <= xosiz || ysiz <= yosiz {
        return (LL_J2C_REJECT_BAD_DIMS, 0, 0, c_i);
    }
    if width > LL_J2C_MAX_DIM || height > LL_J2C_MAX_DIM {
        return (LL_J2C_REJECT_DIMS_TOO_LARGE, w_i, h_i, c_i);
    }
    // Decompression-bomb guard on the real cost axis: total samples Grok would
    // allocate = width * height * components. (dims<=4096, comps<=64 => no u64 overflow.)
    if (width as u64) * (height as u64) * (csiz as u64) > LL_J2C_MAX_SAMPLES {
        return (LL_J2C_REJECT_DIMS_TOO_LARGE, w_i, h_i, c_i);
    }
    // Per-component: 8-bit max, no subsampling (XRsiz==YRsiz==1).
    for i in 0..csiz as usize {
        let base = 42 + i * 3;
        let (ssiz, xr, yr) = match (data.get(base), data.get(base + 1), data.get(base + 2)) {
            (Some(&s), Some(&x), Some(&y)) => (s, x, y),
            _ => return (LL_J2C_REJECT_TOO_SHORT, width as i32, height as i32, csiz as i32),
        };
        let depth = (ssiz & 0x7F) + 1; // low 7 bits = depth-1
        if depth > LL_J2C_MAX_BITDEPTH {
            return (LL_J2C_REJECT_DEPTH, width as i32, height as i32, csiz as i32);
        }
        if xr != 1 || yr != 1 {
            return (LL_J2C_REJECT_SUBSAMPLED, width as i32, height as i32, csiz as i32);
        }
    }
    (LL_J2C_OK, width as i32, height as i32, csiz as i32)
}

/// Ingress gate: validate an untrusted J2C codestream against the SL profile
/// WITHOUT decoding. Fills `*out` and returns 1 (accept) / 0 (reject). Null-safe.
/// Pure safe Rust: a hostile buffer can only be rejected, never crash the caller.
///
/// # Safety
/// `data`/`len` describe a buffer owned by the caller, valid for the call; `out`
/// is a writable `LlJ2cVerdict`.
#[no_mangle]
pub extern "C" fn ll_j2c_validate(data: *const u8, len: usize, out: *mut LlJ2cVerdict) -> i32 {
    if out.is_null() {
        return 0;
    }
    let (reason, w, h, c) = if data.is_null() || len == 0 {
        (LL_J2C_REJECT_TOO_SHORT, 0, 0, 0)
    } else {
        // Safety: caller guarantees `data` points to `len` readable bytes.
        let slice = unsafe { std::slice::from_raw_parts(data, len) };
        validate_j2c(slice)
    };
    let accept = if reason == LL_J2C_OK { 1 } else { 0 };
    unsafe {
        (*out).accept = accept;
        (*out).reason = reason;
        (*out).width = w;
        (*out).height = h;
        (*out).components = c;
    }
    accept
}

#[cfg(test)]
mod tests {
    use super::*;

    // Build a minimal valid SIZ main header for w x h, `comps` 8-bit, no subsample.
    fn siz(w: u32, h: u32, comps: u16) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&0xFF4Fu16.to_be_bytes()); // SOC
        v.extend_from_slice(&0xFF51u16.to_be_bytes()); // SIZ
        v.extend_from_slice(&(38 + 3 * comps).to_be_bytes()); // Lsiz
        v.extend_from_slice(&0u16.to_be_bytes()); // Rsiz
        v.extend_from_slice(&w.to_be_bytes()); // Xsiz
        v.extend_from_slice(&h.to_be_bytes()); // Ysiz
        v.extend_from_slice(&0u32.to_be_bytes()); // XOsiz
        v.extend_from_slice(&0u32.to_be_bytes()); // YOsiz
        v.extend_from_slice(&w.to_be_bytes()); // XTsiz
        v.extend_from_slice(&h.to_be_bytes()); // YTsiz
        v.extend_from_slice(&0u32.to_be_bytes()); // XTOsiz
        v.extend_from_slice(&0u32.to_be_bytes()); // YTOsiz
        v.extend_from_slice(&comps.to_be_bytes()); // Csiz
        for _ in 0..comps {
            v.push(7); // Ssiz: 8-bit unsigned (depth-1 = 7)
            v.push(1); // XRsiz
            v.push(1); // YRsiz
        }
        v
    }

    #[test]
    fn accepts_normal_textures() {
        for &(w, h, c) in &[(1024u32, 1024u32, 3u16), (512, 512, 4), (32, 64, 1), (1, 1, 3)] {
            let (r, vw, vh, vc) = validate_j2c(&siz(w, h, c));
            assert_eq!(r, LL_J2C_OK, "{}x{}x{} should pass", w, h, c);
            assert_eq!((vw, vh, vc), (w as i32, h as i32, c as i32));
        }
    }

    #[test]
    fn rejects_decompression_bomb() {
        let (r, _, _, _) = validate_j2c(&siz(65535, 65535, 4));
        assert_eq!(r, LL_J2C_REJECT_DIMS_TOO_LARGE);
    }

    #[test]
    fn rejects_subsampled_component() {
        let mut b = siz(256, 256, 3);
        // corrupt component 1's XRsiz (offset 42 + 1*3 + 1) to 2 (subsampled)
        b[42 + 3 + 1] = 2;
        assert_eq!(validate_j2c(&b).0, LL_J2C_REJECT_SUBSAMPLED);
    }

    #[test]
    fn rejects_high_bit_depth() {
        let mut b = siz(64, 64, 3);
        b[42] = 15; // Ssiz depth-1 = 15 -> 16-bit
        assert_eq!(validate_j2c(&b).0, LL_J2C_REJECT_DEPTH);
    }

    #[test]
    fn accepts_five_component_textures() {
        // 5-component codestreams occur in the wild and Grok decodes them; the
        // ingress gate must NOT reject them. Regression guard from real shadow data.
        let (r, _, _, c) = validate_j2c(&siz(1024, 1024, 5));
        assert_eq!(r, LL_J2C_OK);
        assert_eq!(c, 5);
    }

    #[test]
    fn rejects_bad_component_count() {
        assert_eq!(validate_j2c(&siz(64, 64, 0)).0, LL_J2C_REJECT_BAD_COMPONENTS);
        assert_eq!(validate_j2c(&siz(64, 64, 100)).0, LL_J2C_REJECT_BAD_COMPONENTS);
    }

    #[test]
    fn rejects_component_bomb_via_budget() {
        // Within the per-dimension cap, but dims x components is a bomb.
        assert_eq!(validate_j2c(&siz(4096, 4096, 64)).0, LL_J2C_REJECT_DIMS_TOO_LARGE);
    }

    #[test]
    fn rejects_missing_markers() {
        assert_eq!(validate_j2c(&[]).0, LL_J2C_REJECT_TOO_SHORT);
        assert_eq!(validate_j2c(&[0x00, 0x00]).0, LL_J2C_REJECT_NO_SOC);
        assert_eq!(validate_j2c(&[0xFF, 0x4F, 0x00, 0x00]).0, LL_J2C_REJECT_NO_SIZ);
    }

    #[test]
    fn rejects_truncated_after_markers() {
        let full = siz(256, 256, 3);
        // Truncate mid-SIZ (after the SOC+SIZ+Lsiz but before all fields).
        assert_eq!(validate_j2c(&full[..10]).0, LL_J2C_REJECT_TOO_SHORT);
    }

    #[test]
    fn no_panic_on_arbitrary_prefixes() {
        // Every truncation of a valid stream must reject cleanly, never panic.
        let full = siz(256, 256, 4);
        for n in 0..full.len() {
            let _ = validate_j2c(&full[..n]);
        }
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
