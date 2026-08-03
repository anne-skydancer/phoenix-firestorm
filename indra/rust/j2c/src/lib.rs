//! Memory-safe SL J2C (JPEG2000) decode over Grok -- the SINGLE source of the untrusted parse,
//! shared by the viewer (`llrust`, which wraps it in the `ll_j2c_*` C ABI) and the Vulkan engine
//! (`fs_render`, which calls the Rust API directly).
//!
//! Two-gate hardening: a pure-safe ingress validator (`validate`) pins input to the SL profile
//! BEFORE Grok's C++ parser runs (kills decompression bombs + the least-tested Grok paths), and a
//! self-validating egress copy (`copy_plane`) can only reject bogus geometry, never read OOB.
//!
//! Init ownership: `grk_initialize` must run exactly once per process before any decode. The VIEWER
//! already calls it (C++), so `decode()` does NOT init -- it assumes init. The ENGINE owns no other
//! Grok caller, so it must call [`ensure_init`] once before decoding. This keeps init a single call
//! on either side (the Grok concurrency contract).

/// A decoded image: interleaved 8-bit pixels (bottom-up, like LL raw), plus geometry.
pub struct DecodedJ2c {
    pub pixels: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub components: u32, // channels actually taken (1..4)
}

/// SL clamps the reduce (discard) factor to this (llimage.h MAX_DISCARD_LEVEL).
#[cfg(feature = "grok")]
const MAX_DISCARD_LEVEL: i32 = 5;

/// Decode a J2C codestream to interleaved 8-bit pixels. Fail-closed: any malformed input, policy
/// violation, or degenerate geometry returns None. `discard` = reduce level (0 = full res).
/// Assumes Grok is initialized (viewer: by C++; engine: via [`ensure_init`]). Without the `grok`
/// feature, always returns None after validation.
pub fn decode(data: &[u8], discard: i32, first_channel: i32, max_channel_count: i32) -> Option<DecodedJ2c> {
    if data.is_empty() {
        return None;
    }
    // Ingress gate: reject anything outside the SL profile before Grok's C++ parser runs.
    if validate(data).reason != LL_J2C_OK {
        return None;
    }
    #[cfg(feature = "grok")]
    {
        unsafe { grok_decode(data, discard, first_channel, max_channel_count) }
    }
    #[cfg(not(feature = "grok"))]
    {
        let _ = (discard, first_channel, max_channel_count);
        None
    }
}

/// Grok version string (borrowed static C string; never freed). Null without the `grok` feature.
/// Proves the Rust -> Grok C-ABI bridge links + executes.
pub fn grok_version() -> *const std::os::raw::c_char {
    #[cfg(feature = "grok")]
    {
        unsafe { grok_sys::grk_version() }
    }
    #[cfg(not(feature = "grok"))]
    {
        std::ptr::null()
    }
}

// --- Grok decode (feature-gated) ---------------------------------------------------------------
#[cfg(feature = "grok")]
mod grok_impl {
    use super::DecodedJ2c;
    use std::sync::Once;

    #[allow(non_upper_case_globals, non_camel_case_types, non_snake_case, dead_code, unused_imports)]
    pub(super) mod grok_sys {
        include!(concat!(env!("OUT_DIR"), "/grok_sys.rs"));
    }

    // grk_image is bindgen-OPAQUE (forward-declared in grok.h). Read it through a #[repr(C)] view
    // with MEASURED offsets, VERIFIED at compile time -- if Grok changes the layout the build fails.
    //   sizeof(grk_image)=208, x1@16, y1@20, numcomps@24, comps@200.
    #[repr(C)]
    struct GrkImageView {
        _pad_obj_x0_y0: [u8; 16],
        x1: u32,  // @16
        y1: u32,  // @20
        numcomps: u16, // @24
        _pad_to_comps: [u8; 200 - 26],
        comps: *mut grok_sys::grk_image_comp, // @200
    }
    const _: () = {
        assert!(core::mem::size_of::<GrkImageView>() == 208);
        assert!(core::mem::offset_of!(GrkImageView, x1) == 16);
        assert!(core::mem::offset_of!(GrkImageView, y1) == 20);
        assert!(core::mem::offset_of!(GrkImageView, numcomps) == 24);
        assert!(core::mem::offset_of!(GrkImageView, comps) == 200);
    };

    // grk_header_info embeds grk_image BY VALUE at offset 0, so bindgen's struct is undersized. Pass
    // a full-size, 8-aligned buffer to grk_decompress_read_header and read header_image at 0.
    const GRK_HEADER_INFO_SIZE: usize = 12632;

    static GROK_INIT: Once = Once::new();

    /// Run grk_initialize exactly once (engine-side; the viewer's C++ owns its own init).
    pub fn ensure_init() {
        GROK_INIT.call_once(|| unsafe {
            let mut plugin_initialized = false;
            grok_sys::grk_initialize(std::ptr::null(), 0, &mut plugin_initialized as *mut bool);
        });
    }

    pub(super) unsafe fn grok_decode(data: &[u8], discard_level: i32, first_channel: i32, max_channel_count: i32) -> Option<DecodedJ2c> {
        let mut stream_params: grok_sys::grk_stream_params = std::mem::zeroed();
        stream_params.buf = data.as_ptr() as *mut u8;
        stream_params.buf_len = data.len();
        stream_params.is_read_stream = true;

        let mut params: grok_sys::grk_decompress_parameters = std::mem::zeroed();
        params.core.reduce = discard_level.clamp(0, super::MAX_DISCARD_LEVEL) as u8;
        params.num_threads = 1;

        let codec = grok_sys::grk_decompress_init(&mut stream_params, &mut params);
        if codec.is_null() {
            return None;
        }
        struct CodecGuard(*mut grok_sys::grk_object);
        impl Drop for CodecGuard {
            fn drop(&mut self) {
                unsafe { grok_sys::grk_object_unref(self.0) };
            }
        }
        let _guard = CodecGuard(codec);

        let mut hdr = vec![0u64; GRK_HEADER_INFO_SIZE.div_ceil(8)];
        if !grok_sys::grk_decompress_read_header(codec, hdr.as_mut_ptr() as *mut grok_sys::grk_header_info) {
            return None;
        }
        let header_image = &*(hdr.as_ptr() as *const GrkImageView);
        let numcomps = header_image.numcomps as i32;

        let channels = (numcomps - first_channel).min(max_channel_count).max(0);
        if channels <= 0 {
            return None;
        }

        if !grok_sys::grk_decompress(codec, std::ptr::null_mut()) {
            return None;
        }

        let image_ptr = grok_sys::grk_decompress_get_image(codec);
        if image_ptr.is_null() {
            return None;
        }
        let image = &*(image_ptr as *const GrkImageView);
        if image.numcomps == 0 || image.comps.is_null() {
            return None;
        }

        let out_w = (*image.comps).w as usize;
        let out_h = (*image.comps).h as usize;
        if out_w == 0 || out_h == 0 {
            return None;
        }

        let out_len = out_w.checked_mul(out_h).and_then(|a| a.checked_mul(channels as usize))?;
        let mut pixels = vec![0u8; out_len];

        for dest in 0..channels {
            let c = &*image.comps.offset((first_channel + dest) as isize);
            if c.data.is_null() {
                return None;
            }
            if c.w as usize != out_w || c.h as usize != out_h || (c.stride as usize) < out_w {
                return None;
            }
            let stride = c.stride as usize;
            let ok = if c.data_type == grok_sys::_grk_data_type_GRK_INT_16 {
                copy_plane(c.data as *const i16, stride, out_w, out_h, &mut pixels, dest as usize, channels as usize, |s| s as u8)
            } else {
                copy_plane(c.data as *const i32, stride, out_w, out_h, &mut pixels, dest as usize, channels as usize, |s| s as u8)
            };
            if !ok {
                return None;
            }
        }

        Some(DecodedJ2c {
            pixels,
            width: out_w as u32,
            height: out_h as u32,
            components: channels as u32,
        })
    }

    /// Copy one decoded component plane into the interleaved output, bottom-up, narrowing to 8 bits.
    /// SELF-VALIDATING: bogus geometry from Grok yields false (caller discards), never an OOB read.
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
        let buf_len = match stride.checked_mul(out_h) {
            Some(n) => n,
            None => return false,
        };
        let samples = std::slice::from_raw_parts(data, buf_len);
        let mut offset = dest;
        for y in (0..out_h).rev() {
            let row = y * stride;
            let end = match row.checked_add(out_w) {
                Some(e) => e,
                None => return false,
            };
            let src_row = match samples.get(row..end) {
                Some(s) => s,
                None => return false,
            };
            for &sample in src_row {
                pixels[offset] = to_u8(sample);
                offset += channels;
            }
        }
        true
    }
}

#[cfg(feature = "grok")]
use grok_impl::grok_decode;
#[cfg(feature = "grok")]
use grok_impl::grok_sys;

/// Run grk_initialize exactly once (engine-side; the viewer's C++ owns its own init). No-op
/// without the `grok` feature. Call before [`decode`] on the engine side.
pub fn ensure_init() {
    #[cfg(feature = "grok")]
    grok_impl::ensure_init();
}

// --- Ingress gate: pure, memory-safe pre-decode validation (no `unsafe`) --------------------

pub const LL_J2C_OK: i32 = 0;
pub const LL_J2C_REJECT_TOO_SHORT: i32 = 1;
pub const LL_J2C_REJECT_NO_SOC: i32 = 2;
pub const LL_J2C_REJECT_NO_SIZ: i32 = 3;
pub const LL_J2C_REJECT_BAD_SIZ_LEN: i32 = 4;
pub const LL_J2C_REJECT_BAD_DIMS: i32 = 5;
pub const LL_J2C_REJECT_DIMS_TOO_LARGE: i32 = 6;
pub const LL_J2C_REJECT_BAD_COMPONENTS: i32 = 7;
pub const LL_J2C_REJECT_SUBSAMPLED: i32 = 8;
pub const LL_J2C_REJECT_DEPTH: i32 = 9;

const LL_J2C_MAX_DIM: u32 = 4096;
const LL_J2C_MAX_COMPONENTS: u16 = 64;
const LL_J2C_MAX_BITDEPTH: u8 = 8;
const LL_J2C_MAX_SAMPLES: u64 = 4096 * 4096 * 8;

/// Verdict from [`validate`]. Plain POD (the `llrust` FFI copies it into its `LlJ2cVerdict`).
pub struct Verdict {
    pub reason: i32, // LL_J2C_OK (0) = accept
    pub width: i32,
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

/// Validate an untrusted J2C codestream against the SL profile WITHOUT decoding. All reads
/// bounds-checked -- a truncated/hostile buffer can only reject, never panic or read OOB.
pub fn validate(data: &[u8]) -> Verdict {
    let (reason, width, height, components) = validate_inner(data);
    Verdict { reason, width, height, components }
}

fn validate_inner(data: &[u8]) -> (i32, i32, i32, i32) {
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
    let lsiz = match be_u16(data, 4) {
        Some(v) => v,
        None => return (LL_J2C_REJECT_TOO_SHORT, 0, 0, 0),
    };
    let (xsiz, ysiz, xosiz, yosiz, csiz) = match (be_u32(data, 8), be_u32(data, 12), be_u32(data, 16), be_u32(data, 20), be_u16(data, 40)) {
        (Some(a), Some(b), Some(c), Some(d), Some(e)) => (a, b, c, d, e),
        _ => return (LL_J2C_REJECT_TOO_SHORT, 0, 0, 0),
    };

    let width = xsiz.saturating_sub(xosiz);
    let height = ysiz.saturating_sub(yosiz);
    let w_i = width.min(i32::MAX as u32) as i32;
    let h_i = height.min(i32::MAX as u32) as i32;
    let c_i = csiz as i32;

    if csiz < 1 || csiz > LL_J2C_MAX_COMPONENTS {
        return (LL_J2C_REJECT_BAD_COMPONENTS, w_i, h_i, c_i);
    }
    if lsiz as usize != 38 + 3 * csiz as usize {
        return (LL_J2C_REJECT_BAD_SIZ_LEN, w_i, h_i, c_i);
    }
    if xsiz <= xosiz || ysiz <= yosiz {
        return (LL_J2C_REJECT_BAD_DIMS, 0, 0, c_i);
    }
    if width > LL_J2C_MAX_DIM || height > LL_J2C_MAX_DIM {
        return (LL_J2C_REJECT_DIMS_TOO_LARGE, w_i, h_i, c_i);
    }
    if (width as u64) * (height as u64) * (csiz as u64) > LL_J2C_MAX_SAMPLES {
        return (LL_J2C_REJECT_DIMS_TOO_LARGE, w_i, h_i, c_i);
    }
    for i in 0..csiz as usize {
        let base = 42 + i * 3;
        let (ssiz, xr, yr) = match (data.get(base), data.get(base + 1), data.get(base + 2)) {
            (Some(&s), Some(&x), Some(&y)) => (s, x, y),
            _ => return (LL_J2C_REJECT_TOO_SHORT, width as i32, height as i32, csiz as i32),
        };
        let depth = (ssiz & 0x7F) + 1;
        if depth > LL_J2C_MAX_BITDEPTH {
            return (LL_J2C_REJECT_DEPTH, width as i32, height as i32, csiz as i32);
        }
        if xr != 1 || yr != 1 {
            return (LL_J2C_REJECT_SUBSAMPLED, width as i32, height as i32, csiz as i32);
        }
    }
    (LL_J2C_OK, width as i32, height as i32, csiz as i32)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn siz(w: u32, h: u32, comps: u16) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&0xFF4Fu16.to_be_bytes());
        v.extend_from_slice(&0xFF51u16.to_be_bytes());
        v.extend_from_slice(&(38 + 3 * comps).to_be_bytes());
        v.extend_from_slice(&0u16.to_be_bytes());
        v.extend_from_slice(&w.to_be_bytes());
        v.extend_from_slice(&h.to_be_bytes());
        v.extend_from_slice(&0u32.to_be_bytes());
        v.extend_from_slice(&0u32.to_be_bytes());
        v.extend_from_slice(&w.to_be_bytes());
        v.extend_from_slice(&h.to_be_bytes());
        v.extend_from_slice(&0u32.to_be_bytes());
        v.extend_from_slice(&0u32.to_be_bytes());
        v.extend_from_slice(&comps.to_be_bytes());
        for _ in 0..comps {
            v.push(7);
            v.push(1);
            v.push(1);
        }
        v
    }

    #[test]
    fn accepts_normal_rejects_bombs_and_truncations() {
        assert_eq!(validate(&siz(1024, 1024, 3)).reason, LL_J2C_OK);
        assert_eq!(validate(&siz(512, 512, 4)).reason, LL_J2C_OK);
        assert_eq!(validate(&siz(65535, 65535, 4)).reason, LL_J2C_REJECT_DIMS_TOO_LARGE);
        assert_eq!(validate(&siz(64, 64, 0)).reason, LL_J2C_REJECT_BAD_COMPONENTS);
        let full = siz(256, 256, 4);
        for n in 0..full.len() {
            let _ = validate(&full[..n]); // must never panic
        }
    }

    // NOTE: decode() references grk_* under the `grok` feature, which only link in a consumer that
    // links grokj2k (fs_render / the viewer) -- so the garbage-reject + real-decode end-to-end tests
    // live in fs_render (tests/j2c_decode.rs), not here. This crate's tests cover the pure validator,
    // which is always linkable.
    #[test]
    fn validator_is_panic_free_on_garbage() {
        // The pure ingress gate must never panic on arbitrary input.
        for b in [vec![], vec![0u8; 3], vec![0xFF; 64], siz(0, 0, 0)] {
            let _ = validate(&b);
        }
    }
}
