/**
 * @file fsscenedump.cpp
 * <FS:VkBridge> P2b one-frame scene dump. See fsscenedump.h.
 */

#include "linden_common.h"
#include "fsscenedump.h"

#include "llvertexbuffer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llgl.h"

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
