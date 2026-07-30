//! FULL shader-program table transcribed from the viewer's shader manager
//! (c:/fs/firestorm-upstream/indra/newview/llviewershadermgr.cpp, branch master).
//!
//! Every createShader() call site is mirrored as one ProgramDef (232 C++ sites incl.
//! loop bodies -> 229 table entries under canonical settings; the helper sites
//! make_rigged_variant/:267 and make_gltf_variant/:339,:351 expand per caller).
//!
//! Canonical settings (same as sweep.rs): RenderShadowDetail=2 -> use_sun_shadow=true,
//! SSAO ON, SSR OFF, reflection probes ON level 3 (hasReflectionProbes gates that
//! compare mShaderLevel[SHADER_DEFERRED] > 2 -> true), mirrors OFF, emissive OFF
//! (RenderEnableEmissiveBuffer=false => add_common_permutations() is a NO-OP: its only
//! effect is HAS_EMISSIVE under emissive; call sites are therefore not annotated),
//! sIndexedTextureChannels=4, GL 4.x AMD (FXAA/SMAA/CAS version gates all pass,
//! cube-map arrays present).
//!
//! Other setting-derived values baked in (viewer settings.xml defaults):
//!   RenderTransparentWater=TRUE  -> TRANSPARENT_WATER=1 on water/underwater
//!   RenderGlowNoise=TRUE         -> HAS_NOISE=1 on glow_extract
//!   RenderAvatarCloth=FALSE      -> AVATAR_CLOTH=0 on deferred_avatar
//!   RenderTerrainPBRDetail=0 (clamped [-4,0]), RenderTerrainPBRPlanarSampleCount=3
//!                                -> pbr terrain permutations (see VERIFY there)
//!   TerrainPaintBitDepth=5       -> TERRAIN_PAINT_PRECISION=31 on pbr_terrain_bake
//!   GLTFEnabled / LocalTerrainPaintEnabled default OFF but their programs are
//!   transcribed anyway (they must compile), like the mirrors/hero-probe programs.
//!
//! Ordering: SOURCE order of the createShader() sites in llviewershadermgr.cpp
//! (water :923 -> effects :1016 -> deferred :1094 -> object :3084 -> avatar :3176 ->
//! interface :3239). NB: setShaders() *executes* water, effects, interface, object,
//! avatar, deferred -- a live viewer dump lists the sections in that order; join on
//! program name when diffing.
//!
//! make_rigged_variant (:254-268) callers are emitted base-then-`<name>_rigged`
//! (the C++ actually links the rigged program first, then the base).
//! addConstant(SHADER_CONST_*) is a compile-time define (llglslshader.cpp:810,
//! addPermutation(gShaderConstsKey/Val)) and is transcribed as such.

#![allow(dead_code)]

use crate::sweep::{Features, ProgramDef};

// ---- canonical shader levels (llviewershadermgr.cpp:613-636) ---------------------
const LVL_LIGHTING: i32 = 3;
const LVL_ENVIRONMENT: i32 = 2;
const LVL_WINDLIGHT: i32 = 2;
const LVL_WATER: i32 = 3;
const LVL_DEFERRED: i32 = 3;
const LVL_OBJECT: i32 = 2;
const LVL_EFFECT: i32 = 2;
const LVL_INTERFACE: i32 = 2;
const LVL_AVATAR: i32 = 1;
const INDEXED_CHANNELS: i32 = 4; // LLGLSLShader::sIndexedTextureChannels (:576, hardcoded 4)

/// Shorthand for one addPermutation("K","V").
fn d(k: &'static str, v: &str) -> (&'static str, String) {
    (k, v.into())
}

/// make_rigged_variant (llviewershadermgr.cpp:254-268): copy of the base at the moment
/// of the call -- same files/level/defines/features + hasObjectSkinning=true + HAS_SKIN=1.
/// (At every call site all permutations are already on the base, so a copy here == C++.)
fn rigged_of(p: &ProgramDef, name: &'static str) -> ProgramDef {
    let mut f = p.features.clone();
    f.has_object_skinning = true;
    let mut defs = p.defines.clone();
    defs.push(d("HAS_SKIN", "1"));
    ProgramDef {
        name,
        level: p.level,
        features: f,
        defines: defs,
        vert: p.vert.clone(),
        frag: p.frag.clone(),
    }
}

/// Push a base program and its make_rigged_variant copy.
fn push_with_rigged(v: &mut Vec<ProgramDef>, p: ProgramDef, rigged_name: &'static str) {
    let r = rigged_of(&p, rigged_name);
    v.push(p);
    v.push(r);
}

// ---- loop-family name tables (ProgramDef.name is &'static str) -------------------

/// gDeferredMaterialProgram[LLMaterial::SHADER_COUNT*2] (SHADER_COUNT=16 -> 32).
/// C++ names: "Material Shader %d" (i<16) / "Skinned Material Shader %d" (i>=16, raw i).
const MATERIAL_NAMES: [&str; 32] = [
    "deferred_material_0",
    "deferred_material_1",
    "deferred_material_2",
    "deferred_material_3",
    "deferred_material_4",
    "deferred_material_5",
    "deferred_material_6",
    "deferred_material_7",
    "deferred_material_8",
    "deferred_material_9",
    "deferred_material_10",
    "deferred_material_11",
    "deferred_material_12",
    "deferred_material_13",
    "deferred_material_14",
    "deferred_material_15",
    "deferred_material_skinned_16",
    "deferred_material_skinned_17",
    "deferred_material_skinned_18",
    "deferred_material_skinned_19",
    "deferred_material_skinned_20",
    "deferred_material_skinned_21",
    "deferred_material_skinned_22",
    "deferred_material_skinned_23",
    "deferred_material_skinned_24",
    "deferred_material_skinned_25",
    "deferred_material_skinned_26",
    "deferred_material_skinned_27",
    "deferred_material_skinned_28",
    "deferred_material_skinned_29",
    "deferred_material_skinned_30",
    "deferred_material_skinned_31",
];

/// gGLTFPBRMetallicRoughnessProgram variants (LLGLSLShader::NUM_GLTF_VARIANTS=16).
/// Bit i&1=ALPHA_BLEND, i&2=RIGGED, i&4=UNLIT, i&8=MULTI_UV (llglslshader.h:347-355).
const GLTF_NAMES: [&str; 16] = [
    "gltf_pbr_mr",
    "gltf_pbr_mr_alpha",
    "gltf_pbr_mr_rigged",
    "gltf_pbr_mr_alpha_rigged",
    "gltf_pbr_mr_unlit",
    "gltf_pbr_mr_alpha_unlit",
    "gltf_pbr_mr_rigged_unlit",
    "gltf_pbr_mr_alpha_rigged_unlit",
    "gltf_pbr_mr_multiuv",
    "gltf_pbr_mr_alpha_multiuv",
    "gltf_pbr_mr_rigged_multiuv",
    "gltf_pbr_mr_alpha_rigged_multiuv",
    "gltf_pbr_mr_unlit_multiuv",
    "gltf_pbr_mr_alpha_unlit_multiuv",
    "gltf_pbr_mr_rigged_unlit_multiuv",
    "gltf_pbr_mr_alpha_rigged_unlit_multiuv",
];

/// gDeferredMultiLightProgram[LL_DEFERRED_MULTI_LIGHT_COUNT=16]; LIGHT_COUNT=i+1.
const MULTI_LIGHT_NAMES: [&str; 16] = [
    "deferred_multi_light_0",
    "deferred_multi_light_1",
    "deferred_multi_light_2",
    "deferred_multi_light_3",
    "deferred_multi_light_4",
    "deferred_multi_light_5",
    "deferred_multi_light_6",
    "deferred_multi_light_7",
    "deferred_multi_light_8",
    "deferred_multi_light_9",
    "deferred_multi_light_10",
    "deferred_multi_light_11",
    "deferred_multi_light_12",
    "deferred_multi_light_13",
    "deferred_multi_light_14",
    "deferred_multi_light_15",
];

/// gFXAAProgram[4] quality presets (:2612-2615): FXAA_QUALITY__PRESET value + slug.
const FXAA_PRESETS: [(&str, &str); 4] = [
    ("12", "fxaa_low"),
    ("23", "fxaa_medium"),
    ("28", "fxaa_high"),
    ("39", "fxaa_ultra"),
];

/// SMAA quality presets (:2665-2668): the preset macro set to "1".
const SMAA_PRESETS: [&str; 4] = [
    "SMAA_PRESET_LOW",
    "SMAA_PRESET_MEDIUM",
    "SMAA_PRESET_HIGH",
    "SMAA_PRESET_ULTRA",
];
const SMAA_EDGE_NAMES: [&str; 4] = [
    "smaa_edge_detect_low",
    "smaa_edge_detect_medium",
    "smaa_edge_detect_high",
    "smaa_edge_detect_ultra",
];
const SMAA_BLEND_NAMES: [&str; 4] = [
    "smaa_blend_weights_low",
    "smaa_blend_weights_medium",
    "smaa_blend_weights_high",
    "smaa_blend_weights_ultra",
];
const SMAA_NEIGHBORHOOD_NAMES: [&str; 4] = [
    "smaa_neighborhood_blend_low",
    "smaa_neighborhood_blend_medium",
    "smaa_neighborhood_blend_high",
    "smaa_neighborhood_blend_ultra",
];

/// gDeferredPBRTerrainProgram[TERRAIN_PAINT_TYPE_COUNT=2]: paint_type 0 = heightmap
/// with noise, 1 = PBR paintmap (llviewershadermgr.h:329-333).
const PBR_TERRAIN_NAMES: [&str; 2] = [
    "deferred_pbr_terrain_heightmap",
    "deferred_pbr_terrain_paintmap",
];

/// The complete program table (all ~132 createShader sites, loops expanded).
pub fn programs_full() -> Vec<ProgramDef> {
    let mut v: Vec<ProgramDef> = Vec::with_capacity(240);

    // =========================================================================
    // loadShadersWater (:923-1014). use_sun_shadow = deferred>1 && shadow detail>0 -> true.
    // =========================================================================

    // gWaterProgram (:942-966) -- mShaderGroup=SG_WATER (no table slot; group only
    // affects uniform refresh cadence, not assembly).
    v.push(ProgramDef {
        name: "water",
        level: LVL_WATER,
        features: Features {
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            has_srgb: true,
            has_reflection_probes: true,
            has_tonemap: true,
            has_shadows: true, // use_sun_shadow
            ..Default::default()
        },
        // TRANSPARENT_WATER: LLPipeline::sRenderTransparentWater (RenderTransparentWater default TRUE)
        defines: vec![d("TRANSPARENT_WATER", "1"), d("HAS_SUN_SHADOW", "1")],
        vert: vec!["environment/waterV.glsl"],
        frag: vec!["environment/waterF.glsl"],
    });

    // gUnderWaterProgram (:973-986)
    v.push(ProgramDef {
        name: "underwater",
        level: LVL_WATER,
        features: Features {
            calculates_atmospherics: true,
            has_atmospherics: true,
            ..Default::default()
        },
        defines: vec![d("TRANSPARENT_WATER", "1")],
        vert: vec!["environment/waterV.glsl"],
        frag: vec!["environment/underWaterF.glsl"],
    });

    // =========================================================================
    // loadShadersEffects (:1016-1092)
    // =========================================================================

    // gGlowProgram (:1032-1037)
    v.push(ProgramDef {
        name: "glow",
        level: LVL_EFFECT,
        features: Features::default(),
        defines: vec![],
        vert: vec!["effects/glowV.glsl"],
        frag: vec!["effects/glowF.glsl"],
    });

    // gGlowExtractProgram (:1046-1060) -- HAS_NOISE under RenderGlowNoise (default TRUE)
    v.push(ProgramDef {
        name: "glow_extract",
        level: LVL_EFFECT,
        features: Features::default(),
        defines: vec![d("HAS_NOISE", "1")],
        vert: vec!["effects/glowExtractV.glsl"],
        frag: vec!["effects/glowExtractF.glsl"],
    });

    // gPostVignetteProgram (:1070-1075) <FS:CR>
    v.push(ProgramDef {
        name: "post_vignette",
        level: LVL_EFFECT,
        features: Features::default(),
        defines: vec![],
        vert: vec!["post/exoPostBaseV.glsl"],
        frag: vec!["post/exoVignetteF.glsl"],
    });

    // gPostSnapshotFrameProgram (:1081-1086) <FS:Beq>
    v.push(ProgramDef {
        name: "post_snapshot_frame",
        level: LVL_EFFECT,
        features: Features::default(),
        defines: vec![],
        vert: vec!["post/exoPostBaseV.glsl"],
        frag: vec!["post/snapshotFrameF.glsl"],
    });

    // =========================================================================
    // loadShadersDeferred (:1094-3082). use_sun_shadow = true (canonical).
    // =========================================================================

    // gDeferredHighlightProgram (:1221-1227) -- INTERFACE level
    v.push(ProgramDef {
        name: "deferred_highlight",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/highlightV.glsl"],
        frag: vec!["deferred/highlightF.glsl"],
    });

    // gDeferredDiffuseProgram (+ gDeferredSkinnedDiffuseProgram) (:1232-1241)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_diffuse",
            level: LVL_DEFERRED,
            features: Features {
                has_srgb: true,
                indexed_texture_channels: INDEXED_CHANNELS,
                ..Default::default()
            },
            defines: vec![],
            vert: vec!["deferred/diffuseV.glsl"],
            frag: vec!["deferred/diffuseIndexedF.glsl"],
        },
        "deferred_diffuse_rigged",
    );

    // gDeferredDiffuseAlphaMaskProgram (+Skinned) (:1246-1254)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_diffuse_alpha_mask",
            level: LVL_DEFERRED,
            features: Features {
                indexed_texture_channels: INDEXED_CHANNELS,
                ..Default::default()
            },
            defines: vec![],
            vert: vec!["deferred/diffuseV.glsl"],
            frag: vec!["deferred/diffuseAlphaMaskIndexedF.glsl"],
        },
        "deferred_diffuse_alpha_mask_rigged",
    );

    // gDeferredNonIndexedDiffuseAlphaMaskProgram (:1259-1265)
    v.push(ProgramDef {
        name: "deferred_nonindexed_diffuse_alpha_mask",
        level: LVL_DEFERRED,
        features: Features::default(),
        defines: vec![],
        vert: vec!["deferred/diffuseV.glsl"],
        frag: vec!["deferred/diffuseAlphaMaskF.glsl"],
    });

    // gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram (:1271-1277)
    v.push(ProgramDef {
        name: "deferred_nonindexed_diffuse_alpha_mask_no_color",
        level: LVL_DEFERRED,
        features: Features::default(),
        defines: vec![],
        vert: vec!["deferred/diffuseNoColorV.glsl"],
        frag: vec!["deferred/diffuseAlphaMaskNoColorF.glsl"],
    });

    // gDeferredBumpProgram (+Skinned) (:1283-1290)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_bump",
            level: LVL_DEFERRED,
            features: Features::default(),
            defines: vec![],
            vert: vec!["deferred/bumpV.glsl"],
            frag: vec!["deferred/bumpF.glsl"],
        },
        "deferred_bump_rigged",
    );

    // gDeferredMaterialProgram[32] (:1294-1385).
    // i&0x10 = skinned, i&0x3 = DIFFUSE_ALPHA_MODE, i&0x8 = normal map, i&0x4 = spec map.
    // hasLighting: the out-of-band pokes (:1294-1301 pre / :1378-1385 post) force
    // hasLighting=false on indices 1,5,9,13(+16) BEFORE the loop and true AFTER --
    // so at the createShader() moment has_lighting is false for ALL 32 (the post-poke
    // is a runtime uniform-binding hack for alpha-mode-BLEND and never affects
    // compilation; the pre-poke only matters on shader RELOAD).
    for i in 0..32usize {
        let has_skin = (i & 0x10) != 0;
        let alpha_mode = i & 0x3;
        let has_normal_map = (i & 0x8) != 0;
        let has_specular_map = (i & 0x4) != 0;

        let mut defines: Vec<(&'static str, String)> = Vec::new();
        if has_normal_map {
            defines.push(d("HAS_NORMAL_MAP", "1"));
        }
        if has_specular_map {
            defines.push(d("HAS_SPECULAR_MAP", "1"));
        }
        defines.push(("DIFFUSE_ALPHA_MODE", alpha_mode.to_string()));
        if alpha_mode != 0 {
            defines.push(d("HAS_ALPHA_MASK", "1"));
        }
        defines.push(d("HAS_SUN_SHADOW", "1")); // use_sun_shadow canonical
        if has_skin {
            defines.push(d("HAS_SKIN", "1"));
        }

        v.push(ProgramDef {
            name: MATERIAL_NAMES[i],
            level: LVL_DEFERRED,
            features: Features {
                has_alpha_mask: alpha_mode != 0,
                has_srgb: true,
                calculates_atmospherics: true,
                has_atmospherics: true,
                has_gamma: true,
                has_shadows: true, // use_sun_shadow
                has_reflection_probes: true,
                has_object_skinning: has_skin,
                ..Default::default()
            },
            defines,
            vert: vec!["deferred/materialV.glsl"],
            frag: vec!["deferred/materialF.glsl"],
        });
    }

    // gDeferredPBROpaqueProgram (+Skinned) (:1389-1404)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_pbr_opaque",
            level: LVL_DEFERRED,
            features: Features {
                has_srgb: true,
                ..Default::default()
            },
            defines: vec![],
            vert: vec!["deferred/pbropaqueV.glsl"],
            frag: vec!["deferred/pbropaqueF.glsl"],
        },
        "deferred_pbr_opaque_rigged",
    );

    // gGLTFPBRMetallicRoughnessProgram 16 variants (:1408-1434 + make_gltf_variant
    // :281-353). The BASE program is never createShader()d itself -- only the 16
    // variants are. Gated on GLTFEnabled (default off) but transcribed regardless.
    // Caps from canonical mMaxUniformBlockSize=65536: nodes=65536/48=1365,
    // materials=65536/192=341, vec4s=65536/16=4096 (dup the sweep globals; harmless).
    // NB make_gltf_variant does NOT set hasObjectSkinning for RIGGED -- only the
    // HAS_SKIN define (GLTF skinning lives in the gltf shader's own node UBO system).
    for i in 0..16usize {
        let alpha_blend = (i & 1) != 0; // GLTFVariant::ALPHA_BLEND
        let is_rigged = (i & 2) != 0; // GLTFVariant::RIGGED
        let unlit = (i & 4) != 0; // GLTFVariant::UNLIT
        let multi_uv = (i & 8) != 0; // GLTFVariant::MULTI_UV

        let mut defines: Vec<(&'static str, String)> = vec![
            d("MAX_NODES_PER_GLTF_OBJECT", "1365"),
            d("MAX_MATERIALS_PER_GLTF_OBJECT", "341"),
            d("MAX_UBO_VEC4S", "4096"),
        ];
        if is_rigged {
            defines.push(d("HAS_SKIN", "1"));
        }
        if unlit {
            defines.push(d("UNLIT", "1"));
        }
        if multi_uv {
            defines.push(d("MULTI_UV", "1"));
        }

        // base features: hasSrgb (from gGLTFPBRMetallicRoughnessProgram, :1413)
        let mut f = Features {
            has_srgb: true,
            ..Default::default()
        };
        if alpha_blend {
            defines.push(d("ALPHA_BLEND", "1"));
            // :323-332 -- snapshot at the createShader moment (the post-success
            // calculatesLighting/hasLighting=true "Alpha Shader Hack" is runtime-only)
            f.is_alpha_lighting = true;
            f.calculates_atmospherics = true;
            f.has_atmospherics = true;
            f.has_gamma = true;
            f.has_shadows = true; // use_sun_shadow
            f.is_deferred = true;
            f.has_reflection_probes = true;
            defines.push(d("HAS_SUN_SHADOW", "1"));
        }

        v.push(ProgramDef {
            name: GLTF_NAMES[i],
            level: LVL_DEFERRED,
            features: f,
            defines,
            vert: vec!["gltf/pbrmetallicroughnessV.glsl"],
            frag: vec!["gltf/pbrmetallicroughnessF.glsl"],
        });
    }

    // gPBRGlowProgram (+Skinned) (:1438-1451)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "pbr_glow",
            level: LVL_DEFERRED,
            features: Features {
                has_srgb: true,
                ..Default::default()
            },
            defines: vec![],
            vert: vec!["deferred/pbrglowV.glsl"],
            frag: vec!["deferred/pbrglowF.glsl"],
        },
        "pbr_glow_rigged",
    );

    // gHUDPBROpaqueProgram (:1457-1468)
    v.push(ProgramDef {
        name: "hud_pbr_opaque",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![d("IS_HUD", "1")],
        vert: vec!["deferred/pbropaqueV.glsl"],
        frag: vec!["deferred/pbropaqueF.glsl"],
    });

    // gDeferredPBRAlphaProgram (+Skinned) (:1475-1524). Snapshot BEFORE the post-
    // create "Alpha Shader Hack" (calculatesLighting/hasLighting stay false).
    // hasReflectionProbes = mShaderLevel[SHADER_DEFERRED] (=3, nonzero -> true).
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_pbr_alpha",
            level: LVL_DEFERRED,
            features: Features {
                is_alpha_lighting: true,
                has_srgb: true,
                calculates_atmospherics: true,
                has_atmospherics: true,
                has_gamma: true,
                has_shadows: true, // use_sun_shadow
                is_deferred: true,
                has_reflection_probes: true,
                ..Default::default()
            },
            defines: vec![
                d("DIFFUSE_ALPHA_MODE", "1"), // LLMaterial::DIFFUSE_ALPHA_MODE_BLEND
                d("HAS_NORMAL_MAP", "1"),
                d("HAS_SPECULAR_MAP", "1"), // PBR packed: Occlusion, Metal, Roughness
                d("HAS_EMISSIVE_MAP", "1"),
                d("USE_VERTEX_COLOR", "1"),
                d("HAS_SUN_SHADOW", "1"),
            ],
            vert: vec!["deferred/pbralphaV.glsl"],
            frag: vec!["deferred/pbralphaF.glsl"],
        },
        "deferred_pbr_alpha_rigged",
    );

    // gHUDPBRAlphaProgram (:1528-1545)
    v.push(ProgramDef {
        name: "hud_pbr_alpha",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![d("IS_HUD", "1")],
        vert: vec!["deferred/pbralphaV.glsl"],
        frag: vec!["deferred/pbralphaF.glsl"],
    });

    // gDeferredPBRTerrainProgram[2] (:1548-1581), paint_type 0/1 both transcribed.
    // VERIFY: detail/mapping come from settings defaults (RenderTerrainPBRDetail=0
    // clamped [-4,0]; RenderTerrainPBRPlanarSampleCount=3 -> clamp_terrain_mapping=3),
    // but sweep.rs's canonical GLOBALS pin TERRAIN_PBR_DETAIL=1 and
    // TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT=1, and assemble() keeps the GLOBAL value on
    // key collision -- these two per-program values are inert there; TERRAIN_PAINT_TYPE
    // is per-program-only and always applies.
    // NB the C++ also sets mFeatures.hasTransport=true (:1565) -- dead flag in this
    // branch (attachShaderFeatures never reads it); Features has no field for it.
    for paint_type in 0..2usize {
        v.push(ProgramDef {
            name: PBR_TERRAIN_NAMES[paint_type],
            level: LVL_DEFERRED,
            features: Features {
                has_srgb: true,
                is_alpha_lighting: true,
                calculates_atmospherics: true,
                has_atmospherics: true,
                has_gamma: true,
                is_pbr_terrain: true,
                ..Default::default()
            },
            defines: vec![
                d("TERRAIN_PBR_DETAIL", "0"),
                ("TERRAIN_PAINT_TYPE", paint_type.to_string()),
                d("TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT", "3"),
            ],
            vert: vec!["deferred/pbrterrainV.glsl"],
            frag: vec!["deferred/pbrterrainF.glsl"],
        });
    }

    // gDeferredTreeProgram (:1585-1593)
    v.push(ProgramDef {
        name: "deferred_tree",
        level: LVL_DEFERRED,
        features: Features::default(),
        defines: vec![],
        vert: vec!["deferred/treeV.glsl"],
        frag: vec!["deferred/treeF.glsl"],
    });

    // gDeferredTreeShadowProgram (:1598-1605)
    v.push(ProgramDef {
        name: "deferred_tree_shadow",
        level: LVL_DEFERRED,
        features: Features::default(),
        defines: vec![],
        vert: vec!["deferred/treeShadowV.glsl"],
        frag: vec!["deferred/treeShadowF.glsl"],
    });

    // gDeferredSkinnedTreeShadowProgram (:1610-1617) -- hand-written skinned block
    v.push(ProgramDef {
        name: "deferred_skinned_tree_shadow",
        level: LVL_DEFERRED,
        features: Features {
            has_object_skinning: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/treeShadowSkinnedV.glsl"],
        frag: vec!["deferred/treeShadowF.glsl"],
    });

    // gDeferredImpostorProgram (:1622-1632)
    v.push(ProgramDef {
        name: "deferred_impostor",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/impostorV.glsl"],
        frag: vec!["deferred/impostorF.glsl"],
    });

    // gDeferredLightProgram (:1637-1653)
    v.push(ProgramDef {
        name: "deferred_light",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            has_full_gbuffer: true,
            has_shadows: true,
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/pointLightV.glsl"],
        frag: vec!["deferred/pointLightF.glsl"],
    });

    // gDeferredMultiLightProgram[16] (:1656-1678) -- LIGHT_COUNT = i+1
    for i in 0..16usize {
        v.push(ProgramDef {
            name: MULTI_LIGHT_NAMES[i],
            level: LVL_DEFERRED,
            features: Features {
                is_deferred: true,
                has_full_gbuffer: true,
                has_shadows: true,
                has_srgb: true,
                ..Default::default()
            },
            defines: vec![("LIGHT_COUNT", (i + 1).to_string())],
            vert: vec!["deferred/multiPointLightV.glsl"],
            frag: vec!["deferred/multiPointLightF.glsl"],
        });
    }

    // gDeferredSpotLightProgram (:1682-1697)
    v.push(ProgramDef {
        name: "deferred_spot_light",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_deferred: true,
            has_full_gbuffer: true,
            has_shadows: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/pointLightV.glsl"],
        frag: vec!["deferred/spotLightF.glsl"],
    });

    // gDeferredMultiSpotLightProgram (:1702-1718)
    v.push(ProgramDef {
        name: "deferred_multi_spot_light",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_deferred: true,
            has_full_gbuffer: true,
            has_shadows: true,
            ..Default::default()
        },
        defines: vec![d("MULTI_SPOTLIGHT", "1")],
        vert: vec!["deferred/multiPointLightV.glsl"],
        frag: vec!["deferred/spotLightF.glsl"],
    });

    // gDeferredSunProgram (:1721-1748) -- SSAO ON canonical: fragment =
    // sunLightSSAOF.glsl, hasAmbientOcclusion = true.
    v.push(ProgramDef {
        name: "deferred_sun",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            has_shadows: true,
            has_ambient_occlusion: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/sunLightV.glsl"],
        frag: vec!["deferred/sunLightSSAOF.glsl"],
    });

    // gDeferredSunProbeProgram (:1752-1764) -- always the non-SSAO fragment
    v.push(ProgramDef {
        name: "deferred_sun_probe",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            has_shadows: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/sunLightV.glsl"],
        frag: vec!["deferred/sunLightF.glsl"],
    });

    // gDeferredBlurLightProgram (:1769-1780)
    v.push(ProgramDef {
        name: "deferred_blur_light",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/blurLightV.glsl"],
        frag: vec!["deferred/blurLightF.glsl"],
    });

    // gDeferredAlphaProgram / gDeferredSkinnedAlphaProgram / gHUDAlphaProgram
    // (:1783-1854, 3-iteration loop). Snapshot BEFORE the post-create lighting hack.
    for (name, is_rigged, hud) in [
        ("deferred_alpha", false, false),
        ("deferred_skinned_alpha", true, false),
        ("hud_alpha", false, true),
    ] {
        let mut defines: Vec<(&'static str, String)> = vec![
            d("USE_VERTEX_COLOR", "1"),
            d("HAS_ALPHA_MASK", "1"),
            d("USE_INDEXED_TEX", "1"),
            d("HAS_SUN_SHADOW", "1"), // use_sun_shadow
        ];
        if is_rigged {
            defines.push(d("HAS_SKIN", "1"));
        }
        if hud {
            defines.push(d("IS_HUD", "1"));
        }
        v.push(ProgramDef {
            name,
            level: LVL_DEFERRED,
            features: Features {
                is_alpha_lighting: true,
                has_srgb: true,
                calculates_atmospherics: true,
                has_atmospherics: true,
                has_gamma: true,
                has_shadows: true, // use_sun_shadow
                has_reflection_probes: true,
                indexed_texture_channels: INDEXED_CHANNELS,
                has_object_skinning: is_rigged,
                ..Default::default()
            },
            defines,
            vert: vec!["deferred/alphaV.glsl"],
            frag: vec!["deferred/alphaF.glsl"],
        });
    }

    // gDeferredAlphaImpostorProgram / gDeferredSkinnedAlphaImpostorProgram
    // (:1856-1915, 2-iteration loop). No atmospherics/gamma here; snapshot pre-hack.
    for (name, is_rigged) in [
        ("deferred_alpha_impostor", false),
        ("deferred_skinned_alpha_impostor", true),
    ] {
        let mut defines: Vec<(&'static str, String)> = vec![
            d("USE_INDEXED_TEX", "1"),
            d("FOR_IMPOSTOR", "1"),
            d("HAS_ALPHA_MASK", "1"),
            d("USE_VERTEX_COLOR", "1"),
        ];
        if is_rigged {
            defines.push(d("HAS_SKIN", "1"));
        }
        defines.push(d("HAS_SUN_SHADOW", "1")); // use_sun_shadow
        v.push(ProgramDef {
            name,
            level: LVL_DEFERRED,
            features: Features {
                has_srgb: true,
                is_alpha_lighting: true,
                has_shadows: true, // use_sun_shadow
                has_reflection_probes: true,
                indexed_texture_channels: INDEXED_CHANNELS,
                has_object_skinning: is_rigged,
                ..Default::default()
            },
            defines,
            vert: vec!["deferred/alphaV.glsl"],
            frag: vec!["deferred/alphaF.glsl"],
        });
    }

    // gDeferredAvatarEyesProgram (:1919-1934) -- hasShadows unconditional
    v.push(ProgramDef {
        name: "deferred_avatar_eyes",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true,
            has_gamma: true,
            has_atmospherics: true,
            has_srgb: true,
            has_shadows: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/avatarEyesV.glsl"],
        frag: vec!["deferred/diffuseF.glsl"],
    });

    // gDeferredFullbrightProgram (+Skinned) (:1939-1954)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_fullbright",
            level: LVL_DEFERRED,
            features: Features {
                calculates_atmospherics: true,
                has_gamma: true,
                has_atmospherics: true,
                has_srgb: true,
                indexed_texture_channels: INDEXED_CHANNELS,
                ..Default::default()
            },
            defines: vec![],
            vert: vec!["deferred/fullbrightV.glsl"],
            frag: vec!["deferred/fullbrightF.glsl"],
        },
        "deferred_fullbright_rigged",
    );

    // gHUDFullbrightProgram (:1959-1975)
    v.push(ProgramDef {
        name: "hud_fullbright",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true,
            has_gamma: true,
            has_atmospherics: true,
            has_srgb: true,
            indexed_texture_channels: INDEXED_CHANNELS,
            ..Default::default()
        },
        defines: vec![d("IS_HUD", "1")],
        vert: vec!["deferred/fullbrightV.glsl"],
        frag: vec!["deferred/fullbrightF.glsl"],
    });

    // gDeferredFullbrightAlphaMaskProgram (+Skinned) (:1980-1997)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_fullbright_alpha_mask",
            level: LVL_DEFERRED,
            features: Features {
                calculates_atmospherics: true,
                has_gamma: true,
                has_atmospherics: true,
                has_srgb: true,
                indexed_texture_channels: INDEXED_CHANNELS,
                ..Default::default()
            },
            defines: vec![d("HAS_ALPHA_MASK", "1")],
            vert: vec!["deferred/fullbrightV.glsl"],
            frag: vec!["deferred/fullbrightF.glsl"],
        },
        "deferred_fullbright_alpha_mask_rigged",
    );

    // gHUDFullbrightAlphaMaskProgram (:2002-2019)
    v.push(ProgramDef {
        name: "hud_fullbright_alpha_mask",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true,
            has_gamma: true,
            has_atmospherics: true,
            has_srgb: true,
            indexed_texture_channels: INDEXED_CHANNELS,
            ..Default::default()
        },
        defines: vec![d("HAS_ALPHA_MASK", "1"), d("IS_HUD", "1")],
        vert: vec!["deferred/fullbrightV.glsl"],
        frag: vec!["deferred/fullbrightF.glsl"],
    });

    // gDeferredFullbrightAlphaMaskAlphaProgram (+Skinned) (:2024-2043)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_fullbright_alpha_mask_alpha",
            level: LVL_DEFERRED,
            features: Features {
                calculates_atmospherics: true,
                has_gamma: true,
                has_atmospherics: true,
                has_srgb: true,
                is_deferred: true,
                indexed_texture_channels: INDEXED_CHANNELS,
                ..Default::default()
            },
            defines: vec![d("HAS_ALPHA_MASK", "1"), d("IS_ALPHA", "1")],
            vert: vec!["deferred/fullbrightV.glsl"],
            frag: vec!["deferred/fullbrightF.glsl"],
        },
        "deferred_fullbright_alpha_mask_alpha_rigged",
    );

    // gHUDFullbrightAlphaMaskAlphaProgram (:2048-2067)
    v.push(ProgramDef {
        name: "hud_fullbright_alpha_mask_alpha",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true,
            has_gamma: true,
            has_atmospherics: true,
            has_srgb: true,
            is_deferred: true,
            indexed_texture_channels: INDEXED_CHANNELS,
            ..Default::default()
        },
        defines: vec![d("HAS_ALPHA_MASK", "1"), d("IS_ALPHA", "1"), d("IS_HUD", "1")],
        vert: vec!["deferred/fullbrightV.glsl"],
        frag: vec!["deferred/fullbrightF.glsl"],
    });

    // gDeferredFullbrightShinyProgram (+Skinned) (:2072-2088)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_fullbright_shiny",
            level: LVL_DEFERRED,
            features: Features {
                calculates_atmospherics: true,
                has_atmospherics: true,
                has_gamma: true,
                has_srgb: true,
                has_reflection_probes: true,
                indexed_texture_channels: INDEXED_CHANNELS,
                ..Default::default()
            },
            defines: vec![],
            vert: vec!["deferred/fullbrightShinyV.glsl"],
            frag: vec!["deferred/fullbrightShinyF.glsl"],
        },
        "deferred_fullbright_shiny_rigged",
    );

    // gHUDFullbrightShinyProgram (:2093-2110)
    v.push(ProgramDef {
        name: "hud_fullbright_shiny",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            has_srgb: true,
            has_reflection_probes: true,
            indexed_texture_channels: INDEXED_CHANNELS,
            ..Default::default()
        },
        defines: vec![d("IS_HUD", "1")],
        vert: vec!["deferred/fullbrightShinyV.glsl"],
        frag: vec!["deferred/fullbrightShinyF.glsl"],
    });

    // gDeferredEmissiveProgram (+Skinned) (:2115-2129) -- no srgb
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_emissive",
            level: LVL_DEFERRED,
            features: Features {
                calculates_atmospherics: true,
                has_gamma: true,
                has_atmospherics: true,
                indexed_texture_channels: INDEXED_CHANNELS,
                ..Default::default()
            },
            defines: vec![],
            vert: vec!["deferred/emissiveV.glsl"],
            frag: vec!["deferred/emissiveF.glsl"],
        },
        "deferred_emissive_rigged",
    );

    // gDeferredSoftenProgram (:2134-2165) -- probes: level>2 -> true; SSAO ON adds
    // HAS_SSAO (and llmax(level,2), already 3).
    v.push(ProgramDef {
        name: "deferred_soften",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            is_deferred: true,
            has_full_gbuffer: true,
            has_shadows: true, // use_sun_shadow
            has_reflection_probes: true,
            ..Default::default()
        },
        defines: vec![d("HAS_SUN_SHADOW", "1"), d("HAS_SSAO", "1")],
        vert: vec!["deferred/softenLightV.glsl"],
        frag: vec!["deferred/softenLightF.glsl"],
    });

    // gHazeProgram (:2169-2189) -- NO HAS_SUN_SHADOW permutation (feature only)
    v.push(ProgramDef {
        name: "haze",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            is_deferred: true,
            has_shadows: true, // use_sun_shadow
            has_reflection_probes: true, // level>2
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/softenLightV.glsl"],
        frag: vec!["deferred/hazeF.glsl"],
    });

    // gHazeWaterProgram (:2194-2214) -- SG_WATER group
    v.push(ProgramDef {
        name: "haze_water",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            is_deferred: true,
            has_shadows: true, // use_sun_shadow
            has_reflection_probes: true, // level>2
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/waterHazeV.glsl"],
        frag: vec!["deferred/waterHazeF.glsl"],
    });

    // gDeferredShadowProgram (:2220-2228) -- NO features on the base
    v.push(ProgramDef {
        name: "deferred_shadow",
        level: LVL_DEFERRED,
        features: Features::default(),
        defines: vec![],
        vert: vec!["deferred/shadowV.glsl"],
        frag: vec!["deferred/shadowF.glsl"],
    });

    // gDeferredSkinnedShadowProgram (:2232-2245) -- hand-written skinned block
    v.push(ProgramDef {
        name: "deferred_skinned_shadow",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            has_shadows: true,
            has_object_skinning: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/shadowSkinnedV.glsl"],
        frag: vec!["deferred/shadowF.glsl"],
    });

    // gDeferredShadowCubeProgram (:2250-2259)
    v.push(ProgramDef {
        name: "deferred_shadow_cube",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            has_shadows: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/shadowCubeV.glsl"],
        frag: vec!["deferred/shadowF.glsl"],
    });

    // gDeferredShadowFullbrightAlphaMaskProgram (+Skinned) (:2264-2281)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_shadow_fullbright_alpha_mask",
            level: LVL_DEFERRED,
            features: Features {
                indexed_texture_channels: INDEXED_CHANNELS,
                ..Default::default()
            },
            defines: vec![d("DEPTH_CLAMP", "1"), d("IS_FULLBRIGHT", "1")],
            vert: vec!["deferred/shadowAlphaMaskV.glsl"],
            frag: vec!["deferred/shadowAlphaMaskF.glsl"],
        },
        "deferred_shadow_fullbright_alpha_mask_rigged",
    );

    // gDeferredShadowAlphaMaskProgram (+Skinned) (:2285-2294)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_shadow_alpha_mask",
            level: LVL_DEFERRED,
            features: Features {
                indexed_texture_channels: INDEXED_CHANNELS,
                ..Default::default()
            },
            defines: vec![],
            vert: vec!["deferred/shadowAlphaMaskV.glsl"],
            frag: vec!["deferred/shadowAlphaMaskF.glsl"],
        },
        "deferred_shadow_alpha_mask_rigged",
    );

    // gDeferredShadowGLTFAlphaMaskProgram (+Skinned) (:2300-2312)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_shadow_gltf_alpha_mask",
            level: LVL_DEFERRED,
            features: Features::default(),
            defines: vec![],
            vert: vec!["deferred/pbrShadowAlphaMaskV.glsl"],
            frag: vec!["deferred/pbrShadowAlphaMaskF.glsl"],
        },
        "deferred_shadow_gltf_alpha_mask_rigged",
    );

    // gDeferredShadowGLTFAlphaBlendProgram (+Skinned) (:2316-2328)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "deferred_shadow_gltf_alpha_blend",
            level: LVL_DEFERRED,
            features: Features::default(),
            defines: vec![],
            vert: vec!["deferred/pbrShadowAlphaMaskV.glsl"],
            frag: vec!["deferred/pbrShadowAlphaBlendF.glsl"],
        },
        "deferred_shadow_gltf_alpha_blend_rigged",
    );

    // gDeferredAvatarShadowProgram (:2332-2341)
    v.push(ProgramDef {
        name: "deferred_avatar_shadow",
        level: LVL_DEFERRED,
        features: Features {
            has_skinning: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/avatarShadowV.glsl"],
        frag: vec!["deferred/avatarShadowF.glsl"],
    });

    // gDeferredAvatarAlphaShadowProgram (:2345-2352)
    v.push(ProgramDef {
        name: "deferred_avatar_alpha_shadow",
        level: LVL_DEFERRED,
        features: Features {
            has_skinning: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/avatarAlphaShadowV.glsl"],
        frag: vec!["deferred/avatarAlphaShadowF.glsl"],
    });

    // gDeferredAvatarAlphaMaskShadowProgram (:2356-2363)
    v.push(ProgramDef {
        name: "deferred_avatar_alpha_mask_shadow",
        level: LVL_DEFERRED,
        features: Features {
            has_skinning: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/avatarAlphaShadowV.glsl"],
        frag: vec!["deferred/avatarAlphaMaskShadowF.glsl"],
    });

    // gDeferredTerrainProgram (:2368-2383) -- legacy (non-PBR) terrain
    v.push(ProgramDef {
        name: "deferred_terrain",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_alpha_lighting: true,
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/terrainV.glsl"],
        frag: vec!["deferred/terrainF.glsl"],
    });

    // gDeferredAvatarProgram (:2388-2400) -- AVATAR_CLOTH from RenderAvatarCloth (FALSE)
    v.push(ProgramDef {
        name: "deferred_avatar",
        level: LVL_DEFERRED,
        features: Features {
            has_skinning: true,
            ..Default::default()
        },
        defines: vec![d("AVATAR_CLOTH", "0")],
        vert: vec!["deferred/avatarV.glsl"],
        frag: vec!["deferred/avatarF.glsl"],
    });

    // gDeferredAvatarAlphaProgram (:2405-2439) -- hasShadows unconditional; snapshot
    // BEFORE the post-create lighting pokes (:2437-2438).
    v.push(ProgramDef {
        name: "deferred_avatar_alpha",
        level: LVL_DEFERRED,
        features: Features {
            has_skinning: true,
            is_alpha_lighting: true,
            has_srgb: true,
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            is_deferred: true,
            has_shadows: true,
            has_reflection_probes: true,
            ..Default::default()
        },
        defines: vec![
            d("USE_DIFFUSE_TEX", "1"),
            d("IS_AVATAR_SKIN", "1"),
            d("HAS_SUN_SHADOW", "1"), // use_sun_shadow
        ],
        vert: vec!["deferred/alphaV.glsl"],
        frag: vec!["deferred/alphaF.glsl"],
    });

    // gExposureProgram (:2443-2453)
    v.push(ProgramDef {
        name: "exposure",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![d("USE_LAST_EXPOSURE", "1")],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/exposureF.glsl"],
    });

    // gExposureProgramNoFade (:2458-2467)
    v.push(ProgramDef {
        name: "exposure_no_fade",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/exposureF.glsl"],
    });

    // gLuminanceProgram (:2472-2479)
    v.push(ProgramDef {
        name: "luminance",
        level: LVL_DEFERRED,
        features: Features::default(),
        defines: vec![],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/luminanceF.glsl"],
    });

    // gDeferredPostGammaCorrectProgram (:2484-2493)
    v.push(ProgramDef {
        name: "deferred_post_gamma_correct",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredGammaCorrect.glsl"],
    });

    // gLegacyPostGammaCorrectProgram (:2498-2508)
    v.push(ProgramDef {
        name: "legacy_post_gamma_correct",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![d("LEGACY_GAMMA", "1")],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredGammaCorrect.glsl"],
    });

    // gDeferredPostTonemapProgram (:2513-2523)
    v.push(ProgramDef {
        name: "deferred_post_tonemap",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_deferred: true,
            has_tonemap: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredTonemap.glsl"],
    });

    // gNoPostTonemapProgram (:2528-2539)
    v.push(ProgramDef {
        name: "no_post_tonemap",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_deferred: true,
            has_tonemap: true,
            ..Default::default()
        },
        defines: vec![d("NO_POST", "1")],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredTonemap.glsl"],
    });

    // gDeferredPostTonemapGammaCorrectProgram (:2544-2555)
    v.push(ProgramDef {
        name: "deferred_post_tonemap_gamma_correct",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_deferred: true,
            has_tonemap: true,
            ..Default::default()
        },
        defines: vec![d("GAMMA_CORRECT", "1")],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredTonemap.glsl"],
    });

    // gNoPostTonemapGammaCorrectProgram (:2560-2572)
    v.push(ProgramDef {
        name: "no_post_tonemap_gamma_correct",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_deferred: true,
            has_tonemap: true,
            ..Default::default()
        },
        defines: vec![d("GAMMA_CORRECT", "1"), d("NO_POST", "1")],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredTonemap.glsl"],
    });

    // gDeferredPostTonemapLegacyGammaCorrectProgram (:2577-2589)
    // VERIFY: the C++ (:2579) sets isDeferred on gDeferredPostTonemapProgram -- the
    // WRONG (already-created) program, an apparent copy/paste bug -- so THIS program
    // compiles WITHOUT isDeferred. Transcribed faithfully (is_deferred: false).
    v.push(ProgramDef {
        name: "deferred_post_tonemap_legacy_gamma_correct",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            has_tonemap: true,
            ..Default::default()
        },
        defines: vec![d("GAMMA_CORRECT", "1"), d("LEGACY_GAMMA", "1")],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredTonemap.glsl"],
    });

    // gNoPostTonemapLegacyGammaCorrectProgram (:2594-2607)
    v.push(ProgramDef {
        name: "no_post_tonemap_legacy_gamma_correct",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            is_deferred: true,
            has_tonemap: true,
            ..Default::default()
        },
        defines: vec![d("NO_POST", "1"), d("GAMMA_CORRECT", "1"), d("LEGACY_GAMMA", "1")],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredTonemap.glsl"],
    });

    // gFXAAProgram[4] (:2610-2661) -- gate GL>3.9 passes canonically; FXAA_GLSL_400.
    for (preset, name) in FXAA_PRESETS {
        v.push(ProgramDef {
            name,
            level: LVL_DEFERRED,
            features: Features {
                is_deferred: true,
                ..Default::default()
            },
            defines: vec![("FXAA_QUALITY__PRESET", preset.into()), d("FXAA_GLSL_400", "1")],
            vert: vec!["deferred/postDeferredV.glsl"],
            frag: vec!["deferred/fxaaF.glsl"],
        });
    }

    // SMAA programs [3 x 4 presets] (:2663-2773) -- gate GL>3.15 passes; GL>=4.0 ->
    // SMAA_GLSL_4. Per program the C++ pushes F-main, V-main, then SMAA.glsl for BOTH
    // stages -- per-stage order preserved here (main file first, SMAA.glsl second).
    for i in 0..4usize {
        let preset = SMAA_PRESETS[i];
        let smaa_defines = || -> Vec<(&'static str, String)> {
            vec![
                d("SMAA_GLSL_4", "1"),
                d("SMAA_PREDICATION", "0"),
                d("SMAA_REPROJECTION", "0"),
                (preset, "1".into()),
            ]
        };

        // gSMAAEdgeDetectProgram[i] (:2686-2698)
        v.push(ProgramDef {
            name: SMAA_EDGE_NAMES[i],
            level: LVL_DEFERRED,
            features: Features {
                is_deferred: true,
                ..Default::default()
            },
            defines: smaa_defines(),
            vert: vec!["deferred/SMAAEdgeDetectV.glsl", "deferred/SMAA.glsl"],
            frag: vec!["deferred/SMAAEdgeDetectF.glsl", "deferred/SMAA.glsl"],
        });

        // gSMAABlendWeightsProgram[i] (:2712-2724)
        v.push(ProgramDef {
            name: SMAA_BLEND_NAMES[i],
            level: LVL_DEFERRED,
            features: Features {
                is_deferred: true,
                ..Default::default()
            },
            defines: smaa_defines(),
            vert: vec!["deferred/SMAABlendWeightsV.glsl", "deferred/SMAA.glsl"],
            frag: vec!["deferred/SMAABlendWeightsF.glsl", "deferred/SMAA.glsl"],
        });

        // gSMAANeighborhoodBlendProgram[i] (:2738-2750)
        v.push(ProgramDef {
            name: SMAA_NEIGHBORHOOD_NAMES[i],
            level: LVL_DEFERRED,
            features: Features {
                is_deferred: true,
                ..Default::default()
            },
            defines: smaa_defines(),
            vert: vec!["deferred/SMAANeighborhoodBlendV.glsl", "deferred/SMAA.glsl"],
            frag: vec!["deferred/SMAANeighborhoodBlendF.glsl", "deferred/SMAA.glsl"],
        });
    }

    // gCASProgram (:2775-2791) -- gate GL>4.05 passes canonically
    v.push(ProgramDef {
        name: "cas",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/CASF.glsl"],
    });

    // gCASLegacyGammaProgram (:2793-2812)
    v.push(ProgramDef {
        name: "cas_legacy_gamma",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![d("GAMMA_CORRECT", "1"), d("LEGACY_GAMMA", "1")],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/CASF.glsl"],
    });

    // gDeferredPostProgram (:2816-2823)
    v.push(ProgramDef {
        name: "deferred_post",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredF.glsl"],
    });

    // gDeferredCoFProgram (:2828-2835)
    v.push(ProgramDef {
        name: "deferred_cof",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/cofF.glsl"],
    });

    // gDeferredDoFCombineProgram (:2840-2847)
    v.push(ProgramDef {
        name: "deferred_dof_combine",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/dofCombineF.glsl"],
    });

    // gDeferredPostNoDoFProgram (:2852-2859)
    v.push(ProgramDef {
        name: "deferred_post_no_dof",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredNoDoFF.glsl"],
    });

    // gOITAccumProgram (:2865-2875) <FS> WBOIT -- interface level, NOT deferred
    // (deferredUtil's `uniform vec3 color` would clash with the vec4 DIFFUSE_COLOR).
    v.push(ProgramDef {
        name: "oit_accum",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["deferred/oitAccumV.glsl"],
        frag: vec!["deferred/oitAccumF.glsl"],
    });

    // gOITCompositeProgram (:2880-2887) <FS> WBOIT
    v.push(ProgramDef {
        name: "oit_composite",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/oitCompositeF.glsl"],
    });

    // gDeferredPostNoDoFNoiseProgram (:2892-2903)
    v.push(ProgramDef {
        name: "deferred_post_no_dof_noise",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![d("HAS_NOISE", "1")],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredNoDoFF.glsl"],
    });

    // gEnvironmentMapProgram (:2908-2924) -- SG_SKY group
    v.push(ProgramDef {
        name: "environment_map",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![d("HAS_HDRI", "1")],
        vert: vec!["deferred/skyV.glsl"],
        frag: vec!["deferred/skyF.glsl"],
    });

    // gDeferredWLSkyProgram (:2929-2944) -- SG_SKY
    v.push(ProgramDef {
        name: "deferred_wl_sky",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/skyV.glsl"],
        frag: vec!["deferred/skyF.glsl"],
    });

    // gDeferredWLCloudProgram (:2949-2965) -- SG_SKY; addConstant CLOUD_MOON_DEPTH
    // is a compile-time define (llglslshader.cpp:74-85,810).
    v.push(ProgramDef {
        name: "deferred_wl_cloud",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![d("LL_SHADER_CONST_CLOUD_MOON_DEPTH", "0.99998")],
        vert: vec!["deferred/cloudsV.glsl"],
        frag: vec!["deferred/cloudsF.glsl"],
    });

    // gDeferredWLSunProgram (:2970-2985) -- SG_SKY
    v.push(ProgramDef {
        name: "deferred_wl_sun",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/sunDiscV.glsl"],
        frag: vec!["deferred/sunDiscF.glsl"],
    });

    // gDeferredWLMoonProgram (:2990-3007) -- SG_SKY; addConstant CLOUD_MOON_DEPTH
    v.push(ProgramDef {
        name: "deferred_wl_moon",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![d("LL_SHADER_CONST_CLOUD_MOON_DEPTH", "0.99998")],
        vert: vec!["deferred/moonV.glsl"],
        frag: vec!["deferred/moonF.glsl"],
    });

    // gDeferredStarProgram (:3012-3023) -- SG_SKY; addConstant STAR_DEPTH
    v.push(ProgramDef {
        name: "deferred_star",
        level: LVL_DEFERRED,
        features: Features::default(),
        defines: vec![d("LL_SHADER_CONST_STAR_DEPTH", "0.99999")],
        vert: vec!["deferred/starsV.glsl"],
        frag: vec!["deferred/starsF.glsl"],
    });

    // gNormalMapGenProgram (:3028-3035) -- SG_SKY
    v.push(ProgramDef {
        name: "normal_map_gen",
        level: LVL_DEFERRED,
        features: Features::default(),
        defines: vec![],
        vert: vec!["deferred/normgenV.glsl"],
        frag: vec!["deferred/normgenF.glsl"],
    });

    // gDeferredGenBrdfLutProgram (:3039-3045)
    v.push(ProgramDef {
        name: "deferred_gen_brdf_lut",
        level: LVL_DEFERRED,
        features: Features::default(),
        defines: vec![],
        vert: vec!["deferred/genbrdflutV.glsl"],
        frag: vec!["deferred/genbrdflutF.glsl"],
    });

    // gPostScreenSpaceReflectionProgram (:3047-3056) -- literal mShaderLevel = 3
    v.push(ProgramDef {
        name: "post_screen_space_reflection",
        level: 3,
        features: Features {
            has_screen_space_reflections: true,
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/screenSpaceReflPostV.glsl"],
        frag: vec!["deferred/screenSpaceReflPostF.glsl"],
    });

    // gDeferredBufferVisualProgram (:3058-3068)
    v.push(ProgramDef {
        name: "deferred_buffer_visual",
        level: LVL_DEFERRED,
        features: Features::default(),
        defines: vec![],
        vert: vec!["deferred/postDeferredNoTCV.glsl"],
        frag: vec!["deferred/postDeferredVisualizeBuffers.glsl"],
    });

    // gRlvSphereProgram (:3070-3079) [RLVa:KB] @setsphere
    v.push(ProgramDef {
        name: "rlv_sphere",
        level: LVL_DEFERRED,
        features: Features {
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/rlvV.glsl"],
        frag: vec!["deferred/rlvF.glsl"],
    });

    // =========================================================================
    // loadShadersObject (:3084-3174)
    // =========================================================================

    // gObjectBumpProgram (+Skinned) (:3091-3097)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "object_bump",
            level: LVL_OBJECT,
            features: Features::default(),
            defines: vec![],
            vert: vec!["objects/bumpV.glsl"],
            frag: vec!["objects/bumpF.glsl"],
        },
        "object_bump_rigged",
    );

    // gObjectAlphaMaskNoColorProgram (:3113-3124)
    v.push(ProgramDef {
        name: "object_alpha_mask_no_color",
        level: LVL_OBJECT,
        features: Features {
            calculates_lighting: true,
            calculates_atmospherics: true,
            has_gamma: true,
            has_atmospherics: true,
            has_lighting: true,
            has_alpha_mask: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["objects/simpleNoColorV.glsl"],
        frag: vec!["objects/simpleF.glsl"],
    });

    // gImpostorProgram (:3129-3135)
    v.push(ProgramDef {
        name: "impostor",
        level: LVL_OBJECT,
        features: Features {
            has_srgb: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["objects/impostorV.glsl"],
        frag: vec!["objects/impostorF.glsl"],
    });

    // gObjectPreviewProgram (+Skinned) (:3140-3149) -- the hasLighting pokes at
    // :3147-3148 happen AFTER createShader (runtime-only); snapshot has no features.
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "object_preview",
            level: LVL_OBJECT,
            features: Features::default(),
            defines: vec![],
            vert: vec!["objects/previewV.glsl"],
            frag: vec!["objects/previewF.glsl"],
        },
        "object_preview_rigged",
    );

    // gPhysicsPreviewProgram (:3153-3164) -- everything explicitly false
    v.push(ProgramDef {
        name: "physics_preview",
        level: LVL_OBJECT,
        features: Features::default(),
        defines: vec![],
        vert: vec!["objects/previewPhysicsV.glsl"],
        frag: vec!["objects/previewPhysicsF.glsl"],
    });

    // =========================================================================
    // loadShadersAvatar (:3176-3237) -- level = mShaderLevel[SHADER_AVATAR] = 1
    // =========================================================================

    // gAvatarProgram (:3191-3203)
    v.push(ProgramDef {
        name: "avatar",
        level: LVL_AVATAR,
        features: Features {
            has_skinning: true,
            calculates_atmospherics: true,
            calculates_lighting: true,
            has_gamma: true,
            has_atmospherics: true,
            has_lighting: true,
            has_alpha_mask: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["avatar/avatarV.glsl"],
        frag: vec!["avatar/avatarF.glsl"],
    });

    // gAvatarEyeballProgram (:3214-3226)
    v.push(ProgramDef {
        name: "avatar_eyeball",
        level: LVL_AVATAR,
        features: Features {
            calculates_lighting: true,
            is_specular: true,
            calculates_atmospherics: true,
            has_gamma: true,
            has_atmospherics: true,
            has_lighting: true,
            has_alpha_mask: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["avatar/eyeballV.glsl"],
        frag: vec!["avatar/eyeballF.glsl"],
    });

    // =========================================================================
    // loadShadersInterface (:3239-3636)
    // =========================================================================

    // gHighlightProgram (+Skinned) (:3246-3253)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "highlight",
            level: LVL_INTERFACE,
            features: Features::default(),
            defines: vec![],
            vert: vec!["interface/highlightV.glsl"],
            frag: vec!["interface/highlightF.glsl"],
        },
        "highlight_rigged",
    );

    // gHighlightNormalProgram (:3257-3263)
    v.push(ProgramDef {
        name: "highlight_normal",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/highlightNormV.glsl"],
        frag: vec!["interface/highlightF.glsl"],
    });

    // gHighlightSpecularProgram (:3267-3273)
    v.push(ProgramDef {
        name: "highlight_specular",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/highlightSpecV.glsl"],
        frag: vec!["interface/highlightF.glsl"],
    });

    // gUIProgram (:3277-3283)
    v.push(ProgramDef {
        name: "ui",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/uiV.glsl"],
        frag: vec!["interface/uiF.glsl"],
    });

    // gPathfindingProgram (:3287-3293)
    v.push(ProgramDef {
        name: "pathfinding",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/pathfindingV.glsl"],
        frag: vec!["interface/pathfindingF.glsl"],
    });

    // gPathfindingNoNormalsProgram (:3297-3303)
    v.push(ProgramDef {
        name: "pathfinding_no_normals",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/pathfindingNoNormalV.glsl"],
        frag: vec!["interface/pathfindingF.glsl"],
    });

    // gGlowCombineProgram (:3307-3320)
    v.push(ProgramDef {
        name: "glow_combine",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/glowcombineV.glsl"],
        frag: vec!["interface/glowcombineF.glsl"],
    });

    // gGlowCombineFXAAProgram (:3324-3337)
    v.push(ProgramDef {
        name: "glow_combine_fxaa",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/glowcombineFXAAV.glsl"],
        frag: vec!["interface/glowcombineFXAAF.glsl"],
    });

    // gTwoTextureCompareProgram (:3342-3355) -- #ifdef LL_WINDOWS (our target)
    v.push(ProgramDef {
        name: "two_texture_compare",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/twotexturecompareV.glsl"],
        frag: vec!["interface/twotexturecompareF.glsl"],
    });

    // gOneTextureFilterProgram (:3359-3370) -- #ifdef LL_WINDOWS
    v.push(ProgramDef {
        name: "one_texture_filter",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/onetexturefilterV.glsl"],
        frag: vec!["interface/onetexturefilterF.glsl"],
    });

    // gSolidColorProgram (:3375-3387)
    v.push(ProgramDef {
        name: "solid_color",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/solidcolorV.glsl"],
        frag: vec!["interface/solidcolorF.glsl"],
    });

    // gOcclusionProgram (:3391-3398)
    v.push(ProgramDef {
        name: "occlusion",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/occlusionV.glsl"],
        frag: vec!["interface/occlusionF.glsl"],
    });

    // gSkinnedOcclusionProgram (:3402-3409) -- hand-written skinned block
    v.push(ProgramDef {
        name: "skinned_occlusion",
        level: LVL_INTERFACE,
        features: Features {
            has_object_skinning: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["interface/occlusionSkinnedV.glsl"],
        frag: vec!["interface/occlusionF.glsl"],
    });

    // gOcclusionCubeProgram (:3413-3419)
    v.push(ProgramDef {
        name: "occlusion_cube",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/occlusionCubeV.glsl"],
        frag: vec!["interface/occlusionF.glsl"],
    });

    // gDebugProgram (+Skinned) (:3423-3431)
    push_with_rigged(
        &mut v,
        ProgramDef {
            name: "debug",
            level: LVL_INTERFACE,
            features: Features::default(),
            defines: vec![],
            vert: vec!["interface/debugV.glsl"],
            frag: vec!["interface/debugF.glsl"],
        },
        "debug_rigged",
    );

    // gNormalDebugProgram[NORMAL_DEBUG_SHADER_COUNT=2] (+Skinned each) (:3433-3458).
    // KNOWN LIMITATION: the C++ also attaches interface/normaldebugG.glsl as a
    // GL_GEOMETRY_SHADER -- ProgramDef has no geometry slot, so the G stage is
    // SKIPPED here (vert+frag only). Variant 1 = NORMAL_DEBUG_SHADER_WITH_TANGENTS.
    for (name, rigged_name, tangents) in [
        ("normal_debug", "normal_debug_rigged", false),
        ("normal_debug_tangents", "normal_debug_tangents_rigged", true),
    ] {
        let mut defines: Vec<(&'static str, String)> = Vec::new();
        if tangents {
            defines.push(d("HAS_ATTRIBUTE_TANGENT", "1"));
        }
        push_with_rigged(
            &mut v,
            ProgramDef {
                name,
                level: LVL_INTERFACE,
                features: Features::default(),
                defines,
                vert: vec!["interface/normaldebugV.glsl"],
                frag: vec!["interface/normaldebugF.glsl"],
            },
            rigged_name,
        );
    }

    // gClipProgram (:3462-3468)
    v.push(ProgramDef {
        name: "clip",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/clipV.glsl"],
        frag: vec!["interface/clipF.glsl"],
    });

    // gBenchmarkProgram (:3472-3478)
    v.push(ProgramDef {
        name: "benchmark",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/benchmarkV.glsl"],
        frag: vec!["interface/benchmarkF.glsl"],
    });

    // gReflectionProbeDisplayProgram (:3482-3494)
    v.push(ProgramDef {
        name: "reflection_probe_display",
        level: LVL_INTERFACE,
        features: Features {
            has_reflection_probes: true,
            has_srgb: true,
            calculates_atmospherics: true,
            has_atmospherics: true,
            has_gamma: true,
            is_deferred: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["interface/reflectionprobeV.glsl"],
        frag: vec!["interface/reflectionprobeF.glsl"],
    });

    // gCopyProgram (:3498-3504)
    v.push(ProgramDef {
        name: "copy",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/copyV.glsl"],
        frag: vec!["interface/copyF.glsl"],
    });

    // gCopyDepthProgram (:3508-3516)
    v.push(ProgramDef {
        name: "copy_depth",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![d("COPY_DEPTH", "1")],
        vert: vec!["interface/copyV.glsl"],
        frag: vec!["interface/copyF.glsl"],
    });

    // gDrawColorProgram (:3520-3527) -- OBJECT level, lives in loadShadersInterface
    v.push(ProgramDef {
        name: "draw_color",
        level: LVL_OBJECT,
        features: Features::default(),
        defines: vec![],
        vert: vec!["objects/simpleNoAtmosV.glsl"],
        frag: vec!["objects/simpleColorF.glsl"],
    });

    // gPBRTerrainBakeProgram (:3529-3556) -- gated on LocalTerrainPaintEnabled
    // (default OFF); transcribed anyway (must compile). TerrainPaintBitDepth default
    // 5 -> TERRAIN_PAINT_PRECISION = (1<<5)-1 = 31.
    v.push(ProgramDef {
        name: "pbr_terrain_bake",
        level: LVL_INTERFACE,
        features: Features {
            is_pbr_terrain: true,
            ..Default::default()
        },
        defines: vec![d("TERRAIN_PAINT_PRECISION", "31")],
        vert: vec!["interface/pbrTerrainBakeV.glsl"],
        frag: vec!["interface/pbrTerrainBakeF.glsl"],
    });

    // gAlphaMaskProgram (:3560-3566)
    v.push(ProgramDef {
        name: "alpha_mask",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/alphamaskV.glsl"],
        frag: vec!["interface/alphamaskF.glsl"],
    });

    // gReflectionMipProgram (:3570-3580)
    v.push(ProgramDef {
        name: "reflection_mip",
        level: LVL_INTERFACE,
        features: Features {
            is_deferred: true,
            has_gamma: true,
            has_atmospherics: true,
            calculates_atmospherics: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["interface/splattexturerectV.glsl"],
        frag: vec!["interface/reflectionmipF.glsl"],
    });

    // gGaussianProgram (:3584-3594) -- C++ reuses the name "Reflection Mip Shader"
    v.push(ProgramDef {
        name: "gaussian",
        level: LVL_INTERFACE,
        features: Features {
            is_deferred: true,
            has_gamma: true,
            has_atmospherics: true,
            calculates_atmospherics: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["interface/splattexturerectV.glsl"],
        frag: vec!["interface/gaussianF.glsl"],
    });

    // gRadianceGenProgram (:3598-3605) -- gate mHasCubeMapArray passes canonically
    v.push(ProgramDef {
        name: "radiance_gen",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![d("PROBE_FILTER_SAMPLES", "32")],
        vert: vec!["interface/radianceGenV.glsl"],
        frag: vec!["interface/radianceGenF.glsl"],
    });

    // gHeroRadianceGenProgram (:3609-3617) -- mirrors OFF canonically, transcribed
    // anyway (must compile)
    v.push(ProgramDef {
        name: "hero_radiance_gen",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![d("HERO_PROBES", "1"), d("PROBE_FILTER_SAMPLES", "4")],
        vert: vec!["interface/radianceGenV.glsl"],
        frag: vec!["interface/radianceGenF.glsl"],
    });

    // gIrradianceGenProgram (:3621-3627)
    v.push(ProgramDef {
        name: "irradiance_gen",
        level: LVL_INTERFACE,
        features: Features::default(),
        defines: vec![],
        vert: vec!["interface/irradianceGenV.glsl"],
        frag: vec!["interface/irradianceGenF.glsl"],
    });

    v
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The table must have unique names (mirrors no_redundant_shaders, :379).
    #[test]
    fn names_unique() {
        let t = programs_full();
        let mut seen = std::collections::HashSet::new();
        for p in &t {
            assert!(seen.insert(p.name), "duplicate program name: {}", p.name);
        }
    }
}
