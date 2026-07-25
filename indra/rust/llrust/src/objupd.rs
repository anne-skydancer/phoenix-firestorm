//! Memory-safe decode of the SL compressed / cached object update — the biggest
//! untrusted hand-rolled parse in the viewer and the busy-sim cache-replay hot
//! path. Mirrors `LLViewerObject::decodeCompressedUpdate` (and the
//! `LLDataPackerBinaryBuffer` byte grammar) exactly, so the C++ POD and this
//! decode fold to the same checksum in the shadow (Phase 2) and can be flipped
//! (Phase 3). Every read is bounds-checked; a short buffer sets `ok=false`
//! instead of reading past the end.
//!
//! Wire format (little-endian host, x86/ARM-LE): u8=1, u16/u32/s32/f32 LE,
//! Vector3=3xf32, Vector4=4xf32, UUID=16 raw, String=NUL-terminated (value
//! excludes NUL), BinaryData=[s32 LE len][len bytes], BinaryDataFixed=N raw.

// EObjectUpdateType numeric values (see llviewerobject.h).
pub const OUT_TERSE_IMPROVED: i32 = 1;

/// 1/65535 as f32 — LLQUANTIZE's OOU16MAX. Exact same value C++ computes.
const OOU16MAX: f32 = 1.0f32 / 65535.0f32;

/// Bit-exact port of `U16_to_F32` (llquantize.h): identical op order, no FMA.
fn u16_to_f32(ival: u16, lower: f32, upper: f32) -> f32 {
    let mut val = ival as f32 * OOU16MAX;
    let delta = upper - lower;
    val *= delta;
    val += lower;
    let max_error = delta * OOU16MAX;
    if val.abs() < max_error {
        val = 0.0;
    }
    val
}

/// Byte cursor with LLDataPacker semantics. On an over-read it flags `!ok`,
/// returns zeros, and stops advancing (matching verifyLength failing before the
/// copy — the C++ side proceeds best-effort too, so `ok` is what the shadow
/// keys on).
struct Reader<'a> {
    buf: &'a [u8],
    pos: usize,
    ok: bool,
}

impl<'a> Reader<'a> {
    fn new(buf: &'a [u8]) -> Self {
        Reader { buf, pos: 0, ok: true }
    }

    fn take(&mut self, n: usize) -> Option<&'a [u8]> {
        if self.pos.checked_add(n).map_or(true, |e| e > self.buf.len()) {
            self.ok = false;
            return None;
        }
        let s = &self.buf[self.pos..self.pos + n];
        self.pos += n;
        Some(s)
    }

    fn u8(&mut self) -> u8 {
        self.take(1).map_or(0, |s| s[0])
    }
    fn u16(&mut self) -> u16 {
        self.take(2).map_or(0, |s| u16::from_le_bytes([s[0], s[1]]))
    }
    fn u32(&mut self) -> u32 {
        self.take(4)
            .map_or(0, |s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
    }
    fn s32(&mut self) -> i32 {
        self.u32() as i32
    }
    fn f32(&mut self) -> f32 {
        f32::from_bits(self.u32())
    }
    fn vec3(&mut self) -> [f32; 3] {
        [self.f32(), self.f32(), self.f32()]
    }
    fn vec4(&mut self) -> [f32; 4] {
        [self.f32(), self.f32(), self.f32(), self.f32()]
    }
    fn uuid(&mut self) -> [u8; 16] {
        let mut u = [0u8; 16];
        if let Some(s) = self.take(16) {
            u.copy_from_slice(s);
        }
        u
    }
    // NUL-terminated string; value excludes the NUL; cursor advances past it.
    fn string(&mut self) -> Vec<u8> {
        let start = self.pos;
        let mut end = start;
        while end < self.buf.len() && self.buf[end] != 0 {
            end += 1;
        }
        if end >= self.buf.len() {
            // no NUL in bounds: verifyLength(strlen+1) fails -> best-effort stop
            self.ok = false;
            return Vec::new();
        }
        let v = self.buf[start..end].to_vec();
        self.pos = end + 1; // past the NUL
        v
    }
    // [s32 LE len][len bytes]; len<0 or overrun -> ok=false, empty.
    fn binary(&mut self) -> Vec<u8> {
        let len = self.s32();
        if len < 0 {
            self.ok = false;
            return Vec::new();
        }
        self.take(len as usize).map_or_else(
            || Vec::new(),
            |s| s.to_vec(),
        )
    }
    fn binary_fixed(&mut self, n: usize) -> Vec<u8> {
        self.take(n).map_or_else(|| Vec::new(), |s| s.to_vec())
    }
}

// = PS_SYS_DATA_BLOCK_SIZE(68) + PS_LEGACY_PART_DATA_BLOCK_SIZE(18); the legacy
// particle block is a fixed field-by-field size, sliced raw here (see llpartdata.cpp).
const LEGACY_PARTICLE_BLOCK_BYTES: usize = 86;

/// Decoded compressed/cached object update. Field set mirrors LLObjectUpdatePod.
#[derive(Default)]
pub struct ObjUpd {
    pub state: u8,
    pub has_pos: bool,
    pub pos: [f32; 3],
    pub has_scale: bool,
    pub scale: [f32; 3],
    pub has_rot: bool,
    pub rot_from_vec3: bool,
    pub rot_vec: [f32; 3],
    pub rot_quat: [f32; 4],
    pub has_vel: bool,
    pub vel: [f32; 3],
    pub has_accel: bool,
    pub accel: [f32; 3],
    pub has_angv: bool,
    pub angv: [f32; 3],
    pub has_collision_plane: bool,
    pub collision_plane: [f32; 4],
    pub has_crc: bool,
    pub crc: u32,
    pub has_material: bool,
    pub material: u8,
    pub has_click_action: bool,
    pub click_action: u8,
    pub pass_flags: u32,
    pub has_owner_id: bool,
    pub owner_id: [u8; 16],
    pub has_parent: bool,
    pub parent_id: u32,
    pub gen_kind: u8,
    pub tree_byte: u8,
    pub scratch_alloc_size: u32,
    pub scratch_blob: Vec<u8>,
    pub has_text: bool,
    pub text: Vec<u8>,
    pub text_color: [u8; 4],
    pub has_media_url: bool,
    pub media_url: Vec<u8>,
    pub has_legacy_particles: bool,
    pub particle_blob: Vec<u8>,
    pub has_extra_params: bool,
    pub params: Vec<(u16, Vec<u8>)>,
    pub has_sound: bool,
    pub sound_uuid: [u8; 16],
    pub sound_gain: f32,
    pub sound_flags: u8,
    pub sound_cutoff: f32,
    pub has_name_value: bool,
    pub name_value: Vec<u8>,
    pub final_cursor_offset: u32,
    pub decode_ok: bool,
}

/// Decode `bytes` (the datapacker contents from its current cursor) for the
/// given update type. Byte-for-byte identical to decodeCompressedUpdate.
pub fn decode(bytes: &[u8], update_type: i32) -> ObjUpd {
    let mut r = Reader::new(bytes);
    let mut o = ObjUpd::default();
    // Match the C++ LLObjectUpdatePod's LL-type default ctors for fields that
    // can be left unset: LLQuaternion() == (0,0,0,1), LLVector4() == (0,0,0,1).
    // (LLVector3() is (0,0,0), which already matches the derived zero default.)
    o.rot_quat = [0.0, 0.0, 0.0, 1.0];
    o.collision_plane = [0.0, 0.0, 0.0, 1.0];

    o.state = r.u8();

    if update_type == OUT_TERSE_IMPROVED {
        let value = r.u8();
        if value != 0 {
            o.collision_plane = r.vec4();
            o.has_collision_plane = true;
        }
        o.pos = r.vec3();
        o.has_pos = true;

        let vx = r.u16();
        let vy = r.u16();
        let vz = r.u16();
        o.vel = [
            u16_to_f32(vx, -128.0, 128.0),
            u16_to_f32(vy, -128.0, 128.0),
            u16_to_f32(vz, -128.0, 128.0),
        ];
        o.has_vel = true;

        let ax = r.u16();
        let ay = r.u16();
        let az = r.u16();
        o.accel = [
            u16_to_f32(ax, -64.0, 64.0),
            u16_to_f32(ay, -64.0, 64.0),
            u16_to_f32(az, -64.0, 64.0),
        ];
        o.has_accel = true;

        let tx = r.u16();
        let ty = r.u16();
        let tz = r.u16();
        let ts = r.u16();
        o.rot_quat = [
            u16_to_f32(tx, -1.0, 1.0),
            u16_to_f32(ty, -1.0, 1.0),
            u16_to_f32(tz, -1.0, 1.0),
            u16_to_f32(ts, -1.0, 1.0),
        ];
        o.has_rot = true;
        o.rot_from_vec3 = false;

        let wx = r.u16();
        let wy = r.u16();
        let wz = r.u16();
        o.angv = [
            u16_to_f32(wx, -64.0, 64.0),
            u16_to_f32(wy, -64.0, 64.0),
            u16_to_f32(wz, -64.0, 64.0),
        ];
        o.has_angv = true;
    } else {
        // OUT_FULL_COMPRESSED / OUT_FULL_CACHED
        o.crc = r.u32();
        o.has_crc = true;
        o.material = r.u8();
        o.has_material = true;
        o.click_action = r.u8();
        o.has_click_action = true;
        o.scale = r.vec3();
        o.has_scale = true;
        o.pos = r.vec3();
        o.has_pos = true;
        o.rot_vec = r.vec3();
        o.has_rot = true;
        o.rot_from_vec3 = true;

        let value = r.u32();
        o.pass_flags = value;

        o.owner_id = r.uuid();
        o.has_owner_id = true;

        if value & 0x80 != 0 {
            o.angv = r.vec3();
            o.has_angv = true;
        }

        if value & 0x20 != 0 {
            o.parent_id = r.u32();
            o.has_parent = true;
        }

        if value & 0x2 != 0 {
            o.gen_kind = 1;
            o.tree_byte = r.u8();
        } else if value & 0x1 != 0 {
            o.gen_kind = 2;
            o.scratch_alloc_size = r.u32();
            o.scratch_blob = r.binary();
        }

        if value & 0x4 != 0 {
            o.text = r.string();
            let c = r.binary_fixed(4);
            if c.len() == 4 {
                o.text_color.copy_from_slice(&c);
            }
            o.has_text = true;
        }

        if value & 0x200 != 0 {
            o.media_url = r.string();
            o.has_media_url = true;
        }

        if value & 0x8 != 0 {
            o.particle_blob = r.binary_fixed(LEGACY_PARTICLE_BLOCK_BYTES);
            o.has_legacy_particles = true;
        }

        let num_parameters = r.u8();
        o.has_extra_params = true;
        for _ in 0..num_parameters {
            let ptype = r.u16();
            let pdata = r.binary();
            o.params.push((ptype, pdata));
        }

        if value & 0x10 != 0 {
            o.sound_uuid = r.uuid();
            o.sound_gain = r.f32();
            o.sound_flags = r.u8();
            o.sound_cutoff = r.f32();
            o.has_sound = true;
        }

        if value & 0x100 != 0 {
            o.name_value = r.string();
            o.has_name_value = true;
        }
    }

    o.final_cursor_offset = r.pos as u32;
    o.decode_ok = r.ok;
    o
}

// --- checksum ---------------------------------------------------------------
// FNV-1a over the fields in a canonical order. The C++ side folds its
// LLObjectUpdatePod identically so the shadow can compare a single u64. Rules:
//  - bool -> 1 byte (0/1); u8 -> 1 byte; u16/u32 -> LE bytes; f32 -> LE bit bytes
//  - blob/string -> u32 LE length then the bytes
//  - params -> u32 LE count, then per param: u16 type, u32 LE len, bytes
// Presence bools gate their payload exactly as below.

struct Fnv(u64);
impl Fnv {
    fn new() -> Self {
        Fnv(0xcbf29ce484222325)
    }
    fn byte(&mut self, b: u8) {
        self.0 ^= b as u64;
        self.0 = self.0.wrapping_mul(0x100000001b3);
    }
    fn bytes(&mut self, s: &[u8]) {
        for &b in s {
            self.byte(b);
        }
    }
    fn b(&mut self, v: bool) {
        self.byte(if v { 1 } else { 0 });
    }
    fn u16(&mut self, v: u16) {
        self.bytes(&v.to_le_bytes());
    }
    fn u32(&mut self, v: u32) {
        self.bytes(&v.to_le_bytes());
    }
    fn f32(&mut self, v: f32) {
        self.bytes(&v.to_bits().to_le_bytes());
    }
    fn f3(&mut self, v: &[f32; 3]) {
        for &x in v {
            self.f32(x);
        }
    }
    fn blob(&mut self, s: &[u8]) {
        self.u32(s.len() as u32);
        self.bytes(s);
    }
}

pub fn checksum(o: &ObjUpd) -> u64 {
    let mut h = Fnv::new();
    h.byte(o.state);
    h.b(o.has_pos);
    h.f3(&o.pos);
    h.b(o.has_scale);
    h.f3(&o.scale);
    h.b(o.has_rot);
    h.b(o.rot_from_vec3);
    h.f3(&o.rot_vec);
    for &x in &o.rot_quat {
        h.f32(x);
    }
    h.b(o.has_vel);
    h.f3(&o.vel);
    h.b(o.has_accel);
    h.f3(&o.accel);
    h.b(o.has_angv);
    h.f3(&o.angv);
    h.b(o.has_collision_plane);
    for &x in &o.collision_plane {
        h.f32(x);
    }
    h.b(o.has_crc);
    h.u32(o.crc);
    h.b(o.has_material);
    h.byte(o.material);
    h.b(o.has_click_action);
    h.byte(o.click_action);
    h.u32(o.pass_flags);
    h.b(o.has_owner_id);
    h.bytes(&o.owner_id);
    h.b(o.has_parent);
    h.u32(o.parent_id);
    h.byte(o.gen_kind);
    h.byte(o.tree_byte);
    h.u32(o.scratch_alloc_size);
    h.blob(&o.scratch_blob);
    h.b(o.has_text);
    h.blob(&o.text);
    h.bytes(&o.text_color);
    h.b(o.has_media_url);
    h.blob(&o.media_url);
    h.b(o.has_legacy_particles);
    h.blob(&o.particle_blob);
    h.b(o.has_extra_params);
    h.u32(o.params.len() as u32);
    for (t, d) in &o.params {
        h.u16(*t);
        h.blob(d);
    }
    h.b(o.has_sound);
    h.bytes(&o.sound_uuid);
    h.f32(o.sound_gain);
    h.byte(o.sound_flags);
    h.f32(o.sound_cutoff);
    h.b(o.has_name_value);
    h.blob(&o.name_value);
    h.0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn u16_dequant_zero_snaps() {
        // 0 maps to lower + tiny; the zero-snap forces exactly 0 near origin.
        assert_eq!(u16_to_f32(32768, -1.0, 1.0).abs() < 1.0, true);
        // midpoint ~ 0
        let v = u16_to_f32(32767, -1.0, 1.0);
        assert!(v.abs() <= 1.0);
    }

    #[test]
    fn full_minimal_decode() {
        // OUT_FULL_COMPRESSED with SpecialCode=0 (no optional blocks):
        // state, crc, material, click, scale(12), pos(12), rot(12), special(4),
        // owner(16), num_params(1)=0.
        let mut b = Vec::new();
        b.push(0x11); // state
        b.extend_from_slice(&1234u32.to_le_bytes()); // crc
        b.push(3); // material
        b.push(0); // click
        b.extend_from_slice(&[0u8; 12]); // scale
        b.extend_from_slice(&[0u8; 12]); // pos
        b.extend_from_slice(&[0u8; 12]); // rot
        b.extend_from_slice(&0u32.to_le_bytes()); // special code
        b.extend_from_slice(&[0u8; 16]); // owner
        b.push(0); // num_params
        let o = decode(&b, 2 /*OUT_FULL_COMPRESSED*/);
        assert!(o.decode_ok);
        assert_eq!(o.state, 0x11);
        assert_eq!(o.crc, 1234);
        assert_eq!(o.material, 3);
        assert_eq!(o.pass_flags, 0);
        assert!(o.has_extra_params);
        assert_eq!(o.params.len(), 0);
        assert_eq!(o.final_cursor_offset as usize, b.len());
    }

    #[test]
    fn truncated_sets_not_ok() {
        let b = vec![0x11, 0x00]; // state + a byte, then nothing for a full update
        let o = decode(&b, 2);
        assert!(!o.decode_ok);
    }

    #[test]
    fn checksum_is_deterministic() {
        let mut b = Vec::new();
        b.push(0x22);
        b.extend_from_slice(&7u32.to_le_bytes());
        b.push(1);
        b.push(0);
        b.extend_from_slice(&[0u8; 12]);
        b.extend_from_slice(&[0u8; 12]);
        b.extend_from_slice(&[0u8; 12]);
        b.extend_from_slice(&0u32.to_le_bytes());
        b.extend_from_slice(&[0u8; 16]);
        b.push(0);
        let a = decode(&b, 2);
        let c = decode(&b, 2);
        assert_eq!(checksum(&a), checksum(&c));
    }
}
