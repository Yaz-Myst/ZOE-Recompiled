// OpenGL GS rasterizer. See gs_gpu_backend.h for the architecture.
#include "ps2x/state_io.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "ps2_debug_trace.h"
#include "ps2_host_settings.h"
#include "ps2_frame_log.h"
#include "ps2_game_clock.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#ifndef APIENTRY
#define APIENTRY
#endif

#include <algorithm>
#include <map>
#include <set>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

extern std::atomic<const char *> g_ps2ActiveOverlayName;

// ---------------------------------------------------------------------------------------------
// Minimal OpenGL 3.3 loader (the window's context is created by raylib; we only need the
// function pointers). Loaded lazily on the GL thread.
// ---------------------------------------------------------------------------------------------
namespace gl
{
    using GLenum = unsigned int;
    using GLuint = unsigned int;
    using GLint = int;
    using GLsizei = int;
    using GLboolean = unsigned char;
    using GLfloat = float;
    using GLbitfield = unsigned int;
    using GLchar = char;
    using GLsizeiptr = ptrdiff_t;
    using GLintptr = ptrdiff_t;

    constexpr GLenum TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0, TEXTURE1 = 0x84C1, RED = 0x1903, R8 = 0x8229,
                     RGBA = 0x1908, RGBA8 = 0x8058, RGBA16F = 0x881A,
                     UNSIGNED_BYTE = 0x1401, FLOAT = 0x1406, HALF_FLOAT = 0x140B, TEXTURE_MIN_FILTER = 0x2801,
                     TEXTURE_MAG_FILTER = 0x2800, NEAREST = 0x2600, LINEAR = 0x2601, TEXTURE_WRAP_S = 0x2802,
                     TEXTURE_WRAP_T = 0x2803, CLAMP_TO_EDGE = 0x812F, FRAMEBUFFER = 0x8D40, READ_FRAMEBUFFER = 0x8CA8,
                     DRAW_FRAMEBUFFER = 0x8CA9, COLOR_ATTACHMENT0 = 0x8CE0, DEPTH_ATTACHMENT = 0x8D00,
                     FRAMEBUFFER_COMPLETE = 0x8CD5, DEPTH_COMPONENT = 0x1902, DEPTH_COMPONENT32F = 0x8CAC,
                     ARRAY_BUFFER = 0x8892, STREAM_DRAW = 0x88E0, VERTEX_SHADER = 0x8B31, FRAGMENT_SHADER = 0x8B30,
                     COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82, INFO_LOG_LENGTH = 0x8B84, TRIANGLES = 0x0004,
                     LINES = 0x0001, POINTS = 0x0000, TRIANGLE_STRIP = 0x0005, BLEND = 0x0BE2, DEPTH_TEST = 0x0B71,
                     SCISSOR_TEST = 0x0C11, CULL_FACE = 0x0B44, ZERO = 0, ONE = 1, SRC_ALPHA = 0x0302,
                     ONE_MINUS_SRC_ALPHA = 0x0303, DST_ALPHA = 0x0304, ONE_MINUS_DST_ALPHA = 0x0305,
                     CONSTANT_ALPHA = 0x8003, ONE_MINUS_CONSTANT_ALPHA = 0x8004, FUNC_ADD = 0x8006,
                     FUNC_SUBTRACT = 0x800A, FUNC_REVERSE_SUBTRACT = 0x800B, NEVER = 0x0200, ALWAYS = 0x0207,
                     GEQUAL = 0x0206, GREATER = 0x0204, COLOR_BUFFER_BIT = 0x4000, DEPTH_BUFFER_BIT = 0x0100,
                     VIEWPORT = 0x0BA2, CURRENT_PROGRAM = 0x8B8D, VERTEX_ARRAY_BINDING = 0x85B5,
                     ARRAY_BUFFER_BINDING = 0x8894, ACTIVE_TEXTURE = 0x84E0, TEXTURE_BINDING_2D = 0x8069,
                     FRAMEBUFFER_BINDING = 0x8CA6, UNPACK_ALIGNMENT = 0x0CF5, DEPTH_WRITEMASK = 0x0B72,
                     BLEND_SRC_RGB = 0x80C9, BLEND_DST_RGB = 0x80C8, BLEND_SRC_ALPHA = 0x80CB, BLEND_DST_ALPHA = 0x80CA,
                     BLEND_EQUATION_RGB = 0x8009, BLEND_EQUATION_ALPHA = 0x883D, SRC1_ALPHA = 0x8589,
                     ONE_MINUS_SRC1_ALPHA = 0x88FB, DEPTH_STENCIL = 0x84F9, DEPTH32F_STENCIL8 = 0x8CAD,
                     FLOAT_32_UNSIGNED_INT_24_8_REV = 0x8DAD, DEPTH_STENCIL_ATTACHMENT = 0x821A,
                     STENCIL_TEST = 0x0B90, STENCIL_BUFFER_BIT = 0x0400, KEEP = 0x1E00, REPLACE = 0x1E01,
                     EQUAL = 0x0202, MAX_TEXTURE_SIZE = 0x0D33, LINEAR_MIPMAP_LINEAR = 0x2703,
                     PROGRAM_POINT_SIZE = 0x8642, TIME_ELAPSED = 0x88BF, QUERY_RESULT = 0x8866,
                     QUERY_RESULT_AVAILABLE = 0x8867, TIMESTAMP = 0x8E28;
    using GLuint64 = unsigned long long;

#define PS2X_GL_FUNCS(X)                                                                                   \
    X(void, GenTextures, (GLsizei, GLuint *))                                                              \
    X(void, DeleteTextures, (GLsizei, const GLuint *))                                                     \
    X(void, BindTexture, (GLenum, GLuint))                                                                 \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))     \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))  \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                                        \
    X(void, ActiveTexture, (GLenum))                                                                       \
    X(void, GenFramebuffers, (GLsizei, GLuint *))                                                          \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                             \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                 \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                            \
    X(void, GenBuffers, (GLsizei, GLuint *))                                                               \
    X(void, BindBuffer, (GLenum, GLuint))                                                                  \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum))                                        \
    X(void, GenVertexArrays, (GLsizei, GLuint *))                                                          \
    X(void, BindVertexArray, (GLuint))                                                                     \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))                \
    X(void, EnableVertexAttribArray, (GLuint))                                                             \
    X(GLuint, CreateShader, (GLenum))                                                                      \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))                         \
    X(void, CompileShader, (GLuint))                                                                       \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *))                                                        \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                                      \
    X(GLuint, CreateProgram, ())                                                                           \
    X(void, AttachShader, (GLuint, GLuint))                                                                \
    X(void, LinkProgram, (GLuint))                                                                         \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *))                                                       \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                                     \
    X(void, UseProgram, (GLuint))                                                                          \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *))                                                 \
    X(void, Uniform1i, (GLint, GLint))                                                                     \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat))                                                          \
    X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat))                                                 \
    X(void, Uniform2i, (GLint, GLint, GLint))                                                              \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                        \
    X(void, ClearDepth, (double))                                                                          \
    X(void, Uniform4i, (GLint, GLint, GLint, GLint, GLint))                                                \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                                    \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                                     \
    X(void, Enable, (GLenum))                                                                              \
    X(void, Disable, (GLenum))                                                                             \
    X(GLboolean, IsEnabled, (GLenum))                                                                      \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                           \
    X(void, BlendEquationSeparate, (GLenum, GLenum))                                                       \
    X(void, BlendColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                              \
    X(void, DepthFunc, (GLenum))                                                                           \
    X(void, DepthMask, (GLboolean))                                                                        \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                                       \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                              \
    X(void, Clear, (GLbitfield))                                                                           \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                                                          \
    X(void, GetIntegerv, (GLenum, GLint *))                                                                \
    X(void, PixelStorei, (GLenum, GLint))                                                                  \
    X(void, GetTexImage, (GLenum, GLint, GLenum, GLenum, void *))                                          \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, Uniform3i, (GLint, GLint, GLint, GLint))                                                      \
    X(void, StencilFunc, (GLenum, GLint, GLuint))                                                          \
    X(void, StencilOp, (GLenum, GLenum, GLenum))                                                           \
    X(void, StencilMask, (GLuint))                                                                         \
    X(void, ClearStencil, (GLint))                                                                         \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *))                                                 \
    X(void, GenerateMipmap, (GLenum))                                                                      \
    X(void, LineWidth, (GLfloat))                                                                          \
    X(void, GenQueries, (GLsizei, GLuint *))                                                               \
    X(void, BeginQuery, (GLenum, GLuint))                                                                  \
    X(void, EndQuery, (GLenum))                                                                            \
    X(void, GetQueryObjectiv, (GLuint, GLenum, GLint *))                                                   \
    X(void, GetQueryObjectui64v, (GLuint, GLenum, GLuint64 *))                                             \
    X(void, QueryCounter, (GLuint, GLenum))

#define PS2X_GL_DECLARE(ret, name, args) \
    using PFN_##name = ret(APIENTRY *) args; \
    PFN_##name name = nullptr;
    PS2X_GL_FUNCS(PS2X_GL_DECLARE)
#undef PS2X_GL_DECLARE

    bool g_loaded = false;

    bool load()
    {
        if (g_loaded)
            return true;
#if defined(_WIN32)
        HMODULE opengl = GetModuleHandleA("opengl32.dll");
        auto get = [&](const char *name) -> void *
        {
            void *p = reinterpret_cast<void *>(wglGetProcAddress(name));
            if (p == nullptr || p == reinterpret_cast<void *>(1) || p == reinterpret_cast<void *>(2) ||
                p == reinterpret_cast<void *>(3) || p == reinterpret_cast<void *>(-1))
                p = opengl ? reinterpret_cast<void *>(GetProcAddress(opengl, name)) : nullptr;
            return p;
        };
        bool ok = true;
#define PS2X_GL_LOAD(ret, name, args)                                        \
    name = reinterpret_cast<PFN_##name>(get("gl" #name));                    \
    if (!name)                                                               \
    {                                                                        \
        std::cerr << "[gpu] missing GL function gl" #name << std::endl;      \
        ok = false;                                                          \
    }
        PS2X_GL_FUNCS(PS2X_GL_LOAD)
#undef PS2X_GL_LOAD
        g_loaded = ok;
        return ok;
#else
        return false;
#endif
    }
}

namespace
{
    // Per framebuffer page: was its latest draw made with a half-line Y drawing offset (XYOFFSET.OFY fraction)?
    // Games rendering half-height fields in interlaced FRAME mode alternate this per field
    // (libgraph sceGsSetHalfOffset); the presenter puts such frames one output line lower, like a TV.
    std::array<std::atomic<uint8_t>, 512> g_fbHalfLineOffset{};
    // Set by the presenter while a displayed circuit uses half-height interlaced fields. In progressive mode
    // the draws then drop that half-line offset, so both fields land on the same lines.
    std::atomic<bool> g_halfHeightFields{false};
    // Internal resolution (render targets drawn this many times larger per axis), set by the presenter on the GL
    // thread. Draw recording reads it for sprite texture coordinates (see Submit).
    std::atomic<uint32_t> g_scaleX{1}, g_scaleY{1};

    // Tight sample range for scaled sprites: a sprite that is not drawn 1:1 (a game squeezing its 2D UI for a wider
    // picture, ZOE's 16:9 patch narrows panels by 0.75) can place its first covered pixel exactly on its rectangle's
    // left texel boundary, and bilinear + REPEAT then blends 50 % of the texel before the rectangle, the far side of
    // the texture. Measured case: ZOE's item-acquired box, right edge sprite local x 186..194 (8 texels, only its last
    // two carry the green outline), stock HUD 346..354 -> narrowed 323.5..329.5 = GS x 2115..2121 (an integer edge):
    // a one-pixel green line at HUD x 323, at x1 and x3 alike; stock 4:3 (2137.5..2145.5, first pixel on a texel
    // centre) never showed it. With this on, a scaled sprite keeps its samples inside the whole texels its rectangle
    // touches, horizontally: from the centre of texel floor(min) to the centre of texel ceil(max) - 1, so a bilinear
    // footprint never reaches a texel the rectangle does not cover. floor/ceil rather than min + 0.5 / max - 0.5,
    // because games also place rectangles on half texels (the +0.5 bilinear convention): ZOE's STQ sprite
    // s 0.05..0.7125 on a 10-texel texture is 0.5..7.125 texels, whose range is 0.5..7.5, so its native first sample
    // (0.68) is kept; min + 0.5 would have pushed it to 1.0 and shaded its edge column differently (measured: 176 px
    // differing in the item scene, 621 in a battle frame). For a rectangle on whole texels (the box: 0..8) both agree.
    // Only MINIFIED axes (more than one texel per GS pixel) are tightened: a squeeze only ever minifies, and a magnified
    // sprite's edge blending is what the GS itself draws in 4:3 too (review, 11 Oct: with magnified axes included a
    // 2-row-high HUD band in a battle frame changed although the squeeze never touches the y axis).
    // Generic runtime state: on while the game's widescreen is active (ps2WidescreenActive), PS2X_GPU_SPRITECLAMP=0
    // forces it off, =1 forces it on (to test in 4:3). 1:1 and magnified sprites, triangles and lines are never touched.
    bool spriteTightClampOn()
    {
        static const int s_forced = []
        {
            const char *v = std::getenv("PS2X_GPU_SPRITECLAMP");
            return (v != nullptr && *v != 0) ? (std::atoi(v) != 0 ? 1 : 0) : -1;
        }();
        return s_forced >= 0 ? s_forced == 1 : ps2WidescreenActive();
    }

    // Vertical re-fit of minified STQ sprites at internal resolution > 1 (PS2X_GPU_SPRITEREFIT=0 turns it off). ZOE draws
    // its text and HUD art from 448-line images (2 texels per GS row), and the snapped sprite rows reach up to ~0.75 row
    // past the sprite's own texture range; the old code extrapolated t to the snapped rows and then held the last native
    // sample, so the last texel row of a glyph was drawn 7 GL rows tall instead of 3 at x3 (the blob under the subtitle "g",
    // the bright stems under the ammo digits; same in 4:3; user report 16 Oct, save ws_ada_speak.p2s). The sprite's own t
    // range is now spread over the snapped rows and clamped to it (nearest: [t0, t1); bilinear: half a texel inside).
    // x1 is unchanged (the native-sample clamp is not applied there).
    bool spriteRefitOn()
    {
        static const bool s_on = []
        {
            const char *v = std::getenv("PS2X_GPU_SPRITEREFIT");
            return !(v != nullptr && *v != 0 && std::atoi(v) == 0);
        }();
        return s_on;
    }

    // Narrows [lo, hi] (the sprite's sample range on one axis, +-1e30 = none yet) to the range of the whole texels the
    // sprite's own rectangle touches (see above). c0, c1: the unsnapped texture coordinates at its two edges;
    // unitsToTexels: texels per coordinate unit (1 for FST, the texture size for STQ's s/q); gsSpan: its unsnapped
    // width in GS pixels.
    void tightenSpriteRange(float c0, float c1, float unitsToTexels, float gsSpan, float &lo, float &hi)
    {
        const float texels = std::fabs(c1 - c0) * unitsToTexels;
        if (!(gsSpan > 0.0f) || !(unitsToTexels > 0.0f) || !(texels / gsSpan > 1.001f))
            return; // 1:1 or magnified (or no extent on this axis): left exactly as the GS samples it
        // Texel units; the small offsets keep s * size = 2.9999998 (float rounding) from counting as texel 2.
        const float eps = 1e-4f;
        const float tmin = std::min(c0, c1) * unitsToTexels, tmax = std::max(c0, c1) * unitsToTexels;
        float tlo = std::floor(tmin + eps) + 0.5f, thi = std::ceil(tmax - eps) - 0.5f;
        if (tlo > thi)
            tlo = thi = 0.5f * (tlo + thi); // a rectangle of no width on a texel boundary: that boundary
        tlo /= unitsToTexels;
        thi /= unitsToTexels;
        const float nlo = lo, nhi = hi; // the native-sample range
        lo = std::max(nlo, tlo);
        hi = std::min(nhi, thi);
        if (lo > hi)
        {
            // No overlap: the native samples lie wholly on one side of the covered texels. Take the covered bound
            // nearest to them (a midpoint would sit outside the covered texels and still blend part of the far edge:
            // a one-texel strip starting on an integer x, native [0, 0], tight [0.5, 0.5] -> 0.25 = 25 % bleed).
            lo = hi = (nhi < tlo) ? tlo : thi;
        }
    }

    // GPU vertex: GS pixel coordinates, normalized depth, STQ (or UV texels), colour and fog.
    struct GpuVertex
    {
        float x, y, depth;
        float s, t, q;
        uint8_t r, g, b, a;
        float fog;
        // Sprites: the range of texture coordinates (same units as s, t) the GS samples at its own resolution. At a
        // higher internal resolution the extra samples near the edges are kept inside it, so bilinear filtering
        // does not reach past the sprite's image into the next one on the sheet. Everything else: unlimited.
        float cu0, cv0, cu1, cv1;
    };

    enum PrimClass : uint8_t
    {
        kPrimTriangles = 0,
        kPrimLines = 1,
        kPrimPoints = 2,
    };

    // Everything that changes how a batch is rasterized. Zero-initialized and compared with memcmp.
    struct GpuDrawState
    {
        uint32_t fbp, fbw, fbmsk, zbp;
        uint32_t textureId;
        uint32_t paletteId; // indexed texture: textureId holds 8-bit palette indices, paletteId the 256x1 colours
        uint16_t sc[4];
        uint16_t minu, maxu, minv, maxv;
        uint16_t tw, th;
        uint8_t fpsm, zpsm, zmsk, zte, ztst;
        uint8_t ate, atst, aref, afail, date, datm;
        uint8_t abe, blendA, blendB, blendC, blendD, fix, fba, colclamp;
        uint8_t tme, tfx, tcc, fst, linear, wms, wmt;
        uint8_t fge, fogR, fogG, fogB;
        uint8_t primClass;
        // Texture sampled straight from a GPU render target (render-to-texture).
        uint8_t texRt;     // 0 = decoded texture, 1 = RT as CT32, 2 = RT as CT24, 3 = RT as CT16
        uint8_t ta0, ta1, aem;
        uint16_t texOffX, texOffY;
        uint32_t texRtFbp, texRtFbw;
    };

    // Everything that selects which decoded texture a draw samples.
    struct TextureKey
    {
        uint64_t packed; // tbp0, tbw, psm, cpsm, csm, csa, texa
        uint32_t size;   // textureWidth | textureHeight << 16
        bool operator==(const TextureKey &o) const { return packed == o.packed && size == o.size; }
    };

    struct TextureKeyHash
    {
        size_t operator()(const TextureKey &k) const
        {
            return static_cast<size_t>(k.packed * 0x9E3779B97F4A7C15ull) ^ k.size;
        }
    };

    enum class CmdType : uint8_t
    {
        Draw,
        TextureUpload,
        Clear,
        FramebufferWrite,
        CopyRect,
    };

    struct Command
    {
        CmdType type;
        uint8_t texFormat = 0;   // TextureUpload: 0 = RGBA8 words, 1 = 8-bit palette indices (packed bytes)
        uint32_t state = 0;      // Draw: index into states
        uint32_t first = 0;      // Draw: first vertex; uploads: byte offset into pixel pool
        uint32_t count = 0;      // Draw: vertex count
        uint32_t id = 0;         // TextureUpload: texture id; Clear/FramebufferWrite: fbp
        uint32_t fbw = 0;        // Clear/FramebufferWrite
        uint32_t x = 0, y = 0, w = 0, h = 0; // rects / texture size
        uint32_t rgba = 0;       // Clear colour
        uint32_t dstFbp = 0, dstFbw = 0, dx = 0, dy = 0; // CopyRect destination
    };

    struct CommandList
    {
        std::vector<Command> commands;
        std::vector<GpuDrawState> states;
        std::vector<GpuVertex> vertices;
        std::vector<uint32_t> pixels; // RGBA8 payloads for uploads
        void clear()
        {
            commands.clear();
            states.clear();
            vertices.clear();
            pixels.clear();
        }
    };

    constexpr uint32_t kPageBytes = 8192u;
    constexpr uint32_t kPageCount = 4u * 1024u * 1024u / kPageBytes; // 512

    // Page geometry (pixels) per storage format, used for conservative dirty tracking.
    void pageDims(uint32_t psm, uint32_t &pw, uint32_t &ph)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            pw = 64; ph = 64; return;
        case GS_PSM_T8:
            pw = 128; ph = 64; return;
        case GS_PSM_T4:
            pw = 128; ph = 128; return;
        default:
            pw = 64; ph = 32; return; // 32-bit formats (incl. T8H/T4HL/T4HH stored in CT32 pages)
        }
    }

    // Pages touched by a width x height rectangle at base block `bp` with buffer width `bw`.
    void pageSpan(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t rows, uint32_t &first, uint32_t &last)
    {
        uint32_t pw, ph;
        pageDims(psm, pw, ph);
        const uint32_t pagesPerRow = std::max<uint32_t>(1u, (std::max<uint32_t>(bw, 1u) * 64u + pw - 1u) / pw);
        const uint32_t pageRows = std::max<uint32_t>(1u, (rows + ph - 1u) / ph);
        first = (bp / 32u) % kPageCount;
        last = std::min<uint32_t>(kPageCount - 1u, first + pagesPerRow * pageRows);
    }

    // Calls fn(page) for every page a w x h rectangle at (x, y) of buffer (bp, bw, psm) touches, following the GS
    // address formula (page = row * pagesPerRow + column, so columns past the buffer width spill into the next row).
    // A base pointer that is not page aligned also touches the following page.
    template <typename Fn>
    void forEachRectPage(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h, Fn &&fn)
    {
        if (w == 0u || h == 0u)
            return;
        uint32_t pw, ph;
        pageDims(psm, pw, ph);
        const uint32_t pagesPerRow = std::max<uint32_t>(1u, (std::max<uint32_t>(bw, 1u) * 64u + pw - 1u) / pw);
        const uint32_t px0 = x / pw, px1 = (x + w - 1u) / pw;
        const uint32_t py0 = y / ph, py1 = (y + h - 1u) / ph;
        const uint32_t base = bp / 32u;
        const bool straddles = (bp & 31u) != 0u;
        // Local memory ends at the last page: the whole-page estimate (and the extra page of an unaligned base) must not
        // wrap round to page 0, usually the front framebuffer. ZOE uploads a 512x312 T4 image at 0x3e90 every frame
        // during explosions; wrapping released framebuffer A, so the glow pass read stale memory on alternate frames.
        for (uint32_t py = py0; py <= py1; ++py)
            for (uint32_t px = px0; px <= px1; ++px)
            {
                const uint32_t page = base + py * pagesPerRow + px;
                if (page < kPageCount)
                    fn(page);
                if (straddles && page + 1u < kPageCount)
                    fn(page + 1u);
            }
    }

    constexpr uint32_t kRtHeight = 1024u;

    // Rows a render target allocates for a need of `rows`: whole 32-row pages, at most kRtHeight. Targets start
    // with the rows first used and grow on demand; at a large internal resolution a full 1024-row target costs
    // ~0.6 GB (ZOE's frame buffers use 224 rows).
    uint32_t roundRows(uint32_t rows)
    {
        return std::min(kRtHeight, std::max(32u, (rows + 31u) & ~31u));
    }

    // GS block order inside a page ([block row][block column]); 32-bit and 8-bit pages share one
    // table, 16-bit and 4-bit pages the other.
    constexpr uint8_t kBlockTable32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21},
                                             {2, 3, 6, 7, 18, 19, 22, 23},
                                             {8, 9, 12, 13, 24, 25, 28, 29},
                                             {10, 11, 14, 15, 26, 27, 30, 31}};
    constexpr uint8_t kBlockTable16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
                                             {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};

    struct BlockGeometry
    {
        uint32_t pageW, pageH, blockW, blockH, tableCols;
        const uint8_t *table;
    };

    bool blockGeometry(uint32_t psm, BlockGeometry &g)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24:
        case GS_PSM_T8H:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            g = {64, 32, 8, 8, 8, &kBlockTable32[0][0]};
            return true;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            g = {64, 64, 16, 8, 4, &kBlockTable16[0][0]};
            return true;
        case GS_PSM_T8:
            g = {128, 64, 16, 16, 8, &kBlockTable32[0][0]};
            return true;
        case GS_PSM_T4:
            g = {128, 128, 32, 16, 4, &kBlockTable16[0][0]};
            return true;
        default:
            return false; // Z formats: fall back to hashing whole pages
        }
    }

    bool isFourBitPsm(uint32_t psm) { return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH; }

    // Texture decode cost on the EE thread (reported with the presentation stats).
    std::atomic<uint64_t> g_decodeNs{0}, g_decodePixels{0};
    std::atomic<uint64_t> g_contentHits{0};
    // PS2X_TEXDBG: why textures are re-validated.
    bool texDbg()
    {
        static const bool on = std::getenv("PS2X_TEXDBG") != nullptr;
        return on;
    }
    struct TexDbg
    {
        uint64_t cpuWrites = 0, cpuPages = 0, claimBumps = 0, rehash = 0;
        uint8_t pageSource[512]{};    // 1 cpu write, 2 claim, 3 release
        uint32_t pageWriterDbp[512]{}; // cpu write dbp
        std::map<uint64_t, uint64_t> rehashBy; // (tbp<<32 | writer dbp<<8 | source) -> count
    } g_texDbg;
    std::atomic<uint32_t> g_presentCount{0};

    // PS2X_GPU_TRACE=1 traces everywhere; PS2X_GPU_TRACE=<overlay name> (e.g. GAME) only there.
    bool gpuTraceActive()
    {
        static const char *env = std::getenv("PS2X_GPU_TRACE");
        if (env == nullptr || *env == 0)
            return false;
        if (std::strcmp(env, "1") == 0)
            return true;
        const char *mode = g_ps2ActiveOverlayName.load(std::memory_order_relaxed);
        return mode != nullptr && std::strcmp(mode, env) == 0;
    }
}

struct GSGpuBackend::Impl
{
    GSCpuBackend cpu;

    // ---- EE-thread recording state (guarded by recordMutex) ----
    std::mutex recordMutex;
    CommandList recording;
    std::array<uint32_t, kPageCount> pageVersion{};
    uint32_t versionCounter = 1;
    uint32_t clutVersion = 1;
    // Last CLUT load actually performed (LoadClut skips an identical reload).
    bool lastClutLoadValid = false;
    uint64_t lastClutLoadKey = 0;
    uint32_t lastClutLoadPageVersion = 0;
    GSTransferCommand transfer{};
    bool transferActive = false;

    uint8_t *vram = nullptr;

    struct TextureEntry
    {
        uint32_t id;
        uint32_t paletteId; // indexed textures: id holds the texel indices, paletteId the colours (see resolveTexture)
        uint32_t firstPage, lastPage;
        uint32_t pageVersionMax;
        uint32_t clutVersion;
        uint64_t clutHash;
        uint64_t contentKey, paletteKey;
        uint64_t blockHash; // hash of the texel blocks at pageVersionMax (reused when only the CLUT changed)
    };
    struct TexIds
    {
        uint32_t tex = 0, pal = 0;
    };
    // Decoded textures by content (texture parameters + raw page bytes + CLUT), so data the game
    // re-uploads every frame, or several textures cycled through one slot, decode only once.
    struct ContentEntry
    {
        uint32_t id;
        uint32_t lastUsed;
    };
    std::unordered_map<uint64_t, ContentEntry> contentCache;
    std::vector<uint32_t> freeTextureIds;

    uint64_t hashPages(uint32_t first, uint32_t last) const
    {
        if (vram == nullptr || first > last)
        {
            static uint64_t s_unique = 0; // no access to local memory: never share decoded textures
            return ++s_unique * 0x2545F4914F6CDD1Dull;
        }
        const auto *q = reinterpret_cast<const uint64_t *>(vram + static_cast<size_t>(first) * kPageBytes);
        const size_t n = static_cast<size_t>(last - first + 1u) * kPageBytes / 8u;
        constexpr uint64_t K = 0x9E3779B97F4A7C15ull;
        uint64_t h0 = 1, h1 = 2, h2 = 3, h3 = 4;
        for (size_t i = 0; i + 4 <= n; i += 4)
        {
            h0 = (h0 ^ q[i]) * K;
            h1 = (h1 ^ q[i + 1]) * K;
            h2 = (h2 ^ q[i + 2]) * K;
            h3 = (h3 ^ q[i + 3]) * K;
        }
        return (h0 ^ (h1 >> 17) ^ (h1 << 47)) * K ^ (h2 ^ (h3 >> 29) ^ (h3 << 35));
    }

    // Hash of exactly the 256-byte blocks a w x h texture reads (so neighbouring textures in the
    // same page don't invalidate it).
    uint64_t hashTextureBlocks(uint32_t tbp0, uint32_t tbw, uint32_t psm, uint32_t w, uint32_t h,
                               uint32_t firstPage, uint32_t lastPage) const
    {
        BlockGeometry g{};
        if (vram == nullptr || !blockGeometry(psm, g))
            return hashPages(firstPage, lastPage);
        const uint32_t pagesPerRow = std::max<uint32_t>(1u, std::max<uint32_t>(tbw, 1u) * 64u / g.pageW);
        const uint32_t bxCount = (w + g.blockW - 1u) / g.blockW, byCount = (h + g.blockH - 1u) / g.blockH;
        // Page and block sizes are powers of two: shifts and masks instead of ~5 divisions per 256-byte block,
        // which dominated this function (it runs for every re-validated texture: ~500 per frame in ZOE battles).
        const auto log2u = [](uint32_t v)
        {
            uint32_t r = 0;
            while ((1u << r) < v)
                ++r;
            return r;
        };
        const uint32_t pageWShift = log2u(g.pageW), pageHShift = log2u(g.pageH);
        const uint32_t blockWShift = log2u(g.blockW), blockHShift = log2u(g.blockH);
        const uint32_t pageWMask = g.pageW - 1u, pageHMask = g.pageH - 1u;
        constexpr uint64_t K = 0x9E3779B97F4A7C15ull;
        uint64_t h0 = 1, h1 = 2, h2 = 3, h3 = 4;
        for (uint32_t by = 0; by < byCount; ++by)
        {
            const uint32_t py = by << blockHShift;
            const uint32_t pageRow = (py >> pageHShift) * pagesPerRow;
            const uint8_t *tableRow = g.table + ((py & pageHMask) >> blockHShift) * g.tableCols;
            for (uint32_t bx = 0; bx < bxCount; ++bx)
            {
                const uint32_t px = bx << blockWShift;
                const uint32_t page = pageRow + (px >> pageWShift);
                const uint32_t inner = tableRow[(px & pageWMask) >> blockWShift];
                const uint32_t block = (tbp0 + page * 32u + inner) & 0x3FFFu;
                const auto *q = reinterpret_cast<const uint64_t *>(vram + static_cast<size_t>(block) * 256u);
                for (int i = 0; i < 32; i += 4)
                {
                    h0 = (h0 ^ q[i]) * K;
                    h1 = (h1 ^ q[i + 1]) * K;
                    h2 = (h2 ^ q[i + 2]) * K;
                    h3 = (h3 ^ q[i + 3]) * K;
                }
            }
        }
        return (h0 ^ (h1 >> 17) ^ (h1 << 47)) * K ^ (h2 ^ (h3 >> 29) ^ (h3 << 35));
    }

    // Hash of the CLUT entries an indexed texture can reference (mirrors GSCpuBackend::LookupCLUT).
    uint64_t hashClutEntries(const GSTex0Reg &t) const
    {
        const auto &clut = cpu.ClutBuffer();
        const bool sixteenBit = t.cpsm == GS_PSM_CT16 || t.cpsm == GS_PSM_CT16S;
        const uint32_t clutBase = (static_cast<uint32_t>(t.csa) & (sixteenBit ? 0x1Fu : 0x0Fu)) << 4u;
        const uint32_t count = isFourBitPsm(t.psm) ? 16u : 256u;
        // The entries a texture can reference, in order (same selection as GSCpuBackend::LookupCLUT), then a
        // multiply-mix over 64-bit words.
        alignas(16) uint16_t e[512];
        uint32_t n = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t idx = (clutBase + i) & (sixteenBit ? 0x1FFu : 0x0FFu);
            if (!sixteenBit && t.csm == 0u && count == 256u)
                idx = std::min((i & 0xF0u) + clutBase, 240u) + (i & 0x0Fu);
            e[n++] = clut[idx];
            if (!sixteenBit)
                e[n++] = clut[(idx + 256u) & 511u];
        }
        constexpr uint64_t K = 0x9E3779B97F4A7C15ull;
        uint64_t h0 = 1469598103934665603ull ^ (static_cast<uint64_t>(t.cpsm) << 8) ^ t.csm, h1 = 2, h2 = 3, h3 = n;
        uint32_t i = 0;
        for (; i + 16u <= n; i += 16u)
        {
            uint64_t w[4];
            std::memcpy(w, e + i, 32);
            h0 = (h0 ^ w[0]) * K;
            h1 = (h1 ^ w[1]) * K;
            h2 = (h2 ^ w[2]) * K;
            h3 = (h3 ^ w[3]) * K;
        }
        for (; i < n; ++i)
            h0 = (h0 ^ e[i]) * K;
        return (h0 ^ (h1 >> 17) ^ (h1 << 47)) * K ^ (h2 ^ (h3 >> 29) ^ (h3 << 35));
    }

    uint32_t allocTextureId()
    {
        if (!freeTextureIds.empty())
        {
            const uint32_t id = freeTextureIds.back();
            freeTextureIds.pop_back();
            return id;
        }
        return nextTextureId++;
    }

    void evictTextures()
    {
        // Rarely, drop the least recently used half in one pass.
        if (contentCache.size() < 8192u)
            return;
        std::vector<uint32_t> ages;
        ages.reserve(contentCache.size());
        for (const auto &[k, e] : contentCache)
            ages.push_back(e.lastUsed);
        std::nth_element(ages.begin(), ages.begin() + ages.size() / 2, ages.end());
        const uint32_t cutoff = ages[ages.size() / 2];
        const uint32_t frame = g_presentCount.load();
        for (auto it = contentCache.begin(); it != contentCache.end();)
        {
            if (it->second.lastUsed <= cutoff && frame - it->second.lastUsed > 2u)
            {
                freeTextureIds.push_back(it->second.id);
                it = contentCache.erase(it);
            }
            else
                ++it;
        }
        // Address-keyed entries may point at freed ids: rebuild them through the content cache.
        textures.clear();
        lastKey = TextureKey{~0ull, 0};
    }
    std::unordered_map<TextureKey, TextureEntry, TextureKeyHash> textures;
    uint32_t nextTextureId = 1;
    std::vector<uint32_t> decodeScratch;
    // Fast path: consecutive primitives almost always use the same texture.
    TextureKey lastKey{~0ull, 0};
    uint32_t lastId = 0, lastPaletteId = 0, lastVersionCounter = 0, lastClutVersion = 0;
    std::vector<uint8_t> indexScratch;

    // Which render target last wrote each 8 KiB page of local memory (0 = the CPU copy is current).
    // Textures and transfers that read GPU-owned pages are served from the render target instead.
    std::array<uint32_t, kPageCount> pageOwner{};
    std::map<std::pair<uint32_t, uint32_t>, uint8_t> targets; // (fbp, fbw) -> frame psm
    std::map<uint64_t, bool> warned;
    bool warnedDate = false;

    static uint32_t ownerKey(uint32_t fbp, uint32_t fbw) { return 0x80000000u | (fbp & 0x1FFu) | ((fbw & 0x3Fu) << 16); }

    bool warnOnce(uint64_t key)
    {
        return warned.emplace(key, true).second;
    }

    static int bppClass(uint32_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24:
        case GS_PSM_Z32:
        case GS_PSM_Z24:
            return 32;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return 16;
        default:
            return 0;
        }
    }

    // Marks the pages covered by a rectangle of render target (fbp, fbw) as GPU-owned.
    void claimPages(uint32_t fbp, uint32_t fbw, uint32_t psm, int x0, int y0, int x1, int y1)
    {
        if (fbw == 0u || x1 < x0 || y1 < y0)
            return;
        uint32_t pw, ph;
        pageDims(psm, pw, ph);
        const uint32_t key = ownerKey(fbp, fbw);
        const uint32_t px0 = static_cast<uint32_t>(std::max(x0, 0)) / pw, px1 = static_cast<uint32_t>(std::max(x1, 0)) / pw;
        const uint32_t py0 = static_cast<uint32_t>(std::max(y0, 0)) / ph, py1 = static_cast<uint32_t>(std::max(y1, 0)) / ph;
        const uint32_t pagesPerRow = std::max<uint32_t>(1u, fbw * 64u / pw);
        bool changed = false;
        for (uint32_t py = py0; py <= py1; ++py)
            for (uint32_t px = std::min(px0, pagesPerRow - 1u); px <= std::min(px1, pagesPerRow - 1u); ++px)
            {
                const uint32_t page = (fbp + py * pagesPerRow + px) % kPageCount;
                if (pageOwner[page] != key)
                {
                    pageOwner[page] = key;
                    pageVersion[page] = versionCounter + 1u;
                    changed = true;
                    if (texDbg())
                    {
                        ++g_texDbg.claimBumps;
                        g_texDbg.pageSource[page] = 2;
                        g_texDbg.pageWriterDbp[page] = fbp;
                    }
                }
            }
        if (changed)
            ++versionCounter;
    }

    void releasePages(uint32_t first, uint32_t last)
    {
        for (uint32_t p = first; p <= last && p < kPageCount; ++p)
            pageOwner[p] = 0u;
        markPages(first, last);
    }

    // Exact-page variants for CPU writes: marking whole page rows from the buffer base (pageSpan) re-validated
    // every texture above an upload, ~500 texture re-hashes per frame in ZOE's battles.
    void markPagesRect(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool release)
    {
        const uint32_t v = ++versionCounter;
        if (texDbg())
            ++g_texDbg.cpuWrites;
        forEachRectPage(bp, bw, psm, x, y, w, h, [&](uint32_t page)
                        {
                            if (release)
                                pageOwner[page] = 0u;
                            pageVersion[page] = v;
                            if (texDbg())
                            {
                                ++g_texDbg.cpuPages;
                                g_texDbg.pageSource[page] = 1;
                                g_texDbg.pageWriterDbp[page] = bp | (psm << 16) | (w << 24);
                            }
                        });
    }

    // Where a buffer (bp, bw, psm) lives inside a GPU render target, if it does.
    struct RtRef
    {
        bool owned = false; // first page is GPU-owned
        bool ok = false;    // and the layout is compatible, so (fbp, fbw, x, y) are valid
        uint32_t fbp = 0, fbw = 0, x = 0, y = 0;
        uint8_t rtPsm = 0;
    };

    RtRef locate(uint32_t bp, uint32_t bw, uint32_t psm) const
    {
        RtRef r{};
        static const bool s_noRtt = std::getenv("PS2X_GPU_NO_RTT") != nullptr; // debug: never read GPU targets
        if (s_noRtt)
            return r;
        uint32_t owner = pageOwner[(bp / 32u) % kPageCount];
        if (owner == 0u)
        {
            // The first page was never drawn, but a render target may start exactly here and be drawn only further
            // in (ZOE's map: a 512x256 target at 0x3800 drawn only inside its panel, x 180-482, y 40-178, and read
            // back whole as one texture). Serve it from that target if it still owns pages of it; a texture of a
            // different pixel size there (ZOE keeps a 4-bit menu texture in the undrawn first pages) stays local.
            if ((bp % 32u) != 0u || bppClass(psm) == 0)
                return r;
            const uint32_t base = bp / 32u;
            auto it = targets.find({base, bw});
            if (it == targets.end() || bppClass(psm) != bppClass(it->second))
                return r;
            const uint32_t key = ownerKey(base, bw);
            uint32_t pw, ph;
            pageDims(it->second, pw, ph);
            const uint32_t span = std::max<uint32_t>(1u, bw * 64u / pw) * (kRtHeight / ph);
            bool live = false;
            for (uint32_t p = base; p < std::min<uint32_t>(kPageCount, base + span) && !live; ++p)
                live = pageOwner[p] == key;
            if (!live)
                return r;
            owner = key;
        }
        r.owned = true;
        r.fbp = owner & 0x1FFu;
        r.fbw = (owner >> 16) & 0x3Fu;
        auto it = targets.find({r.fbp, r.fbw});
        r.rtPsm = it != targets.end() ? it->second : static_cast<uint8_t>(GS_PSM_CT32);
        const uint32_t page = bp / 32u;
        if ((bp % 32u) != 0u || page < r.fbp || bw != r.fbw || bppClass(psm) == 0 || bppClass(psm) != bppClass(r.rtPsm))
            return r;
        uint32_t pw, ph;
        pageDims(r.rtPsm, pw, ph);
        const uint32_t pagesPerRow = std::max<uint32_t>(1u, r.fbw * 64u / pw);
        const uint32_t d = page - r.fbp;
        r.x = (d % pagesPerRow) * pw;
        r.y = (d / pagesPerRow) * ph;
        r.ok = r.y < kRtHeight;
        return r;
    }

    // A CPU-side write (host upload or local copy) of a rectangle finished. If the pages belong to a
    // GPU render target in a compatible layout, mirror the rectangle into it; otherwise local memory
    // becomes the current copy.
    void finishCpuWrite(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        const RtRef dst = locate(dbp, dbw, dpsm);
        if (!dst.ok || w == 0u || h == 0u)
        {
            markPagesRect(dbp, dbw, dpsm, x, y, w, h, true);
            return;
        }
        markPagesRect(dbp, dbw, dpsm, x, y, w, h, false);
        Command cmd{};
        cmd.type = CmdType::FramebufferWrite;
        cmd.id = dst.fbp;
        cmd.fbw = dst.fbw;
        cmd.x = dst.x + x;
        cmd.y = dst.y + y;
        cmd.w = w;
        cmd.h = h;
        cmd.first = static_cast<uint32_t>(recording.pixels.size());
        ps2_frame_log::counters().gsMirrorPx.fetch_add(static_cast<uint64_t>(w) * h, std::memory_order_relaxed);
        const bool is16 = bppClass(dpsm) == 16;
        for (uint32_t yy = 0; yy < h; ++yy)
            for (uint32_t xx = 0; xx < w; ++xx)
            {
                uint32_t v = cpu.ReadVram(dpsm, dbp, dbw, x + xx, y + yy);
                if (dpsm == GS_PSM_CT24)
                    v = (v & 0x00FFFFFFu) | 0x80000000u;
                else if (is16)
                    v = ((v & 0x1Fu) << 3) | (((v >> 5) & 0x1Fu) << 11) | (((v >> 10) & 0x1Fu) << 19) |
                        ((v & 0x8000u) ? 0x80000000u : 0u);
                recording.pixels.push_back(v);
            }
        recording.commands.push_back(cmd);
    }

    void logDraw(const GpuDrawState &ds, const GSDrawState &st, const GSVertex &v0)
    {
        const GSTex0Reg &t = st.context.tex0;
        std::cout << "[gpu:draw] prim=" << static_cast<int>(st.prim.type) << " fb=0x" << std::hex << ds.fbp << "/"
                  << ds.fbw << " fpsm=0x" << static_cast<int>(ds.fpsm) << " fbmsk=0x" << ds.fbmsk << " z=0x" << ds.zbp
                  << "/" << static_cast<int>(ds.zpsm) << " ztst=" << static_cast<int>(ds.ztst) << " zmsk=" << static_cast<int>(ds.zmsk)
                  << " test=0x" << st.context.test << " abe=" << static_cast<int>(ds.abe) << " alpha=0x" << st.context.alpha
                  << " fba=" << static_cast<int>(ds.fba) << " fge=" << static_cast<int>(ds.fge) << " iip=" << st.prim.iip;
        if (ds.tme)
            std::cout << " tex=0x" << t.tbp0 << "/" << static_cast<int>(t.tbw) << " tpsm=0x" << static_cast<int>(t.psm)
                      << std::dec << " " << ds.tw << "x" << ds.th << std::hex << " tfx=" << static_cast<int>(ds.tfx)
                      << " tcc=" << static_cast<int>(ds.tcc) << " cbp=0x" << t.cbp << " cpsm=0x" << static_cast<int>(t.cpsm)
                      << " csa=" << static_cast<int>(t.csa) << " texa=" << static_cast<int>(st.texa.ta0) << "/"
                      << static_cast<int>(st.texa.ta1) << "/" << st.texa.aem << " fst=" << static_cast<int>(ds.fst)
                      << " clamp=0x" << st.context.clamp << " lin=" << static_cast<int>(ds.linear)
                      << " src=" << (ds.texRt ? "rt" : "vram");
        std::cout << std::dec << " sc=" << ds.sc[0] << "," << ds.sc[2] << "-" << ds.sc[1] << "," << ds.sc[3]
                  << " pabe=" << st.pabe << " colclamp=" << st.colclamp << " v0=(" << v0.x << "," << v0.y << " z=" << v0.z << " rgba="
                  << static_cast<int>(v0.r) << "/" << static_cast<int>(v0.g) << "/" << static_cast<int>(v0.b) << "/"
                  << static_cast<int>(v0.a) << " uv=" << v0.u / 16.0f << "," << v0.v / 16.0f << ")" << std::endl;
    }

    // ---- GL-thread state ----
    // Render targets and depth buffers: width and height in GS pixels (height = rows allocated so far); the GL
    // textures are (sx, sy) times larger.
    struct RenderTarget
    {
        gl::GLuint color = 0;
        uint32_t width = 0, height = 0;
    };
    std::map<std::pair<uint32_t, uint32_t>, RenderTarget> renderTargets; // (fbp, fbw)
    std::map<std::pair<uint32_t, uint32_t>, RenderTarget> depthBuffers;  // (zbp, fbw); `color` holds the depth texture
    // Internal resolution in use (GL thread). PS2X_GPU_SCALE / Graphics > Internal resolution picks sx; above x1 sy
    // is twice that when the display doubles every line (ZOE's 224-line fields shown on 448 lines), so pixels stay
    // square.
    uint32_t sx = 1, sy = 1;
    uint32_t presentTargetGen = 0; // g_presentTargetGen the present FBO attachment belongs to
    gl::GLint maxTextureSize = 0;
    gl::GLuint uploadScratch = 0; // FramebufferWrite at scale > 1: the CPU rectangle before it is scaled into the RT
    uint32_t uploadScratchW = 0, uploadScratchH = 0;
    // GPU time of Present (replay + composition), read back a few frames later.
    std::array<gl::GLuint, 4> timerQueries{};
    uint32_t timerNext = 0, timerPending = 0;
    // Copies made during replay (reported with [gpu:pace]): a draw sampling its own target, DATE, local copies,
    // and render-target growth. Pixels in GS pixels.
    uint64_t statSelfCopies = 0, statSelfCopyPx = 0, statDateCopies = 0, statDatePx = 0, statRectCopies = 0, statGrows = 0,
             statWriteRuns = 0, statWriteBlits = 0;

    // Redundant-state filter for executeDraw (PS2X_GPU_STATECACHE=0 turns it off). Each GL call costs GPU time per
    // draw, and ZOE's heaviest frames are thousands of tiny particle draws (Tempest: ~2,000 per frame at ~3 us of
    // GPU each) that mostly share their target, scissor, blend and shader values. Anything else that touches GL
    // state bumps glEpoch, so the next draw sets everything again.
    static bool stateCache()
    {
        static const bool on = []
        {
            const char *v = std::getenv("PS2X_GPU_STATECACHE");
            return !(v && v[0] == '0');
        }();
        return on;
    }
    uint64_t glEpoch = 1;
    struct DrawCache
    {
        uint64_t epoch = 0;
        gl::GLuint fbo = ~0u, tex0 = ~0u, tex1 = ~0u;
        int vp[2] = {-1, -1};
        int sc[4] = {-1, -1, -1, -1};
        int scissorOn = -1, depthOn = -1, blendOn = -1, masks = -1;
        gl::GLenum depthFunc = ~0u, blendEq = ~0u, blendSrc = ~0u, blendDst = ~0u;
        float blendConstant = -1.0f;
    } dc;
    // Uniform groups of the draw program, with their last values (valid within one replay).
    enum UniformSlot
    {
        kURtSize, kUTex, kULinear, kUWrap, kUClamp, kUFog, kUAlphaTest, kUTexRt, kUTexOffset, kUTexa, kUPalette,
        kUAlphaPass, kUFbMask, kUSlotCount
    };
    std::array<std::array<float, 4>, kUSlotCount> uCache{};
    std::array<bool, kUSlotCount> uValid{};
    // True when uniform group `slot` already holds these values; otherwise records them (the caller sets them).
    bool uSame(int slot, float a, float b = 0.0f, float c = 0.0f, float d = 0.0f)
    {
        auto &v = uCache[slot];
        if (stateCache() && uValid[slot] && v[0] == a && v[1] == b && v[2] == c && v[3] == d)
            return true;
        v = {a, b, c, d};
        uValid[slot] = true;
        return false;
    }

    // PS2X_GPU_DRAWPROF=1 (diagnostic, slows the game): GPU timestamps around every draw. Every 300 presents
    // [gpu:drawprof] lists the kinds of draw that took the most GPU time and the costliest draws of the worst frame.
    static bool drawProf()
    {
        static const bool on = std::getenv("PS2X_GPU_DRAWPROF") != nullptr;
        return on;
    }
    struct ProfDraw
    {
        gl::GLuint q0, q1;
        uint32_t state, count;
        double area; // GS pixels covered (triangles: sum of areas, each at most the scissor area)
    };
    struct ProfAgg
    {
        double ms = 0.0, area = 0.0;
        uint64_t draws = 0;
    };
    std::vector<gl::GLuint> profPool;
    size_t profUsed = 0;
    std::vector<ProfDraw> profDraws;
    std::map<std::string, ProfAgg> profAgg;
    uint32_t profFrames = 0;
    double profFramesMs = 0.0, profWorstMs = 0.0;
    std::string profWorst;

    gl::GLuint profQuery()
    {
        if (profUsed == profPool.size())
        {
            profPool.resize(profPool.size() + 256u);
            gl::GenQueries(256, profPool.data() + profUsed);
        }
        return profPool[profUsed++];
    }

    std::string profKey(const GpuDrawState &ds) const
    {
        char b[256];
        const char *prim = ds.primClass == kPrimLines ? "lines" : ds.primClass == kPrimPoints ? "points" : "tris";
        const char *kind = !ds.tme ? "none" : ds.texRt ? "rt" : ds.paletteId ? "pal" : "rgba";
        std::snprintf(b, sizeof(b), "fb=%x/%u %s tex=%s %ux%u%s src=%x abe=%u(%u%u%u%u) ztst=%u zmsk=%u date=%u ate=%u",
                      ds.fbp, ds.fbw, prim, kind, ds.tw, ds.th, ds.linear ? " lin" : "", ds.texRt ? ds.texRtFbp : 0u, ds.abe,
                      ds.blendA, ds.blendB, ds.blendC, ds.blendD, ds.ztst, ds.zmsk, ds.date, ds.ate);
        return b;
    }

    double drawArea(const GpuDrawState &ds, const Command &cmd) const
    {
        if (ds.primClass != kPrimTriangles)
            return cmd.count;
        const double scArea = (static_cast<double>(ds.sc[1]) - ds.sc[0] + 1.0) * (static_cast<double>(ds.sc[3]) - ds.sc[2] + 1.0);
        double a = 0.0;
        for (uint32_t i = 0; i + 2u < cmd.count; i += 3u)
        {
            const GpuVertex &p = replay.vertices[cmd.first + i], &q = replay.vertices[cmd.first + i + 1u],
                            &r = replay.vertices[cmd.first + i + 2u];
            const double t = std::abs((static_cast<double>(q.x) - p.x) * (static_cast<double>(r.y) - p.y) -
                                      (static_cast<double>(r.x) - p.x) * (static_cast<double>(q.y) - p.y)) * 0.5;
            a += std::min(t, scArea);
        }
        return a;
    }

    void profFinish()
    {
        if (profDraws.empty())
            return;
        double frameMs = 0.0;
        std::vector<std::pair<double, size_t>> per;
        per.reserve(profDraws.size());
        for (size_t i = 0; i < profDraws.size(); ++i)
        {
            const ProfDraw &d = profDraws[i];
            gl::GLuint64 t0 = 0, t1 = 0;
            gl::GetQueryObjectui64v(d.q0, gl::QUERY_RESULT, &t0);
            gl::GetQueryObjectui64v(d.q1, gl::QUERY_RESULT, &t1);
            const double ms = t1 > t0 ? (t1 - t0) / 1e6 : 0.0;
            frameMs += ms;
            per.emplace_back(ms, i);
            ProfAgg &a = profAgg[profKey(replay.states[d.state])];
            a.ms += ms;
            a.area += d.area;
            ++a.draws;
        }
        ++profFrames;
        profFramesMs += frameMs;
        if (frameMs > profWorstMs)
        {
            profWorstMs = frameMs;
            std::sort(per.begin(), per.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
            std::ostringstream o;
            o << "[gpu:drawprof] worst frame " << frameMs << " ms in " << profDraws.size() << " draws at scale " << sx << "x" << sy
              << ", costliest:\n";
            for (size_t k = 0; k < per.size() && k < 10u; ++k)
            {
                const ProfDraw &d = profDraws[per[k].second];
                o << "  " << per[k].first << " ms  verts=" << d.count << " areaKpx=" << d.area / 1e3 << "  "
                  << profKey(replay.states[d.state]) << "\n";
            }
            profWorst = o.str();
        }
        if (profFrames >= 300u)
        {
            std::vector<std::pair<std::string, ProfAgg>> top(profAgg.begin(), profAgg.end());
            std::sort(top.begin(), top.end(), [](const auto &x, const auto &y) { return x.second.ms > y.second.ms; });
            std::ostringstream o;
            o << "[gpu:drawprof] " << profFrames << " presents, " << profFramesMs / profFrames << " ms of draws per present at scale "
              << sx << "x" << sy << "; top kinds (ms/present, draws/present, kpx/draw):\n";
            for (size_t k = 0; k < top.size() && k < 15u; ++k)
                o << "  " << top[k].second.ms / profFrames << "  " << static_cast<double>(top[k].second.draws) / profFrames << "  "
                  << top[k].second.area / 1e3 / static_cast<double>(std::max<uint64_t>(1, top[k].second.draws)) << "  " << top[k].first
                  << "\n";
            std::cerr << o.str() << profWorst << std::flush;
            profAgg.clear();
            profFrames = 0;
            profFramesMs = profWorstMs = 0.0;
            profWorst.clear();
        }
        profDraws.clear();
        profUsed = 0;
    }
    std::map<std::pair<gl::GLuint, gl::GLuint>, gl::GLuint> framebuffers; // (color, depth)
    std::unordered_map<uint32_t, gl::GLuint> glTextures;
    struct GlTextureShape
    {
        uint32_t w = 0, h = 0;
        uint8_t format = 0; // Command::texFormat
    };
    std::unordered_map<uint32_t, GlTextureShape> glTextureShapes;
    std::map<std::pair<uint32_t, uint32_t>, gl::GLuint> scratchTextures; // (w, h) -> copy of an RT
    gl::GLuint program = 0, presentProgram = 0, vao = 0, presentVao = 0, vbo = 0, presentFbo = 0, presentFboTexture = 0;
    // Blend deinterlacing of half-height interlaced frames: the previous presented frame and a copy of the current.
    gl::GLuint fieldHistory = 0, fieldCurrent = 0;
    uint32_t fieldTexW = 0, fieldTexH = 0;
    bool fieldHistoryValid = false;
    // DATE: marks the stencil where the destination alpha's top bit matches DATM.
    gl::GLuint dateProgram = 0;
    gl::GLint dateRect = -1, dateRtSize = -1, dateTex = -1, dateDatm = -1;
    std::map<std::pair<uint32_t, uint32_t>, gl::GLuint> dateScratchTextures; // (w, h) -> RT copy for DATE
    struct Uniforms
    {
        gl::GLint rtSize, tex, pal, palette, tme, tfx, tcc, fst, linear, texSize, wrap, clampUV, fge, fogColor, ate, atst,
            aref, alphaPass, fba, texRt, texOffset, texa, scale, scaleI, texRtSize, spriteClamp, dst, fbMask, maskBits;
    } u{};
    // Partial frame masks (FBMSK bits that are neither all 0 nor all 1 in a channel): copy of the target, read per bit.
    std::map<std::pair<uint32_t, uint32_t>, gl::GLuint> maskScratchTextures; // (w, h) -> RT copy
    uint64_t statMaskDraws = 0; // partial-mask draws merged per bit since the last [gpu:pace] line
    // One log line per mask value; replay (window) thread only, so not the shared warnOnce map (recording thread).
    std::set<uint64_t> maskLogged;
    gl::GLint presentTex = -1, presentRect = -1, presentSrcSize = -1, presentAlphaMode = -1;
    bool glReady = false;
    bool glFailed = false;
    CommandList replay;

    void markPages(uint32_t first, uint32_t last)
    {
        const uint32_t v = ++versionCounter;
        for (uint32_t p = first; p <= last && p < kPageCount; ++p)
            pageVersion[p] = v;
    }

    uint32_t maxPageVersion(uint32_t first, uint32_t last) const
    {
        uint32_t v = 0;
        for (uint32_t p = first; p <= last && p < kPageCount; ++p)
            v = std::max(v, pageVersion[p]);
        return v;
    }

    TexIds remember(const TextureKey &key, uint32_t id, uint32_t paletteId = 0)
    {
        lastKey = key;
        lastId = id;
        lastPaletteId = paletteId;
        lastVersionCounter = versionCounter;
        lastClutVersion = clutVersion;
        return {id, paletteId};
    }

    // Fills the texture fields of ds: either an RT source or a decoded texture id.
    void bindTexture(const GSDrawState &state, GpuDrawState &ds)
    {
        const GSTex0Reg &t = state.context.tex0;
        const RtRef rt = locate(t.tbp0, t.tbw, t.psm);
        if (rt.ok)
        {
            ds.texRt = (t.psm == GS_PSM_CT24 || t.psm == GS_PSM_Z24) ? 2 : bppClass(t.psm) == 16 ? 3 : 1;
            ds.texRtFbp = rt.fbp;
            ds.texRtFbw = rt.fbw;
            ds.texOffX = static_cast<uint16_t>(rt.x);
            ds.texOffY = static_cast<uint16_t>(rt.y);
            ds.ta0 = state.texa.ta0;
            ds.ta1 = state.texa.ta1;
            ds.aem = state.texa.aem ? 1 : 0;
            return;
        }
        if (rt.owned && warnOnce((1ull << 40) | (static_cast<uint64_t>(t.tbp0) << 8) | t.psm))
            std::cerr << "[gpu] texture tbp=0x" << std::hex << t.tbp0 << " tbw=" << static_cast<int>(t.tbw) << " psm=0x"
                      << static_cast<int>(t.psm) << " reads GPU target fbp=0x" << rt.fbp << "/" << rt.fbw << " psm=0x"
                      << static_cast<int>(rt.rtPsm) << std::dec << " in an unsupported layout (using stale local memory)"
                      << std::endl;
        const TexIds ids = resolveTexture(state);
        ds.textureId = ids.tex;
        ds.paletteId = ids.pal;
    }

    TexIds resolveTexture(const GSDrawState &state)
    {
        const GSTex0Reg &t = state.context.tex0;
        const uint32_t tw = state.textureWidth, th = state.textureHeight;
        const bool indexed = t.psm == GS_PSM_T8 || t.psm == GS_PSM_T4 || t.psm == GS_PSM_T8H ||
                             t.psm == GS_PSM_T4HL || t.psm == GS_PSM_T4HH;
        TextureKey key{};
        key.packed = static_cast<uint64_t>(t.tbp0 & 0x3FFFu) | (static_cast<uint64_t>(t.tbw & 0x3Fu) << 14) |
                     (static_cast<uint64_t>(t.psm & 0x3Fu) << 20) |
                     (static_cast<uint64_t>(indexed ? (t.cpsm & 0x3Fu) : 0u) << 26) |
                     (static_cast<uint64_t>(indexed ? (t.csm & 1u) : 0u) << 32) |
                     (static_cast<uint64_t>(indexed ? (t.csa & 0x1Fu) : 0u) << 33) |
                     (static_cast<uint64_t>(state.texa.ta0) << 38) | (static_cast<uint64_t>(state.texa.ta1) << 46) |
                     (static_cast<uint64_t>(state.texa.aem ? 1u : 0u) << 54);
        key.size = tw | (th << 16);
        static const bool s_noCache = std::getenv("PS2X_GPU_NO_TEXCACHE") != nullptr; // debug: decode every time
        if (!s_noCache && key == lastKey && lastVersionCounter == versionCounter && (!indexed || lastClutVersion == clutVersion))
            return {lastId, lastPaletteId};
        struct SlowTimer // per-frame log (PS2X_FRAME_LOG): time of lookups past the fast path
        {
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~SlowTimer()
            {
                ps2_frame_log::counters().gsTexNs.fetch_add(
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                    std::memory_order_relaxed);
            }
        } slowTimer;

        uint32_t firstPage, lastPage;
        pageSpan(t.tbp0, t.tbw, t.psm, th, firstPage, lastPage);
        uint32_t pv = 0;
        forEachRectPage(t.tbp0, t.tbw, t.psm, 0u, 0u, tw, th, [&](uint32_t page) { pv = std::max(pv, pageVersion[page]); });

        auto it = textures.find(key);
        const bool exists = it != textures.end();
        auto touch = [&](const TextureEntry &e)
        {
            for (int part = 0; part < (e.paletteId != 0 ? 2 : 1); ++part)
            {
                auto c = contentCache.find(part == 0 ? e.contentKey : e.paletteKey);
                if (c != contentCache.end())
                    c->second.lastUsed = g_presentCount.load();
            }
        };
        if (!s_noCache && exists && it->second.pageVersionMax == pv && (!indexed || it->second.clutVersion == clutVersion))
        {
            touch(it->second);
            return remember(key, it->second.id, it->second.paletteId);
        }

        uint64_t clutHash = 0;
        if (indexed)
        {
            clutHash = hashClutEntries(t);
            if (!s_noCache && exists && it->second.pageVersionMax == pv && it->second.clutHash == clutHash)
            {
                it->second.clutVersion = clutVersion;
                touch(it->second);
                return remember(key, it->second.id, it->second.paletteId);
            }
        }

        if (texDbg() && exists)
        {
            ++g_texDbg.rehash;
            uint32_t worst = 0, worstPage = 0;
            forEachRectPage(t.tbp0, t.tbw, t.psm, 0u, 0u, tw, th, [&](uint32_t page)
                            {
                                if (pageVersion[page] > worst)
                                {
                                    worst = pageVersion[page];
                                    worstPage = page;
                                }
                            });
            const uint64_t k = (static_cast<uint64_t>(t.tbp0) << 40) | (static_cast<uint64_t>(g_texDbg.pageWriterDbp[worstPage]) << 8) |
                               g_texDbg.pageSource[worstPage] | (it->second.pageVersionMax == pv ? 0x80u : 0u);
            ++g_texDbg.rehashBy[k];
            if ((g_texDbg.rehash % 30000u) == 0u)
            {
                std::vector<std::pair<uint64_t, uint64_t>> top(g_texDbg.rehashBy.begin(), g_texDbg.rehashBy.end());
                std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                std::cerr << "[texdbg] rehash=" << g_texDbg.rehash << " cpuWrites=" << g_texDbg.cpuWrites
                          << " cpuPages=" << g_texDbg.cpuPages << " claimBumps=" << g_texDbg.claimBumps << std::hex;
                for (size_t i = 0; i < top.size() && i < 12u; ++i)
                    std::cerr << " | tbp=" << (top[i].first >> 40) << " src=" << (top[i].first & 0x7Fu)
                              << ((top[i].first & 0x80u) ? "(clut)" : "") << " by=" << ((top[i].first >> 8) & 0xFFFFFFFFu)
                              << " n=" << std::dec << top[i].second << std::hex;
                std::cerr << std::dec << std::endl;
                g_texDbg.rehashBy.clear();
            }
        }
        constexpr uint64_t K = 0x9E3779B97F4A7C15ull;
        // Content identity: the texel bytes plus everything that affects decoding, but not the
        // address (games stream the same textures through rotating VRAM slots).
        const uint64_t decodeParams = key.packed & ~0xFFFFFull; // drop tbp0 (14 bits) and tbw (6 bits)
        // Unchanged pages with a new CLUT: the texel bytes are the same, so is their hash.
        const uint64_t blockHash = (exists && it->second.pageVersionMax == pv)
                                       ? it->second.blockHash
                                       : hashTextureBlocks(t.tbp0, t.tbw, t.psm, tw, th, firstPage, lastPage);
        // Eviction clears `textures`, so `it` is not used past this point.
        evictTextures();
        static const bool s_gpuPalette = []
        {
            const char *v = std::getenv("PS2X_GPU_PALETTE"); // 0 = decode indexed textures to RGBA on the CPU (old path)
            return !(v && v[0] == '0');
        }();
        if (indexed && s_gpuPalette)
            return resolveIndexed(state, key, firstPage, lastPage, pv, clutHash, blockHash, decodeParams);
        const uint64_t contentKey = blockHash ^ (decodeParams * K) ^
                                    ((static_cast<uint64_t>(key.size) + 0x1234567ull) * 0xC2B2AE3D27D4EB4Full) ^
                                    ((clutHash + 1u) * 0x165667B19E3779F9ull);
        TextureEntry entry{};
        entry.firstPage = firstPage;
        entry.lastPage = lastPage;
        entry.pageVersionMax = pv;
        entry.clutVersion = clutVersion;
        entry.clutHash = clutHash;
        entry.contentKey = contentKey;
        entry.blockHash = blockHash;
        auto cached = s_noCache ? contentCache.end() : contentCache.find(contentKey);
        if (cached != contentCache.end())
        {
            cached->second.lastUsed = g_presentCount.load();
            entry.id = cached->second.id;
            textures[key] = entry;
            ++g_contentHits;
            return remember(key, entry.id);
        }
        entry.id = allocTextureId();
        contentCache[contentKey] = ContentEntry{entry.id, g_presentCount.load()};
        textures[key] = entry;

        if (decodeLog())
            std::cerr << "[gpu:decode] present=" << g_presentCount.load() << " tbp=0x" << std::hex << t.tbp0 << " psm=0x"
                      << static_cast<int>(t.psm) << std::dec << " " << tw << "x" << th << " rgba" << std::endl;
        const auto decodeStart = std::chrono::steady_clock::now();
        cpu.DecodeTexture(state, tw, th, decodeScratch);
        g_decodeNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - decodeStart)
                                                .count());
        g_decodePixels += static_cast<uint64_t>(tw) * th;
        ps2_frame_log::counters().gsDecodes.fetch_add(1u, std::memory_order_relaxed);
        ps2_frame_log::counters().gsDecodePx.fetch_add(static_cast<uint64_t>(tw) * th, std::memory_order_relaxed);
        if (gpuTraceActive() || ps2_debug::traceBurst())
        {
            uint32_t amin = 255, amax = 0;
            for (uint32_t px : decodeScratch)
            {
                amin = std::min(amin, px >> 24);
                amax = std::max(amax, px >> 24);
            }
            std::cout << "[gpu:tex] id=" << entry.id << " tbp=0x" << std::hex << t.tbp0 << " psm=0x" << static_cast<int>(t.psm)
                      << " cbp=0x" << t.cbp << std::dec << " " << tw << "x" << th << " alpha=" << amin << ".." << amax << std::endl;
        }
        Command cmd{};
        cmd.type = CmdType::TextureUpload;
        cmd.id = entry.id;
        cmd.w = tw;
        cmd.h = th;
        cmd.first = static_cast<uint32_t>(recording.pixels.size());
        recording.pixels.insert(recording.pixels.end(), decodeScratch.begin(), decodeScratch.end());
        recording.commands.push_back(cmd);
        return remember(key, entry.id);
    }

    // Indexed textures: the texel indices and the palette are cached and uploaded separately, and the shader applies
    // the palette (colour = palette[index], exactly what GSCpuBackend::DecodeTexture produces). A new CLUT then costs a
    // 256-entry palette instead of a full decode: ZOE draws one 512x512 4-bit effect sheet with ~30 palettes in a frame
    // (Tempest's attacks, big explosions); decoding every combination to RGBA took 40-55 ms on the GS thread.
    TexIds resolveIndexed(const GSDrawState &state, const TextureKey &key, uint32_t firstPage, uint32_t lastPage, uint32_t pv,
                          uint64_t clutHash, uint64_t blockHash, uint64_t decodeParams)
    {
        const GSTex0Reg &t = state.context.tex0;
        const uint32_t tw = state.textureWidth, th = state.textureHeight;
        constexpr uint64_t K = 0x9E3779B97F4A7C15ull;
        TextureEntry entry{};
        entry.firstPage = firstPage;
        entry.lastPage = lastPage;
        entry.pageVersionMax = pv;
        entry.clutVersion = clutVersion;
        entry.clutHash = clutHash;
        entry.blockHash = blockHash;
        // Indices depend on the texel bytes, the index format and the size; the palette on the CLUT entries the
        // texture can reference and the CLUT/TEXA parameters. Different final mixes keep these keys apart from each
        // other and from the RGBA textures' keys.
        entry.contentKey = (blockHash ^ ((static_cast<uint64_t>(t.psm) + 0x5BD1E995ull) * K) ^
                            ((static_cast<uint64_t>(key.size) + 0x1234567ull) * 0xC2B2AE3D27D4EB4Full)) *
                           0xFF51AFD7ED558CCDull;
        entry.paletteKey = (((clutHash + 1u) * 0x165667B19E3779F9ull) ^ (decodeParams * K)) * 0xC4CEB9FE1A85EC53ull;
        static const bool s_noCache = std::getenv("PS2X_GPU_NO_TEXCACHE") != nullptr;
        const uint32_t frame = g_presentCount.load();
        auto lookup = [&](uint64_t contentKey, bool &created)
        {
            auto c = s_noCache ? contentCache.end() : contentCache.find(contentKey);
            created = c == contentCache.end();
            if (!created)
            {
                c->second.lastUsed = frame;
                ++g_contentHits;
                return c->second.id;
            }
            const uint32_t id = allocTextureId();
            contentCache[contentKey] = ContentEntry{id, frame};
            return id;
        };
        bool newIndices = false, newPalette = false;
        entry.id = lookup(entry.contentKey, newIndices);
        entry.paletteId = lookup(entry.paletteKey, newPalette);
        textures[key] = entry;

        uint32_t palette[256];
        if (newIndices)
        {
            const auto decodeStart = std::chrono::steady_clock::now();
            cpu.DecodeTextureIndices(state, tw, th, indexScratch);
            g_decodeNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - decodeStart).count());
            g_decodePixels += static_cast<uint64_t>(tw) * th;
            ps2_frame_log::counters().gsDecodes.fetch_add(1u, std::memory_order_relaxed);
            ps2_frame_log::counters().gsDecodePx.fetch_add(static_cast<uint64_t>(tw) * th, std::memory_order_relaxed);
            Command cmd{};
            cmd.type = CmdType::TextureUpload;
            cmd.texFormat = 1;
            cmd.id = entry.id;
            cmd.w = tw;
            cmd.h = th;
            cmd.first = static_cast<uint32_t>(recording.pixels.size());
            recording.pixels.resize(recording.pixels.size() + (indexScratch.size() + 3u) / 4u);
            std::memcpy(recording.pixels.data() + cmd.first, indexScratch.data(), indexScratch.size());
            recording.commands.push_back(cmd);
        }
        if (newPalette)
        {
            cpu.DecodePalette(state, palette);
            Command cmd{};
            cmd.type = CmdType::TextureUpload;
            cmd.id = entry.paletteId;
            cmd.w = 256u;
            cmd.h = 1u;
            cmd.first = static_cast<uint32_t>(recording.pixels.size());
            recording.pixels.insert(recording.pixels.end(), palette, palette + 256);
            recording.commands.push_back(cmd);
        }
        if (palCheck())
            checkIndexed(state, entry, newIndices, newPalette);
        if (decodeLog())
            std::cerr << "[gpu:decode] present=" << frame << " tbp=0x" << std::hex << t.tbp0 << " psm=0x" << static_cast<int>(t.psm)
                      << " cbp=0x" << t.cbp << std::dec << " " << tw << "x" << th << " indices=" << (newIndices ? "new" : "cached")
                      << " palette=" << (newPalette ? "new" : "cached") << std::endl;
        return remember(key, entry.id, entry.paletteId);
    }

    // PS2X_GPU_PALCHECK=1 (debug): every indexed lookup past the fast paths re-decodes the texture the old way and
    // compares it with palette[index], and checks that a cached index/palette texture holds the same data as a fresh
    // decode (i.e. the content keys are sound). Mismatches are reported on stderr; a summary every 2,000 checks.
    static bool palCheck()
    {
        static const bool on = std::getenv("PS2X_GPU_PALCHECK") != nullptr;
        return on;
    }
    static bool decodeLog()
    {
        static const bool on = std::getenv("PS2X_GPU_DECODELOG") != nullptr; // one line per decode (diagnostic)
        return on;
    }
    std::unordered_map<uint32_t, uint64_t> palCheckSums; // texture id -> hash of what was uploaded
    uint64_t palChecks = 0, palCheckFails = 0;

    void checkIndexed(const GSDrawState &state, const TextureEntry &entry, bool newIndices, bool newPalette)
    {
        const uint32_t tw = state.textureWidth, th = state.textureHeight;
        uint32_t palette[256];
        cpu.DecodePalette(state, palette);
        cpu.DecodeTextureIndices(state, tw, th, indexScratch);
        cpu.DecodeTexture(state, tw, th, decodeScratch);
        auto hashBytes = [](const void *p, size_t n)
        {
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < n; ++i)
                h = (h ^ static_cast<const uint8_t *>(p)[i]) * 1099511628211ull;
            return h;
        };
        const uint64_t indexSum = hashBytes(indexScratch.data(), indexScratch.size()) ^ (static_cast<uint64_t>(tw) << 48 | th);
        const uint64_t paletteSum = hashBytes(palette, sizeof(palette));
        size_t bad = 0;
        for (size_t i = 0; i < decodeScratch.size(); ++i)
            bad += decodeScratch[i] != palette[indexScratch[i]] ? 1u : 0u;
        bool cacheBad = false;
        for (int part = 0; part < 2; ++part)
        {
            const uint32_t id = part == 0 ? entry.id : entry.paletteId;
            const uint64_t sum = part == 0 ? indexSum : paletteSum;
            const bool created = part == 0 ? newIndices : newPalette;
            auto [slot, inserted] = palCheckSums.try_emplace(id, sum);
            if (created || inserted)
                slot->second = sum;
            else if (slot->second != sum)
                cacheBad = true;
        }
        ++palChecks;
        if (bad != 0 || cacheBad)
        {
            ++palCheckFails;
            const GSTex0Reg &t = state.context.tex0;
            std::cerr << "[gpu:palcheck] MISMATCH tbp=0x" << std::hex << t.tbp0 << " psm=0x" << static_cast<int>(t.psm)
                      << " cpsm=0x" << static_cast<int>(t.cpsm) << " cbp=0x" << t.cbp << std::dec << " csm=" << static_cast<int>(t.csm)
                      << " csa=" << static_cast<int>(t.csa) << " " << tw << "x" << th << " texels=" << bad
                      << (cacheBad ? " cached-texture-differs" : "") << std::endl;
        }
        if (palChecks % 2000u == 0u)
            std::cerr << "[gpu:palcheck] checks=" << palChecks << " mismatches=" << palCheckFails << std::endl;
    }

    void emitDraw(const GpuDrawState &ds, const GpuVertex *verts, uint32_t count)
    {
        auto &cmds = recording.commands;
        const uint32_t first = static_cast<uint32_t>(recording.vertices.size());
        recording.vertices.insert(recording.vertices.end(), verts, verts + count);
        if (!cmds.empty() && cmds.back().type == CmdType::Draw &&
            std::memcmp(&recording.states[cmds.back().state], &ds, sizeof(GpuDrawState)) == 0 &&
            cmds.back().first + cmds.back().count == first)
        {
            cmds.back().count += count;
            return;
        }
        Command cmd{};
        cmd.type = CmdType::Draw;
        if (recording.states.empty() || std::memcmp(&recording.states.back(), &ds, sizeof(GpuDrawState)) != 0)
            recording.states.push_back(ds);
        cmd.state = static_cast<uint32_t>(recording.states.size() - 1u);
        cmd.first = first;
        cmd.count = count;
        cmds.push_back(cmd);
    }

    // ------------------------------------------------------------------------------------------
    // GL thread
    // ------------------------------------------------------------------------------------------
    gl::GLuint compile(gl::GLenum type, const char *src)
    {
        const gl::GLuint s = gl::CreateShader(type);
        gl::ShaderSource(s, 1, &src, nullptr);
        gl::CompileShader(s);
        gl::GLint ok = 0;
        gl::GetShaderiv(s, gl::COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[2048] = {};
            gl::GetShaderInfoLog(s, sizeof(log), nullptr, log);
            std::cerr << "[gpu] shader compile failed: " << log << std::endl;
        }
        return s;
    }

    gl::GLuint link(const char *vs, const char *fs)
    {
        const gl::GLuint p = gl::CreateProgram();
        gl::AttachShader(p, compile(gl::VERTEX_SHADER, vs));
        gl::AttachShader(p, compile(gl::FRAGMENT_SHADER, fs));
        gl::LinkProgram(p);
        gl::GLint ok = 0;
        gl::GetProgramiv(p, gl::LINK_STATUS, &ok);
        if (!ok)
        {
            char log[2048] = {};
            gl::GetProgramInfoLog(p, sizeof(log), nullptr, log);
            std::cerr << "[gpu] program link failed: " << log << std::endl;
            return 0;
        }
        return p;
    }

    bool initGl()
    {
        if (glReady || glFailed)
            return glReady;
        if (!gl::load())
        {
            glFailed = true;
            return false;
        }
        static const char *kVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aStq;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aFog;
layout(location = 4) in vec4 aClamp;
uniform vec2 uRtSize;
uniform vec2 uScale; // internal resolution per axis
noperspective out vec3 vStq;
noperspective out vec4 vColor;
noperspective out float vFog;
flat out vec4 vClamp;
void main()
{
    // GS pixel i is covered when x0 <= i < x1; GL samples at i + 0.5, so shift by half a pixel. At internal
    // resolution s, GS pixel i is the s GL pixels [i*s, i*s + s) sampled at i + k/s: the shift is half a GL pixel,
    // so sprites with whole-pixel edges cover whole GS pixels.
    vec2 p = (aPos.xy + 0.5 / uScale) / uRtSize * 2.0 - 1.0;
    gl_Position = vec4(p, aPos.z * 2.0 - 1.0, 1.0);
    gl_PointSize = max(uScale.x, uScale.y);
    vStq = aStq;
    vColor = aColor;
    vFog = aFog;
    vClamp = aClamp;
}
)";
        static const char *kFs = R"(#version 330 core
noperspective in vec3 vStq;
noperspective in vec4 vColor;
noperspective in float vFog;
flat in vec4 vClamp;
uniform sampler2D uTex;
uniform sampler2D uPal; // indexed textures: uTex holds 8-bit palette indices, uPal the 256x1 colours
uniform int uPalette;
uniform int uTme, uTfx, uTcc, uFst, uLinear, uFge, uAte, uAtst, uAref, uAlphaPass, uFba;
uniform ivec2 uTexSize;
uniform ivec2 uWrap;
uniform ivec4 uClampUV;
uniform int uTexRt;
uniform ivec2 uTexOffset;
uniform ivec2 uTexRtSize; // sampled render target: width and allocated rows in GS pixels
uniform ivec2 uScaleI;    // internal resolution per axis
uniform int uSpriteClamp; // 1: apply the sprites' sample range at every resolution (PS2X_GPU_SPRITECLAMP)
uniform sampler2D uDst;   // partial frame mask: copy of the render target taken before the draw
uniform int uFbMask;      // 1: merge with the destination bit by bit (FBMSK bits not all 0 or all 1 in a channel)
uniform ivec4 uMaskBits;  // per channel (r, g, b, a): the FBMSK bits, 1 = keep the destination bit
uniform ivec3 uTexa;
uniform vec3 uFogColor;
// Index 0 is what gets stored; index 1 carries the source alpha the blender uses (the GS blends
// with As before FBA forces bit 7 of the stored alpha).
layout(location = 0, index = 0) out vec4 oColor;
layout(location = 0, index = 1) out vec4 oBlend;

int wrapCoord(int c, int size, int mode, int mn, int mx)
{
    if (mode == 0) return c & (size - 1);
    if (mode == 1) return clamp(c, 0, size - 1);
    if (mode == 2) return clamp(c, mn, mx);
    return (c & mn) | mx;
}

vec4 texel(ivec2 c)
{
    c.x = wrapCoord(c.x, uTexSize.x, uWrap.x, uClampUV.x, uClampUV.y);
    c.y = wrapCoord(c.y, uTexSize.y, uWrap.y, uClampUV.z, uClampUV.w);
    if (uPalette == 1) // palette lookup per texel, before filtering (as the GS does)
        return floor(texelFetch(uPal, ivec2(int(texelFetch(uTex, c, 0).r * 255.0 + 0.5), 0), 0) * 255.0 + 0.5);
    return floor(texelFetch(uTex, c, 0) * 255.0 + 0.5);
}

// Render target texel, read at the target's own resolution: h is a texel of the scaled texture, counted from the
// texture's origin. Wrapping and clamping work on GS texels (h / scale), like the GS.
int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
vec4 rtTexel(ivec2 h)
{
    ivec2 c = ivec2(floorDiv(h.x, uScaleI.x), floorDiv(h.y, uScaleI.y));
    ivec2 sub = h - c * uScaleI;
    c.x = wrapCoord(c.x, uTexSize.x, uWrap.x, uClampUV.x, uClampUV.y);
    c.y = wrapCoord(c.y, uTexSize.y, uWrap.y, uClampUV.z, uClampUV.w);
    c += uTexOffset;
    // Rows the target never allocated were never drawn: cleared memory.
    bool inside = c.x >= 0 && c.y >= 0 && c.x < uTexRtSize.x && c.y < uTexRtSize.y;
    // Colour stored as byte/255, alpha as byte/128.
    vec4 v = inside ? texelFetch(uTex, c * uScaleI + sub, 0) : vec4(0.0);
    vec3 rgb = floor(clamp(v.rgb, 0.0, 1.0) * 255.0 + 0.5);
    float a = floor(clamp(v.a * 128.0, 0.0, 255.0) + 0.5);
    bool black = rgb == vec3(0.0);
    if (uTexRt == 2)
        a = (uTexa.z == 1 && black) ? 0.0 : float(uTexa.x);
    else if (uTexRt == 3)
    {
        rgb = floor(rgb / 8.0) * 8.0;
        a = (a >= 128.0) ? float(uTexa.y) : ((uTexa.z == 1 && black) ? 0.0 : float(uTexa.x));
    }
    return vec4(rgb, a);
}

bool alphaTest(float a)
{
    if (uAte == 0) return true;
    if (uAtst == 0) return false;
    if (uAtst == 1) return true;
    float r = float(uAref);
    if (uAtst == 2) return a < r;
    if (uAtst == 3) return a <= r;
    if (uAtst == 4) return a == r;
    if (uAtst == 5) return a >= r;
    if (uAtst == 6) return a > r;
    return a != r;
}

void main()
{
    vec4 cf = vColor;
    vec4 c = cf;
    if (uTme == 1)
    {
        vec2 st = vStq.xy;
        // Native resolution samples exactly where the GS does: no clamp needed, unless scaled sprites carry a tight
        // range (uSpriteClamp, see spriteTightClampOn). Vertices without a range hold +-1e30: the clamp is a no-op.
        if (uScaleI != ivec2(1) || uSpriteClamp != 0)
            st = clamp(st, vClamp.xy, vClamp.zw);
        vec2 uv = (uFst == 1) ? st : (st / vStq.z) * vec2(uTexSize);
        vec4 ct;
        if (uTexRt != 0)
        {
            // Render target: filter its scaled texels (at scale 1 the same as below).
            vec2 hp = uv * vec2(uScaleI);
            if (uLinear == 0)
                ct = rtTexel(ivec2(floor(hp)));
            else
            {
                vec2 f = hp - 0.5;
                ivec2 i0 = ivec2(floor(f));
                vec2 fr = f - floor(f);
                ct = mix(mix(rtTexel(i0), rtTexel(i0 + ivec2(1, 0)), fr.x),
                         mix(rtTexel(i0 + ivec2(0, 1)), rtTexel(i0 + ivec2(1, 1)), fr.x), fr.y);
                ct = floor(ct);
            }
        }
        else if (uLinear == 0)
        {
            ct = texel(ivec2(floor(uv)));
        }
        else
        {
            vec2 f = uv - 0.5;
            ivec2 i0 = ivec2(floor(f));
            vec2 fr = f - floor(f);
            ct = mix(mix(texel(i0), texel(i0 + ivec2(1, 0)), fr.x),
                     mix(texel(i0 + ivec2(0, 1)), texel(i0 + ivec2(1, 1)), fr.x), fr.y);
            ct = floor(ct);
        }
        if (uTfx == 0)
        {
            c.rgb = min(floor(ct.rgb * cf.rgb / 128.0), 255.0);
            c.a = (uTcc == 1) ? min(floor(ct.a * cf.a / 128.0), 255.0) : cf.a;
        }
        else if (uTfx == 1)
        {
            c.rgb = ct.rgb;
            c.a = (uTcc == 1) ? ct.a : cf.a;
        }
        else if (uTfx == 2)
        {
            c.rgb = min(floor(ct.rgb * cf.rgb / 128.0) + cf.a, 255.0);
            c.a = (uTcc == 1) ? min(ct.a + cf.a, 255.0) : cf.a;
        }
        else
        {
            c.rgb = min(floor(ct.rgb * cf.rgb / 128.0) + cf.a, 255.0);
            c.a = (uTcc == 1) ? ct.a : cf.a;
        }
    }
    if (uFge == 1)
    {
        float f = floor(vFog + 0.5);
        c.rgb = floor(f * c.rgb / 256.0) + floor((255.0 - f) * uFogColor / 256.0);
    }
    bool pass = alphaTest(floor(c.a + 0.5));
    if ((uAlphaPass == 0 && !pass) || (uAlphaPass == 1 && pass))
        discard;
    // Colour stored as 0..1 (byte/255); alpha stored as byte/128 so blend factors match the GS.
    float a = clamp(c.a, 0.0, 255.0);
    oBlend = vec4(0.0, 0.0, 0.0, a / 128.0);
    if (uFba == 1 && a < 128.0)
        a += 128.0;
    vec3 rgb = clamp(c.rgb, 0.0, 255.0);
    if (uFbMask == 1)
    {
        // FBMSK per bit: stored = (destination & mask) | (new & ~mask), on the stored bytes (rgb byte/255, alpha
        // byte/128). Channels masked in full are also off in glColorMask; this covers the partial ones.
        vec4 d = texelFetch(uDst, ivec2(gl_FragCoord.xy), 0);
        uvec3 drgb = uvec3(clamp(floor(d.rgb * 255.0 + 0.5), 0.0, 255.0));
        uint da = uint(clamp(floor(d.a * 128.0 + 0.5), 0.0, 255.0));
        uvec3 srgb = uvec3(floor(rgb + 0.5));
        uint sa = uint(floor(a + 0.5));
        uvec4 m = uvec4(uMaskBits) & uvec4(255u);
        rgb = vec3((drgb & m.rgb) | (srgb & (~m.rgb & uvec3(255u))));
        a = float((da & m.a) | (sa & (~m.a & 255u)));
    }
    oColor = vec4(rgb / 255.0, a / 128.0);
}
)";
        static const char *kPresentVs = R"(#version 330 core
uniform vec4 uRect; // source rect in texels: x, y, w, h
uniform vec2 uSrcSize;
out vec2 vUv;
void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vUv = (uRect.xy + corner * uRect.zw) / uSrcSize;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";
        static const char *kPresentFs = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform int uAlphaMode; // 0 opaque, 1 constant (blend colour), 2 per-pixel alpha
out vec4 oColor;
void main()
{
    vec4 c = texture(uTex, vUv);
    oColor = vec4(clamp(c.rgb, 0.0, 1.0), uAlphaMode == 2 ? clamp(c.a, 0.0, 1.0) : 1.0);
}
)";
        static const char *kDateVs = R"(#version 330 core
uniform vec4 uRect; // pixel rect in the render target: x, y, w, h
uniform vec2 uRtSize;
void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    gl_Position = vec4((uRect.xy + corner * uRect.zw) / uRtSize * 2.0 - 1.0, 0.0, 1.0);
}
)";
        // Alpha is stored as A/128, so the GS alpha MSB (A >= 0x80) is "stored alpha >= 1.0".
        static const char *kDateFs = R"(#version 330 core
uniform sampler2D uTex; // copy of the render target
uniform int uDatm;
void main()
{
    bool msb = texelFetch(uTex, ivec2(gl_FragCoord.xy), 0).a * 128.0 >= 127.5;
    if (msb != (uDatm == 1))
        discard;
}
)";
        program = link(kVs, kFs);
        presentProgram = link(kPresentVs, kPresentFs);
        dateProgram = link(kDateVs, kDateFs);
        if (dateProgram != 0)
        {
            dateRect = gl::GetUniformLocation(dateProgram, "uRect");
            dateRtSize = gl::GetUniformLocation(dateProgram, "uRtSize");
            dateTex = gl::GetUniformLocation(dateProgram, "uTex");
            dateDatm = gl::GetUniformLocation(dateProgram, "uDatm");
        }
        if (program == 0 || presentProgram == 0)
        {
            glFailed = true;
            return false;
        }
        auto loc = [&](const char *n) { return gl::GetUniformLocation(program, n); };
        u.rtSize = loc("uRtSize");
        u.tex = loc("uTex");
        u.pal = loc("uPal");
        u.palette = loc("uPalette");
        u.tme = loc("uTme");
        u.tfx = loc("uTfx");
        u.tcc = loc("uTcc");
        u.fst = loc("uFst");
        u.linear = loc("uLinear");
        u.texSize = loc("uTexSize");
        u.wrap = loc("uWrap");
        u.clampUV = loc("uClampUV");
        u.fge = loc("uFge");
        u.fogColor = loc("uFogColor");
        u.ate = loc("uAte");
        u.atst = loc("uAtst");
        u.aref = loc("uAref");
        u.alphaPass = loc("uAlphaPass");
        u.fba = loc("uFba");
        u.texRt = loc("uTexRt");
        u.texOffset = loc("uTexOffset");
        u.texa = loc("uTexa");
        u.scale = loc("uScale");
        u.scaleI = loc("uScaleI");
        u.spriteClamp = loc("uSpriteClamp");
        u.dst = loc("uDst");
        u.fbMask = loc("uFbMask");
        u.maskBits = loc("uMaskBits");
        u.texRtSize = loc("uTexRtSize");
        gl::GetIntegerv(gl::MAX_TEXTURE_SIZE, &maxTextureSize);
        gl::GenQueries(static_cast<gl::GLsizei>(timerQueries.size()), timerQueries.data());
        presentTex = gl::GetUniformLocation(presentProgram, "uTex");
        presentRect = gl::GetUniformLocation(presentProgram, "uRect");
        presentSrcSize = gl::GetUniformLocation(presentProgram, "uSrcSize");
        presentAlphaMode = gl::GetUniformLocation(presentProgram, "uAlphaMode");

        gl::GenVertexArrays(1, &vao);
        gl::GenBuffers(1, &vbo);
        gl::BindVertexArray(vao);
        gl::BindBuffer(gl::ARRAY_BUFFER, vbo);
        const gl::GLsizei stride = sizeof(GpuVertex);
        gl::VertexAttribPointer(0, 3, gl::FLOAT, 0, stride, reinterpret_cast<const void *>(offsetof(GpuVertex, x)));
        gl::VertexAttribPointer(1, 3, gl::FLOAT, 0, stride, reinterpret_cast<const void *>(offsetof(GpuVertex, s)));
        gl::VertexAttribPointer(2, 4, gl::UNSIGNED_BYTE, 0, stride, reinterpret_cast<const void *>(offsetof(GpuVertex, r)));
        gl::VertexAttribPointer(3, 1, gl::FLOAT, 0, stride, reinterpret_cast<const void *>(offsetof(GpuVertex, fog)));
        gl::VertexAttribPointer(4, 4, gl::FLOAT, 0, stride, reinterpret_cast<const void *>(offsetof(GpuVertex, cu0)));
        for (gl::GLuint i = 0; i < 5; ++i)
            gl::EnableVertexAttribArray(i);
        gl::BindVertexArray(0);
        gl::GenVertexArrays(1, &presentVao);
        gl::GenFramebuffers(1, &presentFbo);
        glReady = true;
        std::cout << "[gpu] OpenGL GS backend ready" << std::endl;
        return true;
    }

    gl::GLuint newTexture(gl::GLenum internalFormat, gl::GLenum format, gl::GLenum type, uint32_t w, uint32_t h)
    {
        ++glEpoch;
        gl::GLuint t = 0;
        gl::GenTextures(1, &t);
        gl::BindTexture(gl::TEXTURE_2D, t);
        gl::TexImage2D(gl::TEXTURE_2D, 0, static_cast<gl::GLint>(internalFormat), static_cast<gl::GLsizei>(w),
                       static_cast<gl::GLsizei>(h), 0, format, type, nullptr);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
        return t;
    }

    // A colour (RGBA16F) or depth + stencil texture for a w x h area of GS pixels at the current internal resolution,
    // cleared like fresh local memory: black with alpha 0, or depth at the far value (GS depth tests are GEQUAL /
    // GREATER). The stencil implements DATE (destination alpha test).
    gl::GLuint newTargetTexture(bool depth, uint32_t w, uint32_t h)
    {
        uint32_t gw = w * sx, gh = h * sy;
        const uint32_t maxSize = maxTextureSize > 0 ? static_cast<uint32_t>(maxTextureSize) : 16384u;
        if (gw > maxSize || gh > maxSize)
        {
            static bool s_warned = false;
            if (!s_warned)
            {
                s_warned = true;
                std::cerr << "[gpu] render target " << gw << "x" << gh << " exceeds the GPU's texture limit " << maxSize
                          << ": clipped (lower the internal resolution)" << std::endl;
            }
            gw = std::min(gw, maxSize);
            gh = std::min(gh, maxSize);
        }
        const gl::GLuint t = depth ? newTexture(gl::DEPTH32F_STENCIL8, gl::DEPTH_STENCIL, gl::FLOAT_32_UNSIGNED_INT_24_8_REV, gw, gh)
                                   : newTexture(gl::RGBA16F, gl::RGBA, gl::FLOAT, gw, gh);
        gl::BindFramebuffer(gl::FRAMEBUFFER, depth ? framebufferFor(0, t) : framebufferFor(t, 0));
        gl::Disable(gl::SCISSOR_TEST);
        if (depth)
        {
            gl::DepthMask(1);
            gl::ClearDepth(0.0);
            gl::Clear(gl::DEPTH_BUFFER_BIT);
        }
        else
        {
            gl::ColorMask(1, 1, 1, 1);
            gl::ClearColor(0, 0, 0, 0);
            gl::Clear(gl::COLOR_BUFFER_BIT);
        }
        return t;
    }

    // Deletes the framebuffer objects that use texture `tex` (before the texture is deleted or replaced).
    void dropFramebuffers(gl::GLuint tex)
    {
        ++glEpoch;
        for (auto it = framebuffers.begin(); it != framebuffers.end();)
        {
            if (it->first.first == tex || it->first.second == tex)
            {
                gl::DeleteFramebuffers(1, &it->second);
                it = framebuffers.erase(it);
            }
            else
                ++it;
        }
    }

    // Makes a render target or depth buffer at least `rows` GS rows tall, keeping its contents.
    void ensureRows(RenderTarget &t, uint32_t rows, bool depth)
    {
        rows = roundRows(rows);
        if (t.color != 0 && t.height >= rows)
            return;
        const uint32_t newRows = std::max(rows, t.height);
        const gl::GLuint fresh = newTargetTexture(depth, t.width, newRows);
        if (t.color != 0)
        {
            ++statGrows;
            const int w = static_cast<int>(t.width * sx), h = static_cast<int>(t.height * sy);
            // Copies sized for the old colour target are not used again.
            for (auto *copies : {&scratchTextures, &dateScratchTextures, &maskScratchTextures})
            {
                if (depth)
                    break;
                auto old = copies->find({static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
                if (old != copies->end())
                {
                    dropFramebuffers(old->second);
                    gl::DeleteTextures(1, &old->second);
                    copies->erase(old);
                }
            }
            blitPixels(t.color, 0, 0, w, h, fresh, 0, 0, w, h, depth, false);
            dropFramebuffers(t.color);
            gl::DeleteTextures(1, &t.color);
        }
        t.color = fresh;
        t.height = newRows;
    }

    // The render target of frame buffer (fbp, fbw), at least `rows` rows tall.
    RenderTarget &renderTarget(uint32_t fbp, uint32_t fbw, uint32_t rows)
    {
        auto &rt = renderTargets[{fbp, fbw}];
        if (rt.color == 0)
            rt.width = std::max<uint32_t>(fbw, 1u) * 64u;
        ensureRows(rt, rows, false);
        return rt;
    }

    gl::GLuint depthBuffer(uint32_t zbp, uint32_t fbw, uint32_t rows)
    {
        auto &d = depthBuffers[{zbp, fbw}];
        if (d.color == 0)
            d.width = std::max<uint32_t>(fbw, 1u) * 64u;
        ensureRows(d, rows, true);
        return d.color;
    }

    gl::GLuint framebufferFor(gl::GLuint color, gl::GLuint depth)
    {
        auto &fbo = framebuffers[{color, depth}];
        if (fbo == 0)
        {
            ++glEpoch;
            gl::GenFramebuffers(1, &fbo);
            gl::BindFramebuffer(gl::FRAMEBUFFER, fbo);
            if (color)
                gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, color, 0);
            if (depth)
                gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::DEPTH_STENCIL_ATTACHMENT, gl::TEXTURE_2D, depth, 0);
            const gl::GLenum status = gl::CheckFramebufferStatus(gl::FRAMEBUFFER);
            if (status != gl::FRAMEBUFFER_COMPLETE)
                std::cerr << "[gpu] framebuffer incomplete: 0x" << std::hex << status << std::dec << std::endl;
        }
        return fbo;
    }

    // A colour texture for a w x h area of GS pixels at the current internal resolution (copies of render targets).
    gl::GLuint scratchTexture(uint32_t w, uint32_t h)
    {
        auto &t = scratchTextures[{w * sx, h * sy}];
        if (t == 0)
            t = newTexture(gl::RGBA16F, gl::RGBA, gl::FLOAT, w * sx, h * sy);
        return t;
    }

    // Copies a rectangle of GL pixels between two textures (colour, or depth + stencil), scaling when the rectangles
    // differ in size.
    void blitPixels(gl::GLuint src, int x0, int y0, int x1, int y1, gl::GLuint dst, int dx0, int dy0, int dx1, int dy1,
                    bool depth, bool linear)
    {
        ++glEpoch;
        // Look both framebuffers up first: creating one binds it to both read and draw.
        const gl::GLuint readFbo = depth ? framebufferFor(0, src) : framebufferFor(src, 0);
        const gl::GLuint drawFbo = depth ? framebufferFor(0, dst) : framebufferFor(dst, 0);
        gl::BindFramebuffer(gl::READ_FRAMEBUFFER, readFbo);
        gl::BindFramebuffer(gl::DRAW_FRAMEBUFFER, drawFbo);
        gl::Disable(gl::SCISSOR_TEST);
        if (depth)
        {
            gl::DepthMask(1);
            gl::StencilMask(0xFF);
        }
        else
            gl::ColorMask(1, 1, 1, 1);
        gl::BlitFramebuffer(x0, y0, x1, y1, dx0, dy0, dx1, dy1,
                            depth ? (gl::DEPTH_BUFFER_BIT | gl::STENCIL_BUFFER_BIT) : gl::COLOR_BUFFER_BIT,
                            (linear && !depth) ? gl::LINEAR : gl::NEAREST);
        gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    }

    // Copies a rectangle (in GS pixels) between colour textures of render targets / scratch textures.
    void blit(gl::GLuint src, int x, int y, gl::GLuint dst, int dx, int dy, int w, int h)
    {
        const int kx = static_cast<int>(sx), ky = static_cast<int>(sy);
        blitPixels(src, x * kx, y * ky, (x + w) * kx, (y + h) * ky, dst, dx * kx, dy * ky, (dx + w) * kx, (dy + h) * ky,
                   false, false);
    }

    // Scissor rectangle in GS pixels.
    void scissor(int x, int y, int w, int h)
    {
        gl::Scissor(x * static_cast<int>(sx), y * static_cast<int>(sy), w * static_cast<int>(sx), h * static_cast<int>(sy));
    }

    // Switches the internal resolution: every render target and depth buffer is redrawn at the new size from its
    // current contents (so effects that keep pictures in GS memory survive the switch); copies are dropped.
    void applyScale(uint32_t nx, uint32_t ny)
    {
        if (nx == sx && ny == sy)
            return;
        const uint32_t ox = sx, oy = sy;
        sx = nx;
        sy = ny;
        std::vector<gl::GLuint> old;
        uint64_t bytes = 0;
        auto rescale = [&](RenderTarget &t, bool depth)
        {
            if (t.color == 0)
                return;
            const gl::GLuint fresh = newTargetTexture(depth, t.width, t.height);
            // Only the vertical factor changing (display line doubling on/off) is an exact 2:1 in rows: copy rows
            // unfiltered so the picture does not soften.
            blitPixels(t.color, 0, 0, static_cast<int>(t.width * ox), static_cast<int>(t.height * oy), fresh, 0, 0,
                       static_cast<int>(t.width * nx), static_cast<int>(t.height * ny), depth, !depth && nx != ox);
            old.push_back(t.color);
            t.color = fresh;
            bytes += static_cast<uint64_t>(t.width * nx) * (t.height * ny) * 8u;
        };
        for (auto &[key, t] : renderTargets)
            rescale(t, false);
        for (auto &[key, t] : depthBuffers)
            rescale(t, true);
        for (auto &[key, fbo] : framebuffers)
            gl::DeleteFramebuffers(1, &fbo);
        framebuffers.clear();
        for (gl::GLuint t : old)
            gl::DeleteTextures(1, &t);
        for (auto *copies : {&scratchTextures, &dateScratchTextures, &maskScratchTextures})
        {
            for (auto &[key, t] : *copies)
                gl::DeleteTextures(1, &t);
            copies->clear();
        }
        gl::GLuint dropped[3] = {uploadScratch, fieldHistory, fieldCurrent};
        for (gl::GLuint t : dropped)
            if (t != 0)
                gl::DeleteTextures(1, &t);
        uploadScratch = fieldHistory = fieldCurrent = 0;
        uploadScratchW = uploadScratchH = fieldTexW = fieldTexH = 0;
        fieldHistoryValid = false;
        presentFboTexture = 0; // the window loop replaced the present texture for the new size
        g_scaleX.store(nx, std::memory_order_relaxed);
        g_scaleY.store(ny, std::memory_order_relaxed);
        std::cout << "[gpu] internal resolution " << nx << "x" << ny << " (per axis): " << renderTargets.size()
                  << " render targets + " << depthBuffers.size() << " depth buffers, " << bytes / (1024 * 1024) << " MB"
                  << std::endl;
    }

    // CPU uploads into a render target at a higher internal resolution. ZOE's movies write each frame as ~660 small
    // blocks; uploading every block into a small staging texture and copying it out right away made the CPU wait for
    // the GPU on each block (the staging texture was still being read), so movie frames took 1-4 s. A run of uploads
    // to the same target goes into a GS-sized staging copy first, at the blocks' own positions, and is then
    // stretched into the target with as few copies as the blocks allow. Returns the index after the run.
    std::vector<float> writeFloats;
    struct WriteRect
    {
        int x, y, w, h;
    };
    std::vector<WriteRect> writeRects, writeMerged;

    size_t executeWriteRun(size_t first)
    {
        const Command &head = replay.commands[first];
        size_t end = first;
        uint32_t rows = 0;
        while (end < replay.commands.size() && replay.commands[end].type == CmdType::FramebufferWrite &&
               replay.commands[end].id == head.id && replay.commands[end].fbw == head.fbw)
        {
            rows = std::max(rows, replay.commands[end].y + replay.commands[end].h);
            ++end;
        }
        RenderTarget &rt = renderTarget(head.id, head.fbw, std::min(kRtHeight, rows));
        if (uploadScratch == 0 || uploadScratchW < rt.width || uploadScratchH < rt.height)
        {
            if (uploadScratch != 0)
            {
                dropFramebuffers(uploadScratch);
                gl::DeleteTextures(1, &uploadScratch);
            }
            uploadScratchW = std::max(uploadScratchW, rt.width);
            uploadScratchH = std::max(uploadScratchH, rt.height);
            uploadScratch = newTexture(gl::RGBA16F, gl::RGBA, gl::FLOAT, uploadScratchW, uploadScratchH);
        }
        ++glEpoch;
        gl::BindTexture(gl::TEXTURE_2D, uploadScratch);
        writeRects.clear();
        for (size_t i = first; i < end; ++i)
        {
            const Command &c = replay.commands[i];
            if (c.w == 0u || c.h == 0u || c.x + c.w > rt.width || c.y + c.h > rt.height)
                continue; // outside the target (the direct path at x1 cannot write it either)
            const size_t n = static_cast<size_t>(c.w) * c.h;
            writeFloats.resize(n * 4u);
            const uint32_t *src = replay.pixels.data() + c.first;
            for (size_t k = 0; k < n; ++k)
            {
                writeFloats[k * 4 + 0] = (src[k] & 0xFF) / 255.0f;
                writeFloats[k * 4 + 1] = ((src[k] >> 8) & 0xFF) / 255.0f;
                writeFloats[k * 4 + 2] = ((src[k] >> 16) & 0xFF) / 255.0f;
                writeFloats[k * 4 + 3] = ((src[k] >> 24) & 0xFF) / 128.0f;
            }
            gl::TexSubImage2D(gl::TEXTURE_2D, 0, c.x, c.y, c.w, c.h, gl::RGBA, gl::FLOAT, writeFloats.data());
            const WriteRect r{static_cast<int>(c.x), static_cast<int>(c.y), static_cast<int>(c.w), static_cast<int>(c.h)};
            WriteRect *last = writeRects.empty() ? nullptr : &writeRects.back();
            if (last != nullptr && last->y == r.y && last->h == r.h && last->x + last->w == r.x)
                last->w += r.w; // the next block of the same row of blocks
            else
                writeRects.push_back(r);
        }
        // Rows of blocks with the same left edge and width stacked one under the other: one copy.
        writeMerged.clear();
        for (const WriteRect &r : writeRects)
        {
            WriteRect *last = writeMerged.empty() ? nullptr : &writeMerged.back();
            if (last != nullptr && last->x == r.x && last->w == r.w && last->y + last->h == r.y)
                last->h += r.h;
            else
                writeMerged.push_back(r);
        }
        const int kx = static_cast<int>(sx), ky = static_cast<int>(sy);
        for (const WriteRect &r : writeMerged)
            blitPixels(uploadScratch, r.x, r.y, r.x + r.w, r.y + r.h, rt.color, r.x * kx, r.y * ky, (r.x + r.w) * kx,
                       (r.y + r.h) * ky, false, false);
        statWriteRuns += 1u;
        statWriteBlits += writeMerged.size();
        return end;
    }

    void executeCopy(const Command &cmd)
    {
        RenderTarget &src = renderTarget(cmd.id, cmd.fbw, cmd.y + cmd.h);
        RenderTarget &dst = renderTarget(cmd.dstFbp, cmd.dstFbw, cmd.dy + cmd.h);
        const int w = static_cast<int>(std::min(cmd.w, src.width - std::min(src.width, cmd.x)));
        const int h = static_cast<int>(std::min(cmd.h, src.height - std::min(src.height, cmd.y)));
        if (w <= 0 || h <= 0)
            return;
        ++statRectCopies;
        if (src.color == dst.color)
        {
            const gl::GLuint tmp = scratchTexture(src.width, src.height);
            blit(src.color, cmd.x, cmd.y, tmp, cmd.x, cmd.y, w, h);
            blit(tmp, cmd.x, cmd.y, dst.color, cmd.dx, cmd.dy, w, h);
        }
        else
        {
            blit(src.color, cmd.x, cmd.y, dst.color, cmd.dx, cmd.dy, w, h);
        }
    }

    struct BlendSetup
    {
        bool enable;
        gl::GLenum eq, src, dst;
        bool constant;
        float constantAlpha;
    };

    // GS: Cv = (A - B) * C / 128 + D, A/B/D in {Cs, Cd, 0}, C in {As, Ad, FIX}. Colour is
    // stored as byte/255 and alpha as byte/128, so As/Ad/FIX map directly onto GL factors.
    static BlendSetup blendFor(const GpuDrawState &ds)
    {
        BlendSetup b{false, gl::FUNC_ADD, gl::ONE, gl::ZERO, false, 0.0f};
        if (!ds.abe)
            return b;
        b.enable = true;
        gl::GLenum f, omf;
        if (ds.blendC == 0) { f = gl::SRC1_ALPHA; omf = gl::ONE_MINUS_SRC1_ALPHA; }
        else if (ds.blendC == 1) { f = gl::DST_ALPHA; omf = gl::ONE_MINUS_DST_ALPHA; }
        else { f = gl::CONSTANT_ALPHA; omf = gl::ONE_MINUS_CONSTANT_ALPHA; b.constant = true; b.constantAlpha = ds.fix / 128.0f; }
        const int A = ds.blendA > 2 ? 2 : ds.blendA, B = ds.blendB > 2 ? 2 : ds.blendB, D = ds.blendD > 2 ? 2 : ds.blendD;
        // Coefficients for Cs and Cd expressed as: 0, +f, -f, +1, 1-f, 1+f (approximated).
        enum Coef { Z, F, NF, O, OMF, OPF };
        Coef cs = Z, cd = Z;
        auto addF = [&](int which, int sign)
        {
            Coef &c = (which == 0) ? cs : cd;
            c = (sign > 0) ? F : NF;
        };
        if (A != B)
        {
            if (A != 2) addF(A, +1);
            if (B != 2) addF(B, -1);
        }
        if (D != 2)
        {
            Coef &c = (D == 0) ? cs : cd;
            c = (c == Z) ? O : (c == F ? OPF : OMF);
        }
        auto factor = [&](Coef c) -> gl::GLenum
        {
            switch (c)
            {
            case Z: return gl::ZERO;
            case F: case NF: return f;
            case O: case OPF: return gl::ONE; // 1+f approximated as 1
            case OMF: return omf;
            }
            return gl::ZERO;
        };
        const bool csNeg = cs == NF, cdNeg = cd == NF;
        b.src = factor(cs);
        b.dst = factor(cd);
        if (csNeg && cdNeg)
        {
            b.src = gl::ZERO;
            b.dst = gl::ZERO;
        }
        else if (cdNeg)
            b.eq = gl::FUNC_SUBTRACT;
        else if (csNeg)
            b.eq = gl::FUNC_REVERSE_SUBTRACT;
        return b;
    }

    // PS2X_GPU_RTDUMP=<fbp hex>,<vsync tick>: once, at the first draw at or after that tick that samples the render
    // target at that fbp, write the target as it is before the draw (RGBA floats, GL rows as stored) to
    // rtdump_<fbp>_<tick>.f32 in the working directory. Diagnostics (ZOE Nebula waterfall texture).
    void dumpSampledTarget(uint32_t fbp, const RenderTarget &t)
    {
        static uint32_t s_fbp = 0;
        static unsigned long long s_tick = 0;
        static bool s_pending = []
        {
            const char *e = std::getenv("PS2X_GPU_RTDUMP");
            return e && std::sscanf(e, "%x,%llu", &s_fbp, &s_tick) == 2;
        }();
        if (!s_pending || fbp != s_fbp || ps2CurrentVsyncTick() < s_tick || t.color == 0)
            return;
        s_pending = false;
        const uint32_t w = t.width * sx, h = t.height * sy;
        std::vector<float> px(static_cast<size_t>(w) * h * 4u);
        gl::BindTexture(gl::TEXTURE_2D, t.color);
        dc.tex0 = ~0u; // the draw's own texture bind must not be skipped by the state cache
        gl::PixelStorei(0x0D05 /* PACK_ALIGNMENT */, 1);
        gl::GetTexImage(gl::TEXTURE_2D, 0, gl::RGBA, gl::FLOAT, px.data());
        char name[64];
        std::snprintf(name, sizeof(name), "rtdump_%x_%llu.f32", fbp, static_cast<unsigned long long>(ps2CurrentVsyncTick()));
        std::FILE *f = std::fopen(name, "wb");
        const bool written = f != nullptr && std::fwrite(px.data(), sizeof(float), px.size(), f) == px.size();
        if (f != nullptr)
            std::fclose(f);
        std::cerr << "[gpu:rtdump] " << (written ? "" : "FAILED to write ") << name << " " << w << "x" << h << " (GS "
                  << t.width << "x" << t.height << ")" << std::endl;
    }

    void executeDraw(const Command &cmd, int &currentProgramState)
    {

        (void)currentProgramState;
        const GpuDrawState &ds = replay.states[cmd.state];
        gl::GLuint profStart = 0;
        if (drawProf())
        {
            profStart = profQuery();
            gl::QueryCounter(profStart, gl::TIMESTAMP);
        }
        // Rows this draw can touch: up to the bottom of its scissor.
        const uint32_t rows = std::min<uint32_t>(kRtHeight, static_cast<uint32_t>(ds.sc[3]) + 1u);
        RenderTarget &rt = renderTarget(ds.fbp, ds.fbw, rows);
        gl::GLuint rtTexture = 0;
        uint32_t rtTextureW = 0, rtTextureH = 0;
        if (ds.tme && ds.texRt)
        {
            RenderTarget &srcRt = renderTarget(ds.texRtFbp, ds.texRtFbw, 0u);
            rtTexture = srcRt.color;
            rtTextureW = srcRt.width;
            rtTextureH = srcRt.height;
            dumpSampledTarget(ds.texRtFbp, srcRt);
            if (srcRt.color == rt.color)
            {
                // Sampling the target being drawn: read from a copy of the sampled region.
                const gl::GLuint tmp = scratchTexture(srcRt.width, srcRt.height);
                const int w = std::min<int>(ds.tw, static_cast<int>(srcRt.width) - ds.texOffX);
                const int h = std::min<int>(ds.th, static_cast<int>(srcRt.height) - ds.texOffY);
                if (w > 0 && h > 0)
                {
                    blit(srcRt.color, ds.texOffX, ds.texOffY, tmp, ds.texOffX, ds.texOffY, w, h);
                    ++statSelfCopies;
                    statSelfCopyPx += static_cast<uint64_t>(w) * h;
                }
                rtTexture = tmp;
            }
        }
        // Like the CPU rasterizer, ZTE is ignored: ZTST decides the test and ZMSK the write.
        const bool useDepth = ds.ztst != 1;
        const bool writesDepth = !ds.zmsk;
        const bool date = ds.date && dateProgram != 0;
        // DATE needs the stencil that lives with the depth buffer, even for draws without a Z test.
        gl::GLuint depth = (useDepth || writesDepth || date) ? depthBuffer(ds.zbp, ds.fbw, rows) : 0;
        if (date)
            prepareDateStencil(ds, cmd, rt, depth);
        const bool maskPartial = partialFrameMask(ds);
        if (maskPartial)
            prepareMaskCopy(ds, cmd, rt);
        // From here on, GL calls whose value is already in place are skipped (see DrawCache).
        if (dc.epoch != glEpoch || !stateCache())
        {
            dc = DrawCache{};
            dc.epoch = glEpoch;
        }
        const gl::GLuint fbo = framebufferFor(rt.color, depth);
        if (dc.fbo != fbo)
        {
            gl::BindFramebuffer(gl::FRAMEBUFFER, fbo);
            dc.fbo = fbo;
        }
        const int vpW = static_cast<int>(rt.width * sx), vpH = static_cast<int>(rt.height * sy);
        if (dc.vp[0] != vpW || dc.vp[1] != vpH)
        {
            gl::Viewport(0, 0, vpW, vpH);
            dc.vp[0] = vpW;
            dc.vp[1] = vpH;
        }
        if (dc.scissorOn != 1)
        {
            gl::Enable(gl::SCISSOR_TEST);
            dc.scissorOn = 1;
        }
        const int sc[4] = {ds.sc[0], ds.sc[2], std::max(0, ds.sc[1] - ds.sc[0] + 1), std::max(0, ds.sc[3] - ds.sc[2] + 1)};
        if (std::memcmp(dc.sc, sc, sizeof(sc)) != 0)
        {
            scissor(sc[0], sc[1], sc[2], sc[3]);
            std::memcpy(dc.sc, sc, sizeof(sc));
        }

        if (depth && (useDepth || writesDepth))
        {
            if (dc.depthOn != 1)
            {
                gl::Enable(gl::DEPTH_TEST);
                dc.depthOn = 1;
            }
            const gl::GLenum fn = ds.ztst == 0 ? gl::NEVER : ds.ztst == 1 ? gl::ALWAYS : ds.ztst == 2 ? gl::GEQUAL : gl::GREATER;
            if (dc.depthFunc != fn)
            {
                gl::DepthFunc(fn);
                dc.depthFunc = fn;
            }
        }
        else if (dc.depthOn != 0)
        {
            gl::Disable(gl::DEPTH_TEST);
            dc.depthOn = 0;
        }

        const BlendSetup blend = blendFor(ds);
        if (blend.enable)
        {
            if (dc.blendOn != 1)
            {
                gl::Enable(gl::BLEND);
                dc.blendOn = 1;
            }
            if (dc.blendEq != blend.eq)
            {
                gl::BlendEquationSeparate(blend.eq, gl::FUNC_ADD);
                dc.blendEq = blend.eq;
            }
            if (dc.blendSrc != blend.src || dc.blendDst != blend.dst)
            {
                gl::BlendFuncSeparate(blend.src, blend.dst, gl::ONE, gl::ZERO);
                dc.blendSrc = blend.src;
                dc.blendDst = blend.dst;
            }
            if (blend.constant && dc.blendConstant != blend.constantAlpha)
            {
                gl::BlendColor(0, 0, 0, blend.constantAlpha);
                dc.blendConstant = blend.constantAlpha;
            }
        }
        else if (dc.blendOn != 0)
        {
            gl::Disable(gl::BLEND);
            dc.blendOn = 0;
        }

        auto f = [](auto v) { return static_cast<float>(v); };
        if (!uSame(kURtSize, f(rt.width), f(rt.height)))
            gl::Uniform2f(u.rtSize, static_cast<float>(rt.width), static_cast<float>(rt.height));
        if (!uSame(kUTex, f(ds.tme), f(ds.tfx), f(ds.tcc), f(ds.fst)))
        {
            gl::Uniform1i(u.tme, ds.tme);
            gl::Uniform1i(u.tfx, ds.tfx);
            gl::Uniform1i(u.tcc, ds.tcc);
            gl::Uniform1i(u.fst, ds.fst);
        }
        if (!uSame(kULinear, f(ds.linear), f(ds.tw), f(ds.th)))
        {
            gl::Uniform1i(u.linear, ds.linear);
            gl::Uniform2i(u.texSize, ds.tw, ds.th);
        }
        if (!uSame(kUWrap, f(ds.wms), f(ds.wmt)))
            gl::Uniform2i(u.wrap, ds.wms, ds.wmt);
        if (!uSame(kUClamp, f(ds.minu), f(ds.maxu), f(ds.minv), f(ds.maxv)))
            gl::Uniform4i(u.clampUV, ds.minu, ds.maxu, ds.minv, ds.maxv);
        if (!uSame(kUFog, f(ds.fge), f(ds.fogR), f(ds.fogG), f(ds.fogB)))
        {
            gl::Uniform1i(u.fge, ds.fge);
            gl::Uniform3f(u.fogColor, ds.fogR, ds.fogG, ds.fogB);
        }
        if (!uSame(kUAlphaTest, f(ds.ate), f(ds.atst), f(ds.aref), f(ds.fba)))
        {
            gl::Uniform1i(u.ate, ds.ate);
            gl::Uniform1i(u.atst, ds.atst);
            gl::Uniform1i(u.aref, ds.aref);
            gl::Uniform1i(u.fba, ds.fba);
        }
        const int texRtMode = ds.tme ? ds.texRt : 0;
        if (!uSame(kUTexRt, f(texRtMode)))
            gl::Uniform1i(u.texRt, texRtMode);
        if (ds.tme && ds.texRt)
        {
            if (!uSame(kUTexOffset, f(ds.texOffX), f(ds.texOffY), f(rtTextureW), f(rtTextureH)))
            {
                gl::Uniform2i(u.texOffset, ds.texOffX, ds.texOffY);
                gl::Uniform2i(u.texRtSize, static_cast<gl::GLint>(rtTextureW), static_cast<gl::GLint>(rtTextureH));
            }
            if (!uSame(kUTexa, f(ds.ta0), f(ds.ta1), f(ds.aem)))
                gl::Uniform3i(u.texa, ds.ta0, ds.ta1, ds.aem);
            if (dc.tex0 != rtTexture)
            {
                gl::BindTexture(gl::TEXTURE_2D, rtTexture);
                dc.tex0 = rtTexture;
            }
        }
        else if (ds.tme)
        {
            auto it = glTextures.find(ds.textureId);
            const gl::GLuint t0 = it != glTextures.end() ? it->second : 0;
            if (dc.tex0 != t0)
            {
                gl::BindTexture(gl::TEXTURE_2D, t0);
                dc.tex0 = t0;
            }
            if (ds.paletteId != 0)
            {
                auto p = glTextures.find(ds.paletteId);
                const gl::GLuint t1 = p != glTextures.end() ? p->second : 0;
                if (dc.tex1 != t1)
                {
                    gl::ActiveTexture(gl::TEXTURE1);
                    gl::BindTexture(gl::TEXTURE_2D, t1);
                    gl::ActiveTexture(gl::TEXTURE0);
                    dc.tex1 = t1;
                }
            }
        }
        const int paletteMode = (ds.tme && !ds.texRt && ds.paletteId != 0) ? 1 : 0;
        if (!uSame(kUPalette, f(paletteMode)))
            gl::Uniform1i(u.palette, paletteMode);
        const uint32_t maskBitsUsed = maskPartial ? ds.fbmsk : 0u;
        if (!uSame(kUFbMask, f(maskPartial ? 1 : 0), static_cast<float>(maskBitsUsed & 0xFFFFu), static_cast<float>(maskBitsUsed >> 16)))
        {
            gl::Uniform1i(u.fbMask, maskPartial ? 1 : 0);
            gl::Uniform4i(u.maskBits, static_cast<gl::GLint>(maskBitsUsed & 0xFFu), static_cast<gl::GLint>((maskBitsUsed >> 8) & 0xFFu),
                          static_cast<gl::GLint>((maskBitsUsed >> 16) & 0xFFu), static_cast<gl::GLint>(maskBitsUsed >> 24));
        }

        const bool maskR = (ds.fbmsk & 0x000000FFu) == 0x000000FFu, maskG = (ds.fbmsk & 0x0000FF00u) == 0x0000FF00u,
                   maskB = (ds.fbmsk & 0x00FF0000u) == 0x00FF0000u, maskA = (ds.fbmsk & 0xFF000000u) == 0xFF000000u;
        const gl::GLenum mode = ds.primClass == kPrimLines ? gl::LINES : ds.primClass == kPrimPoints ? gl::POINTS : gl::TRIANGLES;

        auto drawPass = [&](int alphaPass, bool colorRgb, bool colorA, bool depthWrite)
        {
            if (!uSame(kUAlphaPass, f(alphaPass)))
                gl::Uniform1i(u.alphaPass, alphaPass);
            const bool r = colorRgb && !maskR, g = colorRgb && !maskG, b = colorRgb && !maskB, a = colorA && !maskA;
            const int masks = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0) | (depthWrite ? 16 : 0);
            if (dc.masks != masks)
            {
                gl::ColorMask(r, g, b, a);
                gl::DepthMask(depthWrite ? 1 : 0);
                dc.masks = masks;
            }
            gl::DrawArrays(mode, static_cast<gl::GLint>(cmd.first), static_cast<gl::GLsizei>(cmd.count));
        };
        if (date)
        {
            // Only pixels that passed DATE when the draw started. If this draw's own alpha write
            // flips a pixel's MSB away from DATM (e.g. a shadow pass writing alpha 0x40 over
            // FBA=1 ground with DATM=1), later primitives of the same draw fail DATE there on the GS:
            // clear the mark as each pixel is drawn so overlapping primitives don't draw it again.
            const bool alphaWritten = (ds.fbmsk & 0xFF000000u) != 0xFF000000u;
            const uint8_t writtenMsb = ds.fba ? 1u : 0u;
            const bool consumes = alphaWritten && writtenMsb != ds.datm;
            gl::Enable(gl::STENCIL_TEST);
            gl::StencilMask(0xFF);
            gl::StencilFunc(gl::EQUAL, 1, 0xFF);
            gl::StencilOp(gl::KEEP, gl::KEEP, consumes ? gl::ZERO : gl::KEEP);
        }
        const bool alphaTested = ds.ate && ds.atst != 1;
        drawPass(0, true, true, writesDepth);
        if (alphaTested && ds.afail != 0)
        {
            // AFAIL: pixels failing the alpha test still update part of the framebuffer.
            if (ds.afail == 1)
                drawPass(1, true, true, false);
            else if (ds.afail == 2)
                drawPass(1, false, false, writesDepth);
            else
                drawPass(1, true, false, false);
        }
        if (date)
            gl::Disable(gl::STENCIL_TEST);
        if (profStart != 0)
        {
            const gl::GLuint profEnd = profQuery();
            gl::QueryCounter(profEnd, gl::TIMESTAMP);
            profDraws.push_back({profStart, profEnd, cmd.state, cmd.count, drawArea(ds, cmd)});
        }
    }

    // Partial frame masks. FBMSK is a per-BIT write mask (1 = keep the destination bit); glColorMask can only drop a
    // whole channel, so a channel whose mask byte is neither 0x00 nor 0xFF used to be written in full. ZOE's virus
    // effect (Raptor's attack) draws black full-screen sprites with FBMSK 0x..fefefe, ..fcfcfc .. ..e0e0e0 (no
    // blending) to clear the low 1-5 bits of every channel: on the GS a posterized, fizzling picture; here the whole
    // 3D picture went black under the HUD (user report 13 Oct, save virus.p2s, black in 4:3 and 16:9, x1 and x3).
    // Such a draw now reads a copy of its area of the target and the shader stores (dst & mask) | (src & ~mask) per
    // channel. Only 32/24-bit targets and draws without blending (the GS masks the blended result; with GL
    // blending after the shader that order cannot be kept): others keep the old whole-channel rule.
    // PS2X_GPU_FBMASK=0: old behaviour everywhere.
    static bool partialMaskOn()
    {
        static const bool on = []
        {
            const char *v = std::getenv("PS2X_GPU_FBMASK");
            return !(v != nullptr && v[0] == '0');
        }();
        return on;
    }

    bool partialFrameMask(const GpuDrawState &ds)
    {
        if (!partialMaskOn())
            return false;
        const bool is32 = ds.fpsm == GS_PSM_CT32 || ds.fpsm == GS_PSM_CT24;
        const unsigned channels = ds.fpsm == GS_PSM_CT32 ? 4u : 3u;
        bool partial = false;
        for (unsigned ch = 0; ch < channels && !partial; ++ch)
        {
            const uint32_t b = (ds.fbmsk >> (8u * ch)) & 0xFFu;
            partial = b != 0u && b != 0xFFu;
        }
        if (!partial && !is32)
        {
            // 16-bit targets: report a partial 5/5/5/1 mask once (not merged)
            const uint32_t m = ds.fbmsk;
            partial = (m & 0x00F8F8F8u) != 0u && (m & 0x00F8F8F8u) != 0x00F8F8F8u;
        }
        if (!partial)
            return false;
        if (ds.abe || !is32)
        {
            // Left on the whole-channel rule: GL blending runs after the shader, so the mask cannot be applied to the
            // blended result; 16-bit targets are not handled. Said once per value, so a missing effect can be traced.
            if (maskLogged.insert((1ull << 40) | (static_cast<uint64_t>(ds.fpsm) << 32) | ds.fbmsk).second)
                std::cout << "[gpu] partial frame mask 0x" << std::hex << ds.fbmsk << std::dec << " NOT merged ("
                          << (ds.abe ? "blending" : "16-bit target") << "): whole-channel rule" << std::endl;
            return false;
        }
        return true;
    }

    // Copies the draw's bounding box (clipped to the scissor) of the target into a scratch texture bound to unit 3.
    void prepareMaskCopy(const GpuDrawState &ds, const Command &cmd, RenderTarget &rt)
    {
        ++glEpoch;
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (uint32_t i = 0; i < cmd.count; ++i)
        {
            const GpuVertex &v = replay.vertices[cmd.first + i];
            minX = std::min(minX, v.x);
            maxX = std::max(maxX, v.x);
            minY = std::min(minY, v.y);
            maxY = std::max(maxY, v.y);
        }
        // One GS pixel of margin: at internal resolution > 1, points and lines (sized in GS pixels) and the half-pixel
        // shift of the vertex shader can reach the neighbouring GS pixel; every covered pixel must be in the copy.
        const int x0 = std::max<int>({ds.sc[0], 0, static_cast<int>(std::floor(minX)) - 1});
        const int y0 = std::max<int>({ds.sc[2], 0, static_cast<int>(std::floor(minY)) - 1});
        const int x1 = std::min<int>({ds.sc[1], static_cast<int>(std::ceil(maxX)) + 1, static_cast<int>(rt.width) - 1});
        const int y1 = std::min<int>({ds.sc[3], static_cast<int>(std::ceil(maxY)) + 1, static_cast<int>(rt.height) - 1});
        auto &copy = maskScratchTextures[{rt.width * sx, rt.height * sy}];
        if (copy == 0)
            copy = newTexture(gl::RGBA16F, gl::RGBA, gl::FLOAT, rt.width * sx, rt.height * sy);
        if (x1 >= x0 && y1 >= y0)
            blit(rt.color, x0, y0, copy, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        ++statMaskDraws;
        if (maskLogged.insert(ds.fbmsk).second)
            std::cout << "[gpu] partial frame mask 0x" << std::hex << ds.fbmsk << std::dec << " drawn bit by bit (fbp 0x" << std::hex
                      << ds.fbp << std::dec << ")" << std::endl;
        gl::ActiveTexture(static_cast<gl::GLenum>(gl::TEXTURE0 + 3));
        gl::BindTexture(gl::TEXTURE_2D, copy);
        gl::ActiveTexture(gl::TEXTURE0);
    }

    // DATE pre-pass: stencil = 1 wherever the destination alpha MSB equals DATM, inside the draw's
    // bounding box (clipped to the scissor), read from a copy of the render target.
    void prepareDateStencil(const GpuDrawState &ds, const Command &cmd, RenderTarget &rt, gl::GLuint depth)
    {
        ++glEpoch;
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (uint32_t i = 0; i < cmd.count; ++i)
        {
            const GpuVertex &v = replay.vertices[cmd.first + i];
            minX = std::min(minX, v.x);
            maxX = std::max(maxX, v.x);
            minY = std::min(minY, v.y);
            maxY = std::max(maxY, v.y);
        }
        const int x0 = std::max<int>(ds.sc[0], static_cast<int>(std::floor(minX)));
        const int y0 = std::max<int>(ds.sc[2], static_cast<int>(std::floor(minY)));
        const int x1 = std::min<int>({ds.sc[1], static_cast<int>(std::ceil(maxX)), static_cast<int>(rt.width) - 1});
        const int y1 = std::min<int>({ds.sc[3], static_cast<int>(std::ceil(maxY)), static_cast<int>(rt.height) - 1});
        if (x1 < x0 || y1 < y0)
            return;
        const int w = x1 - x0 + 1, h = y1 - y0 + 1;

        auto &copy = dateScratchTextures[{rt.width * sx, rt.height * sy}];
        if (copy == 0)
            copy = newTexture(gl::RGBA16F, gl::RGBA, gl::FLOAT, rt.width * sx, rt.height * sy);
        blit(rt.color, x0, y0, copy, x0, y0, w, h);
        ++statDateCopies;
        statDatePx += static_cast<uint64_t>(w) * h;

        gl::BindFramebuffer(gl::FRAMEBUFFER, framebufferFor(rt.color, depth));
        gl::Viewport(0, 0, static_cast<gl::GLsizei>(rt.width * sx), static_cast<gl::GLsizei>(rt.height * sy));
        gl::Enable(gl::SCISSOR_TEST);
        scissor(x0, y0, w, h);
        gl::StencilMask(0xFF);
        gl::ClearStencil(0);
        gl::Clear(gl::STENCIL_BUFFER_BIT);

        gl::Disable(gl::DEPTH_TEST);
        gl::Disable(gl::BLEND);
        gl::ColorMask(0, 0, 0, 0);
        gl::DepthMask(0);
        gl::Enable(gl::STENCIL_TEST);
        gl::StencilFunc(gl::ALWAYS, 1, 0xFF);
        gl::StencilOp(gl::KEEP, gl::KEEP, gl::REPLACE);

        gl::UseProgram(dateProgram);
        gl::Uniform4f(dateRect, static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(w), static_cast<float>(h));
        gl::Uniform2f(dateRtSize, static_cast<float>(rt.width), static_cast<float>(rt.height));
        gl::Uniform1i(dateTex, 0);
        gl::Uniform1i(dateDatm, ds.datm);
        gl::ActiveTexture(gl::TEXTURE0);
        gl::BindTexture(gl::TEXTURE_2D, copy);
        gl::BindVertexArray(presentVao);
        gl::DrawArrays(gl::TRIANGLE_STRIP, 0, 4);

        // Back to the main program / vertex stream for the draw itself.
        gl::Disable(gl::STENCIL_TEST);
        gl::UseProgram(program);
        gl::BindVertexArray(vao);
        gl::BindBuffer(gl::ARRAY_BUFFER, vbo);
    }

    void replayCommands()
    {
        if (replay.commands.empty())
            return;
        gl::BindVertexArray(vao);
        gl::BindBuffer(gl::ARRAY_BUFFER, vbo);
        gl::BufferData(gl::ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(replay.vertices.size() * sizeof(GpuVertex)),
                       replay.vertices.empty() ? nullptr : replay.vertices.data(), gl::STREAM_DRAW);
        gl::UseProgram(program);
        gl::ActiveTexture(gl::TEXTURE0);
        gl::Uniform1i(u.tex, 0);
        gl::Uniform1i(u.pal, 1);
        gl::Uniform1i(u.dst, 3); // partial frame mask: copy of the target (texture unit 3)
        gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
        gl::Disable(gl::CULL_FACE);
        gl::Uniform2f(u.scale, static_cast<float>(sx), static_cast<float>(sy));
        gl::Uniform2i(u.scaleI, static_cast<gl::GLint>(sx), static_cast<gl::GLint>(sy));
        gl::Uniform1i(u.spriteClamp, spriteTightClampOn() ? 1 : 0); // once per present
        // At a higher internal resolution, points and lines keep their size in GS pixels (gl_PointSize in the
        // vertex shader; wide lines where the driver supports them).
        const bool scaled = sx != 1u || sy != 1u;
        if (scaled)
        {
            gl::Enable(gl::PROGRAM_POINT_SIZE);
            gl::LineWidth(static_cast<float>(std::max(sx, sy)));
        }
        int programState = 0;
        ++glEpoch; // state left by raylib and the previous present
        uValid.fill(false);
        for (size_t ci = 0; ci < replay.commands.size(); ++ci)
        {
            const Command &cmd = replay.commands[ci];
            if (cmd.type != CmdType::Draw)
                ++glEpoch; // uploads, copies and clears change bindings
            switch (cmd.type)
            {
            case CmdType::TextureUpload:
            {
                gl::GLuint &t = glTextures[cmd.id];
                GlTextureShape &shape = glTextureShapes[cmd.id];
                const bool indices = cmd.texFormat == 1;
                const gl::GLenum format = indices ? gl::RED : gl::RGBA;
                if (t == 0 || shape.w != cmd.w || shape.h != cmd.h || shape.format != cmd.texFormat)
                {
                    if (t == 0)
                        gl::GenTextures(1, &t);
                    gl::BindTexture(gl::TEXTURE_2D, t);
                    gl::TexImage2D(gl::TEXTURE_2D, 0, static_cast<gl::GLint>(indices ? gl::R8 : gl::RGBA8), cmd.w, cmd.h, 0,
                                   format, gl::UNSIGNED_BYTE, replay.pixels.data() + cmd.first);
                    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
                    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
                    shape = {cmd.w, cmd.h, cmd.texFormat};
                }
                else
                {
                    gl::BindTexture(gl::TEXTURE_2D, t);
                    gl::TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, cmd.w, cmd.h, format, gl::UNSIGNED_BYTE,
                                      replay.pixels.data() + cmd.first);
                }
                break;
            }
            case CmdType::Draw:
                executeDraw(cmd, programState);
                break;
            case CmdType::CopyRect:
                executeCopy(cmd);
                break;
            case CmdType::Clear:
            {
                RenderTarget &rt = renderTarget(cmd.id, cmd.fbw, std::min(kRtHeight, cmd.y + cmd.h));
                gl::BindFramebuffer(gl::FRAMEBUFFER, framebufferFor(rt.color, 0));
                gl::Enable(gl::SCISSOR_TEST);
                scissor(static_cast<int>(cmd.x), static_cast<int>(cmd.y), static_cast<int>(cmd.w), static_cast<int>(cmd.h));
                gl::ColorMask(1, 1, 1, 1);
                gl::ClearColor((cmd.rgba & 0xFF) / 255.0f, ((cmd.rgba >> 8) & 0xFF) / 255.0f,
                               ((cmd.rgba >> 16) & 0xFF) / 255.0f, ((cmd.rgba >> 24) & 0xFF) / 128.0f);
                gl::Clear(gl::COLOR_BUFFER_BIT);
                break;
            }
            case CmdType::FramebufferWrite:
            {
                // CPU upload into a GPU-rendered framebuffer: RGBA8 rect -> RGBA16F target
                // (alpha rescaled to byte/128 in the shader-independent upload path below).
                if (scaled)
                {
                    ci = executeWriteRun(ci) - 1u;
                    break;
                }
                RenderTarget &rt = renderTarget(cmd.id, cmd.fbw, std::min(kRtHeight, cmd.y + cmd.h));
                std::vector<float> rgba(static_cast<size_t>(cmd.w) * cmd.h * 4u);
                const uint32_t *src = replay.pixels.data() + cmd.first;
                for (size_t i = 0; i < static_cast<size_t>(cmd.w) * cmd.h; ++i)
                {
                    rgba[i * 4 + 0] = (src[i] & 0xFF) / 255.0f;
                    rgba[i * 4 + 1] = ((src[i] >> 8) & 0xFF) / 255.0f;
                    rgba[i * 4 + 2] = ((src[i] >> 16) & 0xFF) / 255.0f;
                    rgba[i * 4 + 3] = ((src[i] >> 24) & 0xFF) / 128.0f;
                }
                gl::BindTexture(gl::TEXTURE_2D, rt.color);
                gl::TexSubImage2D(gl::TEXTURE_2D, 0, cmd.x, cmd.y, cmd.w, cmd.h, gl::RGBA, gl::FLOAT, rgba.data());
                break;
            }
            }
        }
        if (scaled)
        {
            gl::Disable(gl::PROGRAM_POINT_SIZE);
            gl::LineWidth(1.0f);
        }
        if (drawProf())
            profFinish();
        gl::UseProgram(0);
        gl::BindVertexArray(0);
    }
};

namespace
{
    std::atomic<uint32_t> g_presentTexture{0};
    std::atomic<uint32_t> g_presentTargetW{0}, g_presentTargetH{0};
    std::atomic<uint32_t> g_presentScale{1}; // internal resolution the present target was sized for
    std::atomic<uint32_t> g_presentTargetGen{0}; // bumped by every SetPresentTarget
    std::atomic<uint32_t> g_presentedW{640}, g_presentedH{448};
    std::atomic<bool> g_active{false};
}

void GSGpuBackend::SetPresentTarget(uint32_t glTexture, uint32_t width, uint32_t height, uint32_t scale)
{
    g_presentTexture = glTexture;
    g_presentTargetW = width;
    g_presentTargetH = height;
    g_presentScale = std::max<uint32_t>(1u, scale);
    ++g_presentTargetGen;
}

uint32_t GSGpuBackend::DesiredScale()
{
    // PS2X_GPU_SCALE=<n> (test runs) overrides the menu setting (Graphics > Internal resolution).
    static const int s_env = []
    {
        const char *v = std::getenv("PS2X_GPU_SCALE");
        return (v != nullptr && *v != 0) ? std::atoi(v) : 0;
    }();
    const int want = s_env > 0 ? s_env : ps2Settings().upscale;
    // The choices offered: 1 (native), 2, 3, 4; anything else rounds down to one of them. Above x4 ZOE's heaviest
    // scenes need more GPU time than a frame has (RTX 5060 Ti, Tempest: x6 6-17 game frames/s) and recorded frames
    // pile up faster than they are replayed.
    static constexpr int kChoices[] = {4, 3, 2};
    for (int c : kChoices)
        if (want >= c)
            return static_cast<uint32_t>(c);
    return 1u;
}

bool GSGpuBackend::Active() { return g_active.load(); }

void GSGpuBackend::GetPresentedSize(uint32_t &width, uint32_t &height)
{
    width = g_presentedW.load();
    height = g_presentedH.load();
}

GSGpuBackend::GSGpuBackend() : m(std::make_unique<Impl>())
{
    g_active = true;
    g_scaleX = 1u; // a new backend starts at the PS2's resolution (its first present sets the scale)
    g_scaleY = 1u;
}
GSGpuBackend::~GSGpuBackend() { g_active = false; }

void GSGpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    m->cpu.Initialize(vram, vramSize);
    m->vram = vram;
}

void GSGpuBackend::Reset()
{
    m->cpu.Reset();
    std::lock_guard<std::mutex> lock(m->recordMutex);
    m->recording.clear();
    m->textures.clear();
    m->contentCache.clear();
    m->freeTextureIds.clear();
    m->lastKey = TextureKey{~0ull, 0};
    m->targets.clear();
    m->pageOwner.fill(0u);
}

void GSGpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    const GSDrawState &st = batch.state;
    const GSContext &c = st.context;
    const GSPrimType type = st.prim.type;

    GpuDrawState ds;
    std::memset(&ds, 0, sizeof(ds));
    ds.fbp = c.frame.fbp;
    ds.fbw = c.frame.fbw;
    ds.fpsm = c.frame.psm;
    ds.fbmsk = c.frame.fbmsk;
    ds.zbp = c.zbuf.zbp;
    ds.zpsm = c.zbuf.psm;
    ds.zmsk = c.zbuf.zmask ? 1 : 0;
    const uint64_t test = c.test;
    ds.ate = test & 1u;
    ds.atst = (test >> 1) & 7u;
    ds.aref = (test >> 4) & 0xFFu;
    ds.afail = (test >> 12) & 3u;
    ds.date = (test >> 14) & 1u;
    ds.datm = (test >> 15) & 1u;
    ds.zte = (test >> 16) & 1u;
    ds.ztst = (test >> 17) & 3u;
    // A draw with a depth-format frame buffer (PSMZ*) writes depth values: the GS stores the pixel colour in that
    // memory and later depth tests read it (PCSX2 converts RGBA8 -> 24-bit depth the same way). Depth lives in a GL
    // depth buffer here, so such draws go to it: the vertex colour becomes the depth value, colour writes are masked.
    // ZOE resets the map panel's depth this way (three flat Z24 sprites at the depth buffer's address) before drawing
    // the 3D map into its own target; drawn as colour, the panel kept the UI's maximum depth and the whole map failed
    // its depth test. Only the frame format decides (FRAME == ZBUF alone is a common "no depth" setup).
    // Untextured draws only; textured ones keep the old path and say so once.
    const bool zFrame = (c.frame.psm & 0x30u) == 0x30u; // PSMZ32 0x30, Z24 0x31, Z16 0x32, Z16S 0x3A
    const bool depthTarget = zFrame && !st.prim.tme;
    if (zFrame && st.prim.tme && m->warnOnce((5ull << 40) | c.frame.fbp))
        std::cerr << "[gpu] textured draw into depth buffer memory fbp=0x" << std::hex << c.frame.fbp << " fpsm=0x"
                  << static_cast<int>(c.frame.psm) << std::dec << " is drawn as colour (unsupported)" << std::endl;
    uint32_t depthBits = 0xFFFFFFFFu;
    if (depthTarget)
    {
        ds.zpsm = c.frame.psm;
        depthBits = (ds.zpsm == GS_PSM_Z24) ? 0x00FFFFFFu : (ds.zpsm == GS_PSM_Z16 || ds.zpsm == GS_PSM_Z16S) ? 0xFFFFu : 0xFFFFFFFFu;
        if ((c.frame.fbmsk & depthBits) == depthBits)
            return; // every depth bit masked: nothing is written
        ds.zbp = c.frame.fbp;
        ds.ztst = 1u; // ALWAYS
        ds.zmsk = 0;
        ds.fbmsk = 0xFFFFFFFFu; // no colour writes
        ds.ate = 0;
        ds.date = 0;
    }
    ds.sc[0] = c.scissor.x0;
    ds.sc[1] = c.scissor.x1;
    ds.sc[2] = c.scissor.y0;
    ds.sc[3] = c.scissor.y1;
    ds.abe = st.prim.abe ? 1 : 0;
    ds.blendA = c.alpha & 3u;
    ds.blendB = (c.alpha >> 2) & 3u;
    ds.blendC = (c.alpha >> 4) & 3u;
    ds.blendD = (c.alpha >> 6) & 3u;
    ds.fix = (c.alpha >> 32) & 0xFFu;
    ds.fba = c.fba & 1u;
    ds.colclamp = st.colclamp & 1u;
    ds.fge = st.prim.fge ? 1 : 0;
    ds.fogR = st.fogR;
    ds.fogG = st.fogG;
    ds.fogB = st.fogB;
    ds.primClass = (type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP) ? kPrimLines
                   : (type == GS_PRIM_POINT)                            ? kPrimPoints
                                                                        : kPrimTriangles;

    std::lock_guard<std::mutex> lock(m->recordMutex);
    if (st.prim.tme)
    {
        ds.tme = 1;
        ds.tfx = c.tex0.tfx;
        ds.tcc = c.tex0.tcc;
        ds.fst = st.prim.fst ? 1 : 0;
        ds.linear = st.linearFilter ? 1 : 0;
        ds.tw = st.textureWidth;
        ds.th = st.textureHeight;
        ds.wms = c.clamp & 3u;
        ds.wmt = (c.clamp >> 2) & 3u;
        ds.minu = (c.clamp >> 4) & 0x3FFu;
        ds.maxu = (c.clamp >> 14) & 0x3FFu;
        ds.minv = (c.clamp >> 24) & 0x3FFu;
        ds.maxv = (c.clamp >> 34) & 0x3FFu;
        m->bindTexture(st, ds);
    }
    if (!depthTarget)
        m->targets[{ds.fbp, ds.fbw}] = ds.fpsm;

    uint32_t ofyFixed = static_cast<uint32_t>(c.xyoffset.ofy);
    const bool progressiveFields = g_halfHeightFields.load(std::memory_order_relaxed) && ps2ProgressiveDisplayActive();
    if ((ofyFixed & 0xFu) == 8u && progressiveFields)
        ofyFixed &= ~0xFu; // progressive: the field's half-line offset (sceGsSetHalfOffset) is dropped
    const float ofx = c.xyoffset.ofx / 16.0f, ofy = ofyFixed / 16.0f;
    g_fbHalfLineOffset[ds.fbp & 511u].store((ofyFixed & 0xFu) != 0u ? 1u : 0u, std::memory_order_relaxed);
    const double zScale = (ds.zpsm == GS_PSM_Z16 || ds.zpsm == GS_PSM_Z16S) ? 65536.0
                          : (ds.zpsm == GS_PSM_Z24)                         ? 16777216.0
                                                                            : 4294967296.0;
    const bool sprite = type == GS_PRIM_SPRITE;
    auto convert = [&](const GSVertex &v, const GSVertex &colorFrom) -> GpuVertex
    {
        GpuVertex o{};
        o.x = v.x - ofx;
        o.y = v.y - ofy;
        o.depth = static_cast<float>(std::min(v.z, zScale - 1.0) / zScale);
        if (depthTarget)
        {
            // The stored "colour" is the depth: R | G<<8 | B<<16 | A<<24 (16-bit formats: RGB5A1), cut to the format.
            uint32_t value;
            if (ds.zpsm == GS_PSM_Z16 || ds.zpsm == GS_PSM_Z16S)
                value = (colorFrom.r >> 3) | ((colorFrom.g >> 3) << 5) | ((colorFrom.b >> 3) << 10) | ((colorFrom.a >> 7) << 15);
            else
                value = colorFrom.r | (colorFrom.g << 8) | (colorFrom.b << 16) | (static_cast<uint32_t>(colorFrom.a) << 24);
            value &= depthBits;
            o.depth = static_cast<float>(std::min(static_cast<double>(value), zScale - 1.0) / zScale);
        }
        if (ds.fst)
        {
            o.s = v.u / 16.0f;
            o.t = v.v / 16.0f;
            o.q = 1.0f;
        }
        else if (sprite)
        {
            // Sprites are screen-aligned: divide once and interpolate affinely.
            const float q = (v.q != 0.0f) ? v.q : 1.0f;
            o.s = v.s / q;
            o.t = v.t / q;
            o.q = 1.0f;
        }
        else
        {
            o.s = v.s;
            o.t = v.t;
            o.q = v.q;
        }
        o.r = colorFrom.r;
        o.g = colorFrom.g;
        o.b = colorFrom.b;
        o.a = colorFrom.a;
        o.fog = v.fog;
        o.cu0 = o.cv0 = -1e30f;
        o.cu1 = o.cv1 = 1e30f;
        return o;
    };

    const auto &vx = batch.vertices;
    {
        // Claim the pages this primitive can touch (bounding box clipped to the scissor).
        const uint32_t n = type == GS_PRIM_SPRITE || ds.primClass == kPrimLines ? 2u : ds.primClass == kPrimPoints ? 1u : 3u;
        float minX = vx[0].x, maxX = vx[0].x, minY = vx[0].y, maxY = vx[0].y;
        for (uint32_t i = 1; i < n; ++i)
        {
            minX = std::min(minX, vx[i].x);
            maxX = std::max(maxX, vx[i].x);
            minY = std::min(minY, vx[i].y);
            maxY = std::max(maxY, vx[i].y);
        }
        // Sprites and triangles cover pixels p with min <= p < max, so their last pixel is ceil(max) - 1. ceil(max)
        // claimed one row/column too many: ZOE's Nebula water sprites end at y 127.25 in a 128-wide target and took
        // the page holding the waterfall texture next to it (fbp 0x1c0 vs 0x1c9), so the waterfalls read the empty
        // part of the water target. Lines and points keep ceil(max): GL rounds them to the nearest pixel (a point at
        // 63.7 lands on 64). PS2X_GPU_CLAIM_EXACT=0: old rule for every primitive.
        static const bool s_exact = []
        {
            const char *v = std::getenv("PS2X_GPU_CLAIM_EXACT");
            return !(v != nullptr && v[0] == '0');
        }();
        const int endTrim = (s_exact && ds.primClass != kPrimLines && ds.primClass != kPrimPoints) ? 1 : 0;
        const int firstX = static_cast<int>(std::floor(minX - ofx)), firstY = static_cast<int>(std::floor(minY - ofy));
        const int lastX = std::max(firstX, static_cast<int>(std::ceil(maxX - ofx)) - endTrim);
        const int lastY = std::max(firstY, static_cast<int>(std::ceil(maxY - ofy)) - endTrim);
        const int x0 = std::max<int>(ds.sc[0], firstX);
        const int x1 = std::min<int>(ds.sc[1], lastX);
        const int y0 = std::max<int>(ds.sc[2], firstY);
        const int y1 = std::min<int>(ds.sc[3], lastY);
        if ((ds.fbmsk & 0xFFFFFFFFu) != 0xFFFFFFFFu && !depthTarget)
            m->claimPages(ds.fbp, ds.fbw, ds.fpsm, x0, y0, x1, y1);
    }
    // F9 burst: log every state change. PS2X_GPU_TRACE: log each distinct state once for the whole run.
    if (ps2_debug::traceBurst())
    {
        static GpuDrawState s_lastLogged{};
        if (std::memcmp(&s_lastLogged, &ds, sizeof(ds)) != 0)
        {
            s_lastLogged = ds;
            m->logDraw(ds, st, vx[0]);
        }
    }
    else if (gpuTraceActive())
    {
        uint64_t h = 1469598103934665603ull;
        const auto *bytes = reinterpret_cast<const uint8_t *>(&ds);
        for (size_t i = 0; i < sizeof(ds); ++i)
            h = (h ^ bytes[i]) * 1099511628211ull;
        h ^= st.pabe ? 0x5555u : 0u;
        if (m->warnOnce(h | (1ull << 63)))
            m->logDraw(ds, st, vx[0]);
    }
    if (type == GS_PRIM_SPRITE)
    {
        const GSVertex &a = vx[0], &b = vx[1];
        GpuVertex p0 = convert(a, b), p1 = convert(b, b);
        // Match the GS (and GSCpuBackend::DrawSprite): the edges cover pixels from ceil(x0) to
        // ceil(x1), and with FST the texture coordinates are whole texels at those edges, sampled
        // at each pixel's centre. Unsnapped sub-pixel edges/UVs picked the neighbouring texel for
        // some sprites, misaligning layered text (e.g. a highlighted menu item drawn in two layers).
        {
            const float ax = std::ceil(p0.x), bx = std::ceil(p1.x);
            float ay = std::ceil(p0.y), by = std::ceil(p1.y);
            if (ay == by && p1.y > p0.y && progressiveFields)
            {
                // Progressive output puts every frame on one set of lines; a sprite under a line tall that covered a row
                // only in the half-line-offset field (ZOE's experience marker, 210.75-211.25) would vanish. Keep the
                // row it gets in that field.
                const float fy0 = std::ceil(p0.y - 0.5f), fy1 = std::ceil(p1.y - 0.5f);
                if (fy1 > fy0)
                {
                    ay = fy0;
                    by = fy1;
                }
            }
            // The sprite's own horizontal texture range from the unsnapped vertices (coordinates at the two edges, and
            // texels per coordinate unit: 1 for FST, the texture size for STQ's s/q), for the tight range at the end.
            float rectU0 = 0.0f, rectU1 = 0.0f, unitsU = 1.0f;
            if (ds.fst)
            {
                const float au = static_cast<float>(a.u >> 4), bu = static_cast<float>(b.u >> 4);
                const float av = static_cast<float>(a.v >> 4), bv = static_cast<float>(b.v >> 4);
                rectU0 = au;
                rectU1 = bu;
                // Signed half-pixel step from each edge towards the other, so pixel i samples
                // u(a) + (i - x(a) + 0.5) * du, like the CPU rasteriser. At internal resolution s each GS pixel
                // is s GL pixels sampled from its left edge, so the step is half of one of those: every GL pixel
                // then samples its own centre (a 1:1 sprite stays on one texel per GS pixel instead of reading
                // the next texel in the right half of each pixel).
                const float spanX = bx - ax, spanY = by - ay;
                const float halfX = 0.5f / static_cast<float>(g_scaleX.load(std::memory_order_relaxed));
                const float halfY = 0.5f / static_cast<float>(g_scaleY.load(std::memory_order_relaxed));
                const float stepU = spanX != 0.0f ? halfX * (bu - au) / spanX : 0.0f;
                const float stepV = spanY != 0.0f ? halfY * (bv - av) / spanY : 0.0f;
                p0.s = au + stepU;
                p1.s = bu + stepU;
                p0.t = av + stepV;
                p1.t = bv + stepV;
                // Native samples: u(a) + 0.5 du for the first pixel to u(b) - 0.5 du for the last.
                if (spanX != 0.0f)
                {
                    const float first = au + 0.5f * (bu - au) / spanX, last = bu - 0.5f * (bu - au) / spanX;
                    p0.cu0 = std::min(first, last);
                    p0.cu1 = std::max(first, last);
                }
                if (spanY != 0.0f)
                {
                    const float first = av + 0.5f * (bv - av) / spanY, last = bv - 0.5f * (bv - av) / spanY;
                    p0.cv0 = std::min(first, last);
                    p0.cv1 = std::max(first, last);
                }
            }
            else
            {
                // STQ: the coordinates belong to the unsnapped edges, so re-evaluate them at the snapped
                // ones. Otherwise the first row/column samples up to a texel before the image; with
                // bilinear + REPEAT that blends in the opposite edge (a dashed line over ZOE's text).
                rectU0 = p0.s;
                rectU1 = p1.s;
                unitsU = static_cast<float>(ds.tw);
                const float spanX = p1.x - p0.x, spanY = p1.y - p0.y;
                bool refitV = false;
                float refitLo = 0.0f, refitHi = 0.0f;
                if (spanX != 0.0f)
                {
                    const float s0 = p0.s, dsdx = (p1.s - p0.s) / spanX;
                    p0.s = s0 + (ax - p0.x) * dsdx;
                    p1.s = s0 + (bx - p0.x) * dsdx;
                }
                if (spanY != 0.0f)
                {
                    const float t0 = p0.t, t1 = p1.t, dtdy = (t1 - t0) / spanY;
                    // Vertically minified (more than one texel per GS row) at internal resolution > 1: keep the sprite's own
                    // t range over the snapped rows (as the FST branch does) and clamp to it (see spriteRefitOn).
                    refitV = by > ay && g_scaleY.load(std::memory_order_relaxed) > 1u && spriteRefitOn() &&
                             std::fabs(t1 - t0) * static_cast<float>(ds.th) / spanY > 1.001f;
                    if (refitV)
                    {
                        const float texel = 1.0f / static_cast<float>(ds.th);
                        const float inset = ds.linear ? 0.5f * texel : 0.001f * texel; // bilinear: no blend with the row outside
                        refitLo = std::min(t0, t1) + (ds.linear ? inset : 0.0f);
                        refitHi = std::max(t0, t1) - inset;
                    }
                    else
                    {
                        p0.t = t0 + (ay - p0.y) * dtdy;
                        p1.t = t0 + (by - p0.y) * dtdy;
                    }
                }
                // Native samples: at each pixel's own coordinate, from the first edge to one pixel before the last.
                if (bx > ax)
                {
                    const float last = p1.s - (p1.s - p0.s) / (bx - ax);
                    p0.cu0 = std::min(p0.s, last);
                    p0.cu1 = std::max(p0.s, last);
                }
                if (refitV)
                {
                    p0.cv0 = refitLo;
                    p0.cv1 = std::max(refitLo, refitHi);
                }
                else if (by > ay)
                {
                    const float last = p1.t - (p1.t - p0.t) / (by - ay);
                    p0.cv0 = std::min(p0.t, last);
                    p0.cv1 = std::max(p0.t, last);
                }
            }
            // A horizontally minified sprite keeps its samples inside the texels its rectangle touches, so its first
            // pixel cannot land on the boundary and pull in the texel before it (see spriteTightClampOn). Horizontal
            // only: a widescreen squeeze never changes the vertical axis, and vertically minified sprites are drawn so
            // in 4:3 too (ZOE's dim strip right of the NONE bracket is such vertical bilinear spill, present in stock;
            // tightening v removed it, 11 Oct). p0.x/p1.x are still the unsnapped edges here.
            if (ds.tme && ax != bx && spriteTightClampOn())
                tightenSpriteRange(rectU0, rectU1, unitsU, std::fabs(p1.x - p0.x), p0.cu0, p0.cu1);
            p0.x = ax;
            p1.x = bx;
            p0.y = ay;
            p1.y = by;
        }
        p1.cu0 = p0.cu0; // every corner carries the sprite's sample range
        p1.cv0 = p0.cv0;
        p1.cu1 = p0.cu1;
        p1.cv1 = p0.cv1;
        GpuVertex c01 = p0, c10 = p0;
        c01.x = p1.x; c01.s = p1.s; // top-right
        c10.y = p1.y; c10.t = p1.t;               // bottom-left
        p0.depth = p1.depth;
        c01.depth = p1.depth;
        c10.depth = p1.depth;
        const GpuVertex quad[6] = {p0, c01, c10, c01, p1, c10};
        m->emitDraw(ds, quad, 6);
    }
    else if (ds.primClass == kPrimTriangles)
    {
        if (batch.vertexCount < 3)
            return;
        const bool gouraud = st.prim.iip;
        const GpuVertex tri[3] = {convert(vx[0], gouraud ? vx[0] : vx[2]), convert(vx[1], gouraud ? vx[1] : vx[2]),
                                  convert(vx[2], vx[2])};
        m->emitDraw(ds, tri, 3);
    }
    else if (ds.primClass == kPrimLines)
    {
        const bool gouraud = st.prim.iip;
        const GpuVertex line[2] = {convert(vx[0], gouraud ? vx[0] : vx[1]), convert(vx[1], vx[1])};
        m->emitDraw(ds, line, 2);
    }
    else
    {
        const GpuVertex point = convert(vx[0], vx[0]);
        m->emitDraw(ds, &point, 1);
    }
}

void GSGpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    // Only indexed formats with CLD 1..5 load anything (GSCpuBackend::LoadClut). ZOE sets CLD on almost every
    // TEX0 and mostly reloads the same palette: a load identical to the previous one (same source, destination
    // and format, source pages unchanged) leaves the CLUT buffer as it is, so skip it and keep clutVersion - every
    // bump made the next draw re-read 256 entries and re-hash the palette.
    const uint32_t psm = tex0.psm;
    const bool fourBit = psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    const bool indexed = fourBit || psm == GS_PSM_T8 || psm == GS_PSM_T8H;
    if (!indexed || tex0.cld == 0u || tex0.cld > 5u)
        return;
    Impl &s = *m;
    {
        std::lock_guard<std::mutex> lock(s.recordMutex);
        uint32_t pv = 0;
        if (tex0.csm == 0u)
            forEachRectPage(tex0.cbp, 1u, tex0.cpsm, 0u, 0u, 16u, 16u, [&](uint32_t page) { pv = std::max(pv, s.pageVersion[page]); });
        else
            forEachRectPage(tex0.cbp, std::max<uint32_t>(texclut.cbw, 1u), tex0.cpsm, static_cast<uint32_t>(texclut.cou) * 16u,
                            texclut.cov, fourBit ? 16u : 256u, 1u, [&](uint32_t page) { pv = std::max(pv, s.pageVersion[page]); });
        const uint64_t key = static_cast<uint64_t>(tex0.cbp & 0x3FFFu) | (static_cast<uint64_t>(tex0.cpsm & 0x3Fu) << 14) |
                             (static_cast<uint64_t>(tex0.csa & 0x1Fu) << 20) | (static_cast<uint64_t>(tex0.csm & 1u) << 25) |
                             (static_cast<uint64_t>(fourBit ? 1u : 0u) << 26) | (static_cast<uint64_t>(tex0.cld & 7u) << 27) |
                             (static_cast<uint64_t>(texclut.cbw) << 30) | (static_cast<uint64_t>(texclut.cou) << 38) |
                             (static_cast<uint64_t>(texclut.cov & 0x3FFu) << 46);
        if (s.lastClutLoadValid && s.lastClutLoadKey == key && s.lastClutLoadPageVersion == pv)
            return;
        s.lastClutLoadValid = true;
        s.lastClutLoadKey = key;
        s.lastClutLoadPageVersion = pv;
    }
    // A reload with the same entries leaves every texture's palette unchanged: keep clutVersion so resolveTexture does
    // not re-hash palettes (ZOE reloads identical palettes constantly).
    static std::array<uint16_t, 512> s_before;
    s_before = m->cpu.ClutBuffer();
    m->cpu.LoadClut(tex0, texclut);
    std::lock_guard<std::mutex> lock(m->recordMutex);
    if (m->cpu.ClutBuffer() != s_before)
        ++m->clutVersion;
}

void GSGpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    Impl &s = *m;
    const GSBitBltBuf &b = command.bitbltbuf;
    const GSTrxPos &pos = command.trxpos;
    const GSTrxReg &reg = command.trxreg;
    {
        std::lock_guard<std::mutex> lock(s.recordMutex);
        s.transfer = command;
        s.transferActive = command.direction == 0u;
        if (command.direction == 1u)
        {
            const Impl::RtRef src = s.locate(b.sbp, b.sbw, b.spsm);
            if (src.owned && s.warnOnce((2ull << 40) | b.sbp))
                std::cerr << "[gpu] local->host read of GPU target fbp=0x" << std::hex << src.fbp << " (sbp=0x" << b.sbp
                          << std::dec << ") returns stale local memory" << std::endl;
        }
        else if (command.direction == 2u)
        {
            const Impl::RtRef src = s.locate(b.sbp, b.sbw, b.spsm);
            if (src.ok && Impl::bppClass(b.dpsm) == Impl::bppClass(b.spsm))
            {
                Impl::RtRef dst = s.locate(b.dbp, b.dbw, b.dpsm);
                if (!dst.ok && (b.dbp % 32u) == 0u && b.dbw != 0u)
                {
                    // Copy into memory the GPU has not rendered: start a render target there.
                    dst = Impl::RtRef{};
                    dst.ok = true;
                    dst.fbp = b.dbp / 32u;
                    dst.fbw = b.dbw;
                    s.targets[{dst.fbp, dst.fbw}] = b.dpsm;
                }
                if (dst.ok)
                {
                    Command cmd{};
                    cmd.type = CmdType::CopyRect;
                    cmd.id = src.fbp;
                    cmd.fbw = src.fbw;
                    cmd.x = src.x + pos.ssax;
                    cmd.y = src.y + pos.ssay;
                    cmd.w = reg.rrw;
                    cmd.h = reg.rrh;
                    cmd.dstFbp = dst.fbp;
                    cmd.dstFbw = dst.fbw;
                    cmd.dx = dst.x + pos.dsax;
                    cmd.dy = dst.y + pos.dsay;
                    s.recording.commands.push_back(cmd);
                    auto it = s.targets.find({dst.fbp, dst.fbw});
                    s.claimPages(dst.fbp, dst.fbw, it != s.targets.end() ? it->second : b.dpsm, static_cast<int>(cmd.dx),
                                 static_cast<int>(cmd.dy), static_cast<int>(cmd.dx + reg.rrw) - 1,
                                 static_cast<int>(cmd.dy + reg.rrh) - 1);
                }
            }
            else if (src.owned && s.warnOnce((3ull << 40) | (static_cast<uint64_t>(b.sbp) << 8) | b.spsm))
            {
                std::cerr << "[gpu] local->local copy from GPU target fbp=0x" << std::hex << src.fbp << " (sbp=0x" << b.sbp
                          << " spsm=0x" << static_cast<int>(b.spsm) << " dbp=0x" << b.dbp << ")" << std::dec
                          << " in an unsupported layout" << std::endl;
            }
        }
    }
    s.cpu.BeginTransfer(command);
    if (command.direction == 2u)
    {
        std::lock_guard<std::mutex> lock(s.recordMutex);
        const Impl::RtRef src = s.locate(b.sbp, b.sbw, b.spsm);
        if (!src.ok)
            s.finishCpuWrite(b.dbp, b.dbw, b.dpsm, pos.dsax, pos.dsay, reg.rrw, reg.rrh);
    }
}

void GSGpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    const auto uploadStart = std::chrono::steady_clock::now();
    struct UploadTimer // per-frame log (PS2X_FRAME_LOG)
    {
        std::chrono::steady_clock::time_point t0;
        uint32_t bytes;
        ~UploadTimer()
        {
            auto &c = ps2_frame_log::counters();
            c.gsUploadNs.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                std::memory_order_relaxed);
            c.gsUploadBytes.fetch_add(bytes, std::memory_order_relaxed);
        }
    } uploadTimer{uploadStart, sizeBytes};
    m->cpu.UploadImage(data, sizeBytes);
    std::lock_guard<std::mutex> lock(m->recordMutex);
    if (!m->transferActive)
        return;
    const GSTransferSnapshot snap = m->cpu.GetTransferSnapshot();
    if (snap.totalPixels != 0 && snap.copiedPixels < snap.totalPixels)
        return;
    m->transferActive = false;
    const GSTransferCommand &t = m->transfer;
    m->finishCpuWrite(t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.bitbltbuf.dpsm, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw,
                      t.trxreg.rrh);
}

void GSGpuBackend::Flush() {}
void GSGpuBackend::TextureFlush() { m->cpu.TextureFlush(); }
void GSGpuBackend::Sync(GSSyncReason reason) { m->cpu.Sync(reason); }

PresentationFrame GSGpuBackend::Present(const GSPresentationRequest &request)
{
    Impl &s = *m;
    {
        std::lock_guard<std::mutex> lock(s.recordMutex);
        std::swap(s.replay, s.recording);
        s.recording.clear();
    }
    if (!s.initGl())
        return s.cpu.Present(request);

    // Save the GL state raylib relies on.
    gl::GLint prevFbo = 0, prevProgram = 0, prevVao = 0, prevBuffer = 0, prevTexture = 0, prevActive = 0, prevViewport[4] = {};
    gl::GLint prevUnpack = 4;
    gl::GetIntegerv(gl::FRAMEBUFFER_BINDING, &prevFbo);
    gl::GetIntegerv(gl::CURRENT_PROGRAM, &prevProgram);
    gl::GetIntegerv(gl::VERTEX_ARRAY_BINDING, &prevVao);
    gl::GetIntegerv(gl::ARRAY_BUFFER_BINDING, &prevBuffer);
    gl::GetIntegerv(gl::ACTIVE_TEXTURE, &prevActive);
    gl::ActiveTexture(gl::TEXTURE0);
    gl::GetIntegerv(gl::TEXTURE_BINDING_2D, &prevTexture);
    gl::GetIntegerv(gl::VIEWPORT, prevViewport);
    gl::GetIntegerv(gl::UNPACK_ALIGNMENT, &prevUnpack);
    const bool prevBlend = gl::IsEnabled(gl::BLEND) != 0;
    const bool prevDepth = gl::IsEnabled(gl::DEPTH_TEST) != 0;
    const bool prevScissor = gl::IsEnabled(gl::SCISSOR_TEST) != 0;
    const bool prevCull = gl::IsEnabled(gl::CULL_FACE) != 0;
    gl::Disable(gl::CULL_FACE);

    // Every 5 s: presentation rate, guest flips and the size of the replayed work.
    static auto s_statStart = std::chrono::steady_clock::now();
    static uint64_t s_presents = 0, s_flips = 0, s_draws = 0, s_verts = 0, s_texUploads = 0, s_texBytes = 0,
                    s_fbWrites = 0;
    static uint64_t s_lastDispfb = ~0ull;
    static std::chrono::nanoseconds s_replayTime{0};
    ++s_presents;
    ++g_presentCount;
    const uint64_t dispfbNow = (request.pmode & 2u) ? request.dispfb2 : request.dispfb1;
    if ((dispfbNow & 0x1FFu) != (s_lastDispfb & 0x1FFu))
        ++s_flips;
    s_lastDispfb = dispfbNow;
    for (const Command &c : s.replay.commands)
    {
        if (c.type == CmdType::Draw)
            ++s_draws;
        else if (c.type == CmdType::TextureUpload)
        {
            ++s_texUploads;
            s_texBytes += static_cast<uint64_t>(c.w) * c.h * (c.texFormat == 1 ? 1u : 4u);
        }
        else if (c.type == CmdType::FramebufferWrite)
            ++s_fbWrites;
    }
    s_verts += s.replay.vertices.size();

    // Display circuits (PMODE: EN1 bit 0, EN2 bit 1, MMOD 5, SLBG 7, ALP 8-15). Circuit 2 (or the
    // background colour) is the base; circuit 1 is blended over it when both are enabled.
    struct Circuit
    {
        bool enabled;
        uint32_t fbp, fbw, dbx, dby, width, height; // height = framebuffer lines read
        uint32_t outHeight;                          // lines on screen
        bool halfHeightFields;                       // interlaced FRAME mode reading DH/2 lines (field offset applies)
    };
    const bool interlaced = (request.smode2 & 1u) != 0u;
    const bool frameMode = (request.smode2 & 2u) != 0u;
    auto decodeCircuit = [interlaced, frameMode](bool enabled, uint64_t dispfb, uint64_t display) -> Circuit
    {
        Circuit c{};
        c.enabled = enabled;
        c.fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
        c.fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
        c.dbx = static_cast<uint32_t>((dispfb >> 32) & 0x7FFu);
        c.dby = static_cast<uint32_t>((dispfb >> 43) & 0x7FFu);
        const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu);
        const uint32_t magv = static_cast<uint32_t>((display >> 27) & 0x3u);
        c.width = static_cast<uint32_t>(((display >> 32) & 0xFFFu) + 1u) / (magh + 1u);
        c.height = static_cast<uint32_t>(((display >> 44) & 0x7FFu) + 1u) / (magv + 1u);
        if (c.width < 64u || c.height < 32u)
        {
            c.width = 640u;
            c.height = 448u;
        }
        // Like PCSX2: interlaced FRAME mode reads DH/2 framebuffer lines per field and the TV shows them on
        // twice as many lines; non-interlaced low-res ("240p") likewise puts each line on two TV lines.
        // Zone of the Enders: SMODE2 = 3, DH = 445, 512x224 buffers.
        c.outHeight = c.height;
        c.halfHeightFields = false;
        if (interlaced && frameMode && c.height >= 64u)
        {
            c.height /= 2u;
            c.outHeight = c.height * 2u;
            c.halfHeightFields = true;
        }
        else if (!interlaced && c.height <= 256u)
            c.outHeight = c.height * 2u;
        c.width = std::min(c.width, std::max<uint32_t>(c.fbw, 1u) * 64u);
        c.enabled = c.enabled && c.fbw != 0u;
        return c;
    };
    const Circuit crt1 = decodeCircuit((request.pmode & 1u) != 0u, request.dispfb1, request.display1);
    const Circuit crt2 = decodeCircuit((request.pmode & 2u) != 0u, request.dispfb2, request.display2);
    g_halfHeightFields.store((crt1.enabled && crt1.halfHeightFields) || (crt2.enabled && crt2.halfHeightFields),
                             std::memory_order_relaxed);

    // Internal resolution: the window loop sized the present target for it (SetPresentTarget). Vertically twice
    // that when the display doubles every line, so pixels stay square; x1 stays the PS2's own resolution.
    {
        if (g_presentTargetGen.load() != s.presentTargetGen)
        {
            s.presentTargetGen = g_presentTargetGen.load();
            s.presentFboTexture = 0; // a new window texture (its id may be a reused one)
        }
        const bool lineDoubled = (crt1.enabled && crt1.outHeight == 2u * crt1.height) ||
                                 (crt2.enabled && crt2.outHeight == 2u * crt2.height);
        const uint32_t want = std::max<uint32_t>(1u, g_presentScale.load());
        s.applyScale(want, want * ((want > 1u && lineDoubled) ? 2u : 1u));
    }
    // GPU time of the replay and composition, read back when the query is done (a few presents later).
    static uint64_t s_gpuNs = 0, s_gpuMaxNs = 0, s_gpuSamples = 0;
    const uint32_t timerSlot = s.timerNext;
    const uint32_t timerBit = 1u << timerSlot;
    s.timerNext = (s.timerNext + 1u) % static_cast<uint32_t>(s.timerQueries.size());
    bool timing = false;
    if (const gl::GLuint q = s.timerQueries[timerSlot]; q != 0)
    {
        if (s.timerPending & timerBit)
        {
            gl::GLint ready = 0;
            gl::GetQueryObjectiv(q, gl::QUERY_RESULT_AVAILABLE, &ready);
            if (ready)
            {
                gl::GLuint64 ns = 0;
                gl::GetQueryObjectui64v(q, gl::QUERY_RESULT, &ns);
                s_gpuNs += ns;
                s_gpuMaxNs = std::max<uint64_t>(s_gpuMaxNs, ns);
                ++s_gpuSamples;
                s.timerPending &= ~timerBit;
            }
        }
        if (!(s.timerPending & timerBit))
        {
            gl::BeginQuery(gl::TIME_ELAPSED, q);
            timing = true;
        }
    }
    const auto replayStart = std::chrono::steady_clock::now();
    s.replayCommands();
    s_replayTime += std::chrono::steady_clock::now() - replayStart;
    if (replayStart - s_statStart > std::chrono::seconds(5))
    {
        const double secs = std::chrono::duration<double>(replayStart - s_statStart).count();
        std::cout << "[gpu:pace] mode=" << g_ps2ActiveOverlayName.load(std::memory_order_relaxed) << " presents/s=" << s_presents / secs << " flips/s=" << s_flips / secs
                  << " drawBatches/s=" << s_draws / secs << " verts/s=" << s_verts / secs
                  << " texUploads/s=" << s_texUploads / secs << " texMB/s=" << s_texBytes / secs / 1e6
                  << " fbWrites/s=" << s_fbWrites / secs
                  << " eeDecodeMs/s=" << g_decodeNs.exchange(0) / 1e6 / secs
                  << " eeDecodeMpix/s=" << g_decodePixels.exchange(0) / 1e6 / secs
                  << " texCacheHits/s=" << g_contentHits.exchange(0) / secs
                  << " replayMs/present=" << std::chrono::duration<double>(s_replayTime).count() * 1000.0 / std::max<uint64_t>(1, s_presents)
                  << " scale=" << s.sx << "x" << s.sy
                  << " gpuMs/present=" << s_gpuNs / 1e6 / static_cast<double>(std::max<uint64_t>(1, s_gpuSamples))
                  << " gpuMsMax=" << s_gpuMaxNs / 1e6
                  << " selfCopies/present=" << s.statSelfCopies / static_cast<double>(std::max<uint64_t>(1, s_presents))
                  << " selfCopyKpx/present=" << s.statSelfCopyPx / 1e3 / static_cast<double>(std::max<uint64_t>(1, s_presents))
                  << " dateCopies/present=" << s.statDateCopies / static_cast<double>(std::max<uint64_t>(1, s_presents))
                  << " dateKpx/present=" << s.statDatePx / 1e3 / static_cast<double>(std::max<uint64_t>(1, s_presents))
                  << " rectCopies=" << s.statRectCopies << " grows=" << s.statGrows << " writeRuns=" << s.statWriteRuns
                  << " writeBlits=" << s.statWriteBlits << " maskDraws=" << s.statMaskDraws
                  << std::endl;
        s.statSelfCopies = s.statSelfCopyPx = s.statDateCopies = s.statDatePx = s.statRectCopies = s.statGrows = 0;
        s.statWriteRuns = s.statWriteBlits = s.statMaskDraws = 0;
        s_gpuNs = s_gpuMaxNs = s_gpuSamples = 0;
        s_statStart = replayStart;
        s_presents = s_flips = s_draws = s_verts = s_texUploads = s_texBytes = s_fbWrites = 0;
        s_replayTime = std::chrono::nanoseconds{0};
    }

    const bool progressive = ps2ProgressiveDisplayActive();
    const bool mmod = (request.pmode & (1u << 5)) != 0u;
    const bool slbg = (request.pmode & (1u << 7)) != 0u;
    const float alp = static_cast<float>((request.pmode >> 8) & 0xFFu) / 255.0f;
    auto findTarget = [&](const Circuit &c) -> Impl::RenderTarget *
    {
        if (!c.enabled)
            return nullptr;
        auto it = s.renderTargets.find({c.fbp, c.fbw});
        if (it == s.renderTargets.end())
            return nullptr;
        s.ensureRows(it->second, std::min(kRtHeight, c.dby + c.height), false); // rows the display reads
        return &it->second;
    };
    // Output scale: the circuits are composed at sx times the PS2 picture in both directions (the vertical
    // line doubling of the display takes the targets' extra rows when sy = 2 sx).
    const uint32_t ps = s.sx;
    Impl::RenderTarget *rt1 = findTarget(crt1);
    Impl::RenderTarget *rt2 = findTarget(crt2);

    PresentationFrame result{};
    const uint32_t target = g_presentTexture.load();
    if ((rt1 != nullptr || rt2 != nullptr) && target != 0)
    {
        const uint32_t tw = g_presentTargetW.load() / ps, th = g_presentTargetH.load() / ps; // in PS2 pixels
        uint32_t width = 0, height = 0;
        if (crt1.enabled)
        {
            width = std::max(width, crt1.width);
            height = std::max(height, crt1.outHeight);
        }
        if (crt2.enabled)
        {
            width = std::max(width, crt2.width);
            height = std::max(height, crt2.outHeight);
        }
        width = std::min(width, tw);
        height = std::min(height, th);

        gl::BindFramebuffer(gl::FRAMEBUFFER, s.presentFbo);
        if (s.presentFboTexture != target)
        {
            gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, target, 0);
            s.presentFboTexture = target;
        }
        gl::Disable(gl::SCISSOR_TEST);
        gl::Disable(gl::DEPTH_TEST);
        gl::Disable(gl::BLEND);
        gl::ColorMask(1, 1, 1, 1);
        gl::Viewport(0, 0, static_cast<gl::GLsizei>(tw * ps), static_cast<gl::GLsizei>(th * ps));
        gl::ClearColor(static_cast<float>(request.bgcolor & 0xFFu) / 255.0f,
                       static_cast<float>((request.bgcolor >> 8) & 0xFFu) / 255.0f,
                       static_cast<float>((request.bgcolor >> 16) & 0xFFu) / 255.0f, 1.0f);
        gl::Clear(gl::COLOR_BUFFER_BIT);
        gl::UseProgram(s.presentProgram);
        gl::Uniform1i(s.presentTex, 0);
        gl::BindVertexArray(s.presentVao);

        auto drawCircuit = [&](const Circuit &c, const Impl::RenderTarget &rt, int alphaMode)
        {
            const uint32_t w = std::min(c.width, tw), h = std::min(c.height, th);
            // Half-height interlaced frames are drawn half a line apart per field (libgraph sceGsSetHalfOffset);
            // a TV puts odd fields one line lower, so do the same ("bob") instead of letting the picture shake.
            static const bool s_invertField = std::getenv("PS2X_FIELD_INVERT") != nullptr;
            const bool halfLineFrame = (g_fbHalfLineOffset[c.fbp & 511u].load(std::memory_order_relaxed) != 0u) != s_invertField;
            const int fieldShift = (!progressive && c.halfHeightFields && halfLineFrame) ? 1 : 0;
            gl::Viewport(0, fieldShift * static_cast<int>(ps), static_cast<gl::GLsizei>(w * ps),
                         static_cast<gl::GLsizei>(std::min(c.outHeight, th) * ps));
            gl::BindTexture(gl::TEXTURE_2D, rt.color);
            gl::Uniform4f(s.presentRect, static_cast<float>(c.dbx), static_cast<float>(c.dby), static_cast<float>(w),
                          static_cast<float>(h));
            gl::Uniform2f(s.presentSrcSize, static_cast<float>(rt.width), static_cast<float>(rt.height));
            gl::Uniform1i(s.presentAlphaMode, alphaMode);
            if (alphaMode == 0)
            {
                gl::Disable(gl::BLEND);
            }
            else
            {
                gl::Enable(gl::BLEND);
                gl::BlendEquationSeparate(gl::FUNC_ADD, gl::FUNC_ADD);
                if (alphaMode == 1)
                {
                    gl::BlendColor(0, 0, 0, alp);
                    gl::BlendFuncSeparate(gl::CONSTANT_ALPHA, gl::ONE_MINUS_CONSTANT_ALPHA, gl::ZERO, gl::ONE);
                }
                else
                {
                    gl::BlendFuncSeparate(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA, gl::ZERO, gl::ONE);
                }
            }
            gl::DrawArrays(gl::TRIANGLE_STRIP, 0, 4);
        };
        const bool both = rt1 != nullptr && rt2 != nullptr;
        if (rt2 != nullptr && !(both && slbg))
            drawCircuit(crt2, *rt2, 0);
        if (rt1 != nullptr)
            drawCircuit(crt1, *rt1, both ? (mmod ? 1 : 2) : 0);

        // Half-height interlaced frames alternate a half-line drawing offset per field (see fieldShift); a TV's
        // interlacing (and PCSX2's default deinterlacer) blends consecutive fields. Average each frame with the
        // previous one so the picture stops bobbing. PS2X_NO_DEINTERLACE=1 shows raw fields.
        static const bool s_noDeinterlace = std::getenv("PS2X_NO_DEINTERLACE") != nullptr;
        const bool blendFields = !progressive && !s_noDeinterlace && ((rt1 != nullptr && crt1.halfHeightFields) ||
                                                      (rt2 != nullptr && crt2.halfHeightFields));
        if (blendFields)
        {
            // Sizes in GL pixels of the present target.
            const uint32_t gtw = tw * ps, gth = th * ps, gw = width * ps, gh = height * ps;
            if (s.fieldTexW != gtw || s.fieldTexH != gth || s.fieldHistory == 0u || s.fieldCurrent == 0u)
            {
                for (gl::GLuint old : {s.fieldHistory, s.fieldCurrent})
                    if (old != 0u)
                    {
                        s.dropFramebuffers(old);
                        gl::DeleteTextures(1, &old);
                    }
                s.fieldHistory = s.newTexture(gl::RGBA16F, gl::RGBA, gl::FLOAT, gtw, gth);
                s.fieldCurrent = s.newTexture(gl::RGBA16F, gl::RGBA, gl::FLOAT, gtw, gth);
                s.fieldTexW = gtw;
                s.fieldTexH = gth;
                s.fieldHistoryValid = false;
            }
            // Keep the unblended frame for the next present, then lay the previous frame over it at 50%.
            s.blitPixels(target, 0, 0, static_cast<int>(gw), static_cast<int>(gh), s.fieldCurrent, 0, 0, static_cast<int>(gw),
                         static_cast<int>(gh), false, false);
            if (s.fieldHistoryValid)
            {
                gl::BindFramebuffer(gl::FRAMEBUFFER, s.presentFbo);
                gl::Disable(gl::SCISSOR_TEST);
                gl::Viewport(0, 0, static_cast<gl::GLsizei>(gw), static_cast<gl::GLsizei>(gh));
                gl::UseProgram(s.presentProgram);
                gl::Uniform1i(s.presentTex, 0);
                gl::BindVertexArray(s.presentVao);
                gl::BindTexture(gl::TEXTURE_2D, s.fieldHistory);
                gl::Uniform4f(s.presentRect, 0.0f, 0.0f, static_cast<float>(gw), static_cast<float>(gh));
                gl::Uniform2f(s.presentSrcSize, static_cast<float>(gtw), static_cast<float>(gth));
                gl::Uniform1i(s.presentAlphaMode, 1);
                gl::Enable(gl::BLEND);
                gl::BlendEquationSeparate(gl::FUNC_ADD, gl::FUNC_ADD);
                gl::BlendColor(0, 0, 0, 0.5f);
                gl::BlendFuncSeparate(gl::CONSTANT_ALPHA, gl::ONE_MINUS_CONSTANT_ALPHA, gl::ZERO, gl::ONE);
                gl::DrawArrays(gl::TRIANGLE_STRIP, 0, 4);
            }
            std::swap(s.fieldHistory, s.fieldCurrent);
            s.fieldHistoryValid = true;
            gl::BindFramebuffer(gl::FRAMEBUFFER, s.presentFbo);
        }
        else
        {
            s.fieldHistoryValid = false;
        }
        gl::Disable(gl::BLEND);
        gl::BindVertexArray(0);

        if (ps > 1u)
        {
            // The window filters this picture (linear + mipmaps), and the picture fills only the top-left corner of
            // the texture. Repeat its last column and row a few texels into the unused area and clamp the edges,
            // so filtering never mixes in the black unused area or (with raylib's default REPEAT wrap) the far
            // side of the texture: the window showed a darker first column at x2-x4.
            const int gw = static_cast<int>(width * ps), gh = static_cast<int>(height * ps);
            const int padX = std::min(8, static_cast<int>(tw * ps) - gw), padY = std::min(8, static_cast<int>(th * ps) - gh);
            gl::BindFramebuffer(gl::READ_FRAMEBUFFER, s.presentFbo);
            gl::BindFramebuffer(gl::DRAW_FRAMEBUFFER, s.presentFbo);
            if (padX > 0 && gw > 0 && gh > 0)
                gl::BlitFramebuffer(gw - 1, 0, gw, gh, gw, 0, gw + padX, gh, gl::COLOR_BUFFER_BIT, gl::NEAREST);
            if (padY > 0 && gw > 0 && gh > 0)
                gl::BlitFramebuffer(0, gh - 1, gw + std::max(padX, 0), gh, 0, gh, gw + std::max(padX, 0), gh + padY,
                                    gl::COLOR_BUFFER_BIT, gl::NEAREST);
            gl::BindFramebuffer(gl::FRAMEBUFFER, s.presentFbo);
            // The window shrinks or stretches this picture to its own size: mipmaps let it average the extra
            // pixels instead of skipping them (shimmering edges at x6 and above).
            gl::BindTexture(gl::TEXTURE_2D, target);
            gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
            gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
            gl::GenerateMipmap(gl::TEXTURE_2D);
            gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR_MIPMAP_LINEAR);
            gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
        }
        g_presentedW = width * ps;
        g_presentedH = height * ps;
        result.width = width * ps;
        result.height = height * ps;
        result.displayFbp = rt1 ? crt1.fbp : crt2.fbp;
        result.sourceFbp = result.displayFbp;
    }
    else if (target != 0)
    {
        // Nothing rendered on the GPU for this buffer yet (e.g. CPU uploads only): show local memory.
        PresentationFrame cpuFrame = s.cpu.Present(request);
        if (cpuFrame)
        {
            gl::BindTexture(gl::TEXTURE_2D, target);
            gl::PixelStorei(gl::UNPACK_ALIGNMENT, 4);
            // CPU frames are tightly packed (stride = width).
            const uint32_t w = std::min(cpuFrame.width, g_presentTargetW.load());
            const uint32_t h = std::min(cpuFrame.height, g_presentTargetH.load());
            if (w == cpuFrame.width)
                gl::TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, static_cast<gl::GLsizei>(w), static_cast<gl::GLsizei>(h), gl::RGBA,
                                  gl::UNSIGNED_BYTE, cpuFrame.pixels.data());
            if (ps > 1u)
            {
                gl::GenerateMipmap(gl::TEXTURE_2D);
                gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR_MIPMAP_LINEAR);
                gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
            }
            g_presentedW = cpuFrame.width;
            g_presentedH = cpuFrame.height;
            result.width = cpuFrame.width;
            result.height = cpuFrame.height;
        }
    }

    if (timing)
    {
        gl::EndQuery(gl::TIME_ELAPSED);
        s.timerPending |= timerBit;
    }

    // Restore GL state.
    gl::BindFramebuffer(gl::FRAMEBUFFER, static_cast<gl::GLuint>(prevFbo));
    gl::UseProgram(static_cast<gl::GLuint>(prevProgram));
    gl::BindVertexArray(static_cast<gl::GLuint>(prevVao));
    gl::BindBuffer(gl::ARRAY_BUFFER, static_cast<gl::GLuint>(prevBuffer));
    gl::BindTexture(gl::TEXTURE_2D, static_cast<gl::GLuint>(prevTexture));
    gl::ActiveTexture(static_cast<gl::GLenum>(prevActive));
    gl::Viewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, prevUnpack);
    gl::ColorMask(1, 1, 1, 1);
    gl::DepthMask(1);
    if (prevBlend) gl::Enable(gl::BLEND); else gl::Disable(gl::BLEND);
    if (prevDepth) gl::Enable(gl::DEPTH_TEST); else gl::Disable(gl::DEPTH_TEST);
    if (prevScissor) gl::Enable(gl::SCISSOR_TEST); else gl::Disable(gl::SCISSOR_TEST);
    if (prevCull) gl::Enable(gl::CULL_FACE); else gl::Disable(gl::CULL_FACE);
    gl::ClearDepth(1.0);
    gl::BlendEquationSeparate(gl::FUNC_ADD, gl::FUNC_ADD);
    gl::BlendFuncSeparate(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA, gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);

    s.replay.clear();
    return result;
}

bool GSGpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    m->cpu.ClearFramebuffer(context, rgba);
    std::lock_guard<std::mutex> lock(m->recordMutex);
    if ((context.fba & 1u) != 0u)
        rgba |= 0x80000000u;
    Command cmd{};
    cmd.type = CmdType::Clear;
    cmd.id = context.frame.fbp;
    cmd.fbw = context.frame.fbw;
    cmd.x = context.scissor.x0;
    cmd.y = context.scissor.y0;
    cmd.w = context.scissor.x1 - context.scissor.x0 + 1u;
    cmd.h = context.scissor.y1 - context.scissor.y0 + 1u;
    cmd.rgba = rgba;
    m->recording.commands.push_back(cmd);
    m->targets[{context.frame.fbp, context.frame.fbw}] = context.frame.psm;
    m->claimPages(context.frame.fbp, context.frame.fbw, context.frame.psm, static_cast<int>(cmd.x), static_cast<int>(cmd.y),
                  static_cast<int>(cmd.x + cmd.w) - 1, static_cast<int>(cmd.y + cmd.h) - 1);
    return true;
}

uint32_t GSGpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) { return m->cpu.ConsumeLocalToHostBytes(dst, maxBytes); }
uint32_t GSGpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const { return m->cpu.ReadVram(psm, base, bw, x, y); }
void GSGpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    m->cpu.WriteVram(psm, base, bw, x, y, value);
    std::lock_guard<std::mutex> lock(m->recordMutex);
    uint32_t p0, p1;
    pageSpan(base, bw, psm, y + 1u, p0, p1);
    m->releasePages(p0, p1);
}
void GSGpuBackend::SnapshotVram(std::vector<uint8_t> &out) const { m->cpu.SnapshotVram(out); }
GSTransferSnapshot GSGpuBackend::GetTransferSnapshot() const { return m->cpu.GetTransferSnapshot(); }

void GSGpuBackend::SaveState(ps2_state::Writer &w) const
{
    m->cpu.SaveState(w);
    std::lock_guard<std::mutex> lock(m->recordMutex);
    w.pod(m->transfer);
    w.boolean(m->transferActive);
}

void GSGpuBackend::LoadState(ps2_state::Reader &r)
{
    m->cpu.LoadState(r);
    std::lock_guard<std::mutex> lock(m->recordMutex);
    r.pod(m->transfer);
    m->transferActive = r.boolean();
    // As Reset(), without clearing local memory: drop recorded draws, decoded textures and render-target page
    // ownership, and move every version forward so nothing cached before the load can match.
    m->recording.clear();
    m->textures.clear();
    m->contentCache.clear();
    m->freeTextureIds.clear();
    m->lastKey = TextureKey{~0ull, 0};
    m->targets.clear();
    m->pageOwner.fill(0u);
    ++m->versionCounter;
    for (uint32_t &v : m->pageVersion)
        v = m->versionCounter;
    ++m->clutVersion;
    m->lastClutLoadValid = false;
}
