//! std140 layout for softenLight's `SoftenFrameBlock` (binding 0). The transform emits an anonymous
//! std140 block; to feed it correctly a wrong offset is a SILENT burn, so offsets are COMPUTED from
//! the member table (the std140 rules), never hand-placed. Fill by name; unset members stay zero.

#[derive(Clone, Copy)]
pub enum Ty {
    Float,
    Int,
    Bool, // std140 bool = 4 bytes
    Vec2,
    Vec3,
    Vec4,
    Mat3,     // 3 columns, each a vec3 padded to 16 -> 48B, align 16
    Mat4,     // 4 columns of vec4 -> 64B, align 16
    Mat4Arr6, // shadow_matrix[6] -> stride 64, 384B, align 16
}

impl Ty {
    fn align(self) -> usize {
        match self {
            Ty::Float | Ty::Int | Ty::Bool => 4,
            Ty::Vec2 => 8,
            Ty::Vec3 | Ty::Vec4 | Ty::Mat3 | Ty::Mat4 | Ty::Mat4Arr6 => 16,
        }
    }
    fn size(self) -> usize {
        match self {
            Ty::Float | Ty::Int | Ty::Bool => 4,
            Ty::Vec2 => 8,
            Ty::Vec3 => 12,
            Ty::Vec4 => 16,
            Ty::Mat3 => 48,
            Ty::Mat4 => 64,
            Ty::Mat4Arr6 => 64 * 6,
        }
    }
}

/// The `SoftenFrameBlock` members, IN ORDER (from the emitted `fsref_softenLight_ubo.frag`). Order +
/// types are load-bearing: they determine every std140 offset.
pub const MEMBERS: &[(&str, Ty)] = &[
    ("mirror_flag", Ty::Float),
    ("clipPlane", Ty::Vec4),
    ("clipSign", Ty::Float),
    ("scene_light_strength", Ty::Float),
    ("proj_mat", Ty::Mat4),
    ("proj_n", Ty::Vec3),
    ("proj_p", Ty::Vec3),
    ("proj_focus", Ty::Float),
    ("proj_lod", Ty::Float),
    ("proj_range", Ty::Float),
    ("proj_ambiance", Ty::Float),
    ("classic_mode", Ty::Int),
    ("color", Ty::Vec3),
    ("size", Ty::Float),
    ("inv_proj", Ty::Mat4),
    ("screen_res", Ty::Vec2),
    ("waterPlane", Ty::Vec4),
    ("waterSign", Ty::Float),
    ("projection_matrix", Ty::Mat4),
    ("modelview_delta", Ty::Mat4),
    ("inv_modelview_delta", Ty::Mat4),
    ("iterationCount", Ty::Float),
    ("rayStep", Ty::Float),
    ("distanceBias", Ty::Float),
    ("depthRejectBias", Ty::Float),
    ("glossySampleCount", Ty::Float),
    ("adaptiveStepMultiplier", Ty::Float),
    ("noiseSine", Ty::Float),
    ("sun_dir", Ty::Vec3),
    ("moon_dir", Ty::Vec3),
    ("shadow_res", Ty::Vec2),
    ("proj_shadow_res", Ty::Vec2),
    ("shadow_matrix", Ty::Mat4Arr6),
    ("shadow_clip", Ty::Vec4),
    ("shadow_bias", Ty::Float),
    ("shadow_offset", Ty::Float),
    ("spot_shadow_bias", Ty::Float),
    ("spot_shadow_offset", Ty::Float),
    ("sun_up_factor", Ty::Int),
    ("cube_snapshot", Ty::Int),
    ("max_probe_lod", Ty::Float),
    ("transparent_surface", Ty::Bool),
    ("env_mat", Ty::Mat3),
    ("lightnorm", Ty::Vec3),
    ("sunlight_color", Ty::Vec3),
    ("moonlight_color", Ty::Vec3),
    ("ambient_color", Ty::Vec3),
    ("blue_horizon", Ty::Vec3),
    ("blue_density", Ty::Vec3),
    ("haze_horizon", Ty::Float),
    ("haze_density", Ty::Float),
    ("cloud_shadow", Ty::Float),
    ("density_multiplier", Ty::Float),
    ("distance_multiplier", Ty::Float),
    ("max_y", Ty::Float),
    ("glow", Ty::Vec3),
    ("sun_moon_glow_factor", Ty::Float),
    ("sky_sunlight_scale", Ty::Float),
    ("sky_ambient_scale", Ty::Float),
    ("sky_hdr_scale", Ty::Float),
    ("waterFogColor", Ty::Vec4),
    ("waterFogDensity", Ty::Float),
    ("waterFogKS", Ty::Float),
    ("blur_size", Ty::Float),
    ("blur_fidelity", Ty::Float),
    ("ssao_irradiance_scale", Ty::Float),
    ("ssao_irradiance_max", Ty::Float),
    ("ssao_effect_mat", Ty::Mat3),
];

fn round_up(x: usize, a: usize) -> usize {
    (x + a - 1) / a * a
}

/// Compute the std140 offset of each member + the total block size (rounded to 16).
fn layout() -> (Vec<(&'static str, Ty, usize)>, usize) {
    let mut off = 0usize;
    let mut out = Vec::with_capacity(MEMBERS.len());
    for &(name, ty) in MEMBERS {
        off = round_up(off, ty.align());
        out.push((name, ty, off));
        off += ty.size();
    }
    (out, round_up(off, 16))
}

/// A zero-initialized std140 buffer for `SoftenFrameBlock`, filled by member name.
pub struct Block {
    buf: Vec<u8>,
    off: std::collections::HashMap<&'static str, (Ty, usize)>,
}

impl Block {
    pub fn new() -> Block {
        let (members, size) = layout();
        let mut off = std::collections::HashMap::new();
        for (name, ty, o) in members {
            off.insert(name, (ty, o));
        }
        Block { buf: vec![0u8; size], off }
    }

    pub fn bytes(&self) -> &[u8] {
        &self.buf
    }

    fn put(&mut self, name: &str, want: usize, data: &[u8]) {
        match self.off.get(name) {
            Some(&(ty, o)) => {
                debug_assert!(ty.size() >= want, "{name}: size mismatch");
                self.buf[o..o + data.len()].copy_from_slice(data);
            }
            None => panic!("std140: unknown SoftenFrameBlock member '{name}'"),
        }
    }

    pub fn f32(&mut self, name: &str, v: f32) -> &mut Self {
        self.put(name, 4, &v.to_le_bytes());
        self
    }
    pub fn i32(&mut self, name: &str, v: i32) -> &mut Self {
        self.put(name, 4, &v.to_le_bytes());
        self
    }
    pub fn vec2(&mut self, name: &str, v: [f32; 2]) -> &mut Self {
        let b: Vec<u8> = v.iter().flat_map(|f| f.to_le_bytes()).collect();
        self.put(name, 8, &b);
        self
    }
    pub fn vec3(&mut self, name: &str, v: [f32; 3]) -> &mut Self {
        let b: Vec<u8> = v.iter().flat_map(|f| f.to_le_bytes()).collect();
        self.put(name, 12, &b);
        self
    }
    pub fn vec4(&mut self, name: &str, v: [f32; 4]) -> &mut Self {
        let b: Vec<u8> = v.iter().flat_map(|f| f.to_le_bytes()).collect();
        self.put(name, 16, &b);
        self
    }
    /// mat4 as 16 floats, column-major (std140 mat4 = 4 tightly-packed vec4 columns).
    pub fn mat4(&mut self, name: &str, m: [f32; 16]) -> &mut Self {
        let b: Vec<u8> = m.iter().flat_map(|f| f.to_le_bytes()).collect();
        self.put(name, 64, &b);
        self
    }
    /// mat3 as 3 columns of vec3, EACH padded to 16 bytes (std140).
    pub fn mat3(&mut self, name: &str, cols: [[f32; 3]; 3]) -> &mut Self {
        let mut b = vec![0u8; 48];
        for (c, col) in cols.iter().enumerate() {
            for (i, f) in col.iter().enumerate() {
                let o = c * 16 + i * 4;
                b[o..o + 4].copy_from_slice(&f.to_le_bytes());
            }
        }
        self.put(name, 48, &b);
        self
    }
}
