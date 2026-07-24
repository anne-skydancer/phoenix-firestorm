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

mod decomp;
mod llsd;
mod mesh;
mod msg;
mod skin;

use std::os::raw::{c_char, c_void};

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

// --- UDP message system ------------------------------------------------------

/// Expand a possibly zero-coded received packet into a caller-owned buffer.
///
/// The C++ call site already owns an 8 KiB decode buffer (`mEncodedRecvBuffer`);
/// rather than allocate + hand back a handle, we write straight into it, matching
/// the shape of `LLMessageSystem::zeroCodeExpand`.
///
/// Returns:
///  *  1 -- expanded; `*out_len` set to the number of bytes written to `out`.
///  *  0 -- packet was not zero-coded (flag clear); `out` untouched, use the
///          original buffer (mirrors the C++ `return 0` no-op).
///  * -1 -- malformed input, or expansion would not fit in `out_cap`: reject.
///
/// Safety: `in_ptr` must point to `in_len` readable bytes; `out_ptr` to `out_cap`
/// writable bytes; `out_len` to a writable `i32`. Any may be null (handled).
/// Never panics (crate is `panic = "abort"`).
#[no_mangle]
pub extern "C" fn ll_zero_code_expand(
    in_ptr: *const u8,
    in_len: i32,
    out_ptr: *mut u8,
    out_cap: i32,
    out_len: *mut i32,
) -> i32 {
    if in_ptr.is_null() || out_ptr.is_null() || out_len.is_null() || in_len <= 0 || out_cap <= 0 {
        return -1;
    }
    let input = unsafe { std::slice::from_raw_parts(in_ptr, in_len as usize) };
    match msg::zero_code_expand(input) {
        msg::Expand::NotCompressed => 0,
        msg::Expand::Bad => -1,
        msg::Expand::Expanded(bytes) => {
            if bytes.len() > out_cap as usize {
                return -1;
            }
            let out = unsafe { std::slice::from_raw_parts_mut(out_ptr, out_cap as usize) };
            out[..bytes.len()].copy_from_slice(&bytes);
            unsafe { *out_len = bytes.len() as i32 };
            1
        }
    }
}

/// A view into one decoded face's dequantized geometry. All pointers are owned
/// by the mesh handle and stay valid until `ll_mesh_free()`; the C++ side copies
/// out of them. Arrays: positions = 4*num_verts (x,y,z,0), normals = 4*num_verts
/// (x,y,z,-1 / all-zero if absent), texcoords = 2*num_verts (u,v), indices =
/// num_indices, weights = weights_len raw asset bytes (C++ decodes them).
/// A null geometry pointer means "none present" (e.g. a NoGeometry face).
#[repr(C)]
pub struct LlMeshFaceView {
    pub no_geometry: i32,
    pub num_verts: u32,
    pub num_indices: u32,
    pub positions: *const f32,
    pub normals: *const f32,
    pub texcoords: *const f32,
    pub indices: *const u16,
    pub weights: *const u8,
    pub weights_len: usize,
    pub has_normalized_scale: i32,
    pub normalized_scale: [f32; 3],
}

/// Decode a zlib+binary-LLSD mesh LOD block (the exact bytes C++ hands to
/// `LLVolume::unpackVolumeFaces`) into dequantized geometry. Returns an opaque
/// handle (free with `ll_mesh_free`), or null if the block did not decode --
/// in which case the caller falls back to the C++ decoder. Never panics.
///
/// Safety: `data` must point to `size` readable bytes (or be null).
#[no_mangle]
pub extern "C" fn ll_mesh_decode(data: *const u8, size: usize) -> *mut c_void {
    if data.is_null() || size == 0 {
        return std::ptr::null_mut();
    }
    let bytes = unsafe { std::slice::from_raw_parts(data, size) };
    match mesh::decode(bytes) {
        Some(m) => Box::into_raw(Box::new(m)) as *mut c_void,
        None => std::ptr::null_mut(),
    }
}

/// Number of faces in a decoded mesh handle (0 if null).
#[no_mangle]
pub extern "C" fn ll_mesh_face_count(handle: *const c_void) -> u32 {
    if handle.is_null() {
        return 0;
    }
    let m = unsafe { &*(handle as *const mesh::DecodedMesh) };
    m.faces.len() as u32
}

/// Fill `out` with a view of face `index`. Returns 0 on success, -1 on bad args.
#[no_mangle]
pub extern "C" fn ll_mesh_get_face(handle: *const c_void, index: u32, out: *mut LlMeshFaceView) -> i32 {
    if handle.is_null() || out.is_null() {
        return -1;
    }
    let m = unsafe { &*(handle as *const mesh::DecodedMesh) };
    let f = match m.faces.get(index as usize) {
        Some(f) => f,
        None => return -1,
    };
    let fptr = |v: &[f32]| if v.is_empty() { std::ptr::null() } else { v.as_ptr() };
    unsafe {
        *out = LlMeshFaceView {
            no_geometry: f.no_geometry as i32,
            num_verts: f.num_verts,
            num_indices: f.num_indices,
            positions: fptr(&f.positions),
            normals: fptr(&f.normals),
            texcoords: fptr(&f.texcoords),
            indices: if f.indices.is_empty() { std::ptr::null() } else { f.indices.as_ptr() },
            weights: if f.weights.is_empty() { std::ptr::null() } else { f.weights.as_ptr() },
            weights_len: f.weights.len(),
            has_normalized_scale: f.has_normalized_scale as i32,
            normalized_scale: f.normalized_scale,
        };
    }
    0
}

/// Free a mesh handle returned by `ll_mesh_decode`. Null is ignored.
#[no_mangle]
pub extern "C" fn ll_mesh_free(handle: *mut c_void) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle as *mut mesh::DecodedMesh)) };
    }
}

// --- Mesh physics decomposition (convex hulls) -------------------------------
// zlib + binary-LLSD parse + dequant of the decomposition sub-block into flat
// hull point lists. Same clean shape as the geometry decode (no derived state);
// C++ copies the points into LLModel::Decomposition::mHull / mBaseHull.

/// Decode a decomposition block. Opaque handle (free with ll_decomp_free) or
/// null if it did not decode (caller falls back to the C++ parser).
#[no_mangle]
pub extern "C" fn ll_decomp_decode(data: *const u8, size: usize) -> *mut c_void {
    if data.is_null() || size == 0 {
        return std::ptr::null_mut();
    }
    let bytes = unsafe { std::slice::from_raw_parts(data, size) };
    match decomp::decode(bytes) {
        Some(d) => Box::into_raw(Box::new(d)) as *mut c_void,
        None => std::ptr::null_mut(),
    }
}

/// Number of convex hulls.
#[no_mangle]
pub extern "C" fn ll_decomp_hull_count(handle: *const c_void) -> u32 {
    if handle.is_null() {
        return 0;
    }
    unsafe { &*(handle as *const decomp::DecompOut) }.hulls.len() as u32
}

/// Points of hull `index`: returns a pointer to `*out_num_points` * 3 f32
/// (x,y,z interleaved), or null. Buffer owned by the handle.
#[no_mangle]
pub extern "C" fn ll_decomp_hull(handle: *const c_void, index: u32, out_num_points: *mut u32) -> *const f32 {
    if !out_num_points.is_null() {
        unsafe { *out_num_points = 0 };
    }
    if handle.is_null() {
        return std::ptr::null();
    }
    let d = unsafe { &*(handle as *const decomp::DecompOut) };
    match d.hulls.get(index as usize) {
        Some(h) => {
            if !out_num_points.is_null() {
                unsafe { *out_num_points = (h.len() / 3) as u32 };
            }
            if h.is_empty() { std::ptr::null() } else { h.as_ptr() }
        }
        None => std::ptr::null(),
    }
}

/// Base-hull points: pointer to `*out_num_points` * 3 f32, or null.
#[no_mangle]
pub extern "C" fn ll_decomp_base_hull(handle: *const c_void, out_num_points: *mut u32) -> *const f32 {
    if !out_num_points.is_null() {
        unsafe { *out_num_points = 0 };
    }
    if handle.is_null() {
        return std::ptr::null();
    }
    let d = unsafe { &*(handle as *const decomp::DecompOut) };
    if !out_num_points.is_null() {
        unsafe { *out_num_points = (d.base_hull.len() / 3) as u32 };
    }
    if d.base_hull.is_empty() { std::ptr::null() } else { d.base_hull.as_ptr() }
}

/// Free a decomposition handle. Null ignored.
#[no_mangle]
pub extern "C" fn ll_decomp_free(handle: *mut c_void) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle as *mut decomp::DecompOut)) };
    }
}

// --- Mesh skin info ----------------------------------------------------------
// Extraction of the untrusted skin block into raw fields. C++ copies them onto
// an LLMeshSkinInfo and calls the shared LLMeshSkinInfo::finalize() for the
// derived bind-pose/hash -- so the derived state cannot be missed.

/// Decode a skin block. Opaque handle (free with ll_skin_free) or null.
#[no_mangle]
pub extern "C" fn ll_skin_decode(data: *const u8, size: usize) -> *mut c_void {
    if data.is_null() || size == 0 {
        return std::ptr::null_mut();
    }
    let bytes = unsafe { std::slice::from_raw_parts(data, size) };
    match skin::decode(bytes) {
        Some(s) => Box::into_raw(Box::new(s)) as *mut c_void,
        None => std::ptr::null_mut(),
    }
}

#[inline]
fn skin_ref<'a>(h: *const c_void) -> Option<&'a skin::SkinOut> {
    if h.is_null() { None } else { Some(unsafe { &*(h as *const skin::SkinOut) }) }
}

#[no_mangle]
pub extern "C" fn ll_skin_joint_count(h: *const c_void) -> u32 {
    skin_ref(h).map_or(0, |s| s.joint_names.len() as u32)
}

/// Joint name `i` as (ptr,len) via out_len; NOT NUL-terminated. Null if absent.
#[no_mangle]
pub extern "C" fn ll_skin_joint_name(h: *const c_void, i: u32, out_len: *mut usize) -> *const u8 {
    if !out_len.is_null() {
        unsafe { *out_len = 0 };
    }
    let s = match skin_ref(h) {
        Some(s) => s,
        None => return std::ptr::null(),
    };
    match s.joint_names.get(i as usize) {
        Some(n) => {
            if !out_len.is_null() {
                unsafe { *out_len = n.len() };
            }
            if n.is_empty() { std::ptr::null() } else { n.as_ptr() }
        }
        None => std::ptr::null(),
    }
}

#[no_mangle]
pub extern "C" fn ll_skin_inv_bind_count(h: *const c_void) -> u32 {
    skin_ref(h).map_or(0, |s| s.inv_bind.len() as u32)
}

/// 1 if the inverse_bind_matrix KEY was present (for the mismatch-clear rule).
#[no_mangle]
pub extern "C" fn ll_skin_has_inv_bind(h: *const c_void) -> i32 {
    skin_ref(h).map_or(0, |s| s.inv_bind_present as i32)
}

/// Inverse-bind matrix `i`: pointer to 16 f32 (array order), or null.
#[no_mangle]
pub extern "C" fn ll_skin_inv_bind(h: *const c_void, i: u32) -> *const f32 {
    match skin_ref(h).and_then(|s| s.inv_bind.get(i as usize)) {
        Some(m) => m.as_ptr(),
        None => std::ptr::null(),
    }
}

/// Bind-shape matrix: pointer to 16 f32, or null if absent (C++ keeps identity).
#[no_mangle]
pub extern "C" fn ll_skin_bind_shape(h: *const c_void) -> *const f32 {
    match skin_ref(h).and_then(|s| s.bind_shape.as_ref()) {
        Some(m) => m.as_ptr(),
        None => std::ptr::null(),
    }
}

#[no_mangle]
pub extern "C" fn ll_skin_alt_count(h: *const c_void) -> u32 {
    skin_ref(h).map_or(0, |s| s.alt_inv_bind.len() as u32)
}

/// Alternate inverse-bind matrix `i`: pointer to 16 f32, or null.
#[no_mangle]
pub extern "C" fn ll_skin_alt(h: *const c_void, i: u32) -> *const f32 {
    match skin_ref(h).and_then(|s| s.alt_inv_bind.get(i as usize)) {
        Some(m) => m.as_ptr(),
        None => std::ptr::null(),
    }
}

/// Pelvis offset; *out_present set to 1 if the field was present, else 0.
#[no_mangle]
pub extern "C" fn ll_skin_pelvis_offset(h: *const c_void, out_present: *mut i32) -> f32 {
    let v = skin_ref(h).and_then(|s| s.pelvis_offset);
    if !out_present.is_null() {
        unsafe { *out_present = v.is_some() as i32 };
    }
    v.unwrap_or(0.0)
}

/// lock_scale_if_joint_position; *out_present set to 1 if present, else 0.
#[no_mangle]
pub extern "C" fn ll_skin_lock_scale(h: *const c_void, out_present: *mut i32) -> i32 {
    let v = skin_ref(h).and_then(|s| s.lock_scale);
    if !out_present.is_null() {
        unsafe { *out_present = v.is_some() as i32 };
    }
    v.unwrap_or(false) as i32
}

#[no_mangle]
pub extern "C" fn ll_skin_free(h: *mut c_void) {
    if !h.is_null() {
        unsafe { drop(Box::from_raw(h as *mut skin::SkinOut)) };
    }
}

// --- Generic binary-LLSD document access -------------------------------------
// The untrusted zlib inflate + binary-LLSD parse happens in Rust; C++ then walks
// the validated tree through these accessors instead of parsing bytes itself.
// Node pointers are borrowed from the document and stay valid until
// `ll_llsd_free()`. This is the reusable primitive for every LLSD-shaped asset
// sub-block: mesh skin, physics convex/decomposition, animations, and so on.

pub const LL_LLSD_UNDEF: i32 = 0;
pub const LL_LLSD_BOOL: i32 = 1;
pub const LL_LLSD_INT: i32 = 2;
pub const LL_LLSD_REAL: i32 = 3;
pub const LL_LLSD_UUID: i32 = 4;
pub const LL_LLSD_STRING: i32 = 5;
pub const LL_LLSD_BINARY: i32 = 6;
pub const LL_LLSD_ARRAY: i32 = 7;
pub const LL_LLSD_MAP: i32 = 8;

#[inline]
fn as_node<'a>(p: *const c_void) -> Option<&'a llsd::Value> {
    if p.is_null() {
        None
    } else {
        Some(unsafe { &*(p as *const llsd::Value) })
    }
}

/// Inflate + parse a zlib-compressed binary-LLSD block (the exact bytes C++
/// hands to LLUZipHelper::unzip_llsd). Returns an opaque document handle, or
/// null if it did not decode -- caller then falls back to the C++ parser.
#[no_mangle]
pub extern "C" fn ll_llsd_unzip_parse(data: *const u8, size: usize) -> *mut c_void {
    if data.is_null() || size == 0 {
        return std::ptr::null_mut();
    }
    let bytes = unsafe { std::slice::from_raw_parts(data, size) };
    match llsd::unzip_parse(bytes) {
        Some(v) => Box::into_raw(Box::new(v)) as *mut c_void,
        None => std::ptr::null_mut(),
    }
}

/// Free a document handle from `ll_llsd_unzip_parse`. Null is ignored.
#[no_mangle]
pub extern "C" fn ll_llsd_free(handle: *mut c_void) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle as *mut llsd::Value)) };
    }
}

/// Root node of a document (borrowed; valid until `ll_llsd_free`).
#[no_mangle]
pub extern "C" fn ll_llsd_root(handle: *const c_void) -> *const c_void {
    handle
}

/// One of the LL_LLSD_* kind constants; UNDEF for null/unknown.
#[no_mangle]
pub extern "C" fn ll_llsd_type(node: *const c_void) -> i32 {
    match as_node(node) {
        None => LL_LLSD_UNDEF,
        Some(v) => match v {
            llsd::Value::Undef => LL_LLSD_UNDEF,
            llsd::Value::Bool(_) => LL_LLSD_BOOL,
            llsd::Value::Int(_) => LL_LLSD_INT,
            llsd::Value::Real(_) => LL_LLSD_REAL,
            llsd::Value::Uuid(_) => LL_LLSD_UUID,
            llsd::Value::Str(_) => LL_LLSD_STRING,
            llsd::Value::Binary(_) => LL_LLSD_BINARY,
            llsd::Value::Array(_) => LL_LLSD_ARRAY,
            llsd::Value::Map(_) => LL_LLSD_MAP,
        },
    }
}

/// Element count for arrays and maps; 0 for anything else.
#[no_mangle]
pub extern "C" fn ll_llsd_size(node: *const c_void) -> u32 {
    match as_node(node) {
        Some(llsd::Value::Array(a)) => a.len() as u32,
        Some(llsd::Value::Map(m)) => m.len() as u32,
        _ => 0,
    }
}

/// Array element `i` (borrowed), or null if not an array / out of range.
#[no_mangle]
pub extern "C" fn ll_llsd_array_get(node: *const c_void, index: u32) -> *const c_void {
    match as_node(node) {
        Some(llsd::Value::Array(a)) => match a.get(index as usize) {
            Some(v) => v as *const llsd::Value as *const c_void,
            None => std::ptr::null(),
        },
        _ => std::ptr::null(),
    }
}

/// Map value for NUL-terminated `key` (borrowed), or null if absent.
#[no_mangle]
pub extern "C" fn ll_llsd_map_get(node: *const c_void, key: *const c_char) -> *const c_void {
    if key.is_null() {
        return std::ptr::null();
    }
    let k = unsafe { std::ffi::CStr::from_ptr(key) };
    let k = match k.to_str() {
        Ok(s) => s,
        Err(_) => return std::ptr::null(),
    };
    match as_node(node).and_then(|v| v.get(k)) {
        Some(v) => v as *const llsd::Value as *const c_void,
        None => std::ptr::null(),
    }
}

/// 1 if the map contains `key`, else 0.
#[no_mangle]
pub extern "C" fn ll_llsd_map_has(node: *const c_void, key: *const c_char) -> i32 {
    if ll_llsd_map_get(node, key).is_null() {
        0
    } else {
        1
    }
}

/// Numeric value (ints promote to real); 0.0 for non-numeric.
#[no_mangle]
pub extern "C" fn ll_llsd_as_real(node: *const c_void) -> f64 {
    as_node(node).and_then(llsd::Value::as_real).unwrap_or(0.0)
}

/// Integer value (reals truncate); 0 for non-numeric.
#[no_mangle]
pub extern "C" fn ll_llsd_as_int(node: *const c_void) -> i32 {
    match as_node(node) {
        Some(llsd::Value::Int(i)) => *i,
        Some(llsd::Value::Real(r)) => *r as i32,
        Some(llsd::Value::Bool(b)) => *b as i32,
        _ => 0,
    }
}

/// 1/0; non-bools follow LLSD-ish truthiness (non-zero number, non-empty string).
#[no_mangle]
pub extern "C" fn ll_llsd_as_bool(node: *const c_void) -> i32 {
    match as_node(node) {
        Some(llsd::Value::Bool(b)) => *b as i32,
        Some(llsd::Value::Int(i)) => (*i != 0) as i32,
        Some(llsd::Value::Real(r)) => (*r != 0.0) as i32,
        Some(llsd::Value::Str(s)) => (!s.is_empty()) as i32,
        _ => 0,
    }
}

/// String bytes (NOT NUL-terminated) + length via `out_len`; null if not a string.
#[no_mangle]
pub extern "C" fn ll_llsd_as_string(node: *const c_void, out_len: *mut usize) -> *const u8 {
    let (ptr, len) = match as_node(node) {
        Some(llsd::Value::Str(s)) => (s.as_ptr(), s.len()),
        _ => (std::ptr::null(), 0usize),
    };
    if !out_len.is_null() {
        unsafe { *out_len = len };
    }
    ptr
}

/// Binary blob + length via `out_len`; null if not binary.
#[no_mangle]
pub extern "C" fn ll_llsd_as_binary(node: *const c_void, out_len: *mut usize) -> *const u8 {
    let (ptr, len) = match as_node(node) {
        Some(llsd::Value::Binary(b)) => (b.as_ptr(), b.len()),
        _ => (std::ptr::null(), 0usize),
    };
    if !out_len.is_null() {
        unsafe { *out_len = len };
    }
    ptr
}
