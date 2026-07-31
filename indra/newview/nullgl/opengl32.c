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

/* =====================  the 47 measured delayload exports  ===================== */
/* no-op state setters */
NG_API void __stdcall glBindTexture(GLenum t, GLuint x) { (void)t; (void)x; }
NG_API void __stdcall glBlendFunc(GLenum a, GLenum b) { (void)a; (void)b; }
NG_API void __stdcall glClear(unsigned int m) { (void)m; }
NG_API void __stdcall glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void)r; (void)g; (void)b; (void)a; }
NG_API void __stdcall glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { (void)r; (void)g; (void)b; (void)a; }
NG_API void __stdcall glCopyTexSubImage2D(GLenum a, GLint b, GLint c, GLint d, GLint e, GLint f, GLsizei g, GLsizei h) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; }
NG_API void __stdcall glCullFace(GLenum m) { (void)m; }
NG_API void __stdcall glDeleteTextures(GLsizei n, const GLuint* t) { (void)n; (void)t; }
NG_API void __stdcall glDepthFunc(GLenum f) { (void)f; }
NG_API void __stdcall glDepthMask(GLboolean f) { (void)f; }
NG_API void __stdcall glDisable(GLenum c) { (void)c; }
NG_API void __stdcall glDrawArrays(GLenum m, GLint f, GLsizei c) { (void)m; (void)f; (void)c; }
NG_API void __stdcall glDrawBuffer(GLenum b) { (void)b; }
NG_API void __stdcall glEnable(GLenum c) { (void)c; }
NG_API void __stdcall glFinish(void) {}
NG_API void __stdcall glFlush(void) {}
NG_API void __stdcall glHint(GLenum t, GLenum m) { (void)t; (void)m; }
NG_API void __stdcall glLineWidth(GLfloat w) { (void)w; }
NG_API void __stdcall glMaterialfv(GLenum f, GLenum p, const GLfloat* v) { (void)f; (void)p; (void)v; }
NG_API void __stdcall glMateriali(GLenum f, GLenum p, GLint v) { (void)f; (void)p; (void)v; }
NG_API void __stdcall glPixelStorei(GLenum p, GLint v) { (void)p; (void)v; }
NG_API void __stdcall glPointSize(GLfloat s) { (void)s; }
NG_API void __stdcall glPolygonMode(GLenum f, GLenum m) { (void)f; (void)m; }
NG_API void __stdcall glPolygonOffset(GLfloat a, GLfloat b) { (void)a; (void)b; }
NG_API void __stdcall glReadBuffer(GLenum b) { (void)b; }
NG_API void __stdcall glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x; (void)y; (void)w; (void)h; }
NG_API void __stdcall glTexImage2D(GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const void* p) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)p; }
NG_API void __stdcall glTexParameterf(GLenum t, GLenum p, GLfloat v) { (void)t; (void)p; (void)v; }
NG_API void __stdcall glTexParameteri(GLenum t, GLenum p, GLint v) { (void)t; (void)p; (void)v; }
NG_API void __stdcall glTexParameteriv(GLenum t, GLenum p, const GLint* v) { (void)t; (void)p; (void)v; }
NG_API void __stdcall glTexSubImage2D(GLenum a, GLint b, GLint c, GLint d, GLsizei e, GLsizei f, GLenum g, GLenum h, const void* p) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)p; }
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
NG_API GLboolean __stdcall glIsEnabled(GLenum cap) { (void)cap; return 0; }
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

/* =====================  wglGetProcAddress special table  ====================== */
static GLuint __stdcall ng_create(GLenum t) { (void)t; return next_id(); }
static GLuint __stdcall ng_create0(void) { return next_id(); }
static void __stdcall ng_genN(GLsizei n, GLuint* ids) { gen_ids(n, ids); }
static void __stdcall ng_shaderiv(GLuint s, GLenum p, GLint* out)
{
    (void)s;
    if (!out) return;
    switch (p)
    {
    case 0x8B81: case 0x8B82: case 0x8B83: out[0] = 1; break; /* COMPILE/LINK/VALIDATE TRUE */
    case 0x8B84: out[0] = 0; break;                            /* INFO_LOG_LENGTH */
    case 0x8B86: case 0x8B89: out[0] = 0; break;               /* ACTIVE_UNIFORMS/ATTRIBUTES */
    case 0x8B87: case 0x8B8A: out[0] = 1; break;               /* ACTIVE_*_MAX_LENGTH */
    case 0x8741: out[0] = 0; break;                            /* PROGRAM_BINARY_LENGTH */
    case 0x8A36: out[0] = 0; break;                            /* ACTIVE_UNIFORM_BLOCKS */
    default: out[0] = 0; break;
    }
}
static GLint __stdcall ng_loc(GLuint p, const char* n) { (void)p; (void)n; return -1; }
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
    { "glGetAttribLocation", (PROC)ng_loc },
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
