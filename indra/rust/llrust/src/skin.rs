//! SL mesh skin-info decode: zlib + binary-LLSD -> raw skin fields. Mirrors the
//! EXTRACTION half of `LLMeshSkinInfo::fromLLSD` (llprimitive/llmodel.cpp).
//!
//! Deliberately does NOT compute derived state: the bind-pose matrices and hash
//! are produced by C++ `LLMeshSkinInfo::finalize()`, called by both the C++ and
//! Rust paths. That shared finalize is the fix for the earlier skin regression,
//! where the derived `mBindPoseMatrix` was silently missed. Here we only hand
//! back the parsed fields.

use crate::llsd::{self, Value};

pub struct SkinOut {
    pub joint_names: Vec<Vec<u8>>,
    /// Each matrix as 16 reals in LLSD array order (element idx = j*4+k), exactly
    /// as fromLLSD reads them; C++ maps flat[j*4+k] -> LLMatrix4::mMatrix[j][k].
    pub inv_bind: Vec<[f32; 16]>,
    /// Whether the `inverse_bind_matrix` KEY was present (distinct from
    /// count==0). fromLLSD's joint/matrix mismatch-clear only fires when the key
    /// was present, so C++ needs this to mirror it exactly.
    pub inv_bind_present: bool,
    pub bind_shape: Option<[f32; 16]>,
    pub alt_inv_bind: Vec<[f32; 16]>,
    pub pelvis_offset: Option<f32>,
    pub lock_scale: Option<bool>,
}

/// 16 reals from an array node (missing entries -> 0.0, matching asReal()).
fn mat16(node: &Value) -> [f32; 16] {
    let mut m = [0.0f32; 16];
    if let Some(a) = node.as_array() {
        for (i, slot) in m.iter_mut().enumerate() {
            *slot = a.get(i).and_then(Value::as_real).unwrap_or(0.0) as f32;
        }
    }
    m
}

/// Collect an array-of-(array-of-16-reals) into a Vec<[f32;16]>.
fn mat_list(node: Option<&Value>) -> Vec<[f32; 16]> {
    match node.and_then(Value::as_array) {
        Some(a) => a.iter().map(mat16).collect(),
        None => Vec::new(),
    }
}

pub fn decode(compressed: &[u8]) -> Option<SkinOut> {
    let root = llsd::unzip_parse(compressed)?;

    let mut out = SkinOut {
        joint_names: Vec::new(),
        inv_bind: Vec::new(),
        inv_bind_present: root.has("inverse_bind_matrix"),
        bind_shape: None,
        alt_inv_bind: Vec::new(),
        pelvis_offset: None,
        lock_scale: None,
    };

    if let Some(jn) = root.get("joint_names").and_then(Value::as_array) {
        for v in jn {
            // Real assets carry string joint names; anything else -> empty,
            // which the shadow would surface (it won't occur in valid data).
            match v {
                Value::Str(s) => out.joint_names.push(s.clone()),
                _ => out.joint_names.push(Vec::new()),
            }
        }
    }

    out.inv_bind = mat_list(root.get("inverse_bind_matrix"));

    if let Some(bs) = root.get("bind_shape_matrix") {
        out.bind_shape = Some(mat16(bs));
    }

    out.alt_inv_bind = mat_list(root.get("alt_inverse_bind_matrix"));

    if let Some(po) = root.get("pelvis_offset") {
        out.pelvis_offset = Some(po.as_real().unwrap_or(0.0) as f32);
    }

    if let Some(ls) = root.get("lock_scale_if_joint_position") {
        // fromLLSD uses asBoolean(); mirror LLSD-ish truthiness.
        let b = match ls {
            Value::Bool(b) => *b,
            Value::Int(i) => *i != 0,
            Value::Real(r) => *r != 0.0,
            _ => false,
        };
        out.lock_scale = Some(b);
    }

    Some(out)
}
