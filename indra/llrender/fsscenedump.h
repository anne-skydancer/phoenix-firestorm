/**
 * @file fsscenedump.h
 * <FS:VkBridge> P2b -- one-frame scene dump (indra/rust/vkharness PHASE2_PLAN.md).
 *
 * Dev-only, env-gated capture of a single frame's draw stream at the LLVertexBuffer
 * draw sites: vertex/index blobs (hash-deduped), typemask + draw params, current
 * program, bound diffuse texture, modelview/projection, minimal GL state, render
 * target. Consumed by `vkharness replay` (P2c) to re-render the frame and diff it
 * against the GL screenshot -- the bridge parity oracle.
 *
 * Gate: FS_SCENE_DUMP=<output dir> (created if needed). Optional
 * FS_SCENE_DUMP_FRAME=N = in-world frame to capture (default 300, ~5s in-world).
 * Env unset = zero effect; render path untouched.
 */
#ifndef FS_SCENEDUMP_H
#define FS_SCENEDUMP_H

#include "stdtypes.h"

class LLVertexBuffer;
class LLVertexBufferData;

namespace FSSceneDump
{
    // Called once per frame from display(); in_world = startup fully complete.
    void onFrame(bool in_world);
    // Called from the LLVertexBuffer draw entry points; no-op unless capturing.
    void recordDraw(const LLVertexBuffer* vb, U32 mode, U32 count, U32 indices_offset, bool indexed);
    bool active();
    // <FS:VkBridge> P3c live bridge
    bool liveActive();
    // <FS:VkBridge> A.3 native-VK UI: honest feed from LLRender::flush (its OWN matrices + the
    // immediate verts; no glGet, no tap). uiActive() gates the flush hook; uiBegin() clears the
    // engine's per-frame UI list (once per frame); uiSubmit() forwards one flush as a UI draw.
    // `mvp16` = projection*modelview (column-major); `verts` = vtx_count * {vec3 pos, vec2 uv,
    // u8x4 color} = 24B each; `mode` = LLRender::eGeomModes.
    bool uiActive();
    void uiBegin();
    void uiSubmit(const float* mvp16, U32 tex_id, U32 mode, U32 vtx_count, const U8* verts);
    // <FS:VkBridge> UI-complete: the cached-font path (LLFontVertexBuffer) replays glyph geometry
    // via LLVertexBufferData::draw() -> LLVertexBuffer::drawArrays, which NEVER calls LLRender::flush
    // -- so the flush hook structurally cannot see cached text (button labels, editors, list rows,
    // read-only text). This captures that replay from the retained CPU vertex buffer + the CURRENT
    // gGL matrix (the UI ortho renderBuffers set) + the bound atlas id. Called from renderBuffers.
    void uiSubmitBufferData(const LLVertexBufferData& d);
    void endFrame();
    void textureUploaded(U32 id, U32 w, U32 h, const U8* rgba);
    void bufferDirty(const void* ptr);
    // <FS:VkBridge> F2 (gap G13): passes whose draws must NOT reach the engine's single
    // screen pass (shadow cascades, probe faces, water copy/exclusion/haze, bake targets).
    // <FS:VkBridge> F7/F8: draw-class tagging + per-class aux payload. Set by the draw
    // pool that OWNS the pass (clean layering: pools know what they draw; the tap does
    // not reach into newview). CLASS_TERRAIN ships detail0-3+ramp ids and the texgen
    // planes; recordDraw copies whatever is current into each DrawDesc.
    const U32 DRAWCLASS_GENERIC = 0;
    const U32 DRAWCLASS_TERRAIN = 1;
    const U32 DRAWCLASS_WATER = 2;
    const U32 DRAWCLASS_SKY_DOME = 3;
    const U32 DRAWCLASS_SKY_SUN = 4;
    const U32 DRAWCLASS_SKY_MOON = 5;
    const U32 DRAWCLASS_SKY_STARS = 6;
    void setDrawClass(U32 c);
    void setAuxTex(U32 slot, U32 tex_id);        // slot < 8
    void setAuxF4(U32 slot, const F32* v4);      // slot < 8 (vec4 each)
    // KHR_texture_transform (base color) for GLTF/PBR draws: planar-mapped mesh
    // repeats live HERE, not in TEXCOORD0 (llface.cpp:1545 skips CPU baking for gltf)
    // and not in texture_matrix0. packed8 = [sx,sy,rot,_, ox,oy,_,_].
    // A5: authoritative per-batch Blinn-Phong material data (one-shot, consumed by
    // the next recordDraw). Staged by LLDrawPoolMaterials right before its drawRange.
    void setMaterialBatch(U32 diffuse_id, U32 normal_id, U32 spec_id, F32 alpha_cutoff);
    void setForceOpaque(bool on); // fullbright-shiny: shader forces alpha 1
    // C: rigged-mesh joint palette (world-space mat3x4 rows, llvoavatar mGLMp packing).
    void setMatrixPalette(U64 skin_key, U32 joint_count, const F32* glmp12);
    void setCurrentSkin(U64 skin_key);   // palette-cache hits: same skin, no re-upload
    void setMsaa(U32 samples);           // RenderFSAASamples, pushed from newview
    void clearCurrentSkin();
    void setKhrTexTransform(const F32* packed8);
    void clearKhrTexTransform();
    // <FS:VkBridge> P3 (ground-up data bridge): ship the typed camera + EEP sky to the engine
    // (forwarded to fs_render's fsr_scene_* ABI). PLAIN FLOATS only -- llrender must not include
    // newview headers, so newview (which owns LLViewerCamera / LLEnvironment) extracts the data
    // and calls these once per frame. The engine derives its eye-space sun from view * sun_dir.
    void setSceneCamera(const float origin[3], const float at[3], const float up[3],
                        float near_clip, float far_clip, float fov_y, float aspect);
    // <FS:VkBridge> Ground P1: forward the region heightmap (dim*dim floats, mSurfaceZ order) +
    // agent-space origin + metres-per-grid. Called from newview only when the terrain changes.
    void setSceneTerrain(int dim, float meters_per_grid, const float origin[3], const float* heights);
    // <FS:VkBridge> A.2: the FULL WindLight sky param set the engine's fullscreen sky consumes.
    // Plain floats (no LL types) so both llrender and newview can name it; newview fills it from
    // LLSettingsSky and passes it once per frame.
    struct FSSkyParams
    {
        float sun_dir[3];       // REAL world-space sun direction (LLEnvironment::getSunDirection) -- sun disc + scene_light_strength
        float moon_dir[3];      // REAL world-space moon direction (LLEnvironment::getMoonDirection) -- moon disc
        float sun_color[3];
        float moon_color[3];
        float ambient[3];
        float blue_horizon[3];
        float blue_density[3];
        float glow[3];
        float lightnorm[3];     // swizzled clamped light norm -- drives the sky integral
        float max_y;
        float gamma;
        float haze_density;
        float haze_horizon;
        float density_multiplier;
        float cloud_shadow;
        float sun_moon_glow_factor;
        float sun_up_factor;
        int   can_auto_adjust;
        float reflection_probe_ambiance;
    };
    void setSceneSky(const FSSkyParams& p);
    // <FS:VkBridge> S3b: the sky-regime settings subset (gSavedSettings the engine derivation reads).
    void setSceneRegime(int auto_adjust_legacy, float sky_sunlight_scale, float hdr_sky_sunlight_scale,
                        float sky_ambient_scale, float auto_adjust_ambient_scale, float auto_adjust_hdr_scale,
                        float sun_dynamic_range, float tonemap_mix, int tonemap_type, float exposure,
                        int hdr_enabled, int reflection_probes_enabled);
    // <FS:VkBridge> A.2 INVARIANT #1: the REAL Vulkan-adapter capabilities (mirrors fs_render's
    // FsrGpuInfo exactly). Under native-vulkan the feature manager reads the graphics class from
    // device_type here -- the real VkPhysicalDevice -- NOT a benchmark over no-op GL. Returns
    // false if the engine/adapter is unavailable (then the caller keeps its normal path).
    struct FSGpuInfo
    {
        char         renderer[128];
        char         vendor[64];
        char         version[64];
        unsigned int vendor_id;
        unsigned int device_id;
        unsigned int device_type;   // 0 other / 1 integrated / 2 discrete / 3 virtual / 4 cpu
        unsigned int max_texture_2d;
        unsigned int max_varying_vectors;
    };
    bool getEngineGpuInfo(FSGpuInfo& out);
    void suppressPush();
    void suppressPop();
    struct SuppressScope
    {
        SuppressScope() { suppressPush(); }
        ~SuppressScope() { suppressPop(); }
    };
}

#endif
