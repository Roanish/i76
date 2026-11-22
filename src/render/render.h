#ifndef RENDER_H
#define RENDER_H

/*
 * render.h — Render abstraction layer
 *
 * Replaces NITSHELL.DLL from the original binary.
 *
 * Original exported an opaque function-pointer table populated via
 * GetProcAddress after LoadLibraryA("NITSHELL.DLL"). The known slots were:
 *
 *   DAT_00653f3c  →  BeginFrame(surface*)
 *   DAT_00653f40  →  EndFrame(surface*)
 *   DAT_00653f48  →  SetPalette(surface*, palette*)
 *   DAT_00653f4c  →  Init(surface*, mode)
 *   DAT_00653f60  →  Flip(surface*)
 *
 * We replicate those roles here with a clean C interface. Backend is Vulkan
 * (render_vk.c); D3D12 would be a second implementation of this header.
 *
 * Rendering model:
 *   The game was 8-bit palettized software-rendered at 640×480.
 *   We upload the raw 8-bit index surface as a texture and do palette lookup
 *   on the GPU (see blit.frag). This preserves palette animations and fades
 *   with zero CPU overhead.
 */

#include <stdbool.h>
#include <stdint.h>

/* One RGB palette entry — matches the PCX on-disk format byte for byte. */
typedef struct { uint8_t r, g, b; } Rgb8;

/*
 * Scene render backend, mirroring the original's software/Glide split:
 *   RENDER_SOFTWARE — CPU rasterizer into the 8-bit framebuffer (softraster.c),
 *                     presented via the palette blit. The faithful path.
 *   RENDER_HARDWARE — GPU geometry pass (render_3d_*), the modern equivalent of
 *                     the 3Dfx hardware path.
 * The present layer (window/swapchain/blit) is shared by both.
 */
typedef enum { RENDER_SOFTWARE, RENDER_HARDWARE } RenderMode;

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

/*
 * render_init()
 *   Initialise Vulkan: instance, device, swapchain, pipeline.
 *   Must be called after platform_init() (needs the SDL_Window*).
 *   Returns false on failure; caller should treat as fatal.
 */
bool render_init(void);

/*
 * render_shutdown()
 *   Destroy all Vulkan objects in reverse-creation order.
 *   Mirrors the original's FUN_0046dc10 + FreeLibrary(NITSHELL) sequence.
 */
void render_shutdown(void);

/* -----------------------------------------------------------------------
 * Per-frame
 * ----------------------------------------------------------------------- */

/*
 * render_begin_frame()
 *   Acquire the next swapchain image, wait on its in-flight fence.
 *   Call once at the top of the game loop — mirrors DAT_00653f3c (BeginFrame).
 */
void render_begin_frame(void);

/*
 * render_end_frame()
 *   Record + submit the command buffer, present the image.
 *   Mirrors DAT_00653f40 (EndFrame) + DAT_00653f60 (Flip).
 */
void render_end_frame(void);

/* -----------------------------------------------------------------------
 * Palettized blit
 * ----------------------------------------------------------------------- */

/*
 * render_set_palette()
 *   Upload 256 RGB entries to the GPU palette UBO.
 *   Mirrors DAT_00653f48 (SetPalette).
 *   Safe to call every frame; the copy is a 4 KB memcpy to a mapped buffer.
 */
void render_set_palette(const Rgb8 *palette_256);

/*
 * render_blit_indexed()
 *   Upload a width×height 8-bit index surface and draw it fullscreen.
 *   The fragment shader expands indices → RGB via the current palette.
 *   width/height must match the surface the game rendered into.
 */
void render_blit_indexed(const uint8_t *pixels, uint32_t width, uint32_t height);

/* -----------------------------------------------------------------------
 * 3D world pass
 *
 * A real depth-buffered geometry pass that renders directly to the swapchain
 * in RGB, drawn BEFORE the palette blit (which becomes a transparent HUD
 * overlay — index 0 keys out). This is what the world/scene viewer uses to put
 * the camera inside the scene; the palettized blit path is untouched for 2D
 * screens that never call render_3d_*.
 *
 * Matrices are 16-float COLUMN-MAJOR (GPU convention). The model matrix's
 * upper-left 3x3 is used to rotate normals, so keep it a pure rotation (plus
 * uniform scale at most) for correct lighting.
 * ----------------------------------------------------------------------- */

/* Opaque GPU vertex buffer. Vertices are interleaved [px,py,pz, u,v, nx,ny,nz]
 * (RENDER_VERT_FLOATS). Normals are OUTWARD-facing, in object space. */
#define RENDER_VERT_FLOATS 8
typedef struct RenderMesh RenderMesh;

/* Opaque GPU texture — an 8-bit palette-index image sampled by the mesh pass,
 * expanded to RGB through the shared palette UBO (render_set_palette). */
typedef struct RenderTexture RenderTexture;

/*
 * render_mesh_create()
 *   Upload `vertex_count` vertices (RENDER_VERT_FLOATS each: position, UV,
 *   normal) to a
 *   device-local vertex buffer. Drawn as a non-indexed triangle list, so
 *   vertex_count must be a multiple of 3 (triangulate on the CPU; see
 *   worldview.c). Returns NULL on failure or empty input.
 */
RenderMesh *render_mesh_create(const float *verts, uint32_t vertex_count);
void        render_mesh_destroy(RenderMesh *m);

/*
 * render_texture_create()
 *   Upload a w*h image of 8-bit palette indices as an R8 texture, sampled
 *   NEAREST/REPEAT by the mesh pass (palette lookup happens in the shader from
 *   the current palette). Returns NULL on failure.
 */
RenderTexture *render_texture_create(const uint8_t *indices, int w, int h);
void           render_texture_destroy(RenderTexture *t);

/*
 * render_3d_begin()
 *   Start a 3D pass for this frame: clears the queued draws and stores the
 *   view+projection. Call once per frame between render_begin_frame() and the
 *   render_3d_draw() calls.
 */
void render_3d_begin(const float view[16], const float proj[16]);

/*
 * render_3d_draw()
 *   Queue one mesh for this frame with its model transform and texture.
 *   Replayed inside the render pass by render_end_frame(). Draws with a NULL
 *   mesh or texture are ignored.
 */
void render_3d_draw(const RenderMesh *m, const float model[16],
                    const RenderTexture *tex);

/*
 * render_3d_draw_decal()
 *   As render_3d_draw(), for geometry lying ON other geometry (roads on
 *   terrain): drawn with a small depth bias toward the camera so it wins
 *   where the two are coplanar, instead of z-fighting.
 */
void render_3d_draw_decal(const RenderMesh *m, const float model[16],
                          const RenderTexture *tex);

/*
 * Global (directional) lighting for the 3D pass, per pixel — the original's
 * model (nitro.exe vertex_light FUN_0046ff00 / terrain FUN_00495f80):
 *   I    = min(1, intensity * max(0, N.L) * haze(depth) + ambient)
 *   haze = clamp(haze_base + haze_slope * depth, haze_min, 1)   (view depth)
 *   row  = (1 - I) * 31.5                     (span stage, 8064/256)
 *   rgb  = albedo * curve[row]                (interpolated between rows)
 * curve[] is the brightness response measured from the mission's .lum shade
 * table (32 rows, per channel; ~1.4x at row 0 .. ~0.21x at row 31).
 * light_dir points TOWARD the light, in world space. Until this is called the
 * pass is unlit (curve all 1).
 */
#define RENDER_LIGHT_ROWS 32
typedef struct {
    float light_dir[3];   /* unit vector toward the sun/moon (world space) */
    float intensity;      /* phase intensity                               */
    float ambient;        /* phase ambient (0 in every original phase)     */
    float haze_base, haze_slope, haze_min;
    float curve[RENDER_LIGHT_ROWS][3];   /* brightness multiplier per row  */
} RenderLighting;

void render_set_lighting(const RenderLighting *l);

#endif /* RENDER_H */
