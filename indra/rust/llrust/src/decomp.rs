//! SL mesh physics **decomposition** decode: zlib + binary-LLSD -> convex-hull
//! point sets. Mirrors `LLModel::Decomposition::fromLLSD` (llprimitive/llmodel.cpp)
//! exactly, including the dequant formula shared with the mesh LOD decode:
//!     p = u16/65535 * (max-min) + min
//!
//! Memory-safety win over the C++ path: C++ walks `Positions` with a bare
//! pointer, advancing by the per-hull counts declared in `HullList` with NO
//! bounds check -- a malicious HullList that over-claims reads out of bounds.
//! Here every read is checked; an over-claim yields `None` (C++ fallback).
//!
//! `fromLLSD` has NO derived-state tail (unlike skin's mBindPoseMatrix/hash) --
//! output is just the flat hull point lists, so this is checksum-validatable
//! bit-for-bit like the geometry decode.

use crate::llsd::{self, Value};

pub struct DecompOut {
    /// One entry per convex hull; each is 3*num_points f32 (x,y,z interleaved).
    pub hulls: Vec<Vec<f32>>,
    /// Base hull (BoundingVerts): 3*num_points f32.
    pub base_hull: Vec<f32>,
}

#[inline]
fn u16_le(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([b[i * 2], b[i * 2 + 1]])
}

/// Read a 3-vector of reals from decomp[key] (missing -> `dflt`).
fn vec3(node: &Value, key: &str, dflt: [f32; 3]) -> [f32; 3] {
    match node.get(key).and_then(Value::as_array) {
        Some(a) => {
            let mut v = [0.0f32; 3];
            for (i, slot) in v.iter_mut().enumerate() {
                *slot = a.get(i).and_then(Value::as_real).unwrap_or(0.0) as f32;
            }
            v
        }
        None => dflt,
    }
}

pub fn decode(compressed: &[u8]) -> Option<DecompOut> {
    let root = llsd::unzip_parse(compressed)?;

    // Domain: present -> Min/Max; absent -> the C++ default cube. Read once;
    // both the HullList and BoundingVerts blocks use the same values in C++.
    let (min, max) = if root.has("Min") {
        (vec3(&root, "Min", [0.0; 3]), vec3(&root, "Max", [0.0; 3]))
    } else {
        ([-0.5, -0.5, -0.5], [0.5, 0.5, 0.5])
    };
    let range = [max[0] - min[0], max[1] - min[1], max[2] - min[2]];

    // Exact C++ op order: (u16/65535)*range + min, component-wise, all f32.
    let dq = |q: u16, c: usize| (q as f32) / 65535.0 * range[c] + min[c];

    let mut out = DecompOut { hulls: Vec::new(), base_hull: Vec::new() };

    if root.has("HullList") && root.has("Positions") {
        let hulls = root.get("HullList").and_then(Value::as_binary)?;
        let pos = root.get("Positions").and_then(Value::as_binary)?;
        let npos = pos.len() / 2; // number of u16s available
        let mut cur = 0usize; // running u16 index, exactly like C++'s `p`

        out.hulls.reserve(hulls.len());
        for &hb in hulls {
            // C++: count = (hulls[i] == 0) ? 256 : hulls[i]
            let count = if hb == 0 { 256usize } else { hb as usize };
            let mut hull = Vec::with_capacity(count * 3);
            for _ in 0..count {
                if cur + 3 > npos {
                    return None; // over-claim -> refuse (C++ would read OOB)
                }
                hull.push(dq(u16_le(pos, cur), 0));
                hull.push(dq(u16_le(pos, cur + 1), 1));
                hull.push(dq(u16_le(pos, cur + 2), 2));
                cur += 3;
            }
            out.hulls.push(hull);
        }
    }

    if root.has("BoundingVerts") {
        let bv = root.get("BoundingVerts").and_then(Value::as_binary)?;
        let count = bv.len() / 6; // C++: position.size()/6
        let mut base = Vec::with_capacity(count * 3);
        for j in 0..count {
            base.push(dq(u16_le(bv, j * 3), 0));
            base.push(dq(u16_le(bv, j * 3 + 1), 1));
            base.push(dq(u16_le(bv, j * 3 + 2), 2));
        }
        out.base_hull = base;
    }

    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::write::ZlibEncoder;
    use flate2::Compression;
    use std::io::Write;

    // --- minimal binary-LLSD builders (match llsd.rs's grammar) ---
    fn key(b: &mut Vec<u8>, k: &str) {
        b.push(b'k');
        b.extend_from_slice(&(k.len() as u32).to_be_bytes());
        b.extend_from_slice(k.as_bytes());
    }
    fn binary(b: &mut Vec<u8>, d: &[u8]) {
        b.push(b'b');
        b.extend_from_slice(&(d.len() as u32).to_be_bytes());
        b.extend_from_slice(d);
    }
    fn real_array(b: &mut Vec<u8>, v: &[f64]) {
        b.push(b'[');
        b.extend_from_slice(&(v.len() as u32).to_be_bytes());
        for &x in v {
            b.push(b'r');
            b.extend_from_slice(&x.to_be_bytes());
        }
        b.push(b']');
    }
    fn u16s(points: &[[u16; 3]]) -> Vec<u8> {
        let mut o = Vec::new();
        for p in points {
            for &c in p {
                o.extend_from_slice(&c.to_le_bytes());
            }
        }
        o
    }
    fn zlib(raw: &[u8]) -> Vec<u8> {
        let mut e = ZlibEncoder::new(Vec::new(), Compression::default());
        e.write_all(raw).unwrap();
        e.finish().unwrap()
    }

    #[test]
    fn two_hulls_with_domain_dequant() {
        let mut m = Vec::new();
        m.push(b'{');
        m.extend_from_slice(&5u32.to_be_bytes());
        key(&mut m, "HullList");
        binary(&mut m, &[2, 3]); // hull0 = 2 pts, hull1 = 3 pts
        key(&mut m, "Positions");
        let pts = [
            [0u16, 0, 0],
            [65535, 65535, 65535],
            [32768, 0, 65535],
            [100, 200, 300],
            [65535, 0, 32768],
        ];
        binary(&mut m, &u16s(&pts));
        key(&mut m, "Min");
        real_array(&mut m, &[-1.0, -2.0, -3.0]);
        key(&mut m, "Max");
        real_array(&mut m, &[1.0, 2.0, 3.0]);
        key(&mut m, "BoundingVerts");
        binary(&mut m, &u16s(&[[16384, 49152, 0]]));
        m.push(b'}');

        let out = decode(&zlib(&m)).expect("decode ok");
        assert_eq!(out.hulls.len(), 2);
        assert_eq!(out.hulls[0].len(), 2 * 3);
        assert_eq!(out.hulls[1].len(), 3 * 3);
        assert_eq!(out.base_hull.len(), 3);

        // min=-1,-2,-3  max=1,2,3  range=2,4,6
        // [0,0,0] -> exactly min
        assert_eq!(&out.hulls[0][0..3], &[-1.0, -2.0, -3.0]);
        // [65535,...] -> exactly max
        assert_eq!(&out.hulls[0][3..6], &[1.0, 2.0, 3.0]);
        // hull1 first point [32768,0,65535], same op order as the code
        let ex = [
            32768.0f32 / 65535.0 * 2.0 + -1.0,
            0.0f32 / 65535.0 * 4.0 + -2.0,
            65535.0f32 / 65535.0 * 6.0 + -3.0,
        ];
        assert_eq!(&out.hulls[1][0..3], &ex);
    }

    #[test]
    fn hull_count_zero_means_256() {
        let mut m = Vec::new();
        m.push(b'{');
        m.extend_from_slice(&2u32.to_be_bytes());
        key(&mut m, "HullList");
        binary(&mut m, &[0]); // 0 -> 256 points
        key(&mut m, "Positions");
        binary(&mut m, &vec![0u8; 256 * 3 * 2]); // exactly 256*3 u16s
        m.push(b'}');
        let out = decode(&zlib(&m)).expect("decode ok");
        assert_eq!(out.hulls.len(), 1);
        assert_eq!(out.hulls[0].len(), 256 * 3);
    }

    #[test]
    fn overclaim_is_rejected_not_oob() {
        // HullList says 10 points, Positions has only 2 -> None (C++ would read OOB)
        let mut m = Vec::new();
        m.push(b'{');
        m.extend_from_slice(&2u32.to_be_bytes());
        key(&mut m, "HullList");
        binary(&mut m, &[10]);
        key(&mut m, "Positions");
        binary(&mut m, &u16s(&[[1, 2, 3], [4, 5, 6]]));
        m.push(b'}');
        assert_eq!(decode(&zlib(&m)).map(|o| o.hulls.len()), None);
    }

    #[test]
    fn missing_domain_defaults_to_unit_cube() {
        // no Min/Max -> min=-0.5, max=0.5, range=1
        let mut m = Vec::new();
        m.push(b'{');
        m.extend_from_slice(&2u32.to_be_bytes());
        key(&mut m, "HullList");
        binary(&mut m, &[1]);
        key(&mut m, "Positions");
        binary(&mut m, &u16s(&[[0, 32768, 65535]]));
        m.push(b'}');
        let out = decode(&zlib(&m)).expect("decode ok");
        let ex = [
            0.0f32 / 65535.0 * 1.0 + -0.5,
            32768.0f32 / 65535.0 * 1.0 + -0.5,
            65535.0f32 / 65535.0 * 1.0 + -0.5,
        ];
        assert_eq!(&out.hulls[0][0..3], &ex);
    }
}
