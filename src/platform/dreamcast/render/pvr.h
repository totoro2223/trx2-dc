#pragma once

// The PowerVR side of the Dreamcast renderer: texture pages in video memory,
// and polygons in screen space submitted to the right list with the right
// header. Everything above this (transform, lighting, clipping) works in
// floats on the SH-4.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// How a polygon reaches the screen. The first two are drawn in the opaque and
// punch-through lists, which the PowerVR depth tests; the rest go to the
// translucent list, which it sorts.
typedef enum {
    DC_BLEND_OPAQUE,
    // Opaque, with texels of zero alpha cut away (TR's colour key).
    DC_BLEND_PUNCH,
    // Punch-through again, over faces already drawn and at their depth, cut
    // where the vertex alpha times the texture's falls under half: the
    // shaded side of a cel-shaded mesh, with a hard edge however the alpha
    // runs across a face.
    DC_BLEND_PUNCH_SHADE,
    // src * a + dst * (1 - a)
    DC_BLEND_ALPHA,
    // src * a + dst
    DC_BLEND_ADD,
    // dst * (1 - src); the PC's take on PlayStation subtractive blending.
    DC_BLEND_SUB,
    // src * dst: darkens what is there (the soft shadows).
    DC_BLEND_MUL,
    // The page's relief lit as the offset colour asks (the PowerVR's bump
    // mapping: K1, K2, K3 and Q from high byte to low), which comes out as
    // the alpha, times what is there.
    // A flat texel keeps the colour when K1 + K2 is whole.
    DC_BLEND_BUMP,
    // The same relief, its alpha added as white light: the slopes turned to
    // the light shine (K1 and K2 nought, K3 the strength).
    DC_BLEND_BUMP_GLOW,
    DC_BLEND_COUNT,
} DC_BLEND;

typedef struct {
    float x;
    float y;
    // The PowerVR's depth: 1/w, bigger is nearer (see DC_PVR_Depth).
    float z;
    float u;
    float v;
    uint32_t argb;
    // Added after texturing; used for fog.
    uint32_t oargb;
} DC_PVR_VERTEX;

#define DC_PVR_NO_TEXTURE (-1)
// The screen capture (DC_PVR_CaptureScreen), as a texture page.
#define DC_PVR_SCREEN_CAPTURE (-2)
// The loading screen's text (DC_PVR_DrawLoadingScreen), as a texture page.
#define DC_PVR_LOADING_TEXT (-3)
// The shadow map (DC_PVR_BeginShadowMapFrame), as a texture page clamped at
// its edges: SLOTS cells of SIZE x SIZE texels side by side.
#define DC_PVR_SHADOW_MAP (-4)
#define DC_PVR_SHADOW_MAP_SLOTS 8
// Each cell's side, in texels.
// (At 128 the world lost faces in Flycast, TR1 snow caves, 29/10/2026.)
#define DC_PVR_SHADOW_MAP_SIZE 64
// The Lua bytecode of the engine (TRXLUA.BIN on the disc), kept in video
// memory for good, or in main memory if that has no room. Brings the
// PowerVR up if it is not. Null if the file cannot be read.
const uint8_t *DC_PVR_LuaBlob(void);
// The environment map the reflective faces add over themselves: a sphere
// map, sampled at its middle by a normal facing the camera and at its rim by
// one at right angles to it, top at the top.
#define DC_PVR_ENV_MAP (-5)
// The picture behind the loading screen and the menu (DC_PVR_SetPicture).
#define DC_PVR_PICTURE (-6)
// The film's frame (DC_PVR_SetVideoFrame).
#define DC_PVR_VIDEO (-7)
// One frame of a film in the disc's format (tools/dreamcast/fmvconv.c): a
// 256x256 VQ texture, a codebook and an index per 2x2 block.
#define DC_PVR_VIDEO_FRAME_BYTES 18432

void DC_PVR_Init(void);
// Gives the memory of the polygon lists back (400 KiB), between frames, for
// something that needs it for a moment; the next frame takes it again.
void DC_PVR_ReleaseVertexPool(void);
void DC_PVR_Shutdown(void);

// Texture pages live in video memory only (see renderer.h).
bool DC_PVR_ReserveTexturePages(int32_t count);
void DC_PVR_StoreTexturePage(int32_t page, const uint16_t *texels, uint8_t format);
uint16_t DC_PVR_GetTexel(int32_t page, int32_t x, int32_t y);
bool DC_PVR_HasTransparency(
    int32_t page, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
void DC_PVR_CommitTexturePages(void);
void DC_PVR_FreeTexturePages(void);
int32_t DC_PVR_GetTexturePageCount(void);
// How many relief pages have been made, which stalls the frame of each.
uint32_t DC_PVR_GetBumpPagesMade(void);
// The screen is 320x240 (Experimental 60), not 640x480.
bool DC_PVR_IsLowRes(void);

void DC_PVR_BeginFrame(void);
void DC_PVR_EndFrame(void);
void DC_PVR_SetBackgroundColor(float r, float g, float b);

// Everything submitted after this draws in front of everything before, as a
// depth buffer clear would make it.
void DC_PVR_NextDepthLayer(void);
// While either is on, polygons draw behind everything the world draws, and
// start no layer of their own: the background pass, and the sky (which the
// desktop clears depth after).
void DC_PVR_SetBackgroundMode(bool enabled);
void DC_PVR_SetSkyMode(bool enabled);
// Depth of a perspective vertex in the current layer from its 1/w (in
// (0, 1], 1 at the near plane). The PowerVR corrects textures for
// perspective with this same value, so it stays proportional to 1/w.
float DC_PVR_Depth3D(float inv_w);
// The same, for a caller that works it out itself per vertex:
// z = max(min(inv_w, 1), lo) * scale; and when track, the nearest z it made
// goes to DC_PVR_UseDepth once, so the next layer starts above it.
typedef struct {
    float lo;
    float scale;
    bool track;
} DC_PVR_DEPTH3D;
void DC_PVR_GetDepth3D(DC_PVR_DEPTH3D *out);
void DC_PVR_UseDepth(float z);
// Depth of a flat vertex in the current layer, in front of every
// perspective one there: v in [0, 1], bigger is nearer.
float DC_PVR_Depth2D(float v);

// Whether the next opaque and punch-through polygons darken inside shadow
// volumes.
void DC_PVR_SetShadowReceiver(bool receive);

// How much of their colour the polygons inside shadow volumes lose, for all
// of them (the PowerVR has one).
void DC_PVR_SetShadowStrength(float strength);

// A triangle of a shadow volume, in screen space: x, y and depth.
typedef struct {
    float v[3][3];
} DC_PVR_TRIANGLE;

// One closed shadow volume; the shadow receivers inside it are darkened.
void DC_PVR_ShadowVolume(const DC_PVR_TRIANGLE *tris, int32_t count);

// A frame drawn into the screen capture instead of onto the screen, to keep
// a picture of the game behind menus that stop it (the inventory); closed by
// DC_PVR_EndFrame. False when no frame can start. The capture is then drawn
// over the whole screen, its colour scaled by brightness, behind everything
// else.
bool DC_PVR_BeginCaptureFrame(void);

// A frame drawn into the shadow map instead of onto the screen, closed by
// DC_PVR_EndFrame: cell k spans x from k * SIZE to (k + 1) * SIZE. False when
// no frame can start.
bool DC_PVR_BeginShadowMapFrame(void);
void DC_PVR_DrawScreenCapture(float brightness);

// Draws a whole frame of the loading screen: two lines of text in the BIOS
// font and a bar filled to percent. Does nothing inside another frame.
void DC_PVR_DrawLoadingScreen(
    const char *line1, const char *line2, int32_t percent);
// The picture behind the loading screen and the title menu, by its gameflow
// name ("india.webp"), read from the disc into video memory once
// (make_disc.sh makes them); nullptr lets it go.
void DC_PVR_SetPicture(const char *name);
// Draws it over the whole screen, behind all else, inside a frame.
void DC_PVR_DrawPicture(float brightness);
// Puts a frame of a film in video memory, and lets it go when asked.
void DC_PVR_SetVideoFrame(const uint8_t *frame);
void DC_PVR_FreeVideo(void);
// Draws it in a box of the 640x480 screen, over the interface.
void DC_PVR_DrawVideo(float x0, float y0, float x1, float y1);
// Whether a picture is in video memory (it is not if the disc lacks it).
bool DC_PVR_HasPicture(void);
// The same, inside a box given as fractions of the screen, fading in with
// opacity, over the interface.
void DC_PVR_DrawPictureBox(
    float x0, float y0, float x1, float y1, float opacity);

void DC_PVR_SetFilter(bool bilinear);
// Pages drawn from here on sample a copy at half their size, made for them
// as they are first drawn so (unfiltered text drawn smaller than its texels
// keeps its strokes).
void DC_PVR_SetHalfPages(bool half);
// Selects how the next polygons are drawn. page is a texture page index, or
// DC_PVR_NO_TEXTURE for flat colour.
void DC_PVR_SetState(DC_BLEND blend, int32_t page, bool depth_test);
// Room in the current list for one polygon of count vertices (a triangle, or
// a strip), its header put first if the state changed, for the caller to
// fill as the PowerVR takes it (x and y times DC_PVR_GetScreenScale) and then
// commit; nullptr when the list is full. Saves the copy DC_PVR_Triangle and
// DC_PVR_Quad make.
struct pvr_vertex;
struct pvr_vertex *DC_PVR_ReservePoly(int32_t count);
void DC_PVR_CommitPoly(int32_t count);
// Polygons written outside, as a run (render/emit_sh4.S): where they go and
// how much room there is, or nullptr for none; header says whether the list
// takes a header first (put by DC_PVR_EndRun, in the room BeginRun left
// before the returned address). Nothing is taken until EndRun, which says
// how many bytes of how many polygons were written.
uint8_t *DC_PVR_BeginRun(size_t *space, bool *header);
void DC_PVR_EndRun(size_t bytes, int32_t polys, bool header);
float DC_PVR_GetScreenScale(void);
void DC_PVR_Triangle(
    const DC_PVR_VERTEX *a, const DC_PVR_VERTEX *b, const DC_PVR_VERTEX *c);
// a, b, c, d in strip order: a-b on one side, c-d on the other.
void DC_PVR_Quad(
    const DC_PVR_VERTEX *a, const DC_PVR_VERTEX *b, const DC_PVR_VERTEX *c,
    const DC_PVR_VERTEX *d);

// Polygon counts for the frame just begun, for the debug console.
int32_t DC_PVR_GetSubmittedCount(void);

// What the frame being drawn costs so far, for the debug console.
typedef struct {
    int32_t polygons;
    // Opaque, translucent, punch-through, shadow volumes.
    uint32_t list_bytes[4];
    // How long the frame waited for the PowerVR to take it.
    uint32_t wait_us;
    // How long the shadow map waited for the PowerVR to take it.
    uint32_t map_wait_us;
    // How long the PowerVR took to render the frame before.
    uint32_t render_us;
} DC_PVR_FRAME_STATS;
void DC_PVR_GetFrameStats(DC_PVR_FRAME_STATS *out);
