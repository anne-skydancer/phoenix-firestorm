//! P0/P1 (GROUNDUP_VULKAN_ENGINE_PLAN.md): the typed scene-bridge scaffold.
//!
//! This is the input the viewer will fill with typed scene + settings DATA, replacing the
//! GL-draw tap. It stands up BESIDE the tap: through P0-P11 the tap still renders whatever
//! has not migrated, and this typed frame contributes nothing until a migration phase fills
//! it. The screen is never blank.
//!
//! P0: frame lifecycle + `CameraBlock`. P1: the full `SettingsSnapshot` (honoring mechanisms
//! 1 & 2 -- the atomic snapshot the render code reads instead of scraping GL state) + camera.
//! The struct is defined in FULL now so "honour ALL settings" is concrete from the start;
//! individual VALUES are wired (viewer-populated + engine-consumed) per phase, so we never
//! claim to honour a setting we have not verified.

/// The camera for a typed frame (P1). `#[repr(C)]`; column-major mat4s (glam/GL order). The
/// engine derives its OWN reverse-Z projection from `near`/`far` rather than a baked MVP.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CameraBlock {
    pub view: [f32; 16],
    pub proj: [f32; 16],
    pub origin: [f32; 3],
    pub near: f32,
    pub far: f32,
    pub fov_y: f32,
    pub aspect: f32,
    pub viewport_w: f32,
    pub viewport_h: f32,
}

/// The full render-settings snapshot the engine reads instead of `gSavedSettings` (honoring
/// mechanisms 1 & 2). Mirrors `LLPipeline::refreshCachedSettings` (pipeline.cpp:1197-1333) plus
/// the per-frame `LLCachedControl` values and the permutation/RT-alloc settings the study
/// flagged. `#[repr(C)]` so the C++ mirror (built when the viewer wiring lands in P4) is a
/// field-order/type match. Bools are `u32` (C `BOOL` is 4 bytes); the viewer ships EFFECTIVE
/// values (LLFeatureManager force-disables already resolved), so the engine never sees a raw
/// saved value. Excluded deliberately: beacons/debug overlays, preview-material-editor lights,
/// FreezeTime, font-buffer collection -- not scene rendering.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SettingsSnapshot {
    // ---- deferred core / G-buffer composition ----
    pub render_deferred_sun_wash: f32,
    pub render_deferred_ssao: u32,        // bool
    pub render_deferred_atmospheric: u32, // bool
    pub render_deferred_noise: u32,       // bool (RT-alloc: FXAA noise)
    pub render_buffer_visualization: i32,
    pub render_hdr_enabled: u32,          // bool -> RGBA16F scene target
    pub render_enable_emissive_buffer: u32, // bool -> 4th G-buffer attachment

    // ---- resolution / anti-aliasing ----
    pub render_fsaa_type: u32,            // 0 none / 1 FXAA / 2 SMAA
    pub render_fsaa_samples: u32,         // 0-3 quality tier (NOT a sample count)
    pub render_resolution_divisor: u32,
    pub render_resolution_multiplier: f32,
    pub render_ui_buffer: u32,            // bool

    // ---- far clip / culling ----
    pub render_use_far_clip: u32,         // bool
    pub render_far_clip: f32,
    pub use_occlusion: u32,               // effective (0 or 2)
    pub render_auto_hide_surface_area_limit: f32,

    // ---- sun/spot shadows (RenderShadowDetail family) ----
    pub render_shadow_detail: i32,        // 0 off / 1 sun / 2 sun+spot
    pub render_shadow_splits: i32,
    pub render_shadow_resolution_scale: f32,
    pub render_shadow_noise: f32,
    pub render_shadow_blur_size: f32,
    pub render_shadow_offset: f32,
    pub render_shadow_bias: f32,
    pub render_shadow_offset_error: f32,
    pub render_shadow_bias_error: f32,
    pub render_spot_shadow_offset: f32,
    pub render_spot_shadow_bias: f32,
    pub render_edge_depth_cutoff: f32,
    pub render_edge_norm_cutoff: f32,
    pub render_shadow_gaussian: [f32; 3],
    pub render_shadow_blur_dist_factor: f32,
    pub render_shadow_split_exponent: [f32; 3],
    pub render_shadow_error_cutoff: f32,
    pub render_shadow_fov_cutoff: f32,

    // ---- SSAO ----
    pub render_ssao_scale: f32,
    pub render_ssao_max_scale: u32,
    pub render_ssao_factor: f32,
    pub render_ssao_effect: [f32; 3],

    // ---- glow / bloom ----
    pub render_glow_max_extract_alpha: f32,
    pub render_glow_warmth_amount: f32,
    pub render_glow_lum_weights: [f32; 3],
    pub render_glow_warmth_weights: [f32; 3],
    pub render_glow_resolution_pow: i32,
    pub render_glow_iterations: i32,
    pub render_glow_width: f32,
    pub render_glow_strength: f32,
    pub render_glow_noise: u32, // bool
    pub render_glow_hdr: u32,   // bool (RGBA16F glow buffers)

    // ---- depth of field / camera lens ----
    pub render_depth_of_field: u32,             // bool
    pub render_depth_of_field_in_edit_mode: u32, // bool
    pub camera_focus_transition_time: f32,
    pub camera_fnumber: f32,
    pub camera_focal_length: f32,
    pub camera_field_of_view: f32,
    pub camera_offset: u32, // bool
    pub camera_max_cof: f32,
    pub camera_dof_res_scale: f32,

    // ---- exposure / tonemap (LLCachedControl, mechanism 2) ----
    pub render_exposure: f32,
    pub render_tonemap_type: u32, // 0 PBRNeutral / 1 ACES
    pub render_tonemap_mix: f32,
    pub render_dynamic_exposure_enabled: u32, // bool
    pub render_dynamic_exposure_coefficient: f32,
    pub render_dynamic_exposure_speed_error: f32,
    pub render_dynamic_exposure_speed_target: f32,

    // ---- screen-space reflections ----
    pub render_screen_space_reflections: u32, // bool
    pub render_ssr_iterations: i32,
    pub render_ssr_ray_step: f32,
    pub render_ssr_distance_bias: f32,
    pub render_ssr_depth_reject_bias: f32,
    pub render_ssr_adaptive_step_multiplier: f32,
    pub render_ssr_glossy_samples: i32,

    // ---- reflection probes / hero mirrors ----
    pub render_reflection_probes_enabled: u32, // effective (feature-gated)
    pub render_reflection_probe_level: i32,
    pub render_reflection_probe_detail: i32,
    pub render_reflection_probe_count: i32,
    pub render_mirrors: u32, // bool
    pub render_hero_probe_resolution: i32,
    pub render_hero_probe_update_rate: i32,
    pub render_hero_probe_conservative_update_multiplier: i32,

    // ---- avatars ----
    pub render_avatar_max_non_impostors: u32,
    pub render_avatar_max_complexity: u32,
    pub render_avatar_max_art: f32,
    pub render_avatar_cloth: u32,             // bool
    pub render_jelly_dolls_as_impostors: u32, // bool

    // ---- lights / particles / LOD ----
    pub render_local_light_count: u32,
    pub render_max_part_count: i32,
    pub render_volume_lod_factor: f32,
    pub render_attached_lights: u32,     // bool
    pub render_attached_particles: u32,  // bool
    pub render_spot_lights_in_nondeferred: u32, // bool

    // ---- water ----
    pub render_transparent_water: u32, // bool

    // ---- terrain (permutation, mechanism 3) ----
    pub render_terrain_pbr_detail: i32,
    pub render_terrain_planar_sample_count: i32,
    pub render_terrain_triplanar_blend_factor: f32,
    pub render_terrain_scale: f32,
    pub render_terrain_pbr_scale: f32,

    // ---- specular / BRDF LUT ----
    pub render_specular_res_x: i32,
    pub render_specular_res_y: i32,
    pub render_specular_exponent: f32,

    // ---- selection highlight (the "brighter circle under the cursor") ----
    pub render_highlight: u32, // bool
    pub render_highlight_brightness: f32,
    pub render_highlight_color: [f32; 4],
    pub render_highlight_thickness: f32,
    pub render_highlight_fade_time: f32,

    // ---- sky/atmospherics regime (S3: llsettingsvo.cpp applySpecial 790-868) ----
    // The engine derives the classic-vs-advanced regime from these + the EEP sky discriminators
    // (SkyRegime, sky_regime()). Ships the settings RAW; the derivation lives in ONE place so it
    // is oracle-tested (mis-routing legacy->advanced is the proven "blew to white" cause).
    pub render_sky_auto_adjust_legacy: u32,      // RenderSkyAutoAdjustLegacy (default false)
    pub render_sky_sunlight_scale: f32,          // RenderSkySunlightScale (1.5)
    pub render_hdr_sky_sunlight_scale: f32,      // RenderHDRSkySunlightScale (1.5)
    pub render_sky_ambient_scale: f32,           // RenderSkyAmbientScale (1.5)
    pub render_sky_auto_adjust_ambient_scale: f32, // RenderSkyAutoAdjustAmbientScale (0.75)
    pub render_sky_auto_adjust_hdr_scale: f32,   // RenderSkyAutoAdjustHDRScale (2.0)
    pub render_sun_dynamic_range: f32,           // RenderSunDynamicRange (1.0)
}

/// P2: the EEP atmospherics/sky block. Fields are the atmospherics uniform set the sky +
/// deferred-resolve shaders consume (the exact set the tap already scrapes from the bound sky
/// shader: LIGHTNORM/SUNLIGHT/MOONLIGHT/AMBIENT/BLUE_HORIZON/BLUE_DENSITY/HAZE_*/DENSITY_MULT/
/// MAX_Y/GLOW/CLOUD_SHADOW/SUN_MOON_GLOW/... + clouds/stars/celestial), sourced authoritatively
/// from `LLSettingsSky` when the viewer wiring lands. World-space `sun_dir`/`moon_dir`; the
/// engine transforms to eye space via the camera (no more sky-dome-modelview scrape). This is
/// what the 12-float `frame_env` bottleneck destroyed -- the physical model, restored.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EepSkyBlock {
    pub sun_dir: [f32; 3],
    pub sun_color: [f32; 3],   // SUNLIGHT_COLOR (also the sun lighting term)
    pub moon_dir: [f32; 3],
    pub moon_color: [f32; 3],  // MOONLIGHT_COLOR
    pub ambient: [f32; 3],     // AMBIENT
    pub blue_horizon: [f32; 3],
    pub blue_density: [f32; 3],
    pub haze_density: f32,
    pub haze_horizon: f32,
    pub density_multiplier: f32,
    pub distance_multiplier: f32,
    pub max_y: f32,
    pub gamma: f32,
    pub glow: [f32; 3],        // GLOW (focus / size)
    pub cloud_shadow: f32,
    pub sun_moon_glow_factor: f32,
    pub sun_up_factor: f32,
    pub sky_hdr_scale: f32,    // reflection-probe ambiance / HDR scale (SKY_HDR_SCALE)
    pub cloud_color: [f32; 3],
    pub cloud_pos_density1: [f32; 3],
    pub cloud_pos_density2: [f32; 3],
    pub cloud_scale: f32,
    pub cloud_variance: f32,
    pub star_brightness: f32,
    pub sun_scale: f32,
    pub moon_scale: f32,
    pub moon_brightness: f32,
    pub moisture_level: f32, // rainbow/halo
    pub droplet_radius: f32,
    pub ice_level: f32,

    // ---- S3 regime discriminators (raw viewer getters; drive sky_regime()) ----
    // NOTE: `sky_hdr_scale` above is now DERIVED by the engine (sky_regime), not a viewer input.
    pub can_auto_adjust: u32,           // psky->canAutoAdjust() = sky lacks a probe-ambiance KEY
    pub reflection_probe_ambiance: f32, // raw mReflectionProbeAmbiance value (0 for legacy skies)
    pub lightnorm: [f32; 3],            // getClampedLightNorm swizzle (y,z,x), z>=-0.1 -- NOT sun_dir
}

/// S3: `LLSettingsSky::DEFAULT_AUTO_ADJUST_PROBE_AMBIANCE` / `sAutoAdjustProbeAmbiance`.
pub const AUTO_ADJUST_PROBE_AMBIANCE: f32 = 1.0;

/// S3: the derived sky regime -- the classic-vs-advanced bundle the sky/resolve/tonemap consume.
/// Computed ONCE by `SceneFrame::sky_regime()` (faithful transcription of llsettingsvo.cpp
/// applySpecial 790-868 + getReflectionProbeAmbiance + pipeline.cpp legacy-gamma selection), so
/// the mis-routing that blew the scene to white is caught by a single unit test rather than an
/// in-world surprise. Engine-internal (not shipped over the C ABI); drives set_post_params + the
/// resolve uniforms.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SkyRegime {
    pub classic_mode: u32,          // canAutoAdjust && !RenderSkyAutoAdjustLegacy
    pub sky_hdr_scale: f32,         // DERIVED: 1.0 legacy / sqrt(gamma)*2 probe / 2.0 auto-adjust
    pub sky_sunlight_scale: f32,    // 1.5 (hdr variant when RenderHDREnabled)
    pub sky_ambient_scale: f32,     // 1.5
    pub scene_light_strength: f32,  // 2*(0.75 + sun_dynamic_range*max(sun.z,0))
    pub tonemap_mix: f32,           // 0 for classic legacy (curve bypassed), else RenderTonemapMix
    pub legacy_gamma: u32,          // getReflectionProbeAmbiance(should_auto)==0 -> classic soft-clip
    pub exposure: f32,              // clamp(RenderExposure, 0.5, 4.0)
    pub tonemap_type: u32,          // 0 PBRNeutral / 1 ACES Hill
    pub gamma: f32,                 // sky gamma -> legacyGamma
}

/// P2: the EEP water block, from `LLSettingsWater` getters. Texture fields are UUID-derived
/// handles resolved to engine texture ids when the material/texture subsystem lands.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct WaterBlock {
    pub fog_color: [f32; 3],
    pub fog_density: f32,
    pub underwater_fog_mod: f32,
    pub fresnel_scale: f32,
    pub fresnel_offset: f32,
    pub scale_above: f32,
    pub scale_below: f32,
    pub blur_multiplier: f32,
    pub normal_scale: [f32; 3],
    pub wave1_dir: [f32; 2],
    pub wave2_dir: [f32; 2],
    pub water_height: f32,
    pub normal_map_tex: u32,
    pub transparent_tex: u32,
}

/// P3: a local point/spot light (the list the tap NEVER sent -- the single biggest cause of
/// the "inside brighter than outside" unphysical resolve). From `LLPipeline::mNearbyLights` +
/// `LLVOVolume` getLightRadius/getLightFalloff/getLightLinearColor/isLightSpotlight. Position
/// in agent/world space; the engine transforms to eye space via the camera. 64 bytes (4 vec4).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Light {
    pub pos: [f32; 3],
    pub radius: f32,
    pub color: [f32; 3], // linear
    pub falloff: f32,
    pub dir: [f32; 3], // spot direction
    pub spot_cos_cutoff: f32,
    pub spot_exponent: f32,
    pub is_spot: u32,
    pub _pad0: f32,
    pub _pad1: f32,
}

/// Ground/terrain (Phase 1). FFI header (POD, passed by pointer); the heightmap floats arrive as a
/// SEPARATE pointer (dim*dim, row-major, metres), copied engine-side into `TerrainBlock`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct TerrainHeader {
    pub dim: u32,             // grids per edge + 1 (e.g. 257); heightmap is dim*dim
    pub meters_per_grid: f32, // world metres between grid samples (e.g. 1.0)
    pub origin: [f32; 3],     // region origin in agent space (mesh vertex positions)
    pub _pad0: f32,
    // P3 composition/splat (the engine computes composition itself; see terrain_noise):
    pub start_height: [f32; 4],   // per-corner SW,SE,NW,NE (sim RegionInfo)
    pub height_range: [f32; 4],   // per-corner SW,SE,NW,NE
    pub origin_global: [f64; 2],  // region SW corner in GLOBAL metres (noise lattice continuity)
    pub detail_scale: f32,        // 1/RenderTerrainScale (default 1/12); detail-UV tiling
    pub _pad1: f32,
    pub detail_tex_ids: [u32; 4], // legacy: 4 detail textures; PBR region: 4 material base-color textures
    pub alpha_ramp_id: u32,       // engine texture id for IMG_ALPHA_GRAD_2D
    pub pbr: u32,                 // 0 = legacy detail splat, 1 = PBR material region (PBR-3 feed)
}

/// Engine-side terrain: the CPU heightmap + metadata. PERSISTS across frames (region-scoped, not
/// per-frame); `gen` bumps on each feed so the render side rebuilds the GPU mesh only on change.
#[derive(Clone, Default)]
pub struct TerrainBlock {
    pub dim: u32,
    pub meters_per_grid: f32,
    pub origin: [f32; 3],
    pub heights: Vec<f32>, // dim*dim, mSurfaceZ order (i + j*dim)
    pub gen: u64,
    // P3 composition/splat params (copied from the header):
    pub start_height: [f32; 4],
    pub height_range: [f32; 4],
    pub origin_global: [f64; 2],
    pub detail_scale: f32,
    pub detail_tex_ids: [u32; 4], // legacy: 4 detail textures; PBR: 4 material base-color textures
    pub alpha_ramp_id: u32,
    pub pbr: bool,                // region uses GLTF/PBR materials -> select the PBR splat pipeline (PBR-3 feed)
}

/// The accumulating typed frame. Grows one payload per phase. Per-frame data (camera, sky,
/// water, lights, geometry) is cleared each `begin`; settings persist (change on event only).
/// Empty of GEOMETRY until a subsystem phase feeds typed draws.
#[derive(Default)]
pub struct SceneFrame {
    /// Typed frames begun (telemetry / co-existence proof).
    pub frames: u64,
    /// P1: the frame camera. `None` until the viewer feeds it.
    pub camera: Option<CameraBlock>,
    /// P1: the render-settings snapshot. Always present (defaulted); the render code reads
    /// this, never a settings store. Atomically replaced by `set_settings` on any change.
    pub settings: SettingsSnapshot,
    /// P2: the EEP atmospherics/sky block (per frame; day-cycle changes it every frame).
    pub eep_sky: Option<EepSkyBlock>,
    /// P2: the EEP water block (per frame).
    pub water: Option<WaterBlock>,
    /// P3: the local point/spot lights this frame (nearby-light list, capped scene-side).
    pub lights: Vec<Light>,
    /// Ground phase 1: the region heightmap. PERSISTS across frames (region-scoped) -- NOT cleared
    /// by `begin`. `gen` lets the render side rebuild the GPU mesh only when it changes.
    pub terrain: Option<TerrainBlock>,
}

impl SceneFrame {
    pub fn new() -> SceneFrame {
        SceneFrame::default()
    }

    /// Feed the region heightmap (Phase 1). Copies `dim*dim` floats from `heights`; bumps `gen` so
    /// the render side rebuilds the GPU mesh. Persists until replaced (region change / terrain edit).
    pub fn set_terrain(&mut self, hdr: &TerrainHeader, heights: &[f32]) {
        let n = (hdr.dim as usize) * (hdr.dim as usize);
        if hdr.dim < 2 || heights.len() < n { return; }
        let gen = self.terrain.as_ref().map(|t| t.gen + 1).unwrap_or(1);
        self.terrain = Some(TerrainBlock {
            dim: hdr.dim,
            meters_per_grid: hdr.meters_per_grid,
            origin: hdr.origin,
            heights: heights[..n].to_vec(),
            gen,
            start_height: hdr.start_height,
            height_range: hdr.height_range,
            origin_global: hdr.origin_global,
            detail_scale: hdr.detail_scale,
            detail_tex_ids: hdr.detail_tex_ids,
            alpha_ramp_id: hdr.alpha_ramp_id,
            pbr: hdr.pbr != 0, // set by the viewer feed from getMaterialType() == PBR
        });
    }

    /// Start a typed frame: clear the previous frame's per-frame data (camera, sky, water,
    /// lights, geometry). Settings persist across frames (they change only on a settings
    /// event), so they are NOT cleared here.
    pub fn begin(&mut self) {
        self.frames = self.frames.wrapping_add(1);
        self.camera = None;
        self.eep_sky = None;
        self.water = None;
        self.lights.clear();
    }

    pub fn set_camera(&mut self, cam: &CameraBlock) {
        self.camera = Some(*cam);
    }

    /// Atomic settings swap (honoring mechanisms 1 & 2). Called on any settings change, not
    /// per frame. Values are effective (feature-manager already applied).
    pub fn set_settings(&mut self, s: &SettingsSnapshot) {
        self.settings = *s;
    }

    /// P2: the EEP atmospherics/sky block for this frame.
    pub fn set_sky(&mut self, sky: &EepSkyBlock) {
        self.eep_sky = Some(*sky);
    }

    /// P2: the EEP water block for this frame.
    pub fn set_water(&mut self, water: &WaterBlock) {
        self.water = Some(*water);
    }

    /// P3: the local point/spot lights for this frame (replaces the list; capped scene-side).
    pub fn set_lights(&mut self, lights: &[Light]) {
        self.lights.clear();
        self.lights.extend_from_slice(lights);
    }

    /// P3 (consumption): derive the deferred resolve's sun/ambient env from the TYPED EEP sky +
    /// camera -- the world-space sun goes to EYE space via the camera view matrix (the correct
    /// transform), replacing the sky-dome-modelview scrape that gave the wrong direction. Returns
    /// `[sun_dir_eye(3), _, sunlight(3), _, ambient(3), _]` (12 floats, the resolve Env layout),
    /// or `None` if sky or camera is absent (the engine then keeps its self-derived fallback).
    pub fn resolve_env(&self) -> Option<[f32; 12]> {
        let sky = self.eep_sky.as_ref()?;
        let cam = self.camera.as_ref()?;
        let view = glam::Mat4::from_cols_array(&cam.view);
        let sun_world = glam::Vec3::new(sky.sun_dir[0], sky.sun_dir[1], sky.sun_dir[2]);
        let sun_eye = (glam::Mat3::from_mat4(view) * sun_world).normalize_or_zero();
        Some([
            sun_eye.x, sun_eye.y, sun_eye.z, 0.0,
            sky.sun_color[0], sky.sun_color[1], sky.sun_color[2], 0.0,
            sky.ambient[0], sky.ambient[1], sky.ambient[2], 0.0,
        ])
    }

    /// S3: derive the sky regime (classic vs advanced) from the EEP sky discriminators + the
    /// settings snapshot. Faithful transcription of llsettingsvo.cpp applySpecial (790-868),
    /// LLSettingsSky::getReflectionProbeAmbiance (1823-1831) and getTonemapMix, and the
    /// pipeline.cpp legacy-gamma program selection (8131/8196). `None` until the sky is fed.
    pub fn sky_regime(&self) -> Option<SkyRegime> {
        let sky = self.eep_sky.as_ref()?;
        let s = &self.settings;
        let can_auto = sky.can_auto_adjust != 0;                 // psky->canAutoAdjust()
        let should_auto = s.render_sky_auto_adjust_legacy != 0;  // RenderSkyAutoAdjustLegacy
        let hdr = s.render_hdr_enabled != 0;                     // RenderHDREnabled
        let g = sky.gamma;
        let raw_probe = sky.reflection_probe_ambiance;           // mReflectionProbeAmbiance

        // "classic" = pre-SL-7.0 shading (llsettingsvo.cpp:810).
        let classic_mode = can_auto && !should_auto;

        // getTonemapMix(should_auto): 0 when (canAutoAdjust && !should_auto), else mTonemapMix,
        // and the non-classic path sets mTonemapMix = RenderTonemapMix (applySpecial:814).
        let tonemap_mix = if can_auto && !should_auto { 0.0 } else { s.render_tonemap_mix };

        let sky_sunlight_scale = if hdr { s.render_hdr_sky_sunlight_scale } else { s.render_sky_sunlight_scale };
        let sky_ambient_scale = s.render_sky_ambient_scale;

        // SKY_HDR_SCALE branch (applySpecial:831-858) -- on the RAW probe ambiance.
        let sky_hdr_scale = if raw_probe != 0.0 {
            g.max(0.0).sqrt() * 2.0
        } else if can_auto && should_auto {
            s.render_sky_auto_adjust_hdr_scale
        } else {
            1.0
        };

        // legacy_gamma = getReflectionProbeAmbiance(should_auto) == 0 (pipeline.cpp:8131/8196).
        let eff_probe = if should_auto && can_auto { AUTO_ADJUST_PROBE_AMBIANCE } else { raw_probe };
        let legacy_gamma = (eff_probe == 0.0) as u32;

        // mSceneLightStrength (llsettingsvo.cpp:665-676). sun_dir is world-space; z is "up".
        let dp = sky.sun_dir[2].max(0.0);
        let sdr = s.render_sun_dynamic_range.max(0.0001);
        let scene_light_strength = 2.0 * (0.75 + sdr * dp);

        Some(SkyRegime {
            classic_mode: classic_mode as u32,
            sky_hdr_scale,
            sky_sunlight_scale,
            sky_ambient_scale,
            scene_light_strength,
            tonemap_mix,
            legacy_gamma,
            exposure: s.render_exposure.clamp(0.5, 4.0), // pipeline.cpp:8164
            tonemap_type: s.render_tonemap_type,
            gamma: g,
        })
    }

    /// Phase A.2: pack the fullscreen-sky UBO (52 floats) consumed by sky_fullscreen.frag,
    /// derived ENTIRELY from the typed camera + EEP sky (parallel-read; no tap, no dome). Layout:
    /// `inv_view_proj`(16) + a0..a8(36). The projection is rebuilt from fov/aspect/near/far --
    /// only the frustum xy matters for the sky ray, so depth convention (reverse-Z etc.) is
    /// irrelevant. `sky_hdr_scale` comes from the derived regime. `None` until camera + sky are fed.
    pub fn fullscreen_sky_ubo(&self) -> Option<[f32; 60]> {
        let cam = self.camera.as_ref()?;
        let sky = self.eep_sky.as_ref()?;
        let view = glam::Mat4::from_cols_array(&cam.view);
        let proj = glam::Mat4::perspective_rh(cam.fov_y, cam.aspect, cam.near, cam.far);
        let inv_vp = (proj * view).inverse();
        let regime = self.sky_regime();
        let sky_hdr_scale = regime.as_ref().map(|r| r.sky_hdr_scale).unwrap_or(1.0);
        // P2b resolve: the atmospherics scales the softenLight port needs (regime-derived).
        let sky_sunlight_scale = regime.as_ref().map(|r| r.sky_sunlight_scale).unwrap_or(1.0);
        let sky_ambient_scale = regime.as_ref().map(|r| r.sky_ambient_scale).unwrap_or(1.0);

        let mut u = [0.0f32; 60];
        u[0..16].copy_from_slice(&inv_vp.to_cols_array());
        // a0: camPosLocal.xyz, max_y
        u[16] = cam.origin[0]; u[17] = cam.origin[1]; u[18] = cam.origin[2]; u[19] = sky.max_y;
        // a1: lightnorm.xyz, sun_up_factor
        u[20] = sky.lightnorm[0]; u[21] = sky.lightnorm[1]; u[22] = sky.lightnorm[2]; u[23] = sky.sun_up_factor;
        // a2: sunlight_color.xyz, density_multiplier
        u[24] = sky.sun_color[0]; u[25] = sky.sun_color[1]; u[26] = sky.sun_color[2]; u[27] = sky.density_multiplier;
        // a3: moonlight_color.xyz, sun_moon_glow_factor
        u[28] = sky.moon_color[0]; u[29] = sky.moon_color[1]; u[30] = sky.moon_color[2]; u[31] = sky.sun_moon_glow_factor;
        // a4: ambient_color.xyz, haze_density
        u[32] = sky.ambient[0]; u[33] = sky.ambient[1]; u[34] = sky.ambient[2]; u[35] = sky.haze_density;
        // a5: blue_horizon.xyz, haze_horizon
        u[36] = sky.blue_horizon[0]; u[37] = sky.blue_horizon[1]; u[38] = sky.blue_horizon[2]; u[39] = sky.haze_horizon;
        // a6: blue_density.xyz, cloud_shadow
        u[40] = sky.blue_density[0]; u[41] = sky.blue_density[1]; u[42] = sky.blue_density[2]; u[43] = sky.cloud_shadow;
        // a7: glow.xyz, sky_sunlight_scale (P2b resolve)
        u[44] = sky.glow[0]; u[45] = sky.glow[1]; u[46] = sky.glow[2]; u[47] = sky_sunlight_scale;
        // a8: sky_hdr_scale, sky_ambient_scale (P2b resolve), viewport_w, viewport_h
        u[48] = sky_hdr_scale; u[49] = sky_ambient_scale; u[50] = cam.viewport_w; u[51] = cam.viewport_h;
        // a9: sun_dir.xyz, _ ; a10: moon_dir.xyz, _ (REAL world-space, for the sun/moon discs)
        u[52] = sky.sun_dir[0]; u[53] = sky.sun_dir[1]; u[54] = sky.sun_dir[2]; u[55] = 0.0;
        u[56] = sky.moon_dir[0]; u[57] = sky.moon_dir[1]; u[58] = sky.moon_dir[2]; u[59] = 0.0;
        Some(u)
    }

    /// Ground Phase 1: the terrain UBO. view_proj is REVERSE-Z (remap [0,1]->[1,0]) so terrain depth
    /// matches the engine's GreaterEqual + clear-0 convention; plus world-space sun_dir/sunlight/ambient
    /// for the forward N.L shading and sky_hdr_scale to sit in the sky's HDR range. Layout matches
    /// terrain_typed.{vert,frag}: view_proj(16) + sun_dir(4) + sunlight(4) + ambient(4) + misc(4).
    pub fn terrain_ubo(&self) -> Option<[f32; 32]> {
        let cam = self.camera.as_ref()?;
        let sky = self.eep_sky.as_ref()?;
        let view = glam::Mat4::from_cols_array(&cam.view);
        let proj = glam::Mat4::perspective_rh(cam.fov_y, cam.aspect, cam.near, cam.far);
        let rev = glam::Mat4::from_cols(
            glam::Vec4::new(1.0, 0.0, 0.0, 0.0),
            glam::Vec4::new(0.0, 1.0, 0.0, 0.0),
            glam::Vec4::new(0.0, 0.0, -1.0, 0.0),
            glam::Vec4::new(0.0, 0.0, 1.0, 1.0),
        );
        let vp = (rev * proj) * view;
        let hdr = self.sky_regime().map(|r| r.sky_hdr_scale).unwrap_or(1.0);
        let mut u = [0.0f32; 32];
        u[0..16].copy_from_slice(&vp.to_cols_array());
        u[16] = sky.sun_dir[0]; u[17] = sky.sun_dir[1]; u[18] = sky.sun_dir[2]; u[19] = 0.0;
        u[20] = sky.sun_color[0]; u[21] = sky.sun_color[1]; u[22] = sky.sun_color[2]; u[23] = 0.0;
        u[24] = sky.ambient[0]; u[25] = sky.ambient[1]; u[26] = sky.ambient[2]; u[27] = 0.0;
        // misc: P3 detail-UV texgen. offset = fmod(origin_global, 1/detail_scale)*detail_scale,
        // matching stock OBJECT_PLANE_S/T (lldrawpoolterrain.cpp:262-269). The G-buffer vertex shader
        // does UV = pos.xy*detail_scale + offset. (Forward terrain_typed that used misc.x=hdr is dead.)
        let _ = hdr;
        let (detail_scale, off_x, off_y) = match self.terrain.as_ref() {
            Some(t) if t.detail_scale > 0.0 => {
                let ds = t.detail_scale as f64;
                let tile = 1.0 / ds;
                let ox = ((t.origin_global[0] % tile) * ds) as f32;
                let oy = ((t.origin_global[1] % tile) * ds) as f32;
                (t.detail_scale, ox, oy)
            }
            _ => (1.0 / 12.0, 0.0, 0.0),
        };
        u[28] = detail_scale; u[29] = off_x; u[30] = off_y; u[31] = 0.0;
        Some(u)
    }

    /// P1 diag: which typed feed has arrived this frame (separates "no camera" from "no sky").
    pub fn has_camera(&self) -> bool { self.camera.is_some() }
    pub fn has_sky(&self) -> bool { self.eep_sky.is_some() }

    /// Whether this typed frame carries GEOMETRY to render. P0/P1 contribute none, so the tap
    /// still renders the whole scene. A subsystem phase flips this once it feeds typed draws.
    pub fn is_empty(&self) -> bool {
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lifecycle_and_snapshot_roundtrip() {
        let mut s = SceneFrame::new();
        assert!(s.is_empty(), "no geometry yet");
        assert!(s.camera.is_none());

        s.begin();
        assert_eq!(s.frames, 1);
        let cam = CameraBlock { near: 0.1, far: 4096.0, fov_y: 1.0, aspect: 1.5, ..Default::default() };
        s.set_camera(&cam);
        assert_eq!(s.camera.unwrap().far, 4096.0);

        // Settings persist across begin(); a change swaps the whole snapshot.
        let mut snap = SettingsSnapshot::default();
        snap.render_shadow_detail = 2;
        snap.render_fsaa_type = 1;
        snap.render_far_clip = 512.0;
        snap.render_tonemap_type = 1;
        s.set_settings(&snap);
        assert_eq!(s.settings.render_shadow_detail, 2);

        s.begin(); // clears camera, keeps settings
        assert_eq!(s.frames, 2);
        assert!(s.camera.is_none());
        assert_eq!(s.settings.render_shadow_detail, 2, "settings persist across frames");
        assert_eq!(s.settings.render_far_clip, 512.0);

        // P2/P3 payloads round-trip and clear on begin(); settings still persist.
        s.set_sky(&EepSkyBlock { max_y: 1000.0, gamma: 1.0, ..Default::default() });
        s.set_water(&WaterBlock { water_height: 20.0, fresnel_scale: 0.4, ..Default::default() });
        s.set_lights(&[
            Light { radius: 5.0, is_spot: 0, ..Default::default() },
            Light { radius: 8.0, is_spot: 1, ..Default::default() },
        ]);
        assert_eq!(s.eep_sky.unwrap().max_y, 1000.0);
        assert_eq!(s.water.unwrap().water_height, 20.0);
        assert_eq!(s.lights.len(), 2);
        assert_eq!(s.lights[1].is_spot, 1);

        s.begin();
        assert!(s.eep_sky.is_none() && s.water.is_none() && s.lights.is_empty(), "per-frame data cleared");
        assert_eq!(s.settings.render_shadow_detail, 2, "settings survive the frame reset");
        assert!(s.is_empty(), "still no geometry -> tap renders the scene");
    }

    /// A settings snapshot with the real stock defaults for the regime inputs (Default is all
    /// zero, which would misderive; the viewer ships real values, and the oracle sets them).
    fn regime_settings() -> SettingsSnapshot {
        SettingsSnapshot {
            render_sky_auto_adjust_legacy: 0,
            render_sky_sunlight_scale: 1.5,
            render_hdr_sky_sunlight_scale: 1.5,
            render_sky_ambient_scale: 1.5,
            render_sky_auto_adjust_ambient_scale: 0.75,
            render_sky_auto_adjust_hdr_scale: 2.0,
            render_sun_dynamic_range: 1.0,
            render_tonemap_mix: 0.7,
            render_tonemap_type: 1,
            render_exposure: 1.0,
            render_hdr_enabled: 0,
            render_reflection_probes_enabled: 1,
            ..Default::default()
        }
    }

    /// S3 REGIME ORACLE: the classic-vs-advanced derivation matches stock across the three
    /// branches. Mis-routing legacy -> advanced is the proven "blew to white / wrong sun" cause,
    /// so this pins the bundle as one atomic unit.
    #[test]
    fn sky_regime_matches_stock_branches() {
        // (1) Legacy classic sky (no probe-ambiance key), auto-adjust-legacy OFF (default).
        let mut s = SceneFrame::new();
        s.set_settings(&regime_settings());
        s.set_sky(&EepSkyBlock {
            gamma: 1.0, sun_dir: [0.0, 0.0, 0.7], can_auto_adjust: 1, reflection_probe_ambiance: 0.0,
            ..Default::default()
        });
        let r = s.sky_regime().expect("sky present");
        assert_eq!(r.classic_mode, 1, "legacy sky renders classic by default");
        assert_eq!(r.sky_hdr_scale, 1.0, "classic sky_hdr_scale = 1.0");
        assert_eq!(r.tonemap_mix, 0.0, "classic bypasses the tonemap curve");
        assert_eq!(r.legacy_gamma, 1, "classic gets the legacyGamma soft-clip");
        assert_eq!(r.sky_sunlight_scale, 1.5);
        assert_eq!(r.sky_ambient_scale, 1.5);
        assert!((r.scene_light_strength - 2.0 * (0.75 + 1.0 * 0.7)).abs() < 1e-6);

        // (2) Advanced PBR sky: carries reflection_probe_ambiance (key present -> can_auto=0).
        let mut s = SceneFrame::new();
        s.set_settings(&regime_settings());
        s.set_sky(&EepSkyBlock {
            gamma: 2.2, sun_dir: [0.0, 0.0, 0.5], can_auto_adjust: 0, reflection_probe_ambiance: 0.5,
            ..Default::default()
        });
        let r = s.sky_regime().unwrap();
        assert_eq!(r.classic_mode, 0, "probe-ambiance sky is not classic");
        assert!((r.sky_hdr_scale - (2.2_f32).sqrt() * 2.0).abs() < 1e-6, "advanced sky_hdr_scale = sqrt(gamma)*2");
        assert_eq!(r.tonemap_mix, 0.7, "advanced uses the full tonemap curve");
        assert_eq!(r.legacy_gamma, 0, "advanced has no legacyGamma");

        // (3) Auto-adjust-legacy: legacy sky (can_auto=1) but RenderSkyAutoAdjustLegacy ON.
        let mut s = SceneFrame::new();
        let mut set = regime_settings();
        set.render_sky_auto_adjust_legacy = 1;
        s.set_settings(&set);
        s.set_sky(&EepSkyBlock {
            gamma: 1.0, sun_dir: [0.0, 0.0, 0.9], can_auto_adjust: 1, reflection_probe_ambiance: 0.0,
            ..Default::default()
        });
        let r = s.sky_regime().unwrap();
        assert_eq!(r.classic_mode, 0, "auto-adjust disables classic");
        assert_eq!(r.sky_hdr_scale, 2.0, "auto-adjust sky_hdr_scale = 2.0");
        assert_eq!(r.legacy_gamma, 0, "auto-adjust injects probe ambiance -> no legacyGamma");
        assert_eq!(r.tonemap_mix, 0.7, "auto-adjust uses the tonemap curve");

        // No sky -> no regime.
        assert!(SceneFrame::new().sky_regime().is_none());
    }

    #[test]
    fn resolve_env_uses_camera_transform() {
        let mut s = SceneFrame::new();
        assert!(s.resolve_env().is_none(), "no env without sky + camera");
        s.set_camera(&CameraBlock { view: glam::Mat4::IDENTITY.to_cols_array(), ..Default::default() });
        s.set_sky(&EepSkyBlock { sun_dir: [0.0, 0.0, 1.0], sun_color: [1.0, 0.9, 0.8], ambient: [0.2, 0.2, 0.25], ..Default::default() });
        let env = s.resolve_env().expect("sky + camera present");
        // identity view -> the world sun (0,0,1) stays (0,0,1) in eye space.
        assert!(env[0].abs() < 1e-5 && env[1].abs() < 1e-5 && (env[2] - 1.0).abs() < 1e-5, "sun eye {:?}", &env[0..3]);
        assert_eq!([env[4], env[5], env[6]], [1.0, 0.9, 0.8], "sunlight");
        assert_eq!([env[8], env[9], env[10]], [0.2, 0.2, 0.25], "ambient");
    }
}
