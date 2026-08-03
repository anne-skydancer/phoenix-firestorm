//! Faithful port of stock Firestorm's legacy terrain COMPOSITION generation -- the per-texel
//! `[0,3]` band value + per-vertex `[0,1]` noise that drive the 4-detail splat. Ported byte-for-byte
//! from `indra/newview/noise.{h,cpp}` (the terrain noise, NOT `llperlin.cpp`) +
//! `LLVLComposition::generateHeights` + `LLSurfacePatch::eval` (`llvlcomposition.cpp:461-561`,
//! `llsurfacepatch.cpp:254-265`).
//!
//! Determinism: stock seeds the Perlin tables with a FIXED `srand(42)` then draws from the C
//! runtime `rand()`. Stock is MSVC, whose `rand()` is the LCG below. We reproduce that LCG + the
//! exact `init()` draw order, so the tables -- and thus the terrain -- match stock bit-for-bit.
//! (The trailing `srand(time(NULL))` in stock runs AFTER the tables are built, so it never affects
//! them; the tables are static per process.) All math is f32 to match stock.

const B: usize = 0x100; // 256

/// The Perlin tables, initialized exactly once (they're static per process, like stock's srand(42)).
pub fn tables() -> &'static PerlinTables {
    use std::sync::OnceLock;
    static T: OnceLock<PerlinTables> = OnceLock::new();
    T.get_or_init(PerlinTables::init)
}

/// MSVC `rand()` / `srand()`: LCG `state = state*214013 + 2531011; return (state>>16) & 0x7fff`.
struct MsvcRand {
    state: u32,
}
impl MsvcRand {
    fn new(seed: u32) -> Self {
        Self { state: seed }
    }
    #[inline]
    fn next(&mut self) -> i32 {
        self.state = self.state.wrapping_mul(214013).wrapping_add(2531011);
        ((self.state >> 16) & 0x7fff) as i32
    }
}

/// The seeded Perlin lattice: permutation `p` and 2D gradient rows `g2` (g1/g3 are drawn too --
/// they consume `rand()` -- but only g2 is read by `noise2`). Sized `B+B+2` like stock.
pub struct PerlinTables {
    p: [i32; B + B + 2],
    g2: [[f32; 2]; B + B + 2],
}

fn normalize2(v: &mut [f32; 2]) {
    let s = 1.0 / (v[0] * v[0] + v[1] * v[1]).sqrt();
    v[0] *= s;
    v[1] *= s;
}

impl PerlinTables {
    /// Port of `noise.h:311-348` init(): srand(42) then the exact gradient + shuffle draw order.
    pub fn init() -> Self {
        let mut r = MsvcRand::new(42);
        let mut p = [0i32; B + B + 2];
        let mut g2 = [[0.0f32; 2]; B + B + 2];
        for i in 0..B {
            p[i] = i as i32;
            // g1 (1 draw) -- unused by noise2 but MUST be drawn to keep the rand sequence aligned.
            let _g1 = ((r.next() % (B as i32 + B as i32)) - B as i32) as f32 / B as f32;
            for j in 0..2 {
                g2[i][j] = ((r.next() % (B as i32 + B as i32)) - B as i32) as f32 / B as f32;
            }
            normalize2(&mut g2[i]);
            // g3 (3 draws) -- unused by noise2 but drawn for sequence alignment.
            for _j in 0..3 {
                let _ = (r.next() % (B as i32 + B as i32)) - B as i32;
            }
        }
        // shuffle: C `while(--i)` from i=255 down to i=1 (255 iterations)
        let mut i = B;
        loop {
            i -= 1;
            if i == 0 {
                break;
            }
            let k = p[i];
            let j = (r.next() % B as i32) as usize;
            p[i] = p[j];
            p[j] = k;
        }
        // duplicate the first B+2 entries into the upper half
        for i in 0..B + 2 {
            p[B + i] = p[i];
            g2[B + i] = g2[i];
        }
        PerlinTables { p, g2 }
    }

    /// Port of `noise.cpp:43-84` noise2 (2D Perlin lattice). Only x,y are used.
    #[inline]
    pub fn noise2(&self, vx: f32, vy: f32) -> f32 {
        // fast_setup (noise.h:131-141): +NF32 (4096) offset, u8-truncated lattice index.
        #[inline]
        fn fast_setup(v: f32) -> (u8, u8, f32, f32) {
            let r1o = v + 4096.0;
            let t = r1o as i32; // lltrunc (toward zero)
            let b0 = t as u8; // (U8)t == t & 0xff
            let b1 = b0.wrapping_add(1);
            let r0 = r1o - t as f32;
            let r1 = r0 - 1.0;
            (b0, b1, r0, r1)
        }
        #[inline]
        fn s_curve(t: f32) -> f32 {
            t * t * (3.0 - 2.0 * t)
        }
        #[inline]
        fn lerp_m(t: f32, a: f32, b: f32) -> f32 {
            a + t * (b - a)
        }

        let (bx0, bx1, rx0, rx1) = fast_setup(vx);
        let (by0, by1, ry0, ry1) = fast_setup(vy);

        let i = self.p[bx0 as usize];
        let j = self.p[bx1 as usize];

        let b00 = self.p[(i + by0 as i32) as usize] as usize;
        let b10 = self.p[(j + by0 as i32) as usize] as usize;
        let b01 = self.p[(i + by1 as i32) as usize] as usize;
        let b11 = self.p[(j + by1 as i32) as usize] as usize;

        let sx = s_curve(rx0);
        let sy = s_curve(ry0);

        let q = &self.g2[b00];
        let u = rx0 * q[0] + ry0 * q[1];
        let q = &self.g2[b10];
        let v = rx1 * q[0] + ry0 * q[1];
        let a = lerp_m(sx, u, v);

        let q = &self.g2[b01];
        let u = rx0 * q[0] + ry1 * q[1];
        let q = &self.g2[b11];
        let v = rx1 * q[0] + ry1 * q[1];
        let b = lerp_m(sx, u, v);

        lerp_m(sy, a, b)
    }

    /// Port of `noise.h:57-67` turbulence2. For freq=2: noise2(2v)/2 + noise2(1v)/1.
    #[inline]
    pub fn turbulence2(&self, vx: f32, vy: f32, mut freq: f32) -> f32 {
        let mut t = 0.0f32;
        while freq >= 1.0 {
            t += self.noise2(freq * vx, freq * vy) / freq;
            freq *= 0.5;
        }
        t
    }
}

/// Stock `bilinear` (`llvlcomposition.cpp:54-68`). Callsite passes (SW,SE,NW,NE) as (v00,v01,v10,v11).
#[inline]
fn bilinear(v00: f32, v01: f32, v10: f32, v11: f32, x_frac: f32, y_frac: f32) -> f32 {
    let inv_x = 1.0 - x_frac;
    let inv_y = 1.0 - y_frac;
    inv_x * inv_y * v00 + x_frac * inv_y * v10 + inv_x * y_frac * v01 + x_frac * y_frac * v11
}

const XY_SCALE_INV: f32 = 1.0 / 4.9215; // generateHeights xyScale
const Z_SCALE_INV: f32 = 0.25; // 1/zScale (zScale=4); unused by noise2 but part of stock's vec
const SLOPE_SQUARED: f32 = 1.5 * 1.5; // 2.25
const NOISE_MAGNITUDE: f32 = 2.0;

/// Port of `LLVLComposition::generateHeights` (`llvlcomposition.cpp:461-561`). Produces the
/// composition array `mDatap` (dim_c x dim_c, dim_c = grids_per_region_edge = 256), values in [0,3].
///
/// `heights` = the fed heightmap in mSurfaceZ order (`h[i + j*hdim]`, hdim = 257). `mpg` = mScale
/// (metres per grid, 1.0). `origin_global` = region SW corner in GLOBAL metres (F64, for lattice
/// continuity across regions). `start`/`range` = the 4 corner params (SW,SE,NW,NE) from the sim.
pub fn build_composition(
    dim_c: usize,
    heights: &[f32],
    hdim: usize,
    mpg: f32,
    origin_global: [f64; 2],
    start: [f32; 4],
    range: [f32; 4],
    t: &PerlinTables,
) -> Vec<f32> {
    let inv_width = 1.0 / dim_c as f32;
    let mut datap = vec![0.0f32; dim_c * dim_c];
    for j in 0..dim_c {
        for i in 0..dim_c {
            let start_h = bilinear(start[0], start[1], start[2], start[3], i as f32 * inv_width, j as f32 * inv_width);
            let range_h = bilinear(range[0], range[1], range[2], range[3], i as f32 * inv_width, j as f32 * inv_width);
            // height = resolveHeightRegion((i*mScale, j*mScale)); for integer grid = the grid sample.
            let height = heights.get(i + j * hdim).copied().unwrap_or(0.0);
            // vec = (global_xy + location) * xyScaleInv; cast (global+loc) to f32 BEFORE scaling (stock).
            let vx = ((origin_global[0] + (i as f32 * mpg) as f64) as f32) * XY_SCALE_INV;
            let vy = ((origin_global[1] + (j as f32 * mpg) as f64) as f32) * XY_SCALE_INV;
            let _vz = height * Z_SCALE_INV; // set in stock, unused by the 2D noise
            let v1x = vx * 0.2222222222;
            let v1y = vy * 0.2222222222;
            let mut twiddle = t.noise2(v1x, v1y) * 6.5;
            twiddle += t.turbulence2(vx, vy, 2.0) * SLOPE_SQUARED;
            twiddle *= NOISE_MAGNITUDE;
            let scaled = (height + twiddle - start_h) * 4.0 / range_h; // ASSET_COUNT = 4
            datap[i + j * dim_c] = scaled.max(0.0).min(3.0);
        }
    }
    datap
}

const TC1_XY_SCALE_INV: f32 = (1.0 / (4.9215 * 7.0)) * 0.2222222222; // eval xyScale*7

/// Port of `LLSurfacePatch::eval` texcoord1 (`llsurfacepatch.cpp:254-265`). For grid vertex
/// (gx, gy): `.x` = composition (nearest datap cell; getValueScaled bilinear collapses to the cell
/// value at integer coords, clamped to [0,dim_c-1]); `.y` = per-vertex noise in [0,1].
#[inline]
pub fn vertex_texcoord1(gx: usize, gy: usize, dim_c: usize, datap: &[f32], origin_global: [f64; 2], t: &PerlinTables) -> [f32; 2] {
    let cx = gx.min(dim_c - 1);
    let cy = gy.min(dim_c - 1);
    let comp = datap[cx + cy * dim_c];
    // noise: fmod on positive values == Rust % ; noise2*0.75+0.5 clamped [0,1]
    let nx = (((origin_global[0] + gx as f64) as f32) * TC1_XY_SCALE_INV) % 256.0;
    let ny = (((origin_global[1] + gy as f64) as f32) * TC1_XY_SCALE_INV) % 256.0;
    let rand_val = (t.noise2(nx, ny) * 0.75 + 0.5).clamp(0.0, 1.0);
    [comp, rand_val]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tables_are_deterministic_and_valid() {
        let a = PerlinTables::init();
        let b = PerlinTables::init();
        // deterministic
        assert_eq!(a.p, b.p, "p table must be reproducible");
        // p[0..256] is a permutation of 0..255
        let mut seen = [false; B];
        for k in 0..B {
            let v = a.p[k];
            assert!((0..B as i32).contains(&v));
            assert!(!seen[v as usize], "p[0..256] must be a permutation");
            seen[v as usize] = true;
        }
        // upper half duplicates the lower
        for i in 0..B + 2 {
            assert_eq!(a.p[B + i], a.p[i]);
        }
        // g2 rows are unit-length
        for i in 0..B {
            let len = (a.g2[i][0] * a.g2[i][0] + a.g2[i][1] * a.g2[i][1]).sqrt();
            assert!((len - 1.0).abs() < 1e-4, "g2[{i}] not normalized");
        }
    }

    #[test]
    fn noise2_is_bounded_and_stable() {
        let t = PerlinTables::init();
        // Perlin lattice noise is roughly in [-1,1]; just sanity-check finiteness + determinism.
        for k in 0..64 {
            let x = k as f32 * 0.37;
            let y = k as f32 * 0.91;
            let n = t.noise2(x, y);
            assert!(n.is_finite());
            assert_eq!(n, t.noise2(x, y));
            assert!(n.abs() < 2.0);
        }
    }
}
