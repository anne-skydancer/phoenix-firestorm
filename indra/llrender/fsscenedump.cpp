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
    // <FS:VkBridge> P3c live path: submit straight to the engine.
    if (sLive && vb)
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
            for (U32 ti = 0; ti < 4; ++ti)
            {
                d.tex[ti] = gGL.getTexUnit(ti) ? gGL.getTexUnit(ti)->getCurrTexture() : 0u;
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
            sFsrSubmit(&d, vdata, indexed ? idata : nullptr);
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
            LL_INFOS("SceneDump") << "P3c live frame " << sLiveFrames << ": submitted=" << sLiveDraws
                                  << " no_cpu_shadow=" << sLiveNoShadow << LL_ENDL;
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
