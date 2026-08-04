//! The TEST side of the atmospherics A/B bench: runs fs_render's own `resolve.frag` (read from disk,
//! the authoritative source) on a fixture that is the LOGICAL EQUIVALENT of the softenLight oracle
//! fixture (same base color, world normal chosen so dot(n,sun)=nl, same sky params, same regime).
//! fs_ogl_ref temporarily hosts BOTH sides because it owns the GLSL->SPIR-V + fixture infrastructure;
//! the point is the empirical divergence number, not the crate boundary.
//!
//! resolve.frag is plain Vulkan GLSL (its own 60-float std140 Sky UBO + 2 texelFetch G-buffer RTs, no
//! samplers) -- compiles directly via shaderc, no UBO transform needed.

use wgpu::util::DeviceExt;

pub const SCENE_HDR: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

fn f16(x: f32) -> [u8; 2] {
    let bits = x.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exp = ((bits >> 23) & 0xff) as i32 - 127 + 15;
    let mant = (bits >> 13) & 0x3ff;
    let h = if exp <= 0 { sign } else if exp >= 31 { sign | 0x7c00 } else { sign | ((exp as u16) << 10) | mant as u16 };
    h.to_le_bytes()
}
fn rgba16f(r: f32, g: f32, b: f32, a: f32) -> Vec<u8> {
    [f16(r), f16(g), f16(b), f16(a)].concat()
}

/// A full-size R32Float depth texture (resolve.frag texelFetches g_depth at integer fragcoord).
/// Uniform 0.95 to match the oracle's depthMap (getPositionWithDepth reconstructs the same view pos).
fn depth_full(device: &wgpu::Device, queue: &wgpu::Queue, size: u32, depth: f32) -> wgpu::TextureView {
    let mut data = Vec::with_capacity((size * size) as usize * 4);
    let b = depth.to_le_bytes();
    for _ in 0..(size * size) { data.extend_from_slice(&b); }
    let t = device.create_texture_with_data(
        queue,
        &wgpu::TextureDescriptor {
            label: Some("g_depth"),
            size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::R32Float, usage: wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
        },
        wgpu::util::TextureDataOrder::LayerMajor,
        &data,
    );
    t.create_view(&wgpu::TextureViewDescriptor::default())
}

/// A cube-map ARRAY (`colors.len()` cubes, 6 faces each, 1x1) with a uniform color per cube. Used as the
/// radiance/irradiance probe arrays. Uniform-per-cube -> textureLod returns that cube's color for any
/// direction/lod, isolating the weight/mix/refParams + LAYER-INDEXING math from direction+parallax.
pub fn cube_array_layers(device: &wgpu::Device, queue: &wgpu::Queue, colors: &[[f32; 3]], label: &str) -> wgpu::TextureView {
    let n = colors.len().max(1) as u32;
    let mut data = Vec::with_capacity((n * 6) as usize * 8);
    for c in colors {
        let texel = rgba16f(c[0], c[1], c[2], 1.0);
        for _ in 0..6 { data.extend_from_slice(&texel); }
    }
    let t = device.create_texture_with_data(
        queue,
        &wgpu::TextureDescriptor {
            label: Some(label),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: n * 6 },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba16Float, usage: wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
        },
        wgpu::util::TextureDataOrder::LayerMajor,
        &data,
    );
    t.create_view(&wgpu::TextureViewDescriptor { dimension: Some(wgpu::TextureViewDimension::CubeArray), ..Default::default() })
}

// The split-sum environment-BRDF LUT (genbrdflutF.glsl): 1024-sample GGX importance-sampled integral,
// indexed [NoV, roughness], stored (A,B) in .rg. Generated once on the CPU and bound to BOTH the oracle
// (brdfLut, binding 7) and resolve (binding 11) so the PBR IBL specular (pbrIbl) is apples-to-apples.
fn brdf_hammersley(i: u32, n: u32) -> (f32, f32) {
    let mut bits = (i << 16) | (i >> 16);
    bits = ((bits & 0x5555_5555) << 1) | ((bits & 0xAAAA_AAAA) >> 1);
    bits = ((bits & 0x3333_3333) << 2) | ((bits & 0xCCCC_CCCC) >> 2);
    bits = ((bits & 0x0F0F_0F0F) << 4) | ((bits & 0xF0F0_F0F0) >> 4);
    bits = ((bits & 0x00FF_00FF) << 8) | ((bits & 0xFF00_FF00) >> 8);
    (i as f32 / n as f32, bits as f32 * 2.3283064365386963e-10)
}
fn brdf_random(x: f32, z: f32) -> f32 {
    let dt = x * 12.9898 + z * 78.233;
    let sn = dt % 3.14;
    let v = (sn.sin() * 43758.5453).fract();
    if v < 0.0 { v + 1.0 } else { v } // GLSL fract is always >= 0
}
fn brdf_pair(nov: f32, roughness: f32) -> (f32, f32) {
    // Normal along +z; V in the x-z plane. Importance-sample GGX, accumulate (A,B).
    let n = [0.0f32, 0.0, 1.0];
    let v = [(1.0 - nov * nov).max(0.0).sqrt(), 0.0, nov];
    let alpha = roughness * roughness;
    let k = alpha / 2.0; // G_SchlicksmithGGX uses k = (roughness*roughness)/2
    let (mut a, mut b) = (0.0f32, 0.0f32);
    let num = 1024u32;
    for i in 0..num {
        let (xi0, xi1) = brdf_hammersley(i, num);
        let phi = 2.0 * std::f32::consts::PI * xi0 + brdf_random(n[0], n[2]) * 0.1;
        let cos_theta = ((1.0 - xi1) / (1.0 + (alpha * alpha - 1.0) * xi1)).sqrt();
        let sin_theta = (1.0 - cos_theta * cos_theta).sqrt();
        let h_ts = [sin_theta * phi.cos(), sin_theta * phi.sin(), cos_theta];
        // tangent space with normal=(0,0,1): up=(1,0,0); tangentX=norm(cross(up,n)), tangentY=cross(n,tx)
        let up = [1.0f32, 0.0, 0.0];
        let cross = |x: [f32; 3], y: [f32; 3]| [x[1]*y[2]-x[2]*y[1], x[2]*y[0]-x[0]*y[2], x[0]*y[1]-x[1]*y[0]];
        let norm = |x: [f32; 3]| { let l = (x[0]*x[0]+x[1]*x[1]+x[2]*x[2]).sqrt(); [x[0]/l, x[1]/l, x[2]/l] };
        let tx = norm(cross(up, n));
        let ty = cross(n, tx);
        let h = norm([tx[0]*h_ts[0]+ty[0]*h_ts[1]+n[0]*h_ts[2],
                      tx[1]*h_ts[0]+ty[1]*h_ts[1]+n[1]*h_ts[2],
                      tx[2]*h_ts[0]+ty[2]*h_ts[1]+n[2]*h_ts[2]]);
        let vdoth = v[0]*h[0] + v[1]*h[1] + v[2]*h[2];
        let l = [2.0*vdoth*h[0]-v[0], 2.0*vdoth*h[1]-v[1], 2.0*vdoth*h[2]-v[2]];
        let dot_nl = l[2].max(0.0);
        let dot_nv = v[2].max(0.0);
        let dot_vh = vdoth.max(0.0);
        let dot_nh = h[2].max(0.0);
        if dot_nl > 0.0 {
            let gl = dot_nl / (dot_nl * (1.0 - k) + k);
            let gv = dot_nv / (dot_nv * (1.0 - k) + k);
            let g = gl * gv;
            let g_vis = (g * dot_vh) / (dot_nh * dot_nv);
            let fc = (1.0 - dot_vh).powf(5.0);
            a += (1.0 - fc) * g_vis;
            b += fc * g_vis;
        }
    }
    (a / num as f32, b / num as f32)
}
pub fn brdf_lut(device: &wgpu::Device, queue: &wgpu::Queue) -> wgpu::TextureView {
    let res = 128u32;
    let mut data = Vec::with_capacity((res * res) as usize * 8);
    for y in 0..res {
        for x in 0..res {
            let nov = x as f32 / (res - 1) as f32;      // uv.s
            let rough = 1.0 - y as f32 / (res - 1) as f32; // BRDF(uv.s, 1.0 - uv.t)
            let (a, b) = brdf_pair(nov, rough);
            data.extend_from_slice(&rgba16f(a, b, 0.0, 1.0));
        }
    }
    let t = device.create_texture_with_data(
        queue,
        &wgpu::TextureDescriptor {
            label: Some("brdfLut"),
            size: wgpu::Extent3d { width: res, height: res, depth_or_array_layers: 1 },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba16Float, usage: wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
        },
        wgpu::util::TextureDataOrder::LayerMajor,
        &data,
    );
    t.create_view(&wgpu::TextureViewDescriptor::default())
}

// std140 offsets of the ReflectionProbes block (must match resolve.frag / llreflectionmapmanager.h).
pub const RP_REFBOX: usize = 0;         // mat4[256]  16384
pub const RP_HEROBOX: usize = 16384;    // mat4         64
pub const RP_REFSPHERE: usize = 16448;  // vec4[256]  4096
pub const RP_REFPARAMS: usize = 20544;  // vec4[256]  4096
pub const RP_HEROSPHERE: usize = 24640; // vec4         16
pub const RP_REFINDEX: usize = 24656;   // ivec4[256] 4096
pub const RP_REFNEIGHBOR: usize = 28752;// ivec4[1024]16384
pub const RP_REFBUCKET: usize = 45136;  // ivec4[256] 4096
pub const RP_REFMAPCOUNT: usize = 49232;// int
pub const RP_SIZE: usize = 49248;       // rounded to 16

// std140 writers into the ReflectionProbes buffer (element i of a vec4/ivec4 array at base `off`).
fn put_v4(b: &mut [u8], off: usize, v: [f32; 4]) {
    for (i, f) in v.iter().enumerate() { b[off + i * 4..off + i * 4 + 4].copy_from_slice(&f.to_le_bytes()); }
}
fn put_i4(b: &mut [u8], off: usize, v: [i32; 4]) {
    for (i, f) in v.iter().enumerate() { b[off + i * 4..off + i * 4 + 4].copy_from_slice(&f.to_le_bytes()); }
}

/// A description of a probe fixture shared by the oracle + resolve sides (same UBO + cube colors + env).
pub struct ProbeFixture {
    pub ubo: Vec<u8>,            // the std140 ReflectionProbes block bytes (RP_SIZE)
    pub radiance: Vec<[f32; 3]>,   // per cube-array LAYER radiance (glossy) color
    pub irradiance: Vec<[f32; 3]>, // per cube-array LAYER irradiance (diffuse) color
    pub env_identity: bool,      // set env_mat = identity (fixture view==world); false leaves it zeroed
}

impl ProbeFixture {
    /// Neutral probes: zeroed UBO + black cubes. Both sides sample only probe 0 with zero weights ->
    /// sampleProbeAmbient/sampleProbes return 0 (matches the oracle's zeroed-probe behavior).
    pub fn neutral() -> ProbeFixture {
        ProbeFixture { ubo: vec![0u8; RP_SIZE], radiance: vec![[0.0; 3]], irradiance: vec![[0.0; 3]], env_identity: true }
    }

    /// A single default/automatic sphere probe at index 0 covering the fixture fragment cluster, with
    /// unit irradiance+radiance scales and unit weight coeff. refmapCount=1 -> only probe 0 is sampled
    /// (loop empty, probe 0 appended), on BOTH sides (oracle needs no REFMAP_LEVEL for the default probe).
    pub fn default_probe(radiance: [f32; 3], irradiance: [f32; 3]) -> ProbeFixture {
        let mut b = vec![0u8; RP_SIZE];
        // Fragment cluster sits ~9.6 m in front along -z (depth 0.95 through the fixture proj). Center the
        // probe there with a large radius so every pixel is inside; unit weight coeff / irr+rad scales.
        put_v4(&mut b, RP_REFSPHERE, [0.0, 0.0, -9.64, 100.0]);
        put_v4(&mut b, RP_REFPARAMS, [1.0, 1.0, 1.0, 0.1]); // x irr scale, y rad scale, z weight coeff, w znear
        put_i4(&mut b, RP_REFINDEX, [0, -1, 0, 0]);         // cube layer 0, no neighbors, priority 0 (automatic)
        b[RP_REFMAPCOUNT..RP_REFMAPCOUNT + 4].copy_from_slice(&1i32.to_le_bytes());
        ProbeFixture { ubo: b, radiance: vec![radiance], irradiance: vec![irradiance], env_identity: true }
    }

    /// Default (automatic, layer 0) probe 0 + a MANUAL sphere probe 1 (layer 1, priority 1, real radius
    /// -> parallax ON). refmapCount=2, refBucket -> start at probe 1, so this exercises the spatial WALK
    /// (getStartIndex + loop + shouldSampleProbe sphere influence), sphereIntersect PARALLAX, sphereWeight,
    /// the auto/manual MIX (col[0] vs col[1] via dwsum), and cube LAYER indexing (layer 0 vs 1).
    pub fn two_probe(rad0: [f32; 3], irr0: [f32; 3], rad1: [f32; 3], irr1: [f32; 3]) -> ProbeFixture {
        let mut b = vec![0u8; RP_SIZE];
        // probe 0 -- default/automatic, layer 0, large radius (covers everything), priority 0.
        put_v4(&mut b, RP_REFSPHERE + 0 * 16, [0.0, 0.0, -9.64, 200.0]);
        put_v4(&mut b, RP_REFPARAMS + 0 * 16, [1.0, 1.0, 1.0, 0.1]);
        put_i4(&mut b, RP_REFINDEX + 0 * 16, [0, -1, 0, 0]);   // layer 0, no neighbors, priority 0
        // probe 1 -- manual, layer 1, tighter radius (real parallax), priority 1.
        put_v4(&mut b, RP_REFSPHERE + 1 * 16, [0.0, 0.0, -9.64, 30.0]);
        put_v4(&mut b, RP_REFPARAMS + 1 * 16, [1.0, 1.0, 1.0, 0.1]);
        put_i4(&mut b, RP_REFINDEX + 1 * 16, [1, -1, 0, 1]);   // layer 1, no neighbors, priority 1 (manual)
        // bucket: every depth slab starts the walk at probe 1 (so the loop actually visits it).
        for i in 0..256 { put_i4(&mut b, RP_REFBUCKET + i * 16, [1, 0, 0, 0]); }
        b[RP_REFMAPCOUNT..RP_REFMAPCOUNT + 4].copy_from_slice(&2i32.to_le_bytes());
        ProbeFixture { ubo: b, radiance: vec![rad0, rad1], irradiance: vec![irr0, irr1], env_identity: true }
    }
}

/// ProbeParams UBO bytes: std140 { mat4 pp_inv_proj; mat4 pp_env_mat; vec4 pp_misc; } = 144 bytes.
pub fn probe_params_bytes(env_identity: bool) -> Vec<u8> {
    let inv_proj = crate::soften_pass::perspective_inv(60.0f32.to_radians(), 1.0, 0.5, 256.0);
    let env: [f32; 16] = if env_identity {
        [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
    } else { [0.0; 16] };
    let misc: [f32; 4] = [6.0, 0.0, 0.0, 0.0]; // max_probe_lod = 6 (log2(128)-1), cube_snapshot = 0
    let mut v: Vec<f32> = Vec::with_capacity(36);
    v.extend_from_slice(&inv_proj);
    v.extend_from_slice(&env);
    v.extend_from_slice(&misc);
    bytemuck::cast_slice(&v).to_vec()
}

/// A `size`x`size` texture uniformly filled with one rgba16f texel. resolve.frag uses texelFetch with
/// the actual integer fragcoord (0..size), so the G-buffer must be full-resolution -- a 1x1 would read
/// out-of-bounds (returns 0) everywhere and discard the whole frame.
fn tex_full(device: &wgpu::Device, queue: &wgpu::Queue, size: u32, texel: &[u8], label: &str) -> wgpu::TextureView {
    let mut data = Vec::with_capacity((size * size) as usize * texel.len());
    for _ in 0..(size * size) { data.extend_from_slice(texel); }
    let t = device.create_texture_with_data(
        queue,
        &wgpu::TextureDescriptor {
            label: Some(label),
            size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
            format: SCENE_HDR, usage: wgpu::TextureUsages::TEXTURE_BINDING, view_formats: &[],
        },
        wgpu::util::TextureDataOrder::LayerMajor,
        &data,
    );
    t.create_view(&wgpu::TextureViewDescriptor::default())
}

/// Build resolve.frag's 60-float Sky UBO, filled to match fixture_sky_block(). Indices match the
/// `layout(...) uniform Sky` comments in resolve.frag exactly.
fn resolve_sky_ubo() -> [f32; 60] {
    let mut u = [0.0f32; 60];
    // u0-15 inv_view_proj: the SAME 60deg/aspect-1 perspective the oracle fixture uses (view=identity,
    // camera at eye origin). resolve.frag reconstructs the view RAY from this; the ray direction is
    // convention-independent (any ndc-z unprojects to a point ON the ray), so the GL-form inverse is
    // fine. This makes resolve's ray-derived v the SAME physical ray as the oracle's -normalize(pos).
    u[0..16].copy_from_slice(&crate::soften_pass::perspective_inv(60.0f32.to_radians(), 1.0, 0.5, 256.0));
    // u16-19: cam.xyz = origin (matches the oracle's eye-at-origin), max_y
    u[16] = 0.0; u[17] = 0.0; u[18] = 0.0;
    u[19] = 1605.0;
    // u20-23: lightnorm.xyz (OGL Y-up), sun_up_factor
    u[20] = 0.0; u[21] = 0.707; u[22] = 0.707; u[23] = 1.0;
    // u24-27: sun_color.rgb, density_multiplier
    u[24] = 0.7342; u[25] = 0.7815; u[26] = 0.8999; u[27] = 0.0001;
    // u28-31: moon_color.rgb, sun_moon_glow_factor
    u[28] = 0.0; u[29] = 0.0; u[30] = 0.0; u[31] = 1.0;
    // u32-35: ambient.rgb, haze_density
    u[32] = 0.25; u[33] = 0.25; u[34] = 0.25; u[35] = 0.7;
    // u36-39: blue_horizon.rgb, haze_horizon
    u[36] = 0.4954; u[37] = 0.4954; u[38] = 0.6399; u[39] = 0.19;
    // u40-43: blue_density.rgb, cloud_shadow
    u[40] = 0.2447; u[41] = 0.4487; u[42] = 0.7599; u[43] = 0.2699;
    // u44-47: glow.xyz, sky_sunlight_scale
    u[44] = 5.0; u[45] = 0.001; u[46] = -0.4799; u[47] = 1.5;
    // u48-51: sky_hdr_scale, sky_ambient_scale, vp_w, vp_h
    u[48] = 1.0; u[49] = 1.5; u[50] = 256.0; u[51] = 256.0;
    // u52-55: sun_dir.xyz (world, SL Z-up), distance_multiplier
    u[52] = 0.0; u[53] = 0.707; u[54] = 0.707; u[55] = 0.8;
    // u56-59: moon_dir.xyz (world), classic_mode (w) -- matches scene.rs fullscreen_sky_ubo()
    // u[59] set by the caller per regime.
    u
}

pub struct ResolveResult {
    pub target: wgpu::Texture,
    pub view: wgpu::TextureView,
}

/// Compile + run resolve.frag on the equivalent fixture. `classic` is written into u56 (extra.x) so a
/// regime-aware resolve.frag can read it; the current resolve.frag ignores it (always non-classic).
pub fn render(device: &wgpu::Device, queue: &wgpu::Queue, size: u32, classic: bool, material: crate::soften_pass::Material, probes: &ProbeFixture) -> ResolveResult {
    use crate::soften_pass::{Material, LEGACY_DIFFUSE, LEGACY_SPEC_COLOR, LEGACY_GLOSS};
    let src_path = concat!(env!("CARGO_MANIFEST_DIR"), "/../fs_render/shaders/resolve.frag");
    let src = std::fs::read_to_string(src_path)
        .unwrap_or_else(|e| panic!("cannot read resolve.frag at {src_path}: {e}"));
    let compiler = shaderc::Compiler::new().unwrap();
    let mut opts = shaderc::CompileOptions::new().unwrap();
    opts.set_target_env(shaderc::TargetEnv::Vulkan, shaderc::EnvVersion::Vulkan1_2 as u32);
    let spv = compiler
        .compile_into_spirv(&src, shaderc::ShaderKind::Fragment, "resolve.frag", "main", Some(&opts))
        .unwrap_or_else(|e| panic!("resolve.frag compile failed:\n{e}"));
    let fmod = unsafe {
        device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV {
            label: Some("resolve.frag"),
            source: std::borrow::Cow::Borrowed(spv.as_binary()),
        })
    };

    // fixture G-buffer per material. World normal (0,0,1) so dot(n, sun_dir=(0,0.707,0.707)) = 0.707
    // = the oracle's nl. Spec RT stored as Rgba16Float to match the oracle's specularRect EXACTLY, so
    // the ONLY variable in the specular comparison is the view vector (the thing under test).
    use crate::soften_pass::{LEGACY_FULLBRIGHT, PBR_EMISSIVE, PBR_SHINY_BASE, PBR_SHINY_ORM};
    // RT3 = (pbr_emissive.rgb, legacy_fullbright.a) -- must match the oracle's emissiveRect / diffuse.a.
    let (alb3, spec4, flag, em4): ([f32; 3], [f32; 4], f32, [f32; 4]) = match material {
        Material::PbrGround => ([0.5, 0.5, 0.5], [1.0, 1.0, 0.0, 0.0], 0.67,
            [PBR_EMISSIVE[0], PBR_EMISSIVE[1], PBR_EMISSIVE[2], 0.0]),
        Material::PbrShiny => (PBR_SHINY_BASE,
            [PBR_SHINY_ORM[0], PBR_SHINY_ORM[1], PBR_SHINY_ORM[2], 0.0], 0.67, [0.0, 0.0, 0.0, 0.0]),
        Material::LegacySpecular => (
            LEGACY_DIFFUSE, // sRGB (resolve srgb_to_linear's it)
            [LEGACY_SPEC_COLOR[0], LEGACY_SPEC_COLOR[1], LEGACY_SPEC_COLOR[2], LEGACY_GLOSS],
            0.34,
            [0.0, 0.0, 0.0, LEGACY_FULLBRIGHT],
        ),
    };
    let albedo = tex_full(device, queue, size, &rgba16f(alb3[0], alb3[1], alb3[2], flag), "g_albedo");
    let normal = tex_full(device, queue, size, &rgba16f(0.0, 0.0, 1.0, 0.0), "g_normal");
    let spec = tex_full(device, queue, size, &rgba16f(spec4[0], spec4[1], spec4[2], spec4[3]), "g_spec");
    let emissive = tex_full(device, queue, size, &rgba16f(em4[0], em4[1], em4[2], em4[3]), "g_emissive");

    let mut ubo = resolve_sky_ubo();
    ubo[59] = if classic { 1.0 } else { 0.0 }; // classic_mode at moondir_classic.w (u59)
    let ubo_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("resolve-sky"),
        contents: bytemuck::cast_slice(&ubo),
        usage: wgpu::BufferUsages::UNIFORM,
    });

    // --- probe resources (bindings 5-10): depth, ReflectionProbes UBO, radiance/irradiance cube arrays,
    // a filtering sampler, and the ProbeParams UBO. Fed identically to the oracle side.
    let depth = depth_full(device, queue, size, 0.95);
    let probe_ubo = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("ReflectionProbes"), contents: &probes.ubo, usage: wgpu::BufferUsages::UNIFORM });
    let rad_cube = cube_array_layers(device, queue, &probes.radiance, "reflectionProbes");
    let irr_cube = cube_array_layers(device, queue, &probes.irradiance, "irradianceProbes");
    let probe_samp = device.create_sampler(&wgpu::SamplerDescriptor {
        label: Some("probe_smp"), mag_filter: wgpu::FilterMode::Linear, min_filter: wgpu::FilterMode::Linear, ..Default::default() });
    let pp_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("ProbeParams"), contents: &probe_params_bytes(probes.env_identity), usage: wgpu::BufferUsages::UNIFORM });
    let brdf = brdf_lut(device, queue);

    let ntex = |b: u32, filt: bool| wgpu::BindGroupLayoutEntry { binding: b, visibility: wgpu::ShaderStages::FRAGMENT,
        ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: filt }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false }, count: None };
    let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("resolve-bgl"),
        entries: &[
            wgpu::BindGroupLayoutEntry { binding: 0, visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None },
            ntex(1, false), ntex(2, false), ntex(3, false), ntex(4, false),
            ntex(5, false), // g_depth
            wgpu::BindGroupLayoutEntry { binding: 6, visibility: wgpu::ShaderStages::FRAGMENT, // ReflectionProbes UBO
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 7, visibility: wgpu::ShaderStages::FRAGMENT, // radiance cube array
                ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::CubeArray, multisampled: false }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 8, visibility: wgpu::ShaderStages::FRAGMENT, // irradiance cube array
                ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::CubeArray, multisampled: false }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 9, visibility: wgpu::ShaderStages::FRAGMENT, // probe sampler
                ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering), count: None },
            wgpu::BindGroupLayoutEntry { binding: 10, visibility: wgpu::ShaderStages::FRAGMENT, // ProbeParams UBO
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None },
            ntex(11, true), // brdfLut (split-sum env BRDF)
        ],
    });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("resolve-bind"), layout: &bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: ubo_buf.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&albedo) },
            wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(&normal) },
            wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::TextureView(&spec) },
            wgpu::BindGroupEntry { binding: 4, resource: wgpu::BindingResource::TextureView(&emissive) },
            wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::TextureView(&depth) },
            wgpu::BindGroupEntry { binding: 6, resource: probe_ubo.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 7, resource: wgpu::BindingResource::TextureView(&rad_cube) },
            wgpu::BindGroupEntry { binding: 8, resource: wgpu::BindingResource::TextureView(&irr_cube) },
            wgpu::BindGroupEntry { binding: 9, resource: wgpu::BindingResource::Sampler(&probe_samp) },
            wgpu::BindGroupEntry { binding: 10, resource: pp_buf.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 11, resource: wgpu::BindingResource::TextureView(&brdf) },
        ],
    });

    // fullscreen triangle (resolve.frag uses gl_FragCoord; any full-cover vertex works).
    let vsrc = r#"#version 450
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"#;
    let vspv = compiler.compile_into_spirv(vsrc, shaderc::ShaderKind::Vertex, "resolve.vert", "main", Some(&opts)).unwrap();
    let vmod = unsafe {
        device.create_shader_module_spirv(&wgpu::ShaderModuleDescriptorSpirV { label: Some("resolve.vert"), source: std::borrow::Cow::Borrowed(vspv.as_binary()) })
    };

    let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some("resolve-pl"), bind_group_layouts: &[&bgl], push_constant_ranges: &[] });
    let pipe = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("resolve-pipe"),
        layout: Some(&pl),
        vertex: wgpu::VertexState { module: &vmod, entry_point: "main", buffers: &[] },
        primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, ..Default::default() },
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module: &fmod, entry_point: "main",
            targets: &[Some(wgpu::ColorTargetState { format: SCENE_HDR, blend: None, write_mask: wgpu::ColorWrites::ALL })],
        }),
        multiview: None,
    });

    let target = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("resolve-scene-hdr"),
        size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
        mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2,
        format: SCENE_HDR, usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC, view_formats: &[],
    });
    let view = target.create_view(&wgpu::TextureViewDescriptor::default());

    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("resolve-enc") });
    {
        let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("resolve-rp"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &view, resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None,
        });
        rp.set_pipeline(&pipe);
        rp.set_bind_group(0, &bind, &[]);
        rp.draw(0..3, 0..1);
    }
    queue.submit([enc.finish()]);

    ResolveResult { target, view }
}
