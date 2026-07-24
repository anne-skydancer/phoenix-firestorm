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

mod llsd;
mod mesh;

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
