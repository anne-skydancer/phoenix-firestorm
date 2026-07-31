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
    F32 aux[8];      // per-class data (terrain: object_plane_s, object_plane_t)
    F32 texmat[16];  // F5: texture_matrix0 -- llSetTextureAnim sheet-flips/scroll/rotate
                     // and large-face planar all ride this; identity in the common case
    F32 khr[8];      // KHR_texture_transform (base color): [sx,sy,rot,_, ox,oy,_,_]
    U32 indexed_ch;  // bound shader's mIndexedTextureChannels -- 0 = position.w is NOT
                     // a texture index (PBR/materials/avatar draws were mis-indexing)
    F32 min_alpha;   // MASK-mode alpha cutoff (-1 = disabled)
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
static bool sLive = false;
static S32 sSuppressDepth = 0; // F2: >0 while an offscreen pass renders
static U32 sDrawClass = 0;     // F7: current draw class (set by the owning pool)
static U32 sAuxTex[8] = { 0 };
static F32 sAuxF[8] = { 0 };
static F32 sKhr[8] = { 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f }; // identity KHR transform
static U32 sMatTex[3] = { 0, 0, 0 };   // A5: staged diffuse/normal/spec (one-shot)
static F32 sMatCutoff = -1.f;
static bool sMatStaged = false;
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
    sLive = (sFsrBegin && sFsrSubmit && sFsrEnd);
    LL_INFOS("SceneDump") << "P3c live bridge " << (sLive ? "ARMED" : "FAILED")
                          << " (begin=" << (void*)sFsrBegin << " submit=" << (void*)sFsrSubmit
                          << " end=" << (void*)sFsrEnd << ")" << LL_ENDL;
}

bool liveActive() { return sLive; }

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
    if (slot < 2 && v4)
    {
        memcpy(&sAuxF[slot * 4], v4, 4 * sizeof(F32));
    }
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
    if (sLive && sSuppressDepth == 0 && vb)
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
            }
            d.draw_class = sDrawClass;
            memcpy(d.tex_ex, sAuxTex, sizeof(d.tex_ex));
            memcpy(d.aux, sAuxF, sizeof(d.aux));
            memcpy(d.texmat, glm::value_ptr(gGL.getTextureMatrix0()), sizeof(d.texmat));
            memcpy(d.khr, khr_local, sizeof(d.khr));
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
            if (mat_staged)
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
