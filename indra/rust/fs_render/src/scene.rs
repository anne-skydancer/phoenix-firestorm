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
}

impl SceneFrame {
    pub fn new() -> SceneFrame {
        SceneFrame::default()
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
}
