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
    // src * a + dst * (1 - a)
    DC_BLEND_ALPHA,
    // src * a + dst
    DC_BLEND_ADD,
    // dst * (1 - src); the PC's take on PlayStation subtractive blending.
    DC_BLEND_SUB,
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

void DC_PVR_Init(void);
void DC_PVR_Shutdown(void);

// Texture pages live in video memory only. They are reserved as a level
// starts loading and written there linear (DC_PVR_WriteTexels), then
// twiddled in place once the level is complete.
uint16_t *DC_PVR_ReserveTexturePages(int32_t count);
void DC_PVR_WriteTexels(uint16_t *dst, const uint16_t *src, size_t count);
void DC_PVR_CommitTexturePages(int32_t count, const uint8_t *formats);
void DC_PVR_FreeTexturePages(void);
int32_t DC_PVR_GetTexturePageCount(void);

void DC_PVR_BeginFrame(void);
void DC_PVR_EndFrame(void);
void DC_PVR_SetBackgroundColor(float r, float g, float b);

// Everything submitted after this draws in front of everything before, as a
// depth buffer clear would make it.
void DC_PVR_NextDepthLayer(void);
// Maps 1/w (or any value in [0, 1)) into the current depth layer.
float DC_PVR_Depth(float inv_w);

void DC_PVR_SetFilter(bool bilinear);
// Selects how the next polygons are drawn. page is a texture page index, or
// DC_PVR_NO_TEXTURE for flat colour.
void DC_PVR_SetState(DC_BLEND blend, int32_t page, bool depth_test);
void DC_PVR_Triangle(
    const DC_PVR_VERTEX *a, const DC_PVR_VERTEX *b, const DC_PVR_VERTEX *c);
// a, b, c, d in strip order: a-b on one side, c-d on the other.
void DC_PVR_Quad(
    const DC_PVR_VERTEX *a, const DC_PVR_VERTEX *b, const DC_PVR_VERTEX *c,
    const DC_PVR_VERTEX *d);

// Polygon counts for the frame just begun, for the debug console.
int32_t DC_PVR_GetSubmittedCount(void);
