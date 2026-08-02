/**
 * @file fsscenedump.cpp
 * <FS:VkBridge> P2b one-frame scene dump. See fsscenedump.h.
 */

#include "linden_common.h"
#include "fsscenedump.h"

#include "llvertexbuffer.h"
#include "llglslshader.h"
#include "llshadermgr.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llgl.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp> // P3: glm::lookAt for the typed camera view

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <sys/stat.h>
#ifdef LL_WINDOWS
#include <direct.h>
#endif

namespace FSSceneDump
{

static bool sArmed = false;
static bool sChecked = false;
static bool sActive = false;
static bool sDone = false;
static U32 sInWorldFrames = 0;
static U32 sTargetFrame = 300;
static std::string sDir;
static FILE* sDrawsFile = nullptr;
static U32 sDrawCount = 0;
static U32 sSkippedNoShadow = 0;

// blob dedup: bytes-hash set (written once) + per-frame pointer cache to avoid rehashing
static std::unordered_set<U64> sBlobsWritten;
static std::unordered_map<const void*, U64> sPtrHash;
// diffuse textures seen (GL names), read back at finalize
static std::unordered_set<U32> sTexSeen;

// <FS:VkBridge> P3c LIVE mode: same taps, but each draw is submitted to fs_render
// instead of (or as well as) being written to disk. Mode is decided once: engine mode
// (FS_ENGINE_MODE=1) -> live; FS_SCENE_DUMP set -> dump; neither -> off.
struct FsrDrawDesc
{
    U32 mode, count, offset, indexed;
    U32 typemask, num_verts, vtx_bytes, idx_bytes;
    U32 tex0, depth_test, depth_write, blend;
    F32 mvp[16];
    U32 offsets[16]; // the VIEWER's own mOffsets[] -- never recomputed on the far side
    F32 color[4];    // the bound shader's CURRENT diffuse colour (per-draw, not global)
    U32 tex[4];      // texture units 0..3 -- indexed textures pick per-vertex via position.w
    U32 draw_class;  // F7: 0 = generic, 1 = terrain (engine picks the pipeline)
    U32 tex_ex[8];   // per-class texture ids (terrain: detail0-3, alpha_ramp)
    F32 aux[48];     // per-class data: terrain planes / water payload / sky EEP env
    F32 texmat[16];  // F5: texture_matrix0 -- llSetTextureAnim sheet-flips/scroll/rotate
                     // and large-face planar all ride this; identity in the common case
    F32 khr[8];      // KHR_texture_transform (base color): [sx,sy,rot,_, ox,oy,_,_]
    U32 indexed_ch;  // bound shader's mIndexedTextureChannels -- 0 = position.w is NOT
                     // a texture index (PBR/materials/avatar draws were mis-indexing)
    F32 min_alpha;   // MASK-mode alpha cutoff (-1 = disabled)
    U32 skin_id;     // C: nonzero = rigged draw, positions skin via this palette
    U32 cull;        // wave-6: GL_CULL_FACE state -- cull_mode None shaded every
                     // backface of every skinned vertex (GPU saturation)
    U32 blend_add;   // (SRC_ALPHA, ONE): stars/glow additive blending
    // ---- BLOCKER #2 (deferred foundation): geometry + material plumbing ----
    // Appended (ABI-matched with Rust DrawDesc). Viewer + engine rebuild in lockstep on any
    // change here; append-only so an old engine safely ignores the tail.
    F32 modelview[16];     // #2a: GL modelview (view*model) -> normal matrix + eye-space pos
    U32 material_model;    // #2b: 0 simple/generic, 1 legacy-material, 2 PBR
    F32 pbr[4];            // #2b: metallic, roughness, emissive_scale, env_intensity
    F32 emissive_color[4]; // #2b: emissive rgb + _
    U32 orm_tex;           // #2b: occlusion-roughness-metallic texture id
    U32 emissive_tex;      // #2b: emissive texture id
    U32 flags2;            // #4: bit0 = UI pass (bypass tonemap); else reserved
};

// <FS:VkBridge> P3 (ground-up data bridge): #[repr(C)] mirrors of scene::CameraBlock /
// scene::EepSkyBlock (fs_render/src/scene.rs). Field order + types MUST match the Rust exactly.
struct FsrCameraBlock
{
    F32 view[16];
    F32 proj[16];
    F32 origin[3];
    F32 near_clip; // 'near'/'far' are Windows macros -- must not name the fields that
    F32 far_clip;
    F32 fov_y;
    F32 aspect;
    F32 viewport_w;
    F32 viewport_h;
};
struct FsrEepSkyBlock
{
    F32 sun_dir[3];
    F32 sun_color[3];
    F32 moon_dir[3];
    F32 moon_color[3];
    F32 ambient[3];
    F32 blue_horizon[3];
    F32 blue_density[3];
    F32 haze_density;
    F32 haze_horizon;
    F32 density_multiplier;
    F32 distance_multiplier;
    F32 max_y;
    F32 gamma;
    F32 glow[3];
    F32 cloud_shadow;
    F32 sun_moon_glow_factor;
    F32 sun_up_factor;
    F32 sky_hdr_scale;
    F32 cloud_color[3];
    F32 cloud_pos_density1[3];
    F32 cloud_pos_density2[3];
    F32 cloud_scale;
    F32 cloud_variance;
    F32 star_brightness;
    F32 sun_scale;
    F32 moon_scale;
    F32 moon_brightness;
    F32 moisture_level;
    F32 droplet_radius;
    F32 ice_level;
    // <FS:VkBridge> S3 regime discriminators (must match scene.rs EepSkyBlock tail exactly).
    U32 can_auto_adjust;           // psky->canAutoAdjust()
    F32 reflection_probe_ambiance; // psky->getReflectionProbeAmbiance() (raw)
    F32 lightnorm[3];              // getClampedLightNorm swizzle (populated for the resolve, S4)
};
// <FS:VkBridge> S3b: the sky-regime settings subset (matches fs_render RegimeSettings).
struct FsrRegimeSettings
{
    U32 sky_auto_adjust_legacy;
    F32 sky_sunlight_scale;
    F32 hdr_sky_sunlight_scale;
    F32 sky_ambient_scale;
    F32 sky_auto_adjust_ambient_scale;
    F32 sky_auto_adjust_hdr_scale;
    F32 sun_dynamic_range;
    F32 tonemap_mix;
    U32 tonemap_type;
    F32 exposure;
    U32 hdr_enabled;
    U32 reflection_probes_enabled;
};
typedef int(__cdecl* fsr_begin_t)();
typedef int(__cdecl* fsr_submit_t)(const FsrDrawDesc*, const U8*, const U8*);
typedef int(__cdecl* fsr_end_t)();
typedef int(__cdecl* fsr_texup_t)(U32, U32, U32, const U8*);
typedef int(__cdecl* fsr_dirty_t)(size_t);
static fsr_begin_t sFsrBegin = nullptr;
static fsr_submit_t sFsrSubmit = nullptr;
static fsr_end_t sFsrEnd = nullptr;
static fsr_texup_t sFsrTexUpload = nullptr;
static fsr_dirty_t sFsrDirty = nullptr;
typedef int(__cdecl* fsr_msaa_t)(U32);
static fsr_msaa_t sFsrMsaa = nullptr;
static U32 sLastMsaa = 0xFFFFFFFF;
static U32 sPendingMsaa = 1;
static bool sLive = false;
// <FS:VkBridge> A.2: native-vulkan is a PARALLEL renderer -- the legacy GL-draw tap is RETIRED
// (the engine renders from the typed scene: sky now, world/UI in later phases). The frame loop
// (begin/end/present) + the typed sky/camera feed still run. Set FS_TAP_DRAWS=1 to re-arm the
// draw tap for debugging/A-B comparison against the parallel path.
static bool sTapDraws = false;
static S32 sSuppressDepth = 0; // F2: >0 while an offscreen pass renders
static U32 sDrawClass = 0;     // F7: current draw class (set by the owning pool)
static U32 sAuxTex[8] = { 0 };
static F32 sAuxF[32] = { 0 };
static F32 sKhr[8] = { 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f }; // identity KHR transform
static U32 sMatTex[3] = { 0, 0, 0 };   // A5: staged diffuse/normal/spec (one-shot)
static F32 sMatCutoff = -1.f;
static bool sMatStaged = false;
static bool sForceOpaque = false; // scoped: fullbright-shiny pass

// C: skin-key interning + per-frame palette forwarding to the engine
typedef int(__cdecl* fsr_palette_t)(U32, U32, const F32*);
static fsr_palette_t sFsrPalette = nullptr;
// <FS:VkBridge> P3 typed scene bridge
typedef int(__cdecl* fsr_scene_begin_t)();
typedef int(__cdecl* fsr_scene_camera_t)(const FsrCameraBlock*);
typedef int(__cdecl* fsr_scene_sky_t)(const FsrEepSkyBlock*);
typedef int(__cdecl* fsr_scene_regime_t)(const FsrRegimeSettings*);
static fsr_scene_begin_t sFsrSceneBegin = nullptr;
static fsr_scene_camera_t sFsrSceneCamera = nullptr;
static fsr_scene_sky_t sFsrSceneSky = nullptr;
static fsr_scene_regime_t sFsrSceneRegime = nullptr;
// <FS:VkBridge> A.3 native-VK UI feed (honest LLRender::flush -> engine, no glGet, no tap)
typedef int(__cdecl* fsr_ui_begin_t)();
typedef int(__cdecl* fsr_ui_submit_t)(const float*, U32, U32, U32, const U8*);
static fsr_ui_begin_t sFsrUiBegin = nullptr;
static fsr_ui_submit_t sFsrUiSubmit = nullptr;
static std::unordered_map<U64, U32> sSkinIds;
static std::unordered_map<U32, U32> sSkinSentFrame;
static U32 sNextSkinId = 1;
static U32 sCurrentSkinId = 0;
static U64 sSubmitTicks = 0;   // perf: QPC ticks spent inside fsr_submit this frame
static U32 sLiveDraws = 0;
static U32 sLiveFrames = 0;   // increments every frame in live mode (login screen included)
static U32 sLiveNoShadow = 0; // taps hit but no CPU shadow -> cannot submit

static void bindLive()
{
    HMODULE dll = GetModuleHandleA("fs_render.dll"); // already loaded by swapBuffers
    if (!dll)
    {
        dll = LoadLibraryA("fs_render.dll");
    }
    if (!dll)
    {
        LL_WARNS("SceneDump") << "P3c live: fs_render.dll not loadable" << LL_ENDL;
        return;
    }
    sFsrBegin = (fsr_begin_t)GetProcAddress(dll, "fsr_begin_frame");
    sFsrSubmit = (fsr_submit_t)GetProcAddress(dll, "fsr_submit");
    sFsrEnd = (fsr_end_t)GetProcAddress(dll, "fsr_end_frame");
    sFsrTexUpload = (fsr_texup_t)GetProcAddress(dll, "fsr_texture_upload");
    sFsrDirty = (fsr_dirty_t)GetProcAddress(dll, "fsr_buffer_dirty");
    sFsrPalette = (fsr_palette_t)GetProcAddress(dll, "fsr_set_matrix_palette");
    sFsrMsaa = (fsr_msaa_t)GetProcAddress(dll, "fsr_set_msaa");
    sFsrSceneBegin = (fsr_scene_begin_t)GetProcAddress(dll, "fsr_scene_begin");
    sFsrSceneCamera = (fsr_scene_camera_t)GetProcAddress(dll, "fsr_scene_set_camera");
    sFsrSceneSky = (fsr_scene_sky_t)GetProcAddress(dll, "fsr_scene_set_sky");
    sFsrSceneRegime = (fsr_scene_regime_t)GetProcAddress(dll, "fsr_scene_set_regime");
    sFsrUiBegin = (fsr_ui_begin_t)GetProcAddress(dll, "fsr_ui_begin");
    sFsrUiSubmit = (fsr_ui_submit_t)GetProcAddress(dll, "fsr_ui_submit");
    sLive = (sFsrBegin && sFsrSubmit && sFsrEnd);
    sTapDraws = (getenv("FS_TAP_DRAWS") != nullptr); // native-vulkan: draw tap retired unless debugging
    LL_INFOS("SceneDump") << "P3c live bridge " << (sLive ? "ARMED" : "FAILED")
                          << " (begin=" << (void*)sFsrBegin << " submit=" << (void*)sFsrSubmit
                          << " end=" << (void*)sFsrEnd << ")" << LL_ENDL;
}

bool liveActive() { return sLive; }

// <FS:VkBridge> A.3: the native-VK UI feed is active when the engine is live and the legacy draw
// tap is retired (its normal native-vulkan state). With FS_TAP_DRAWS on (debug), the tap already
// renders the UI, so the feed stands down to avoid double-drawing.
bool uiActive() { return sLive && !sTapDraws && sFsrUiSubmit != nullptr; }
void uiBegin() { if (sLive && sFsrUiBegin) sFsrUiBegin(); }
void uiSubmit(const float* mvp16, U32 tex_id, U32 mode, U32 vtx_count, const U8* verts)
{
    if (sFsrUiSubmit) sFsrUiSubmit(mvp16, tex_id, mode, vtx_count, verts);
}

void textureUploaded(U32 id, U32 w, U32 h, const U8* rgba)
{
    if (sLive && sFsrTexUpload && rgba && w && h)
    {
        sFsrTexUpload(id, w, h, rgba);
    }
}

// The viewer just wrote into a CPU-shadow buffer: tell the engine to drop its cached
// GPU copy so it re-uploads exactly once (instead of every buffer, every frame).
void bufferDirty(const void* ptr)
{
    if (sLive && sFsrDirty && ptr)
    {
        sFsrDirty((size_t)ptr);
    }
}

void setDrawClass(U32 c)
{
    sDrawClass = c;
    if (c == DRAWCLASS_GENERIC)
    {
        memset(sAuxTex, 0, sizeof(sAuxTex));
        memset(sAuxF, 0, sizeof(sAuxF));
    }
}

void setAuxTex(U32 slot, U32 tex_id)
{
    if (slot < 8)
    {
        sAuxTex[slot] = tex_id;
    }
}

void setAuxF4(U32 slot, const F32* v4)
{
    if (slot < 8 && v4)
    {
        memcpy(&sAuxF[slot * 4], v4, 4 * sizeof(F32));
    }
}

void setMatrixPalette(U64 skin_key, U32 joint_count, const F32* glmp12)
{
    if (!sLive || !glmp12 || joint_count == 0)
    {
        return;
    }
    U32 id;
    auto it = sSkinIds.find(skin_key);
    if (it == sSkinIds.end())
    {
        id = sNextSkinId++;
        sSkinIds[skin_key] = id;
    }
    else
    {
        id = it->second;
    }
    sCurrentSkinId = id;
    if (sFsrPalette)
    {
        auto sent = sSkinSentFrame.find(id);
        if (sent == sSkinSentFrame.end() || sent->second != sLiveFrames)
        {
            sSkinSentFrame[id] = sLiveFrames;
            sFsrPalette(id, joint_count, glmp12);
        }
    }
}

void setForceOpaque(bool on)
{
    sForceOpaque = on;
}

void setMsaa(U32 samples)
{
    sPendingMsaa = samples;
}

void setCurrentSkin(U64 skin_key)
{
    auto it = sSkinIds.find(skin_key);
    sCurrentSkinId = (it != sSkinIds.end()) ? it->second : 0;
}

void clearCurrentSkin()
{
    sCurrentSkinId = 0;
}

void setMaterialBatch(U32 diffuse_id, U32 normal_id, U32 spec_id, F32 alpha_cutoff)
{
    sMatTex[0] = diffuse_id;
    sMatTex[1] = normal_id;
    sMatTex[2] = spec_id;
    sMatCutoff = alpha_cutoff;
    sMatStaged = true;
}

void setKhrTexTransform(const F32* packed8)
{
    if (packed8)
    {
        memcpy(sKhr, packed8, sizeof(sKhr));
    }
}

void clearKhrTexTransform()
{
    sKhr[0] = 1.f; sKhr[1] = 1.f; sKhr[2] = 0.f; sKhr[3] = 0.f;
    sKhr[4] = 0.f; sKhr[5] = 0.f; sKhr[6] = 0.f; sKhr[7] = 0.f;
}

void suppressPush()
{
    ++sSuppressDepth;
}

void suppressPop()
{
    if (sSuppressDepth > 0)
    {
        --sSuppressDepth;
    }
}

void endFrame()
{
    if (sLive && sFsrEnd)
    {
        sFsrEnd();
    }
}

bool active() { return sActive; }

static U64 fnv1a(const U8* p, size_t n)
{
    U64 h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void write_blob(const char* prefix, U64 hash, const U8* p, size_t n)
{
    if (sBlobsWritten.count(hash))
    {
        return;
    }
    sBlobsWritten.insert(hash);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s_%016llx.bin", sDir.c_str(), prefix, (unsigned long long)hash);
    if (FILE* f = fopen(path, "wb"))
    {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}

static U64 blob_hash(const U8* p, size_t n)
{
    auto it = sPtrHash.find(p);
    if (it != sPtrHash.end())
    {
        return it->second;
    }
    U64 h = fnv1a(p, n);
    sPtrHash[p] = h;
    return h;
}

void recordDraw(const LLVertexBuffer* vb, U32 mode, U32 count, U32 indices_offset, bool indexed)
{
    // One-shot staging is retired HERE, unconditionally: a suppressed draw (shadow/
    // probe/bake) that staged material or KHR data must not leak it onto the next
    // unsuppressed draw of an unrelated pool (measured: one corrupted world draw per
    // leaked staging, including across frame boundaries via probe updates).
    bool mat_staged = sMatStaged;
    sMatStaged = false;
    U32 mat_tex0 = sMatTex[0];
    U32 mat_tex1 = sMatTex[1];
    U32 mat_tex2 = sMatTex[2];
    F32 mat_cutoff = sMatCutoff;
    F32 khr_local[8];
    memcpy(khr_local, sKhr, sizeof(khr_local));
    clearKhrTexTransform();
    // <FS:VkBridge> P3c live path: submit straight to the engine.
    // F2: offscreen passes (shadow/probe/water-copy/exclusion/haze/bakes) are suppressed --
    // replayed into the single screen pass they painted OVER the frame (the transparent-
    // water corruption) and tripled submit volume.
    // meshshape-1: glow passes write ALPHA ONLY (colorMask(false,true)); replaying
    // them as full-color draws repainted/whited the faces underneath.
    GLint fs_cmask[4] = { 1, 1, 1, 1 };
    if (sLive && sTapDraws)
    {
        glGetIntegerv(0x0C23 /*GL_COLOR_WRITEMASK*/, fs_cmask);
    }
    if (sLive && sTapDraws && sSuppressDepth == 0 && vb && (fs_cmask[0] || fs_cmask[1] || fs_cmask[2]))
    {
        const U8* vdata = vb->getMappedData();
        const U8* idata = vb->getMappedIndices();
        if (!vdata || (indexed && !idata))
        {
            ++sLiveNoShadow;
            static U32 s_diag = 0;
            if (s_diag < 8)
            {
                ++s_diag;
                LL_INFOS("SceneDump") << "P3c skip #" << s_diag << ": vdata=" << (const void*)vdata
                                      << " idata=" << (const void*)idata << " indexed=" << (int)indexed
                                      << " mode=" << mode << " count=" << count
                                      << " typemask=" << vb->getTypeMask()
                                      << " nverts=" << vb->getNumVerts()
                                      << " size=" << vb->getSize()
                                      << LL_ENDL;
            }
        }
        if (vdata && (!indexed || idata))
        {
            FsrDrawDesc d;
            d.mode = mode;
            d.count = count;
            d.offset = indices_offset;
            d.indexed = indexed ? 1u : 0u;
            d.typemask = vb->getTypeMask();
            d.num_verts = vb->getNumVerts();
            d.vtx_bytes = vb->getSize();
            d.idx_bytes = indexed ? vb->getIndicesSize() : 0u;
            d.tex0 = gGL.getTexUnit(0) ? gGL.getTexUnit(0)->getCurrTexture() : 0u;
            // sIndexedTextureChannels == 4: world batches bind four textures and select
            // per vertex from position.w (llvertexbuffer.cpp:1133). Capture all four.
            // The diffuse sampler array is NOT guaranteed on units 0-3: channels are
            // assigned per-program at link time, and a settings change swaps programs
            // (measured: paving turned camo -- non-diffuse maps sampled as color).
            // Resolve the shader's actual diffuseMap base channel.
            U32 tex_base = 0;
            if (LLGLSLShader* shd = LLGLSLShader::sCurBoundShaderPtr)
            {
                // INDEXED programs bind their diffuse array to units 0..N-1 by
                // construction (post-link override, llglslshader.cpp:509-513) -- and
                // their mTexture[DIFFUSE_MAP] is POISONED under the stub: the raw-text
                // uniform scan sees the preprocessor-dead "uniform sampler2D diffuseMap"
                // in fullbright shaders, a channel gets assigned, and the indexed
                // override shifts it +4 -> capture read empty units 4..7 -> every HUD
                // button rendered the white fallback (the Slab-A regression).
                if (shd->mFeatures.mIndexedTextureChannels <= 0
                    && (size_t)LLShaderMgr::DIFFUSE_MAP < shd->mTexture.size())
                {
                    S32 ch = shd->mTexture[LLShaderMgr::DIFFUSE_MAP];
                    if (ch >= 0)
                    {
                        tex_base = (U32)ch;
                    }
                }
            }
            for (U32 ti = 0; ti < 4; ++ti)
            {
                LLTexUnit* tu = gGL.getTexUnit(tex_base + ti);
                d.tex[ti] = tu ? tu->getCurrTexture() : 0u;
            }
            d.depth_test = glIsEnabled(GL_DEPTH_TEST) ? 1u : 0u;
            GLint dm = 0;
            glGetIntegerv(GL_DEPTH_WRITEMASK, &dm);
            d.depth_write = dm ? 1u : 0u;
            d.blend = glIsEnabled(GL_BLEND) ? 1u : 0u;
            if (sForceOpaque)
            {
                d.blend = 0u; // engine's blend-off path = alpha 1, matching the shader
            }
            // Diffuse colour, read from the bound shader's own cached uniform value.
            // LLRender::color4ub() sends UI colour either to a vertex attribute or to
            // this uniform (llrender.cpp:2090) depending on the shader's attribute mask,
            // and LLGLSLShader::uniform4f only calls GL when the value CHANGES -- so a
            // global "current colour" in the engine goes stale across program switches
            // (the lavender wash / grey background). Read it per draw instead.
            d.color[0] = d.color[1] = d.color[2] = d.color[3] = 1.f;
            if (LLGLSLShader* sh = LLGLSLShader::sCurBoundShaderPtr)
            {
                if ((U32)LLShaderMgr::DIFFUSE_COLOR < sh->mUniform.size())
                {
                    GLint loc = sh->mUniform[LLShaderMgr::DIFFUSE_COLOR];
                    if (loc >= 0)
                    {
                        auto it = sh->mValue.find(loc);
                        if (it != sh->mValue.end())
                        {
                            d.color[0] = it->second.mV[0];
                            d.color[1] = it->second.mV[1];
                            d.color[2] = it->second.mV[2];
                            d.color[3] = it->second.mV[3];
                        }
                    }
                }
            }
            // Ship the viewer's authoritative attribute offsets. Recomputing calcOffsets
            // on the engine side is a replica that can silently disagree; getOffset() is
            // the source of truth the viewer itself writes data with (setColorData etc).
            for (U32 t = 0; t < 16; ++t)
            {
                d.offsets[t] = (t < LLVertexBuffer::TYPE_MAX) ? vb->getOffset((LLVertexBuffer::AttributeType)t) : 0u;
            }
            // MEASUREMENT (in-world): what do draws actually carry? shader name, whether it
            // uses indexed textures, the real position.w of vertex 0, and the 4 texture units.
            {
                static U32 s_iw = 0;
                if (sInWorldFrames > 60 && s_iw < 14)
                {
                    ++s_iw;
                    LLGLSLShader* shx = LLGLSLShader::sCurBoundShaderPtr;
                    const F32* vp0 = (const F32*)(vdata + d.offsets[LLVertexBuffer::TYPE_VERTEX]);
                    LL_INFOS("SceneDump") << "P3c IW#" << s_iw
                        << " prog=" << (shx ? shx->mName : std::string("none"))
                        << " idxch=" << (shx ? shx->mFeatures.mIndexedTextureChannels : -1)
                        << " posw=" << vp0[3]
                        << " tex=" << (gGL.getTexUnit(0) ? gGL.getTexUnit(0)->getCurrTexture() : 0)
                        << "," << (gGL.getTexUnit(1) ? gGL.getTexUnit(1)->getCurrTexture() : 0)
                        << "," << (gGL.getTexUnit(2) ? gGL.getTexUnit(2)->getCurrTexture() : 0)
                        << "," << (gGL.getTexUnit(3) ? gGL.getTexUnit(3)->getCurrTexture() : 0)
                        << " typemask=" << d.typemask << " nverts=" << d.num_verts
                        << LL_ENDL;
                }
            }
            {
                static U32 s_once = 0;
                if (s_once < 4)
                {
                    ++s_once;
                    const U8* cp = vdata + d.offsets[LLVertexBuffer::TYPE_COLOR];
                    const F32* vp = (const F32*)(vdata + d.offsets[LLVertexBuffer::TYPE_VERTEX]);
                    const F32* tp = (const F32*)(vdata + d.offsets[LLVertexBuffer::TYPE_TEXCOORD0]);
                    LL_INFOS("SceneDump") << "P3c bytes #" << (s_once + 1)
                        << ": color[0]=(" << (int)cp[0] << "," << (int)cp[1] << "," << (int)cp[2] << "," << (int)cp[3] << ")"
                        << " color[1]=(" << (int)cp[4] << "," << (int)cp[5] << "," << (int)cp[6] << "," << (int)cp[7] << ")"
                        << " pos0=(" << vp[0] << "," << vp[1] << "," << vp[2] << "," << vp[3] << ")"
                        << " uv0=(" << tp[0] << "," << tp[1] << ")"
                        << " tex0=" << d.tex0 << LL_ENDL;
                    LL_INFOS("SceneDump") << "P3c layout #" << s_once << ": typemask=" << d.typemask
                                          << " nverts=" << d.num_verts << " size=" << d.vtx_bytes
                                          << " off[VERTEX]=" << d.offsets[LLVertexBuffer::TYPE_VERTEX]
                                          << " off[TC0]=" << d.offsets[LLVertexBuffer::TYPE_TEXCOORD0]
                                          << " off[COLOR]=" << d.offsets[LLVertexBuffer::TYPE_COLOR]
                                          << LL_ENDL;
                }
            }
            // mvp from LLRender's CURRENT matrix stack -- NOT gGLModelView/gGLProjection.
            // Those globals hold the world camera; UI draws run under LLRender's ortho
            // stack, so using the globals threw every UI quad off-screen (measured:
            // 34/34 draws rendered, nothing visible). gGL's accessors are correct for
            // world and UI alike -- they are what syncMatrices() feeds the real shaders.
            {
                const glm::mat4 mvp = gGL.getProjectionMatrix() * gGL.getModelviewMatrix();
                memcpy(d.mvp, glm::value_ptr(mvp), sizeof(d.mvp));
                // #2a: ship the modelview alone (view*model) so the engine can build the
                // normal matrix (inverse-transpose of its upper 3x3) and eye-space positions.
                // Same LLRender stack as mvp -- correct for world and UI alike.
                memcpy(d.modelview, glm::value_ptr(gGL.getModelviewMatrix()), sizeof(d.modelview));
            }
            // #2b material fields: reserved/zeroed until the PBR + legacy-material path lands.
            d.material_model = 0u;
            d.pbr[0] = d.pbr[1] = d.pbr[2] = d.pbr[3] = 0.f;
            d.emissive_color[0] = d.emissive_color[1] = d.emissive_color[2] = d.emissive_color[3] = 0.f;
            d.orm_tex = 0u;
            d.emissive_tex = 0u;
            d.flags2 = 0u;
            d.draw_class = sDrawClass;
            memcpy(d.tex_ex, sAuxTex, sizeof(d.tex_ex));
            memset(d.aux, 0, sizeof(d.aux));
            memcpy(d.aux, sAuxF, sizeof(sAuxF));
            memcpy(d.texmat, glm::value_ptr(gGL.getTextureMatrix0()), sizeof(d.texmat));
            memcpy(d.khr, khr_local, sizeof(d.khr));
            // C: MAP_WEIGHT4 (bit 10) marks rigged draws; the typemask gate makes the
            // sticky skin id safe for every other draw.
            d.skin_id = (d.typemask & (1u << 10)) ? sCurrentSkinId : 0u;
            d.cull = glIsEnabled(GL_CULL_FACE) ? 1u : 0u;
            // stars/glow draw ADD_WITH_ALPHA; gGL's CPU blend cache is always current
            d.blend_add = (gGL.getCurrBlendSFactor() == LLRender::BF_SOURCE_ALPHA
                        && gGL.getCurrBlendDFactor() == LLRender::BF_ONE) ? 1u : 0u;
            // SKY: pack the EEP env from the bound shader's mValue cache -- the sky's
            // color IS this data (the dome has no texture at all).
            if (sDrawClass >= DRAWCLASS_SKY_DOME && sDrawClass <= DRAWCLASS_SKY_STARS)
            {
                if (LLGLSLShader* sky = LLGLSLShader::sCurBoundShaderPtr)
                {
                    struct EnvSlot { U32 vec; S32 uni; U32 wuni; };
                    static const EnvSlot slots[] = {
                        { 0, LLShaderMgr::WL_CAMPOSLOCAL, LLShaderMgr::MAX_Y },
                        { 1, LLShaderMgr::LIGHTNORM, LLShaderMgr::SUN_UP_FACTOR },
                        { 2, LLShaderMgr::SUNLIGHT_COLOR, LLShaderMgr::DENSITY_MULTIPLIER },
                        { 3, LLShaderMgr::MOONLIGHT_COLOR, LLShaderMgr::SUN_MOON_GLOW_FACTOR },
                        { 4, LLShaderMgr::AMBIENT, LLShaderMgr::HAZE_DENSITY },
                        { 5, LLShaderMgr::BLUE_HORIZON, LLShaderMgr::HAZE_HORIZON },
                        { 6, LLShaderMgr::BLUE_DENSITY, LLShaderMgr::CLOUD_SHADOW },
                        { 7, LLShaderMgr::GLOW, LLShaderMgr::BLEND_FACTOR },
                    };
                    for (const EnvSlot& es : slots)
                    {
                        LLVector4 v(0, 0, 0, 0);
                        LLVector4 wv(0, 0, 0, 0);
                        if ((size_t)es.uni < sky->mUniform.size())
                        {
                            GLint loc = sky->mUniform[es.uni];
                            auto it = (loc >= 0) ? sky->mValue.find(loc) : sky->mValue.end();
                            if (loc >= 0 && it != sky->mValue.end()) v = it->second;
                        }
                        if ((size_t)es.wuni < sky->mUniform.size())
                        {
                            GLint loc = sky->mUniform[es.wuni];
                            auto it = (loc >= 0) ? sky->mValue.find(loc) : sky->mValue.end();
                            if (loc >= 0 && it != sky->mValue.end()) wv = it->second;
                        }
                        d.aux[es.vec * 4 + 0] = v.mV[0];
                        d.aux[es.vec * 4 + 1] = v.mV[1];
                        d.aux[es.vec * 4 + 2] = v.mV[2];
                        d.aux[es.vec * 4 + 3] = wv.mV[0];
                    }
                    if (sDrawClass == DRAWCLASS_SKY_DOME)
                    {
                        d.tex[0] = 0; d.tex[1] = 0; d.tex[2] = 0; d.tex[3] = 0;
                        // sky_hdr_scale (EEP-driven, softenLightF SKIP_ATMOS multiply) --
                        // WITHOUT this + srgb_to_linear the tonemapper input is wrong-
                        // colorspace and ~2x too bright -> dome blows to white for every
                        // preset. a8.x = aux[32].
                        F32 hdr = 1.f;
                        if ((size_t)LLShaderMgr::SKY_HDR_SCALE < sky->mUniform.size())
                        {
                            GLint loc = sky->mUniform[LLShaderMgr::SKY_HDR_SCALE];
                            auto it = (loc >= 0) ? sky->mValue.find(loc) : sky->mValue.end();
                            if (loc >= 0 && it != sky->mValue.end()) hdr = it->second.mV[0];
                        }
                        d.aux[32] = (hdr > 0.f) ? hdr : 1.f;
                    }
                    if (sDrawClass == DRAWCLASS_SKY_MOON)
                    {
                        // moonF = tex.rgb * moon_brightness: fold into the color multiplier
                        if ((size_t)LLShaderMgr::MOON_BRIGHTNESS < sky->mUniform.size())
                        {
                            GLint loc = sky->mUniform[LLShaderMgr::MOON_BRIGHTNESS];
                            auto it = (loc >= 0) ? sky->mValue.find(loc) : sky->mValue.end();
                            if (loc >= 0 && it != sky->mValue.end())
                            {
                                F32 b = it->second.mV[0];
                                d.color[0] = b; d.color[1] = b; d.color[2] = b;
                            }
                        }
                    }
                }
            }
            // A2: only indexed programs interpret position.w as a texture index.
            d.indexed_ch = 0;
            // A4: MASK cutoff, read the DIFFUSE_COLOR way (cached wrapper uniforms).
            d.min_alpha = -1.f;
            if (LLGLSLShader* shi = LLGLSLShader::sCurBoundShaderPtr)
            {
                d.indexed_ch = (U32)llmax(0, (S32)shi->mFeatures.mIndexedTextureChannels);
                if ((size_t)LLShaderMgr::MINIMUM_ALPHA < shi->mUniform.size())
                {
                    GLint mloc = shi->mUniform[LLShaderMgr::MINIMUM_ALPHA];
                    if (mloc >= 0)
                    {
                        auto mit = shi->mValue.find(mloc);
                        if (mit != shi->mValue.end())
                        {
                            d.min_alpha = mit->second.mV[0];
                        }
                    }
                }
            }
            // A5: authoritative material batch (one-shot). The materials pool sets its
            // per-batch uniforms with RAW glUniform calls (nothing in mValue), so the
            // staged values are the only correct source for these draws.
            if (mat_staged && mat_tex0 != 0)
            {
                d.tex[0] = mat_tex0;
                d.tex[1] = 0;
                d.tex[2] = 0;
                d.tex[3] = 0;
                d.tex_ex[5] = mat_tex1; // normal (future lit path; slots 0-4 = terrain)
                d.tex_ex[6] = mat_tex2; // specular
                if (mat_cutoff >= 0.f)
                {
                    d.min_alpha = mat_cutoff;
                }
            }
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            sFsrSubmit(&d, vdata, indexed ? idata : nullptr);
            QueryPerformanceCounter(&t1);
            sSubmitTicks += (U64)(t1.QuadPart - t0.QuadPart);
            ++sLiveDraws;
        }
    }

    if (!sActive || !vb || !sDrawsFile)
    {
        return;
    }
    // CPU shadow of the buffer contents (retained in this codebase); skip + count if absent
    const U8* vdata = vb->getMappedData();
    const U8* idata = vb->getMappedIndices();
    if (!vdata || (indexed && !idata))
    {
        ++sSkippedNoShadow;
        return;
    }
    U64 vh = blob_hash(vdata, vb->getSize());
    write_blob("vb", vh, vdata, vb->getSize());
    U64 ih = 0;
    if (indexed)
    {
        ih = blob_hash(idata, vb->getIndicesSize());
        write_blob("ib", ih, idata, vb->getIndicesSize());
    }

    const char* prog = LLGLSLShader::sCurBoundShaderPtr ? LLGLSLShader::sCurBoundShaderPtr->mName.c_str() : "fixed_function";
    U32 tex0 = gGL.getTexUnit(0) ? gGL.getTexUnit(0)->getCurrTexture() : 0;
    if (tex0)
    {
        sTexSeen.insert(tex0);
    }
    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);

    GLboolean depth_test = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean cull = glIsEnabled(GL_CULL_FACE);
    GLint depth_mask = 0;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &depth_mask);

    fprintf(sDrawsFile,
        "{\"mode\":%u,\"count\":%u,\"offset\":%u,\"indexed\":%d,"
        "\"typemask\":%u,\"num_verts\":%u,\"vb\":\"%016llx\",\"ib\":\"%016llx\","
        "\"prog\":\"%s\",\"tex0\":%u,\"fbo\":%d,"
        "\"depth_test\":%d,\"depth_write\":%d,\"blend\":%d,\"cull\":%d,"
        "\"mv\":[",
        mode, count, indices_offset, indexed ? 1 : 0,
        vb->getTypeMask(), vb->getNumVerts(), (unsigned long long)vh, (unsigned long long)ih,
        prog, tex0, fbo,
        (int)depth_test, (int)depth_mask, (int)blend, (int)cull);
    for (int i = 0; i < 16; ++i)
    {
        fprintf(sDrawsFile, "%s%.9g", i ? "," : "", gGLModelView[i]);
    }
    fprintf(sDrawsFile, "],\"proj\":[");
    for (int i = 0; i < 16; ++i)
    {
        fprintf(sDrawsFile, "%s%.9g", i ? "," : "", gGLProjection[i]);
    }
    fprintf(sDrawsFile, "]}\n");
    ++sDrawCount;
}

static void finalize()
{
    sActive = false;
    sDone = true;
    if (sDrawsFile)
    {
        fclose(sDrawsFile);
        sDrawsFile = nullptr;
    }
    // read back the diffuse textures seen this frame (2D only), RGBA8
    char path[1024];
    snprintf(path, sizeof(path), "%s/textures.json", sDir.c_str());
    FILE* tj = fopen(path, "w");
    U32 tex_written = 0;
    if (tj)
    {
        fprintf(tj, "{");
        bool first = true;
        for (U32 id : sTexSeen)
        {
            glBindTexture(GL_TEXTURE_2D, id);
            GLint w = 0, h = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
            if (glGetError() != GL_NO_ERROR || w <= 0 || h <= 0 || w > 8192 || h > 8192)
            {
                continue; // not a 2D texture (cube/array) or unreadable -- skip
            }
            std::string px((size_t)w * h * 4, '\0');
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, &px[0]);
            if (glGetError() != GL_NO_ERROR)
            {
                continue;
            }
            snprintf(path, sizeof(path), "%s/tex_%u.bin", sDir.c_str(), id);
            if (FILE* tf = fopen(path, "wb"))
            {
                fwrite(px.data(), 1, px.size(), tf);
                fclose(tf);
            }
            fprintf(tj, "%s\"%u\":{\"w\":%d,\"h\":%d}", first ? "" : ",", id, w, h);
            first = false;
            ++tex_written;
        }
        fprintf(tj, "}\n");
        fclose(tj);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    snprintf(path, sizeof(path), "%s/meta.json", sDir.c_str());
    if (FILE* mj = fopen(path, "w"))
    {
        GLint vp[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, vp);
        fprintf(mj, "{\"draws\":%u,\"skipped_no_shadow\":%u,\"textures\":%u,\"viewport\":[%d,%d,%d,%d],\"frame\":%u}\n",
            sDrawCount, sSkippedNoShadow, tex_written, vp[0], vp[1], vp[2], vp[3], sInWorldFrames);
        fclose(mj);
    }
    LL_INFOS("SceneDump") << "P2b scene dump complete: " << sDrawCount << " draws, "
        << tex_written << " textures, " << sSkippedNoShadow << " skipped (no CPU shadow) -> "
        << sDir << LL_ENDL;
    sBlobsWritten.clear();
    sPtrHash.clear();
    sTexSeen.clear();
}

// <FS:VkBridge> P3: forward the typed camera + EEP sky to the engine (called once per frame
// from newview, which owns LLViewerCamera / LLEnvironment). Builds the view via glm::lookAt so
// the engine gets a column-major RH view matrix; the engine derives eye-space sun = view*sun.
void setSceneCamera(const float origin[3], const float at[3], const float up[3],
                    float near_clip, float far_clip, float fov_y, float aspect)
{
    if (!sFsrSceneCamera) return;
    FsrCameraBlock cb;
    memset(&cb, 0, sizeof(cb));
    glm::vec3 eye(origin[0], origin[1], origin[2]);
    glm::vec3 ctr = eye + glm::vec3(at[0], at[1], at[2]);
    glm::vec3 upv(up[0], up[1], up[2]);
    glm::mat4 view = glm::lookAt(eye, ctr, upv);
    memcpy(cb.view, glm::value_ptr(view), sizeof(cb.view));
    cb.origin[0] = origin[0]; cb.origin[1] = origin[1]; cb.origin[2] = origin[2];
    cb.near_clip = near_clip; cb.far_clip = far_clip; cb.fov_y = fov_y; cb.aspect = aspect;
    sFsrSceneCamera(&cb);
}
void setSceneSky(const FSSkyParams& p)
{
    if (!sFsrSceneSky) return;
    FsrEepSkyBlock sb;
    memset(&sb, 0, sizeof(sb));
    for (int i = 0; i < 3; ++i)
    {
        sb.sun_dir[i]      = p.sun_dir[i];
        sb.sun_color[i]    = p.sun_color[i];
        sb.moon_color[i]   = p.moon_color[i];
        sb.ambient[i]      = p.ambient[i];
        sb.blue_horizon[i] = p.blue_horizon[i];
        sb.blue_density[i] = p.blue_density[i];
        sb.glow[i]         = p.glow[i];
        sb.lightnorm[i]    = p.lightnorm[i];
    }
    sb.max_y                 = p.max_y;
    sb.gamma                 = p.gamma;
    sb.haze_density          = p.haze_density;
    sb.haze_horizon          = p.haze_horizon;
    sb.density_multiplier    = p.density_multiplier;
    sb.cloud_shadow          = p.cloud_shadow;
    sb.sun_moon_glow_factor  = p.sun_moon_glow_factor;
    sb.sun_up_factor         = p.sun_up_factor;
    // <FS:VkBridge> S3: the regime discriminators the engine's sky_regime() needs.
    sb.can_auto_adjust           = (U32)(p.can_auto_adjust ? 1 : 0);
    sb.reflection_probe_ambiance = p.reflection_probe_ambiance;
    sFsrSceneSky(&sb);
}
// <FS:VkBridge> S3b: forward the sky-regime settings subset (gSavedSettings the derivation reads).
void setSceneRegime(int auto_adjust_legacy, float sky_sunlight_scale, float hdr_sky_sunlight_scale,
                    float sky_ambient_scale, float auto_adjust_ambient_scale, float auto_adjust_hdr_scale,
                    float sun_dynamic_range, float tonemap_mix, int tonemap_type, float exposure,
                    int hdr_enabled, int reflection_probes_enabled)
{
    if (!sFsrSceneRegime) return;
    FsrRegimeSettings rs;
    memset(&rs, 0, sizeof(rs));
    rs.sky_auto_adjust_legacy = (U32)(auto_adjust_legacy ? 1 : 0);
    rs.sky_sunlight_scale = sky_sunlight_scale;
    rs.hdr_sky_sunlight_scale = hdr_sky_sunlight_scale;
    rs.sky_ambient_scale = sky_ambient_scale;
    rs.sky_auto_adjust_ambient_scale = auto_adjust_ambient_scale;
    rs.sky_auto_adjust_hdr_scale = auto_adjust_hdr_scale;
    rs.sun_dynamic_range = sun_dynamic_range;
    rs.tonemap_mix = tonemap_mix;
    rs.tonemap_type = (U32)tonemap_type;
    rs.exposure = exposure;
    rs.hdr_enabled = (U32)(hdr_enabled ? 1 : 0);
    rs.reflection_probes_enabled = (U32)(reflection_probes_enabled ? 1 : 0);
    sFsrSceneRegime(&rs);
}
// <FS:VkBridge> A.2 INVARIANT #1: query the REAL Vulkan adapter (fs_render's fsr_query_gpu_info
// is standalone -- it enumerates the VkPhysicalDevice itself, so it works before fsr_init and
// under native-vulkan even before the render device is up). Self-contained load so it does not
// depend on the live-bridge having been armed. FSGpuInfo mirrors FsrGpuInfo exactly.
bool getEngineGpuInfo(FSGpuInfo& out)
{
    memset(&out, 0, sizeof(out));
    HMODULE m = GetModuleHandleA("fs_render.dll");
    if (!m) m = LoadLibraryA("fs_render.dll");
    if (!m) return false;
    typedef int(__cdecl* fsr_query_gpu_t)(FSGpuInfo*);
    fsr_query_gpu_t fn = (fsr_query_gpu_t)GetProcAddress(m, "fsr_query_gpu_info");
    if (!fn) return false;
    return fn(&out) != 0;
}

void onFrame(bool in_world)
{
    if (sDone)
    {
        return;
    }
    if (!sChecked)
    {
        sChecked = true;
        // <FS:VkBridge> P3c: engine mode -> live submission
        if (const char* em = getenv("FS_ENGINE_MODE"))
        {
            if (em[0] == '1')
            {
                bindLive();
            }
        }
        if (const char* dir = getenv("FS_SCENE_DUMP"))
        {
            sDir = dir;
#ifdef LL_WINDOWS
            _mkdir(sDir.c_str());
#else
            mkdir(sDir.c_str(), 0755);
#endif
            if (const char* fr = getenv("FS_SCENE_DUMP_FRAME"))
            {
                sTargetFrame = (U32)atoi(fr);
            }
            sArmed = true;
            LL_INFOS("SceneDump") << "P2b scene dump ARMED -> " << sDir
                << " (in-world frame " << sTargetFrame << ")" << LL_ENDL;
        }
    }
    if (sLive && sFsrBegin)
    {
        ++sLiveFrames;
        // Antialiasing: forward RenderFSAASamples (pushed from newview via setMsaa --
        // llrender must not reach into gSavedSettings). Forward on change only.
        if (sFsrMsaa && sPendingMsaa != sLastMsaa)
        {
            sLastMsaa = sPendingMsaa;
            sFsrMsaa(sPendingMsaa);
        }
        if (sLiveFrames % 120 == 0)
        {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            F64 submit_ms = (F64)sSubmitTicks * 1000.0 / (F64)freq.QuadPart / 120.0;
            LL_INFOS("SceneDump") << "P3c live frame " << sLiveFrames << ": submitted=" << sLiveDraws
                                  << " no_cpu_shadow=" << sLiveNoShadow
                                  << " submit_ms/frame=" << submit_ms << LL_ENDL;
            sSubmitTicks = 0;
        }
        sLiveDraws = 0;
        sLiveNoShadow = 0;
        sFsrBegin();
        // P3: open the typed scene frame (clears last frame's camera/sky/lights); newview
        // fills camera + EEP sky right after this onFrame call.
        if (sFsrSceneBegin) sFsrSceneBegin();
        // A.3: clear the engine's per-frame UI list -- LLRender::flush appends to it all frame.
        if (sFsrUiBegin) sFsrUiBegin();
    }
    if (!sArmed)
    {
        return;
    }
    if (sActive)
    {
        // the capture frame just completed
        finalize();
        return;
    }
    if (in_world)
    {
        ++sInWorldFrames;
        if (sInWorldFrames >= sTargetFrame)
        {
            char path[1024];
            snprintf(path, sizeof(path), "%s/draws.jsonl", sDir.c_str());
            sDrawsFile = fopen(path, "w");
            sActive = (sDrawsFile != nullptr);
            if (sActive)
            {
                LL_INFOS("SceneDump") << "P2b capturing THIS frame" << LL_ENDL;
            }
        }
    }
}

} // namespace FSSceneDump
