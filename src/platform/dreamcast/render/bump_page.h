#pragma once

// The relief (bump map) of a texture page as the PowerVR takes it, shared by
// the game (pvr.c, for the pages the disc has none of) and the tool that makes
// them for the disc (tools/dreamcast/texvq.c): plain C, nothing of KallistiOS.
//
// The relief is at half the page's size, each texel the way its surface faces.
// A texture's brightness stands for its height, which is about right for rock,
// brick and earth; see-through texels lie flat.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define DC_BUMP_SIZE 128
// How steep a step of the whole brightness range makes the surface.
#define DC_BUMP_SLOPE 8.0f
// A texel that stands straight up: see-through, or none of the page's.
#define DC_BUMP_FLAT 0xFF00
#define DC_BUMP_PI 3.14159265358979323846

// One texel of the half page from the 2x2 it covers: the colour of the solid
// ones, and as alpha the most solid. Strokes a texel wide keep all their
// length so (an average would cut the thin ones and the diagonals).
static inline uint16_t DC_Bump_HalfTexel(
    const uint16_t *const t, const bool argb4444)
{
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    uint32_t weight = 0;
    if (argb4444) {
        uint32_t a = 0;
        for (int32_t k = 0; k < 4; k++) {
            const uint32_t ta = t[k] >> 12;
            a = ta > a ? ta : a;
            weight += ta;
            r += ((t[k] >> 8) & 15) * ta;
            g += ((t[k] >> 4) & 15) * ta;
            b += (t[k] & 15) * ta;
        }
        if (weight == 0) {
            return 0;
        }
        return (uint16_t)((a << 12) | ((r / weight) << 8)
                          | ((g / weight) << 4) | (b / weight));
    }
    for (int32_t k = 0; k < 4; k++) {
        if ((t[k] & 0x8000) != 0) {
            weight++;
            r += (t[k] >> 10) & 31;
            g += (t[k] >> 5) & 31;
            b += t[k] & 31;
        }
    }
    if (weight == 0) {
        return 0;
    }
    return (uint16_t)(0x8000 | ((r / weight) << 10) | ((g / weight) << 5)
                      | (b / weight));
}

// A half-page texel's height: its brightness, 0 for see-through (no texel is
// wholly black and solid). Sets *luma to the brightness of a solid one.
static inline uint8_t DC_Bump_Height(
    const uint16_t t, const bool argb4444, int32_t *const luma)
{
    int32_t r, g, b;
    bool solid;
    if (argb4444) {
        solid = (t >> 12) >= 8;
        r = ((t >> 8) & 15) * 17;
        g = ((t >> 4) & 15) * 17;
        b = (t & 15) * 17;
    } else {
        solid = (t & 0x8000) != 0;
        r = ((t >> 10) & 31) * 255 / 31;
        g = ((t >> 5) & 31) * 255 / 31;
        b = (t & 31) * 255 / 31;
    }
    *luma = (r * 77 + g * 151 + b * 28) >> 8;
    return solid ? (uint8_t)(*luma > 1 ? *luma : 1) : 0;
}

// The heights stretched to the whole range, lo and hi being the darkest and
// brightest solid texels: TR2's rock is dark and low in contrast, and would
// come out all but flat.
static inline void DC_Bump_Stretch(
    uint8_t *const height, const int32_t lo, const int32_t hi)
{
    if (hi <= lo) {
        return;
    }
    for (int32_t i = 0; i < DC_BUMP_SIZE * DC_BUMP_SIZE; i++) {
        if (height[i] != 0) {
            const int32_t h = height[i] > lo ? height[i] : lo;
            height[i] = (uint8_t)(1 + (h - lo) * 254 / (hi - lo));
        }
    }
}

// Marks the texels of the relief a texture covers as its own (own: its number
// plus one), from its corners in 1/256 of a texel of the whole page.
static inline void DC_Bump_Own(
    uint16_t *const owner, const uint16_t own, const uint16_t *const u,
    const uint16_t *const v, const int32_t corners)
{
    int32_t x0 = DC_BUMP_SIZE;
    int32_t y0 = DC_BUMP_SIZE;
    int32_t x1 = -1;
    int32_t y1 = -1;
    for (int32_t j = 0; j < corners; j++) {
        const int32_t x = u[j] >> 9;
        const int32_t y = v[j] >> 9;
        x0 = x < x0 ? x : x0;
        y0 = y < y0 ? y : y0;
        x1 = x > x1 ? x : x1;
        y1 = y > y1 ? y : y1;
    }
    x1 = x1 < DC_BUMP_SIZE - 1 ? x1 : DC_BUMP_SIZE - 1;
    y1 = y1 < DC_BUMP_SIZE - 1 ? y1 : DC_BUMP_SIZE - 1;
    for (int32_t y = y0; y <= y1; y++) {
        for (int32_t x = x0; x <= x1; x++) {
            owner[y * DC_BUMP_SIZE + x] = own;
        }
    }
}

// A texel's height in a relief being made; one that is see-through, off the
// page or of another texture on it (owner) stands at the height of the texel
// asking, h: the step to the next texture on the page is no slope of this
// one's, and showed as a dark rim round it when a flash lit it (TR1's snow).
static inline int32_t DC_Bump_HeightAt(
    const uint8_t *const height, const uint16_t *const owner, const int32_t x,
    const int32_t y, const int32_t h, const uint16_t own)
{
    if (x < 0 || x >= DC_BUMP_SIZE || y < 0 || y >= DC_BUMP_SIZE
        || owner[y * DC_BUMP_SIZE + x] != own) {
        return h;
    }
    const int32_t at = height[y * DC_BUMP_SIZE + x];
    return at != 0 ? at : h;
}

// The slope of the relief at (x, y), as the surface normal's lean (nx, ny)
// over a unit rise; false for a see-through texel, which lies flat.
static inline bool DC_Bump_Slope(
    const uint8_t *const height, const uint16_t *const owner, const int32_t x,
    const int32_t y, float *const nx, float *const ny)
{
    const int32_t h = height[y * DC_BUMP_SIZE + x];
    if (h == 0) {
        return false;
    }
    const uint16_t own = owner[y * DC_BUMP_SIZE + x];
    const float dx = (DC_Bump_HeightAt(height, owner, x + 1, y, h, own)
                      - DC_Bump_HeightAt(height, owner, x - 1, y, h, own))
        * (0.5f / 255.0f);
    const float dy = (DC_Bump_HeightAt(height, owner, x, y + 1, h, own)
                      - DC_Bump_HeightAt(height, owner, x, y - 1, h, own))
        * (0.5f / 255.0f);
    *nx = -dx * DC_BUMP_SLOPE;
    *ny = -dy * DC_BUMP_SLOPE;
    return true;
}

// atan on 0 to 1, to about 1e-5: the relief of a page (16384 texels) wants an
// angle and an elevation of each, and the library's asinf and atan2f made a
// page cost 100 ms, a hitch each time a room showed a new one.
static inline float DC_Bump_AtanUnit(const float x)
{
    const float x2 = x * x;
    return x
        * (0.9998660f
           + x2 * (-0.3302995f + x2 * (0.1801410f + x2 * (-0.0851330f + x2 * 0.0208351f))));
}

static inline float DC_Bump_AtanFast(const float x)
{
    return x > 1.0f ? (float)(DC_BUMP_PI * 0.5) - DC_Bump_AtanUnit(1.0f / x)
                    : DC_Bump_AtanUnit(x);
}

// atan2 of (y, x) by turns: 0 to 2 pi.
static inline float DC_Bump_Atan2Turn(const float y, const float x)
{
    const float ax = fabsf(x);
    const float ay = fabsf(y);
    float a = 0.0f;
    if (ax != 0.0f || ay != 0.0f) {
        a = ay > ax ? (float)(DC_BUMP_PI * 0.5) - DC_Bump_AtanUnit(ax / ay)
                    : DC_Bump_AtanUnit(ay / ax);
    }
    if (x < 0.0f) {
        a = (float)DC_BUMP_PI - a;
    }
    if (y < 0.0f) {
        a = (float)(2.0 * DC_BUMP_PI) - a;
    }
    return a;
}

// The PowerVR's bump texel for a slope: the surface's elevation off its plane
// in the high byte (0 to 90 degrees), the way it leans about the normal in the
// low one (a whole turn, from u towards v).
static inline uint16_t DC_Bump_Encode(const float nx, const float ny)
{
    // The elevation: asin(1 / len) with len = sqrt(n^2 + 1) is atan(1 / n),
    // a quarter turn less atan(n).
    const float s =
        (float)(DC_BUMP_PI * 0.5) - DC_Bump_AtanFast(sqrtf(nx * nx + ny * ny));
    const float r = DC_Bump_Atan2Turn(ny, nx);
    uint32_t sb = (uint32_t)(s * (255.0f / ((float)DC_BUMP_PI * 0.5f)) + 0.5f);
    const uint32_t rb = (uint32_t)(r * (256.0f / (2.0f * (float)DC_BUMP_PI))) & 0xFF;
    sb = sb < 255u ? sb : 255u;
    return (uint16_t)((sb << 8) | rb);
}
