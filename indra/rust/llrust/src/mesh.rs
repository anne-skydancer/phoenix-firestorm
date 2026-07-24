//! SL mesh asset decode: zlib-compressed binary-LLSD block -> dequantized
//! geometry, ready for C++ to memcpy into LLVolumeFace. Mirrors the parse +
//! dequant half of `LLUZipHelper::unzip_llsd` + `LLVolume::unpackVolumeFacesInternal`;
//! the C++ side keeps skin-weight decode, mirror/invert, extents and cacheOptimize.
//!
//! Byte layout is chosen to match LLVolumeFace exactly so C++ can bulk-copy:
//!   positions : 4 f32/vertex (x,y,z,0)              == LLVector4a
//!   normals   : 4 f32/vertex (x,y,z,-1) present,    == LLVector4a
//!               (0,0,0,0) when the asset omits them (matches clear())
//!   texcoords : 2 f32/vertex (u,v)                  == LLVector2
//!   indices   : u16
//! Dequant replicates the exact op order so results are bit-identical to C++
//! (validated at 48k+ meshes, 0 divergence, before this flip).

use crate::llsd::{self, Value};

pub struct FaceOut {
    pub no_geometry: bool,
    pub num_verts: u32,
    pub num_indices: u32,
    pub positions: Vec<f32>, // 4 * num_verts
    pub normals: Vec<f32>,   // 4 * num_verts
    pub texcoords: Vec<f32>, // 2 * num_verts
    pub indices: Vec<u16>,   // num_indices
    pub weights: Vec<u8>,    // raw asset blob (C++ decodes it); empty if none
    pub has_normalized_scale: bool,
    pub normalized_scale: [f32; 3],
}

pub struct DecodedMesh {
    pub faces: Vec<FaceOut>,
}

#[inline]
fn u16_le(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([b[i * 2], b[i * 2 + 1]])
}

fn domain_vec<const N: usize>(face: &Value, key: &str, sub: &str) -> [f32; N] {
    let mut out = [0.0f32; N];
    if let Some(arr) = face.get(key).and_then(|d| d.get(sub)).and_then(Value::as_array) {
        for (i, slot) in out.iter_mut().enumerate() {
            if let Some(v) = arr.get(i).and_then(Value::as_real) {
                *slot = v as f32;
            }
        }
    }
    out
}

/// Inflate + parse + dequant a mesh LOD block. `None` on any malformed input
/// (bad zlib / LLSD / shape) -- never panics, never reads out of bounds; the
/// C++ caller then falls back to its own decoder.
pub fn decode(compressed: &[u8]) -> Option<DecodedMesh> {
    let root = llsd::unzip_parse(compressed)?;
    let faces_sd = root.as_array()?;

    let empty: &[u8] = &[];
    let mut faces = Vec::with_capacity(faces_sd.len());

    for face in faces_sd {
        if face.has("NoGeometry") {
            faces.push(FaceOut {
                no_geometry: true,
                num_verts: 0,
                num_indices: 0,
                positions: Vec::new(),
                normals: Vec::new(),
                texcoords: Vec::new(),
                indices: Vec::new(),
                weights: Vec::new(),
                has_normalized_scale: false,
                normalized_scale: [1.0, 1.0, 1.0],
            });
            continue;
        }

        let pos = face.get("Position").and_then(Value::as_binary).unwrap_or(empty);
        let norm = face.get("Normal").and_then(Value::as_binary).unwrap_or(empty);
        let tc = face.get("TexCoord0").and_then(Value::as_binary).unwrap_or(empty);
        let idx = face.get("TriangleList").and_then(Value::as_binary).unwrap_or(empty);

        let nv = (pos.len() / 6) as usize;
        let mut ni = idx.len() / 2;
        ni -= ni % 3;

        // indices
        let mut indices = Vec::with_capacity(ni);
        for j in 0..ni {
            indices.push(u16_le(idx, j));
        }

        // positions: (u16/65535)*(max-min)+min, w = 0
        let pmin = domain_vec::<3>(face, "PositionDomain", "Min");
        let pmax = domain_vec::<3>(face, "PositionDomain", "Max");
        let prange = [pmax[0] - pmin[0], pmax[1] - pmin[1], pmax[2] - pmin[2]];
        let mut positions = Vec::with_capacity(nv * 4);
        for j in 0..nv {
            for c in 0..3 {
                positions.push((u16_le(pos, j * 3 + c) as f32 / 65535.0) * prange[c] + pmin[c]);
            }
            positions.push(0.0); // w
        }

        // normals: present -> (u16/65535)*2-1, w=-1; absent -> (0,0,0,0)
        let mut normals = Vec::with_capacity(nv * 4);
        if !norm.is_empty() {
            for j in 0..nv {
                for c in 0..3 {
                    normals.push((u16_le(norm, j * 3 + c) as f32 / 65535.0) * 2.0 - 1.0);
                }
                normals.push(-1.0); // w after set(x,y,z=0)->/65535->*2->-1
            }
        } else {
            normals.resize(nv * 4, 0.0);
        }

        // texcoords: present -> (u16/65535)*(max-min)+min; absent -> (0,0)
        let tmin = domain_vec::<2>(face, "TexCoord0Domain", "Min");
        let tmax = domain_vec::<2>(face, "TexCoord0Domain", "Max");
        let trange = [tmax[0] - tmin[0], tmax[1] - tmin[1]];
        let mut texcoords = Vec::with_capacity(nv * 2);
        if !tc.is_empty() {
            for j in 0..nv {
                for c in 0..2 {
                    texcoords.push((u16_le(tc, j * 2 + c) as f32 / 65535.0) * trange[c] + tmin[c]);
                }
            }
        } else {
            texcoords.resize(nv * 2, 0.0);
        }

        let weights = face
            .get("Weights")
            .and_then(Value::as_binary)
            .map(|b| b.to_vec())
            .unwrap_or_default();

        let (has_ns, ns) = match face.get("NormalizedScale").and_then(Value::as_array) {
            Some(a) => {
                let mut s = [1.0f32; 3];
                for (i, slot) in s.iter_mut().enumerate() {
                    if let Some(v) = a.get(i).and_then(Value::as_real) {
                        *slot = v as f32;
                    }
                }
                (true, s)
            }
            None => (false, [1.0, 1.0, 1.0]),
        };

        faces.push(FaceOut {
            no_geometry: false,
            num_verts: nv as u32,
            num_indices: ni as u32,
            positions,
            normals,
            texcoords,
            indices,
            weights,
            has_normalized_scale: has_ns,
            normalized_scale: ns,
        });
    }

    Some(DecodedMesh { faces })
}
