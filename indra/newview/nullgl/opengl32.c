/**
 * @file opengl32.c
 * <FS:VkBridge> P3b -- the null-GL stub (rhi/PLAN.md Phase 3, PHASE3_PLAN.md).
 *
 * Engine mode (RenderGLBackend="vulkan") puts this DLL's directory on the search
 * path before the /DELAYLOAD-ed opengl32 resolves, so the viewer's entire GL-shaped
 * pipeline (cull -> sort -> state calls -> draw leaves) executes harmlessly as
 * no-ops while fs_render.dll does ALL actual rendering (Vulkan) on the same HWND.
 * This stub renders NOTHING, ever -- it is a compatibility mattress under twenty
 * years of GL-shaped C++, with an expiry date (pruned as engine passes take over).
 *
 * Export surface = the EXACT 47-function delayload import list measured from
 * firestorm-bin.exe (dumpbin /imports), plus a wglGetProcAddress that returns
 * sane specials for the pointer-loaded functions init depends on (compile/link
 * status TRUE, framebuffer COMPLETE, occlusion queries "visible", buffer maps ->
 * scratch memory, uniform locations -1) and a benign returns-zero stub otherwise.
 * Unknown glGetIntegerv pnames + unknown GPA names log once to %TEMP%\nullgl.log
 * for caps-table iteration (the plan's named risk).
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef double GLdouble;
typedef void GLvoid;
typedef ptrdiff_t GLintptr;
typedef ptrdiff_t GLsizeiptr;
typedef unsigned long long GLuint64;
typedef long long GLint64;

#define NG_API __declspec(dllexport)

/* ---- logging (once per unique key) ------------------------------------------- */
static CRITICAL_SECTION g_lock;
static int g_lock_init = 0;
static char g_logged[256][64];
static int g_nlogged = 0;

static void ng_lock(void)
{
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; }
    EnterCriticalSection(&g_lock);
}
static void ng_unlock(void) { LeaveCriticalSection(&g_lock); }

static void log_once(const char* kind, const char* key)
{
    ng_lock();
    for (int i = 0; i < g_nlogged; ++i)
    {
        if (strncmp(g_logged[i], key, 63) == 0) { ng_unlock(); return; }
    }
    if (g_nlogged < 256)
    {
        strncpy(g_logged[g_nlogged], key, 63);
        g_logged[g_nlogged][63] = 0;
        g_nlogged++;
    }
    char buf[256];
    _snprintf(buf, sizeof(buf), "nullgl %s: %s\n", kind, key);
    OutputDebugStringA(buf);
    char path[MAX_PATH];
    if (GetTempPathA(MAX_PATH, path))
    {
        strncat(path, "nullgl.log", MAX_PATH - strlen(path) - 1);
        FILE* f = fopen(path, "a");
        if (f) { fputs(buf, f); fclose(f); }
    }
    ng_unlock();
}

/* ---- canonical identity + caps ------------------------------------------------ */
static const char* NG_VENDOR = "FSVulkan";
static const char* NG_RENDERER = "fs_render null-GL bridge";
static const char* NG_VERSION = "4.6.0 FSVulkan nullgl";
static const char* NG_GLSL = "4.60 FSVulkan";
/* generous-but-plausible extension set; iterate via nullgl.log findings */
static const char* NG_EXTENSIONS =
    "GL_ARB_vertex_buffer_object GL_ARB_vertex_array_object GL_ARB_framebuffer_object "
    "GL_EXT_framebuffer_object GL_EXT_framebuffer_multisample GL_EXT_framebuffer_blit "
    "GL_ARB_texture_compression GL_EXT_texture_compression_s3tc GL_EXT_texture_sRGB "
    "GL_EXT_texture_filter_anisotropic GL_ARB_occlusion_query GL_ARB_occlusion_query2 "
    "GL_ARB_timer_query GL_ARB_map_buffer_range GL_ARB_sync GL_ARB_depth_clamp "
    "GL_ARB_uniform_buffer_object GL_ARB_texture_multisample GL_ARB_texture_swizzle "
    "GL_ARB_get_program_binary GL_ARB_debug_output GL_ARB_texture_storage "
    "GL_ARB_texture_cube_map_array GL_ARB_transform_feedback2 GL_ARB_seamless_cube_map "
    "GL_ARB_shader_objects GL_ARB_vertex_shader GL_ARB_fragment_shader";

/* Tracked GL state (defined early: caps_value reads it). See the state block below. */
static GLint g_depth_mask = 1;

/* PIXEL STORE state -- REQUIRED, not optional. llimagegl.cpp:1190 sets
 * GL_UNPACK_ROW_LENGTH = data_width for every sub-rect upload, so the source rows are
 * STRIDED, not contiguous. Ignoring it skews every partial upload into garbage (the
 * web-panel flicker and the wrong in-world textures). */
static __declspec(thread) GLint g_unpack_row_length = 0;
static __declspec(thread) GLint g_unpack_alignment = 4;
static __declspec(thread) GLint g_unpack_swap_bytes = 0;

static char g_ext_copy[2048]; /* mutable copy for tokenized glGetStringi */
static const char* g_ext_tok[128];
static int g_next = -1;

static void ng_init_ext(void)
{
    if (g_next >= 0) return;
    ng_lock();
    if (g_next < 0)
    {
        strncpy(g_ext_copy, NG_EXTENSIONS, sizeof(g_ext_copy) - 1);
        int n = 0;
        char* ctx = NULL;
        for (char* t = strtok_s(g_ext_copy, " ", &ctx); t && n < 128; t = strtok_s(NULL, " ", &ctx))
        {
            g_ext_tok[n++] = t;
        }
        g_next = n;
    }
    ng_unlock();
}

static GLint caps_value(GLenum pname)
{
    switch (pname)
    {
    case 0x821B: return 4;            /* GL_MAJOR_VERSION */
    case 0x821C: return 6;            /* GL_MINOR_VERSION */
    case 0x821D: ng_init_ext(); return g_next; /* GL_NUM_EXTENSIONS */
    case 0x0D33: return 16384;        /* GL_MAX_TEXTURE_SIZE */
    case 0x8073: return 2048;         /* GL_MAX_3D_TEXTURE_SIZE */
    case 0x851C: return 16384;        /* GL_MAX_CUBE_MAP_TEXTURE_SIZE */
    case 0x88FF: return 2048;         /* GL_MAX_ARRAY_TEXTURE_LAYERS */
    case 0x8872: return 32;           /* GL_MAX_TEXTURE_IMAGE_UNITS */
    case 0x8B4C: return 32;           /* GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS */
    case 0x8B4D: return 192;          /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
    case 0x84E2: return 32;           /* GL_MAX_TEXTURE_UNITS (legacy) */
    case 0x8869: return 16;           /* GL_MAX_VERTEX_ATTRIBS */
    case 0x8B4A: return 4096;         /* GL_MAX_VERTEX_UNIFORM_COMPONENTS */
    case 0x8B49: return 4096;         /* GL_MAX_FRAGMENT_UNIFORM_COMPONENTS */
    case 0x8A2F: return 84;           /* GL_MAX_UNIFORM_BUFFER_BINDINGS */
    case 0x8A30: return 65536;        /* GL_MAX_UNIFORM_BLOCK_SIZE */
    case 0x8A34: return 256;          /* GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT */
    case 0x8CDF: return 8;            /* GL_MAX_COLOR_ATTACHMENTS */
    case 0x8D57: return 8;            /* GL_MAX_SAMPLES */
    case 0x84FD: return 16;           /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */
    case 0x0D3A: return 16384;        /* GL_MAX_VIEWPORT_DIMS (first) */
    case 0x80E9: return 1048576;      /* GL_MAX_ELEMENTS_VERTICES */
    case 0x80E8: return 1048576;      /* GL_MAX_ELEMENTS_INDICES */
    case 0x0B72: return g_depth_mask;  /* GL_DEPTH_WRITEMASK -- tracked, not guessed */
    case 0x0D50: return 24;           /* GL_DEPTH_BITS */
    case 0x0D57: return 8;            /* GL_STENCIL_BITS */
    case 0x80A8: return 0;            /* GL_SAMPLE_BUFFERS */
    case 0x80A9: return 0;            /* GL_SAMPLES */
    case 0x9047: return 16 * 1024 * 1024; /* NVX dedicated vidmem (KB) = 16GB */
    case 0x9048: return 16 * 1024 * 1024; /* NVX total available */
    case 0x9049: return 16 * 1024 * 1024; /* NVX current available */
    case 0x87FC: return 16 * 1024 * 1024; /* ATI texture free memory */
    case 0x0B93: return 0;            /* GL_TEXTURE_STACK_DEPTH-ish legacy: 0 */
    default:
    {
        char k[64];
        _snprintf(k, sizeof(k), "glGetIntegerv 0x%04X", pname);
        log_once("pname", k);
        return 16; /* generically-plausible positive */
    }
    }
}

/* ---- GL state we must actually REMEMBER --------------------------------------
 * The engine reads blend/depth state back (glIsEnabled, GL_DEPTH_WRITEMASK) to pick
 * its pipeline. Returning "always off" made every alpha-blended draw opaque: font
 * glyphs became solid blocks. Track the caps the viewer sets. */
#define NG_MAX_CAPS 64
static struct { GLenum cap; int on; } g_caps[NG_MAX_CAPS];
static int g_ncaps = 0;

static void cap_set(GLenum cap, int on)
{
    ng_lock();
    for (int i = 0; i < g_ncaps; ++i)
    {
        if (g_caps[i].cap == cap) { g_caps[i].on = on; ng_unlock(); return; }
    }
    if (g_ncaps < NG_MAX_CAPS) { g_caps[g_ncaps].cap = cap; g_caps[g_ncaps].on = on; g_ncaps++; }
    ng_unlock();
}
static int cap_get(GLenum cap)
{
    int r = 0;
    ng_lock();
    for (int i = 0; i < g_ncaps; ++i)
    {
        if (g_caps[i].cap == cap) { r = g_caps[i].on; break; }
    }
    ng_unlock();
    return r;
}

/* ---- id + handle fountains ---------------------------------------------------- */
static volatile LONG g_id = 1000;
static GLuint next_id(void) { return (GLuint)InterlockedIncrement(&g_id); }

static void gen_ids(GLsizei n, GLuint* ids)
{
    if (!ids) return;
    for (GLsizei i = 0; i < n; ++i) ids[i] = next_id();
}

/* scratch arena for glMapBuffer* (single 64MB region, reused; log if mapped twice) */
static void* g_scratch = NULL;
static void* scratch(size_t want)
{
    if (!g_scratch)
    {
        g_scratch = VirtualAlloc(NULL, 64u * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    (void)want;
    return g_scratch;
}

/* ---- texture mirroring: forward uploads to fs_render ---------------------------
 * The engine needs pixel data; in engine mode glTexImage2D would otherwise drop it.
 * We sit exactly where uploads land, so convert to RGBA8 and hand it over with the
 * SAME GL name the draw taps report (gGL.getTexUnit(0)->getCurrTexture()), which is
 * the id our glGenTextures minted -- so ids match on both sides by construction. */
typedef int(__cdecl* fsr_texup_t)(GLuint, GLuint, GLuint, const unsigned char*);
typedef int(__cdecl* fsr_texsub_t)(GLuint, GLuint, GLuint, GLuint, GLuint, const unsigned char*);
static fsr_texup_t g_texup = NULL;
static fsr_texsub_t g_texsub = NULL;
static int g_texup_tried = 0;
/* PER-THREAD, not global: the viewer updates media textures on a separate thread
 * (llviewermedia.cpp doMediaTexUpdate -> syncToMainThread), so a single global
 * "currently bound texture" lets the media thread and main thread stomp each other
 * and uploads land on the wrong texture -- the web panel cycling content/white/grey. */
static __declspec(thread) GLuint g_bound2d = 0;

static fsr_texup_t get_texup(void)
{
    if (!g_texup_tried)
    {
        g_texup_tried = 1;
        HMODULE m = GetModuleHandleA("fs_render.dll");
        if (!m) m = LoadLibraryA("fs_render.dll");
        if (m) g_texup = (fsr_texup_t)GetProcAddress(m, "fsr_texture_upload");
        if (m) g_texsub = (fsr_texsub_t)GetProcAddress(m, "fsr_texture_subupload");
        log_once("texmirror", g_texup ? "fsr_texture_upload bound" : "fsr_texture_upload MISSING");
    }
    return g_texup;
}

/* Convert the common viewer upload formats to RGBA8. Returns 1 on success. */
static int to_rgba8_tight(GLenum format, GLenum type, GLsizei w, GLsizei h, const void* src, unsigned char* dst);

static int comps_of(GLenum format)
{
    switch (format)
    {
    case 0x1908: case 0x80E1: return 4;            /* RGBA, BGRA */
    case 0x1907: case 0x80E0: return 3;            /* RGB, BGR */
    case 0x8227: case 0x190A: return 2;            /* RG, LUMINANCE_ALPHA */
    default: return 1;                             /* RED, ALPHA, LUMINANCE */
    }
}

/* Convert a w x h sub-rect out of a source whose rows are `row_len` pixels wide
 * (GL_UNPACK_ROW_LENGTH). row_len == 0 means tightly packed. */
static int to_rgba8_strided(GLenum format, GLenum type, GLsizei w, GLsizei h, GLsizei row_len,
                            const void* src, unsigned char* dst);

static int to_rgba8(GLenum format, GLenum type, GLsizei w, GLsizei h, const void* src, unsigned char* dst)
{
    return to_rgba8_strided(format, type, w, h, 0, src, dst);
}

static int to_rgba8_strided(GLenum format, GLenum type, GLsizei w, GLsizei h, GLsizei row_len,
                            const void* src, unsigned char* dst)
{
    if (type != 0x1401 /* GL_UNSIGNED_BYTE */) return 0;
    int nc = comps_of(format);
    GLsizei stride_px = (row_len > 0) ? row_len : w;
    /* repack strided rows into a tight buffer, then run the existing per-format path */
    unsigned char* tight = NULL;
    if (stride_px != w)
    {
        tight = (unsigned char*)malloc((size_t)w * h * nc);
        if (!tight) return 0;
        const unsigned char* sp = (const unsigned char*)src;
        for (GLsizei r = 0; r < h; ++r)
        {
            memcpy(tight + (size_t)r * w * nc, sp + (size_t)r * stride_px * nc, (size_t)w * nc);
        }
        src = tight;
    }
    int ok = to_rgba8_tight(format, type, w, h, src, dst);
    free(tight);
    return ok;
}

static int to_rgba8_tight(GLenum format, GLenum type, GLsizei w, GLsizei h, const void* src, unsigned char* dst)
{
    if (type != 0x1401 /* GL_UNSIGNED_BYTE */) return 0;
    const unsigned char* p = (const unsigned char*)src;
    size_t n = (size_t)w * h;
    switch (format)
    {
    case 0x1908: /* GL_RGBA */
        memcpy(dst, p, n * 4);
        return 1;
    case 0x80E1: /* GL_BGRA */
        for (size_t i = 0; i < n; ++i)
        {
            dst[i * 4 + 0] = p[i * 4 + 2];
            dst[i * 4 + 1] = p[i * 4 + 1];
            dst[i * 4 + 2] = p[i * 4 + 0];
            dst[i * 4 + 3] = p[i * 4 + 3];
        }
        return 1;
    case 0x1907: /* GL_RGB */
        for (size_t i = 0; i < n; ++i)
        {
            dst[i * 4 + 0] = p[i * 3 + 0];
            dst[i * 4 + 1] = p[i * 3 + 1];
            dst[i * 4 + 2] = p[i * 3 + 2];
            dst[i * 4 + 3] = 255;
        }
        return 1;
    case 0x80E0: /* GL_BGR */
        for (size_t i = 0; i < n; ++i)
        {
            dst[i * 4 + 0] = p[i * 3 + 2];
            dst[i * 4 + 1] = p[i * 3 + 1];
            dst[i * 4 + 2] = p[i * 3 + 0];
            dst[i * 4 + 3] = 255;
        }
        return 1;
    case 0x1906: /* GL_ALPHA -- font glyphs: white with the glyph in alpha */
        for (size_t i = 0; i < n; ++i)
        {
            dst[i * 4 + 0] = 255; dst[i * 4 + 1] = 255; dst[i * 4 + 2] = 255;
            dst[i * 4 + 3] = p[i];
        }
        return 1;
    case 0x1903: /* GL_RED -- modern single channel; font atlases keep coverage here,
                  * so present it as white-with-alpha exactly like GL_ALPHA did. */
        for (size_t i = 0; i < n; ++i)
        {
            dst[i * 4 + 0] = 255; dst[i * 4 + 1] = 255; dst[i * 4 + 2] = 255;
            dst[i * 4 + 3] = p[i];
        }
        return 1;
    case 0x8227: /* GL_RG -- this is the FONT ATLAS path (measured: id 2024, 512x512,
                  * every glyph upload arrives as GL_RG). It is luminance+alpha in modern
                  * dress: .r = coverage/intensity, .g = alpha. Mapping it to (r,g,0,255)
                  * painted every glyph pure red -- the red/orange UI text. Present it the
                  * way the old GL_LUMINANCE_ALPHA was presented. */
        for (size_t i = 0; i < n; ++i)
        {
            dst[i * 4 + 0] = p[i * 2]; dst[i * 4 + 1] = p[i * 2]; dst[i * 4 + 2] = p[i * 2];
            dst[i * 4 + 3] = p[i * 2 + 1];
        }
        return 1;
    case 0x1909: /* GL_LUMINANCE */
        for (size_t i = 0; i < n; ++i)
        {
            dst[i * 4 + 0] = p[i]; dst[i * 4 + 1] = p[i]; dst[i * 4 + 2] = p[i];
            dst[i * 4 + 3] = 255;
        }
        return 1;
    case 0x190A: /* GL_LUMINANCE_ALPHA */
        for (size_t i = 0; i < n; ++i)
        {
            dst[i * 4 + 0] = p[i * 2]; dst[i * 4 + 1] = p[i * 2]; dst[i * 4 + 2] = p[i * 2];
            dst[i * 4 + 3] = p[i * 2 + 1];
        }
        return 1;
    default:
    {
        char k[64];
        _snprintf(k, sizeof(k), "texformat 0x%04X", format);
        log_once("tex", k);
        return 0;
    }
    }
}

/* CPU-side mirror of each 2D texture, so incremental sub-rect updates work.
 * Font atlases are BUILT by glTexSubImage2D at arbitrary offsets (one glyph at a
 * time) -- skipping those left every glyph blank (measured: no readable text). We
 * keep the whole image, patch the sub-rect, and re-upload. Capped so world-sized
 * textures don't balloon the cache (they arrive as complete glTexImage2D uploads). */
/* The ENGINE owns texture pixels (it patches sub-rects in place), so the stub keeps
 * only a tiny registry of dimensions -- no CPU atlas copies, no eviction. Rule that
 * matters: glTexImage2D with NULL pixels is a re-spec/allocate. Uploading zeros for
 * those wiped textures that already had content (measured: whole UI vanished), so we
 * create-once and otherwise leave the engine's texture alone. */
#define NG_TEXREG_MAX 8192
typedef struct { GLuint id; GLsizei w, h; } NgTexReg;
static NgTexReg g_texreg[NG_TEXREG_MAX];
static int g_ntexreg = 0;

static NgTexReg* texreg_find(GLuint id)
{
    for (int i = 0; i < g_ntexreg; ++i)
    {
        if (g_texreg[i].id == id) return &g_texreg[i];
    }
    return NULL;
}
static void texreg_set(GLuint id, GLsizei w, GLsizei h)
{
    NgTexReg* r = texreg_find(id);
    if (!r)
    {
        if (g_ntexreg >= NG_TEXREG_MAX) return;
        r = &g_texreg[g_ntexreg++];
        r->id = id;
    }
    r->w = w;
    r->h = h;
}

static void mirror_texture(GLenum target, GLint level, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels)
{
    if (target != 0x0DE1 /* GL_TEXTURE_2D */ || level != 0 || w <= 0 || h <= 0) return;
    if (w > 4096 || h > 4096) return;
    fsr_texup_t up = get_texup();
    if (!up || !g_bound2d) return;

    NgTexReg* known = texreg_find(g_bound2d);
    if (!pixels)
    {
        /* allocate/re-spec: only create if the engine has nothing of this size yet */
        if (known && known->w == w && known->h == h) return;
        size_t bytes = (size_t)w * h * 4;
        unsigned char* zero = (unsigned char*)calloc(bytes, 1);
        if (!zero) return;
        up(g_bound2d, (GLuint)w, (GLuint)h, zero);
        free(zero);
        texreg_set(g_bound2d, w, h);
        return;
    }

    unsigned char* buf = (unsigned char*)malloc((size_t)w * h * 4);
    if (!buf) return;
    if (to_rgba8(format, type, w, h, pixels, buf))
    {
        up(g_bound2d, (GLuint)w, (GLuint)h, buf);
        texreg_set(g_bound2d, w, h);
    }
    free(buf);
}

/* Forward the sub-rect straight to the engine, which patches its own texture IN
 * PLACE (queue.write_texture). No CPU-side atlas rebuild: rebuilding meant zeroing
 * and re-uploading a whole atlas per glyph, erasing every glyph already written. */
static void mirror_subtexture(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels)
{
    if (target != 0x0DE1 || level != 0 || !pixels || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0) return;
    get_texup();
    if (!g_texsub || !g_bound2d) return;
    size_t bytes = (size_t)w * h * 4;
    unsigned char* tmp = (unsigned char*)malloc(bytes);
    if (!tmp) return;
    if (to_rgba8_strided(format, type, w, h, g_unpack_row_length, pixels, tmp))
    {
        g_texsub(g_bound2d, (GLuint)x, (GLuint)y, (GLuint)w, (GLuint)h, tmp);
    }
    free(tmp);
}

/* =====================  the 47 measured delayload exports  ===================== */
/* no-op state setters */
NG_API void __stdcall glBindTexture(GLenum t, GLuint x) { if (t == 0x0DE1) g_bound2d = x; }
NG_API void __stdcall glBlendFunc(GLenum a, GLenum b) { (void)a; (void)b; }
NG_API void __stdcall glClear(unsigned int m) { (void)m; }
NG_API void __stdcall glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void)r; (void)g; (void)b; (void)a; }
NG_API void __stdcall glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { (void)r; (void)g; (void)b; (void)a; }
NG_API void __stdcall glCopyTexSubImage2D(GLenum a, GLint b, GLint c, GLint d, GLint e, GLint f, GLsizei g, GLsizei h) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; }
NG_API void __stdcall glCullFace(GLenum m) { (void)m; }
NG_API void __stdcall glDeleteTextures(GLsizei n, const GLuint* t) { (void)n; (void)t; }
NG_API void __stdcall glDepthFunc(GLenum f) { (void)f; }
NG_API void __stdcall glDepthMask(GLboolean f) { g_depth_mask = f ? 1 : 0; }
NG_API void __stdcall glDisable(GLenum c) { cap_set(c, 0); }
NG_API void __stdcall glDrawArrays(GLenum m, GLint f, GLsizei c) { (void)m; (void)f; (void)c; }
NG_API void __stdcall glDrawBuffer(GLenum b) { (void)b; }
NG_API void __stdcall glEnable(GLenum c) { cap_set(c, 1); }
NG_API void __stdcall glFinish(void) {}
NG_API void __stdcall glFlush(void) {}
NG_API void __stdcall glHint(GLenum t, GLenum m) { (void)t; (void)m; }
NG_API void __stdcall glLineWidth(GLfloat w) { (void)w; }
NG_API void __stdcall glMaterialfv(GLenum f, GLenum p, const GLfloat* v) { (void)f; (void)p; (void)v; }
NG_API void __stdcall glMateriali(GLenum f, GLenum p, GLint v) { (void)f; (void)p; (void)v; }
NG_API void __stdcall glPixelStorei(GLenum p, GLint v)
{
    switch (p)
    {
    case 0x0CF2: g_unpack_row_length = v; break;  /* GL_UNPACK_ROW_LENGTH */
    case 0x0CF5: g_unpack_alignment = v; break;   /* GL_UNPACK_ALIGNMENT */
    case 0x0CF0: g_unpack_swap_bytes = v; break;  /* GL_UNPACK_SWAP_BYTES */
    default: break;
    }
}
NG_API void __stdcall glPointSize(GLfloat s) { (void)s; }
NG_API void __stdcall glPolygonMode(GLenum f, GLenum m) { (void)f; (void)m; }
NG_API void __stdcall glPolygonOffset(GLfloat a, GLfloat b) { (void)a; (void)b; }
NG_API void __stdcall glReadBuffer(GLenum b) { (void)b; }
NG_API void __stdcall glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x; (void)y; (void)w; (void)h; }
NG_API void __stdcall glTexImage2D(GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const void* p) { (void)c; (void)f; mirror_texture(a, b, d, e, g, h, p); }
NG_API void __stdcall glTexParameterf(GLenum t, GLenum p, GLfloat v) { (void)t; (void)p; (void)v; }
NG_API void __stdcall glTexParameteri(GLenum t, GLenum p, GLint v) { (void)t; (void)p; (void)v; }
NG_API void __stdcall glTexParameteriv(GLenum t, GLenum p, const GLint* v) { (void)t; (void)p; (void)v; }
NG_API void __stdcall glTexSubImage2D(GLenum a, GLint b, GLint c, GLint d, GLsizei e, GLsizei f, GLenum g, GLenum h, const void* p) { mirror_subtexture(a, b, c, d, e, f, g, h, p); }
NG_API void __stdcall glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x; (void)y; (void)w; (void)h; }

/* getters with real behavior */
NG_API GLenum __stdcall glGetError(void) { return 0; /* GL_NO_ERROR */ }
NG_API const unsigned char* __stdcall glGetString(GLenum name)
{
    switch (name)
    {
    case 0x1F00: return (const unsigned char*)NG_VENDOR;
    case 0x1F01: return (const unsigned char*)NG_RENDERER;
    case 0x1F02: return (const unsigned char*)NG_VERSION;
    case 0x1F03: return (const unsigned char*)NG_EXTENSIONS;
    case 0x8B8C: return (const unsigned char*)NG_GLSL;
    default: return (const unsigned char*)"";
    }
}
NG_API void __stdcall glGetIntegerv(GLenum pname, GLint* out)
{
    if (!out) return;
    if (pname == 0x0D3A) { out[0] = 16384; out[1] = 16384; return; } /* MAX_VIEWPORT_DIMS pair */
    out[0] = caps_value(pname);
}
NG_API void __stdcall glGetFloatv(GLenum pname, GLfloat* out)
{
    if (!out) return;
    out[0] = (GLfloat)caps_value(pname);
}
NG_API void __stdcall glGetBooleanv(GLenum pname, GLboolean* out)
{
    if (!out) return;
    out[0] = (GLboolean)(caps_value(pname) != 0);
}
NG_API GLboolean __stdcall glIsEnabled(GLenum cap) { return (GLboolean)cap_get(cap); }
NG_API void __stdcall glGenTextures(GLsizei n, GLuint* t) { gen_ids(n, t); }
NG_API void __stdcall glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* px)
{
    (void)x; (void)y; (void)fmt; (void)type;
    if (px && w > 0 && h > 0) memset(px, 0, (size_t)w * h * 4);
}
NG_API void __stdcall glGetTexImage(GLenum t, GLint l, GLenum f, GLenum ty, void* px)
{
    (void)t; (void)l; (void)f; (void)ty; (void)px; /* size unknown here: leave untouched */
}
NG_API void __stdcall glGetTexLevelParameteriv(GLenum t, GLint l, GLenum p, GLint* out)
{
    (void)t; (void)l;
    if (!out) return;
    switch (p)
    {
    case 0x1000: case 0x1001: out[0] = 0; break; /* TEXTURE_WIDTH/HEIGHT: 0 (no texture) */
    default: out[0] = 0; break;
    }
}

/* wgl surface */
static HDC g_dc = NULL;
NG_API HGLRC __stdcall wglCreateContext(HDC dc) { g_dc = dc; return (HGLRC)(uintptr_t)0xFEEDC0DE; }
NG_API BOOL __stdcall wglMakeCurrent(HDC dc, HGLRC rc) { if (dc) g_dc = dc; (void)rc; return TRUE; }
NG_API BOOL __stdcall wglDeleteContext(HGLRC rc) { (void)rc; return TRUE; }
NG_API HDC __stdcall wglGetCurrentDC(void) { return g_dc; }
/* fs_render.dll itself imports this: wgpu links a GL backend we never use (we pin
 * Backends::VULKAN), but its import must still resolve or LoadLibrary fails with
 * "entry point wglGetCurrentContext could not be located" (measured, engine boot #2). */
NG_API HGLRC __stdcall wglGetCurrentContext(void) { return (HGLRC)(uintptr_t)0xFEEDC0DE; }

/* ---- GDI32-forwarded private entry points -------------------------------------
 * GDI32's ChoosePixelFormat/SetPixelFormat/DescribePixelFormat/GetPixelFormat/
 * SwapBuffers do NOT implement OpenGL pixel formats themselves -- they forward to
 * these opengl32 exports. Without them GDI32 fails with ERROR_PROC_NOT_FOUND (127),
 * which surfaces as the viewer's "Can't find suitable pixel format" (measured on the
 * first engine-mode boot). We advertise exactly ONE format: 32-bit RGBA, 24-bit
 * depth, 8-bit stencil, double-buffered, PFD_SUPPORT_OPENGL -- enough for the
 * viewer's ChoosePixelFormat to succeed. Nothing GL ever draws into it; fs_render
 * presents to the same HWND via Vulkan.
 */
#define NG_PF_INDEX 1

NG_API int __stdcall wglDescribePixelFormat(HDC dc, int fmt, UINT bytes, PIXELFORMATDESCRIPTOR* ppfd)
{
    (void)dc; (void)fmt;
    if (ppfd && bytes >= sizeof(PIXELFORMATDESCRIPTOR))
    {
        memset(ppfd, 0, sizeof(*ppfd));
        ppfd->nSize = sizeof(PIXELFORMATDESCRIPTOR);
        ppfd->nVersion = 1;
        ppfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        ppfd->iPixelType = PFD_TYPE_RGBA;
        ppfd->cColorBits = 32;
        ppfd->cRedBits = 8; ppfd->cGreenBits = 8; ppfd->cBlueBits = 8; ppfd->cAlphaBits = 8;
        ppfd->cRedShift = 16; ppfd->cGreenShift = 8; ppfd->cBlueShift = 0; ppfd->cAlphaShift = 24;
        ppfd->cDepthBits = 24;
        ppfd->cStencilBits = 8;
        ppfd->iLayerType = PFD_MAIN_PLANE;
    }
    return NG_PF_INDEX; /* number of formats supported */
}
NG_API int __stdcall wglChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR* ppfd)
{
    (void)dc; (void)ppfd;
    return NG_PF_INDEX; /* our one format satisfies everything */
}
NG_API BOOL __stdcall wglSetPixelFormat(HDC dc, int fmt, const PIXELFORMATDESCRIPTOR* ppfd)
{
    (void)dc; (void)fmt; (void)ppfd;
    return TRUE;
}
NG_API int __stdcall wglGetPixelFormat(HDC dc) { (void)dc; return NG_PF_INDEX; }
NG_API BOOL __stdcall wglSwapBuffers(HDC dc) { (void)dc; return TRUE; } /* engine presents */
NG_API BOOL __stdcall wglShareLists(HGLRC a, HGLRC b) { (void)a; (void)b; return TRUE; }
NG_API BOOL __stdcall wglCopyContext(HGLRC a, HGLRC b, UINT m) { (void)a; (void)b; (void)m; return TRUE; }
NG_API BOOL __stdcall wglSwapLayerBuffers(HDC dc, UINT planes) { (void)dc; (void)planes; return TRUE; }

/* =====================  wglGetProcAddress special table  ====================== */
static GLuint __stdcall ng_create(GLenum t) { (void)t; return next_id(); }
static GLuint __stdcall ng_create0(void) { return next_id(); }
static void __stdcall ng_genN(GLsizei n, GLuint* ids) { gen_ids(n, ids); }
static int proguni_count(GLuint prog); /* fwd: active-uniform count parsed from shader source */

static void __stdcall ng_shaderiv(GLuint s, GLenum p, GLint* out)
{
    if (!out) return;
    switch (p)
    {
    case 0x8B81: case 0x8B82: case 0x8B83: out[0] = 1; break; /* COMPILE/LINK/VALIDATE TRUE */
    case 0x8B84: out[0] = 0; break;                            /* INFO_LOG_LENGTH */
    case 0x8B86: out[0] = proguni_count(s); break;             /* ACTIVE_UNIFORMS (from real GLSL) */
    case 0x8B89: out[0] = 0; break;                            /* ACTIVE_ATTRIBUTES */
    case 0x8B87: case 0x8B8A: out[0] = 64; break;              /* ACTIVE_*_MAX_LENGTH */
    case 0x8741: out[0] = 0; break;                            /* PROGRAM_BINARY_LENGTH */
    case 0x8A36: out[0] = 0; break;                            /* ACTIVE_UNIFORM_BLOCKS */
    default: out[0] = 0; break;
    }
}
/* Uniform locations must be REAL for the ones whose VALUES the engine needs.
 * UI/text colour arrives as the "color" uniform (LLShaderMgr::DIFFUSE_COLOR); with
 * -1 the viewer's uniform4f() early-outs and the colour never exists anywhere --
 * measured as red/orange text and a grey background. Assign a stable index per name
 * and capture the value when it is set. */
#define NG_MAX_UNI 512
static char g_uni_names[NG_MAX_UNI][64];
static int g_nuni = 0;
static GLint g_diffuse_loc = -1;

typedef int(__cdecl* fsr_setcolor_t)(float, float, float, float);
static fsr_setcolor_t g_setcolor = NULL;
static int g_setcolor_tried = 0;
static fsr_setcolor_t get_setcolor(void)
{
    if (!g_setcolor_tried)
    {
        g_setcolor_tried = 1;
        HMODULE m = GetModuleHandleA("fs_render.dll");
        if (!m) m = LoadLibraryA("fs_render.dll");
        if (m) g_setcolor = (fsr_setcolor_t)GetProcAddress(m, "fsr_set_color");
    }
    return g_setcolor;
}

static GLint __stdcall ng_loc(GLuint p, const char* n)
{
    (void)p;
    if (!n) return -1;
    ng_lock();
    int found = -1;
    for (int i = 0; i < g_nuni; ++i)
    {
        if (strcmp(g_uni_names[i], n) == 0) { found = i; break; }
    }
    if (found < 0 && g_nuni < NG_MAX_UNI)
    {
        strncpy(g_uni_names[g_nuni], n, 63);
        g_uni_names[g_nuni][63] = 0;
        found = g_nuni++;
    }
    if (found >= 0 && strcmp(n, "color") == 0) g_diffuse_loc = found;
    ng_unlock();
    return found;
}

static void ng_push_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    fsr_setcolor_t f = get_setcolor();
    {
        static int n = 0;
        if (n < 10)
        {
            char k[96];
            _snprintf(k, sizeof(k), "setcolor #%d (%.3f,%.3f,%.3f,%.3f) fn=%p", ++n, r, g, b, a, (void*)f);
            log_once("color", k);
        }
    }
    if (f) f(r, g, b, a);
}
static void __stdcall ng_uniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    if (loc >= 0 && loc == g_diffuse_loc) ng_push_color(x, y, z, w);
}
static void __stdcall ng_uniform4fv(GLint loc, GLsizei count, const GLfloat* v)
{
    if (loc >= 0 && loc == g_diffuse_loc && v && count >= 1) ng_push_color(v[0], v[1], v[2], v[3]);
}

/* ATTRIBUTE locations must be REAL. LLGLSLShader::mapAttributes() builds
 * mAttributeMask from these; -1 for everything makes the viewer believe no shader
 * consumes vertex attributes, so LLRender allocates ZERO-SIZE, ZERO-TYPEMASK buffers
 * and every UI draw carries no data (measured: typemask=0 size=0 nverts=36).
 * The viewer binds these to fixed indices matching the reserved list order
 * (llshadermgr.cpp:1209-1222), which is also the LLVertexBuffer type order. */
static const char* NG_ATTRIBS[] = {
    "position", "normal", "texcoord0", "texcoord1", "texcoord2", "texcoord3",
    "diffuse_color", "emissive", "tangent", "weight", "weight4", "clothing",
    "joint", "texture_index",
};
/* Shader-source tracking, so glGetAttribLocation can answer HONESTLY per program.
 * Reporting every attribute for every shader made LLRender build buffers with
 * attributes the shader never declares and fill them with stale data -- which we then
 * read as vertex colour (measured: red/orange UI text). Now we look at the real GLSL. */
#define NG_MAX_SHADERS 2048
#define NG_MAX_PROGS 512
typedef struct { GLuint id; char* src; } NgShader;
typedef struct { GLuint id; GLuint sh[8]; int n; } NgProg;
static NgShader g_shaders[NG_MAX_SHADERS];
static int g_nshaders = 0;
static NgProg g_progs[NG_MAX_PROGS];
static int g_nprogs = 0;

static NgShader* shader_find(GLuint id)
{
    for (int i = 0; i < g_nshaders; ++i) if (g_shaders[i].id == id) return &g_shaders[i];
    return NULL;
}
static NgProg* prog_find(GLuint id, int create)
{
    for (int i = 0; i < g_nprogs; ++i) if (g_progs[i].id == id) return &g_progs[i];
    if (!create || g_nprogs >= NG_MAX_PROGS) return NULL;
    NgProg* p = &g_progs[g_nprogs++];
    p->id = id; p->n = 0;
    return p;
}

static void __stdcall ng_shader_source(GLuint shader, GLsizei count, const char* const* strings, const GLint* lengths)
{
    if (!strings || count <= 0) return;
    size_t total = 1;
    for (GLsizei i = 0; i < count; ++i)
    {
        if (!strings[i]) continue;
        total += (lengths && lengths[i] > 0) ? (size_t)lengths[i] : strlen(strings[i]);
    }
    char* buf = (char*)malloc(total);
    if (!buf) return;
    size_t off = 0;
    for (GLsizei i = 0; i < count; ++i)
    {
        if (!strings[i]) continue;
        size_t len = (lengths && lengths[i] > 0) ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(buf + off, strings[i], len);
        off += len;
    }
    buf[off] = 0;
    ng_lock();
    NgShader* sh = shader_find(shader);
    if (sh) { free(sh->src); sh->src = buf; }
    else if (g_nshaders < NG_MAX_SHADERS) { g_shaders[g_nshaders].id = shader; g_shaders[g_nshaders].src = buf; g_nshaders++; }
    else free(buf);
    ng_unlock();
}
static void __stdcall ng_attach_shader(GLuint program, GLuint shader)
{
    ng_lock();
    NgProg* p = prog_find(program, 1);
    if (p && p->n < 8) p->sh[p->n++] = shader;
    ng_unlock();
}

/* Is `name` DECLARED as a vertex input in this source? Looks for the token on a line
 * that also carries an input qualifier ("in "/"attribute"/layout(location=...)). */
static int src_declares_attrib(const char* src, const char* name)
{
    if (!src || !name) return 0;
    size_t nlen = strlen(name);
    const char* p = src;
    while ((p = strstr(p, name)) != NULL)
    {
        char before = (p == src) ? (char)10 : p[-1];
        char after = p[nlen];
        int word = !(isalnum((unsigned char)before) || before == '_') &&
                   !(isalnum((unsigned char)after) || after == '_');
        if (word)
        {
            const char* ls = p;
            while (ls > src && ls[-1] != (char)10) --ls;
            const char* le = strchr(p, (int)10);
            size_t llen = le ? (size_t)(le - ls) : strlen(ls);
            if (llen < 512)
            {
                char line[512];
                memcpy(line, ls, llen);
                line[llen] = 0;
                if (strstr(line, "attribute ") || strstr(line, "layout") ||
                    (strncmp(line, "in ", 3) == 0) || strstr(line, " in ") || strstr(line, "	in "))
                {
                    return 1;
                }
            }
        }
        p += nlen;
    }
    return 0;
}

static GLint __stdcall ng_attrib_loc(GLuint prog, const char* n)
{
    if (!n) return -1;
    int idx = -1;
    for (int i = 0; i < (int)(sizeof(NG_ATTRIBS) / sizeof(NG_ATTRIBS[0])); ++i)
    {
        if (strcmp(NG_ATTRIBS[i], n) == 0) { idx = i; break; }
    }
    if (idx < 0) { log_once("attrib", n); return -1; }
    ng_lock();
    NgProg* p = prog_find(prog, 0);
    int declared = 0;
    if (p)
    {
        for (int i = 0; i < p->n && !declared; ++i)
        {
            NgShader* sh = shader_find(p->sh[i]);
            if (sh && src_declares_attrib(sh->src, n)) declared = 1;
        }
    }
    ng_unlock();
    return declared ? idx : -1;
}

/* Enumerate ACTIVE UNIFORMS from the real shader source. MEASURED: mapUniforms()
 * reads GL_ACTIVE_UNIFORMS and loops glGetActiveUniform; returning 0 meant the viewer
 * mapped NOTHING, so glUniform4f was never called and the engine never learned any
 * colour -- solid UI rects (the login background) stayed white. */
typedef struct { char name[64]; GLenum type; } NgUni;
#define NG_MAX_PROGUNI 128
typedef struct { GLuint prog; NgUni u[NG_MAX_PROGUNI]; int n; } NgProgUni;
#define NG_MAX_PROGUNIS 256
static NgProgUni g_proguni[NG_MAX_PROGUNIS];
static int g_nproguni = 0;

static GLenum glsl_type_enum(const char* t)
{
    if (!strcmp(t, "float")) return 0x1406;
    if (!strcmp(t, "vec2")) return 0x8B50;
    if (!strcmp(t, "vec3")) return 0x8B51;
    if (!strcmp(t, "vec4")) return 0x8B52;
    if (!strcmp(t, "int")) return 0x1404;
    if (!strcmp(t, "bool")) return 0x8B56;
    if (!strcmp(t, "mat3")) return 0x8B5B;
    if (!strcmp(t, "mat4")) return 0x8B5C;
    if (!strcmp(t, "sampler2D")) return 0x8B5E;
    if (!strcmp(t, "samplerCube")) return 0x8B60;
    if (!strcmp(t, "sampler2DShadow")) return 0x8B62;
    if (!strcmp(t, "sampler2DArray")) return 0x8DC1;
    return 0x1406;
}

static void proguni_scan_src(NgProgUni* pu, const char* src)
{
    if (!src) return;
    const char* p2 = src;
    while ((p2 = strstr(p2, "uniform")) != NULL)
    {
        char before = (p2 == src) ? (char)10 : p2[-1];
        if (isalnum((unsigned char)before) || before == '_') { p2 += 7; continue; }
        const char* q = p2 + 7;
        char type[64] = {0}, name[64] = {0};
        while (*q == ' ' || *q == 9) ++q;
        int i = 0;
        while (*q && (isalnum((unsigned char)*q) || *q == '_') && i < 63) type[i++] = *q++;
        while (*q == ' ' || *q == 9) ++q;
        i = 0;
        while (*q && (isalnum((unsigned char)*q) || *q == '_') && i < 63) name[i++] = *q++;
        while (*q == ' ' || *q == 9) ++q;
        if (name[0] && (*q == ';' || *q == '[') && pu->n < NG_MAX_PROGUNI)
        {
            int dup = 0;
            for (int k = 0; k < pu->n; ++k) if (!strcmp(pu->u[k].name, name)) { dup = 1; break; }
            if (!dup)
            {
                strncpy(pu->u[pu->n].name, name, 63);
                pu->u[pu->n].name[63] = 0;
                pu->u[pu->n].type = glsl_type_enum(type);
                pu->n++;
            }
        }
        p2 += 7;
    }
}

static NgProgUni* proguni_get(GLuint prog)
{
    for (int i = 0; i < g_nproguni; ++i) if (g_proguni[i].prog == prog) return &g_proguni[i];
    if (g_nproguni >= NG_MAX_PROGUNIS) return NULL;
    NgProgUni* pu = &g_proguni[g_nproguni++];
    pu->prog = prog;
    pu->n = 0;
    NgProg* pr = prog_find(prog, 0);
    if (pr)
    {
        for (int i = 0; i < pr->n; ++i)
        {
            NgShader* sh = shader_find(pr->sh[i]);
            if (sh) proguni_scan_src(pu, sh->src);
        }
    }
    return pu;
}

static int proguni_count(GLuint prog)
{
    ng_lock();
    NgProgUni* pu = proguni_get(prog);
    int n = pu ? pu->n : 0;
    ng_unlock();
    return n;
}

static void __stdcall ng_get_active_uniform(GLuint prog, GLuint index, GLsizei bufSize,
                                            GLsizei* length, GLint* size, GLenum* type, char* name)
{
    if (length) *length = 0;
    if (size) *size = 0;
    if (type) *type = 0x1406;
    if (name && bufSize > 0) name[0] = 0;
    ng_lock();
    NgProgUni* pu = proguni_get(prog);
    if (pu && (int)index < pu->n)
    {
        const NgUni* u = &pu->u[index];
        size_t len = strlen(u->name);
        if (name && bufSize > 0)
        {
            size_t nn = len < (size_t)bufSize - 1 ? len : (size_t)bufSize - 1;
            memcpy(name, u->name, nn);
            name[nn] = 0;
            if (length) *length = (GLsizei)nn;
        }
        if (size) *size = 1;
        if (type) *type = u->type;
    }
    ng_unlock();
}

static GLuint __stdcall ng_blockidx(GLuint p, const char* n) { (void)p; (void)n; return 0xFFFFFFFFu; }
static void __stdcall ng_infolog(GLuint o, GLsizei max, GLsizei* len, char* log)
{
    (void)o; (void)max;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}
static GLenum __stdcall ng_fbstatus(GLenum t) { (void)t; return 0x8CD5; } /* FRAMEBUFFER_COMPLETE */
static void* __stdcall ng_mapbuffer(GLenum t, GLenum a) { (void)t; (void)a; return scratch(0); }
static void* __stdcall ng_mapbufferrange(GLenum t, GLintptr o, GLsizeiptr l, unsigned int a) { (void)t; (void)o; (void)a; return scratch((size_t)l); }
static GLboolean __stdcall ng_unmap(GLenum t) { (void)t; return 1; }
static void __stdcall ng_queryiv(GLuint q, GLenum p, GLuint* out)
{
    (void)q;
    if (!out) return;
    if (p == 0x8867) out[0] = 1;              /* QUERY_RESULT_AVAILABLE */
    else out[0] = 0x7FFFFFFF;                 /* QUERY_RESULT: everything visible */
}
static void __stdcall ng_query64(GLuint q, GLenum p, GLuint64* out)
{
    (void)q;
    if (!out) return;
    if (p == 0x8867) out[0] = 1;
    else out[0] = 0x7FFFFFFF;
}
static void* __stdcall ng_fencesync(GLenum c, unsigned int f) { (void)c; (void)f; return (void*)(uintptr_t)0x50BAD; }
static GLenum __stdcall ng_clientwait(void* s, unsigned int f, GLuint64 t) { (void)s; (void)f; (void)t; return 0x911A; } /* ALREADY_SIGNALED */
static void __stdcall ng_getsynciv(void* s, GLenum p, GLsizei bufSize, GLsizei* length, GLint* values)
{
    (void)s; (void)bufSize;
    if (length) *length = 1;
    if (values) values[0] = (p == 0x9114) ? 0x9119 : 0; /* SYNC_STATUS -> SIGNALED */
}
static void __stdcall ng_getstringi_hook(void) {}
static const unsigned char* __stdcall ng_getstringi(GLenum name, GLuint i)
{
    if (name == 0x1F03)
    {
        ng_init_ext();
        if ((int)i < g_next) return (const unsigned char*)g_ext_tok[i];
    }
    return (const unsigned char*)"";
}
static void __stdcall ng_progbinary(GLuint p, GLsizei bufSize, GLsizei* length, GLenum* fmt, void* bin)
{
    (void)p; (void)bufSize; (void)bin;
    if (length) *length = 0;
    if (fmt) *fmt = 0;
}
static BOOL __stdcall ng_swapinterval(int i) { (void)i; return TRUE; }
static int __stdcall ng_getswapinterval(void) { return 1; }
static const char* __stdcall ng_wglext(HDC dc) { (void)dc; return "WGL_ARB_extensions_string WGL_ARB_pixel_format WGL_EXT_swap_control WGL_ARB_create_context"; }
static BOOL __stdcall ng_choosepf(HDC dc, const int* ia, const GLfloat* fa, unsigned int max, int* formats, unsigned int* count)
{
    (void)dc; (void)ia; (void)fa;
    if (formats && max > 0) formats[0] = 1;
    if (count) *count = 1;
    return TRUE;
}
static HGLRC __stdcall ng_createctxattribs(HDC dc, HGLRC share, const int* attribs) { (void)share; (void)attribs; return wglCreateContext(dc); }
static long long __stdcall ng_zero(void) { return 0; } /* the universal benign stub */

typedef struct { const char* name; PROC fn; } NgEntry;
static const NgEntry NG_SPECIAL[] = {
    { "glCreateShader", (PROC)ng_create },
    { "glCreateProgram", (PROC)ng_create0 },
    { "glGenBuffers", (PROC)ng_genN },
    { "glGenFramebuffers", (PROC)ng_genN },
    { "glGenRenderbuffers", (PROC)ng_genN },
    { "glGenVertexArrays", (PROC)ng_genN },
    { "glGenQueries", (PROC)ng_genN },
    { "glGenSamplers", (PROC)ng_genN },
    { "glGenTransformFeedbacks", (PROC)ng_genN },
    { "glGetShaderiv", (PROC)ng_shaderiv },
    { "glGetProgramiv", (PROC)ng_shaderiv },
    { "glGetShaderInfoLog", (PROC)ng_infolog },
    { "glGetProgramInfoLog", (PROC)ng_infolog },
    { "glGetUniformLocation", (PROC)ng_loc },
    { "glGetActiveUniform", (PROC)ng_get_active_uniform },
    { "glUniform4f", (PROC)ng_uniform4f },
    { "glUniform4fv", (PROC)ng_uniform4fv },
    { "glGetAttribLocation", (PROC)ng_attrib_loc },
    { "glShaderSource", (PROC)ng_shader_source },
    { "glAttachShader", (PROC)ng_attach_shader },
    { "glGetUniformBlockIndex", (PROC)ng_blockidx },
    { "glCheckFramebufferStatus", (PROC)ng_fbstatus },
    { "glMapBuffer", (PROC)ng_mapbuffer },
    { "glMapBufferRange", (PROC)ng_mapbufferrange },
    { "glUnmapBuffer", (PROC)ng_unmap },
    { "glGetQueryObjectiv", (PROC)ng_queryiv },
    { "glGetQueryObjectuiv", (PROC)ng_queryiv },
    { "glGetQueryObjectui64v", (PROC)ng_query64 },
    { "glFenceSync", (PROC)ng_fencesync },
    { "glClientWaitSync", (PROC)ng_clientwait },
    { "glGetSynciv", (PROC)ng_getsynciv },
    { "glGetStringi", (PROC)ng_getstringi },
    { "glGetProgramBinary", (PROC)ng_progbinary },
    { "wglSwapIntervalEXT", (PROC)ng_swapinterval },
    { "wglGetSwapIntervalEXT", (PROC)ng_getswapinterval },
    { "wglGetExtensionsStringARB", (PROC)ng_wglext },
    { "wglGetExtensionsStringEXT", (PROC)ng_wglext },
    { "wglChoosePixelFormatARB", (PROC)ng_choosepf },
    { "wglCreateContextAttribsARB", (PROC)ng_createctxattribs },
    { "glGetIntegerv", (PROC)glGetIntegerv },
    { "glGetString", (PROC)glGetString },
};

NG_API PROC __stdcall wglGetProcAddress(const char* name)
{
    if (!name) return (PROC)ng_zero;
    for (size_t i = 0; i < sizeof(NG_SPECIAL) / sizeof(NG_SPECIAL[0]); ++i)
    {
        if (strcmp(NG_SPECIAL[i].name, name) == 0) return NG_SPECIAL[i].fn;
    }
    log_once("gpa", name);
    return (PROC)ng_zero;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH)
    {
        OutputDebugStringA("nullgl: attached (engine mode -- GL is a no-op, fs_render renders)\n");
    }
    return TRUE;
}

/* unreferenced hook silencer */
void ng_unused(void) { ng_getstringi_hook(); }
