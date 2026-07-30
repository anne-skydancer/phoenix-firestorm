//! Minimal Collada (.dae) reader for the engine's real-mesh path (H4b).
//!
//! Handles the SL dialects in play: `<triangles>` and `<polylist>` (any `vcount`, fan-
//! triangulated), MULTIPLE primitives per `<mesh>` (material submeshes accumulated), multi-
//! offset inputs (VERTEX / NORMAL / TEXCOORD), `Y_UP` and `Z_UP` (rotated to Y-up), and per-face
//! normals computed when the file has none (physics shapes). Loads the FIRST `<geometry>` —
//! single-geometry meshes; multi-prim linksets (e.g. Nest, 47 geometries + node transforms) are
//! a later step. XML tokenizing via `roxmltree`; the Collada semantics are ours.

use glam::Vec3;
use std::collections::HashMap;

pub struct DaeMesh {
    pub positions: Vec<[f32; 3]>, // de-indexed: one entry per output vertex
    pub normals: Vec<[f32; 3]>,
    pub uvs: Vec<[f32; 2]>, // parsed for H4c; [0,0] where the file has no TEXCOORD
    pub indices: Vec<u32>,  // sequential 0..n (expanded), kept explicit for the render path
}

impl DaeMesh {
    /// Axis-aligned bounds in the loader's Y-up space.
    pub fn bounds(&self) -> (Vec3, Vec3) {
        let mut lo = Vec3::splat(f32::INFINITY);
        let mut hi = Vec3::splat(f32::NEG_INFINITY);
        for p in &self.positions {
            let v = Vec3::from(*p);
            lo = lo.min(v);
            hi = hi.max(v);
        }
        (lo, hi)
    }
    pub fn tri_count(&self) -> usize { self.indices.len() / 3 }
}

fn local(n: &roxmltree::Node, name: &str) -> bool { n.tag_name().name() == name }
fn floats(t: &str) -> Vec<f32> { t.split_whitespace().filter_map(|s| s.parse().ok()).collect() }
fn ints(t: &str) -> Vec<usize> { t.split_whitespace().filter_map(|s| s.parse().ok()).collect() }

pub fn load(path: &std::path::Path) -> Result<DaeMesh, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let doc = roxmltree::Document::parse(&text).map_err(|e| format!("xml parse {}: {e}", path.display()))?;
    let root = doc.root_element();

    let z_up = root.descendants().find(|n| local(n, "up_axis"))
        .and_then(|n| n.text()).map(|t| t.trim() == "Z_UP").unwrap_or(false);
    let conv = |v: [f32; 3]| if z_up { [v[0], v[2], -v[1]] } else { v }; // Z-up -> Y-up (rotate -90 about X)

    let mesh = root.descendants().find(|n| local(n, "mesh")).ok_or("no <mesh>")?;

    // sources: "#id" -> (floats, stride)
    let mut sources: HashMap<String, (Vec<f32>, usize)> = HashMap::new();
    for s in mesh.children().filter(|n| local(n, "source")) {
        let id = s.attribute("id").unwrap_or("");
        let stride = s.descendants().find(|n| local(n, "accessor"))
            .and_then(|a| a.attribute("stride")).and_then(|v| v.parse().ok()).unwrap_or(1);
        if let Some(fa) = s.descendants().find(|n| local(n, "float_array")) {
            sources.insert(format!("#{id}"), (floats(fa.text().unwrap_or("")), stride));
        }
    }
    // <vertices> id -> POSITION source ref
    let mut vpos: HashMap<String, String> = HashMap::new();
    for v in mesh.children().filter(|n| local(n, "vertices")) {
        let id = v.attribute("id").unwrap_or("");
        if let Some(src) = v.children()
            .find(|n| local(n, "input") && n.attribute("semantic") == Some("POSITION"))
            .and_then(|i| i.attribute("source")) {
            vpos.insert(format!("#{id}"), src.to_string());
        }
    }

    let get3 = |f: &[f32], idx: usize, st: usize| -> [f32; 3] {
        let b = idx * st;
        [f.get(b).copied().unwrap_or(0.0), f.get(b + 1).copied().unwrap_or(0.0), f.get(b + 2).copied().unwrap_or(0.0)]
    };

    let mut out = DaeMesh { positions: vec![], normals: vec![], uvs: vec![], indices: vec![] };
    let mut had_prim = false;

    // accumulate every primitive in the mesh (material submeshes)
    for prim in mesh.children().filter(|n| local(n, "triangles") || local(n, "polylist")) {
        had_prim = true;
        let (mut voff, mut vsrc) = (0usize, String::new());
        let (mut noff, mut nsrc) = (None::<usize>, String::new());
        let (mut toff, mut tsrc) = (None::<usize>, String::new());
        let mut stride = 0usize;
        for inp in prim.children().filter(|n| local(n, "input")) {
            let off: usize = inp.attribute("offset").and_then(|v| v.parse().ok()).unwrap_or(0);
            stride = stride.max(off + 1);
            let src = inp.attribute("source").unwrap_or("").to_string();
            match inp.attribute("semantic").unwrap_or("") {
                "VERTEX" => { voff = off; vsrc = src; }
                "NORMAL" => { noff = Some(off); nsrc = src; }
                "TEXCOORD" => { toff = Some(off); tsrc = src; }
                _ => {}
            }
        }
        if stride == 0 { continue; }
        let pos_ref = vpos.get(&vsrc).cloned().unwrap_or(vsrc);
        let (pos, pstride) = match sources.get(&pos_ref) { Some(x) => (&x.0, x.1), None => continue };
        let normal: Option<(usize, &Vec<f32>, usize)> = noff.and_then(|o| sources.get(&nsrc).map(|s| (o, &s.0, s.1)));
        let uv: Option<(usize, &Vec<f32>, usize)> = toff.and_then(|o| sources.get(&tsrc).map(|s| (o, &s.0, s.1)));

        let p = prim.children().find(|n| local(n, "p")).and_then(|n| n.text()).map(ints).unwrap_or_default();
        let ntuples = p.len() / stride;

        // tuple-index triangles (fan-triangulate polylist)
        let mut tris: Vec<[usize; 3]> = Vec::new();
        if local(&prim, "polylist") {
            let vcount = prim.children().find(|n| local(n, "vcount")).and_then(|n| n.text()).map(ints).unwrap_or_default();
            let mut base = 0usize;
            for &vc in &vcount {
                if base + vc > ntuples { break; }
                for k in 1..vc.saturating_sub(1) { tris.push([base, base + k, base + k + 1]); }
                base += vc;
            }
        } else {
            for t in 0..ntuples / 3 { tris.push([t * 3, t * 3 + 1, t * 3 + 2]); }
        }

        let base_out = out.positions.len();
        for tri in &tris {
            let ps: [[f32; 3]; 3] = std::array::from_fn(|k| conv(get3(pos, p[tri[k] * stride + voff], pstride)));
            let face = {
                let (a, b, c) = (Vec3::from(ps[0]), Vec3::from(ps[1]), Vec3::from(ps[2]));
                (b - a).cross(c - a).normalize_or_zero()
            };
            for k in 0..3 {
                out.positions.push(ps[k]);
                let n = if let Some((o, f, st)) = normal {
                    let v = Vec3::from(conv(get3(f, p[tri[k] * stride + o], st)));
                    if v.length_squared() > 1e-8 { v } else { face }
                } else { face };
                out.normals.push(n.into());
                let t = if let Some((o, f, st)) = uv {
                    let b = p[tri[k] * stride + o] * st;
                    [f.get(b).copied().unwrap_or(0.0), f.get(b + 1).copied().unwrap_or(0.0)]
                } else { [0.0, 0.0] };
                out.uvs.push(t);
                out.indices.push(out.positions.len() as u32 - 1);
            }
        }
        // computed (no-normal) prims: orient outward from the submesh centroid (convex shapes)
        if normal.is_none() && out.positions.len() > base_out {
            let mut c = Vec3::ZERO;
            for i in base_out..out.positions.len() { c += Vec3::from(out.positions[i]); }
            c /= (out.positions.len() - base_out) as f32;
            for i in base_out..out.positions.len() {
                let nn = Vec3::from(out.normals[i]);
                if nn.dot(Vec3::from(out.positions[i]) - c) < 0.0 { out.normals[i] = (-nn).into(); }
            }
        }
    }

    if !had_prim { return Err("no <triangles>/<polylist> primitive".into()); }
    if out.positions.is_empty() { return Err("mesh produced no triangles".into()); }
    Ok(out)
}
