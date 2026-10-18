// Dreamcast UI source: trx/game/output/sources/ui.c drawn through the
// PowerVR. Sprites (text, icons), flat and gradient quads and rings are kept
// as screen-space primitives through the frame and drawn in the UI pass, in
// the translucent list, at the depth their z gives them.

#include "pvr.h"

#include <platform/dreamcast/stub.h>
#include <trx/core/memory.h>
#include <trx/core/vector.h>
#include <trx/game/output/const.h>
#include <trx/game/output/scene_compositor.h>
#include <trx/game/output/sources/ui.h>
#include <trx/game/output/textures.h>
#include <trx/game/viewport.h>

#include <math.h>

#define M_RING_SEGMENTS 32

typedef struct {
    // Strip order: top left, top right, bottom left, bottom right; a
    // triangle uses the first three.
    DC_PVR_VERTEX v[4];
    int16_t page;
    bool is_triangle;
} M_PRIM;

typedef struct {
    SCENE_SOURCE source;
    VECTOR *prims; // M_PRIM
    bool binocular_mask;
} M_PRIV;

static M_PRIV m_Priv = {};

static uint32_t M_PackF(const RGBA_F c)
{
    const float a = c.a < 0.0f ? 0.0f : (c.a > 1.0f ? 1.0f : c.a);
    const float r = c.r < 0.0f ? 0.0f : (c.r > 1.0f ? 1.0f : c.r);
    const float g = c.g < 0.0f ? 0.0f : (c.g > 1.0f ? 1.0f : c.g);
    const float b = c.b < 0.0f ? 0.0f : (c.b > 1.0f ? 1.0f : c.b);
    return ((uint32_t)(a * 255.0f) << 24) | ((uint32_t)(r * 255.0f) << 16)
        | ((uint32_t)(g * 255.0f) << 8) | (uint32_t)(b * 255.0f);
}

static uint32_t M_Pack8(const RGBA_8888 c)
{
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16)
        | ((uint32_t)c.g << 8) | c.b;
}

// UI coordinates are in the UI viewport; the screen is 640x480.
static void M_ToScreen(
    const float x, const float y, const float z, DC_PVR_VERTEX *const out)
{
    const VIEWPORT_RECT ui = Viewport_GetRect(VIEWPORT_UI);
    const float sx = ui.width > 0 ? 640.0f / ui.width : 1.0f;
    const float sy = ui.height > 0 ? 480.0f / ui.height : 1.0f;
    out->x = x * sx;
    out->y = y * sy;
    // A smaller z lies nearer, as the desktop's orthographic projection
    // has it; the PowerVR wants the nearer bigger.
    out->z = 1.0f - (z - OUTPUT_UI_NEAR_Z) / (OUTPUT_UI_FAR_Z - OUTPUT_UI_NEAR_Z);
}

static void M_RenderBegin(const SCENE_SOURCE *const source)
{
    M_PRIV *const p = &m_Priv;
    Vector_Clear(p->prims);
    p->binocular_mask = false;
}

static void M_RenderPass(const SCENE_SOURCE *const source, const SCENE_PASS pass)
{
    M_PRIV *const p = &m_Priv;
    if (pass != SCENE_PASS_UI) {
        return;
    }
    for (int32_t i = 0; i < p->prims->count; i++) {
        M_PRIM *const prim = Vector_Get(p->prims, i);
        for (int32_t j = 0; j < 4; j++) {
            prim->v[j].z = DC_PVR_Depth2D(prim->v[j].z);
        }
        DC_PVR_SetState(DC_BLEND_ALPHA, prim->page, true);
        if (prim->is_triangle) {
            DC_PVR_Triangle(&prim->v[0], &prim->v[1], &prim->v[2]);
        } else {
            DC_PVR_Quad(&prim->v[0], &prim->v[1], &prim->v[2], &prim->v[3]);
        }
    }
}

static bool M_IsDirty(const SCENE_SOURCE *const source, const SCENE_PASS pass)
{
    return pass == SCENE_PASS_UI && m_Priv.prims->count > 0;
}

void OutputSource_UI_Init(void)
{
    M_PRIV *const p = &m_Priv;
    p->prims = Vector_CreateAtCapacity(sizeof(M_PRIM), 256);
    p->source = (SCENE_SOURCE) {
        .render_begin = M_RenderBegin,
        .render_pass = M_RenderPass,
        .is_dirty = M_IsDirty,
        .priv = p,
    };
    SceneCompositor_AddSource(&p->source);
}

void OutputSource_UI_Shutdown(void)
{
    M_PRIV *const p = &m_Priv;
    if (p->prims != nullptr) {
        Vector_Free(p->prims);
        p->prims = nullptr;
    }
}

void OutputSource_UI_StageMesh(const OUTPUT_UI_MESH mesh)
{
    // Models in the UI (the inventory) are not drawn yet.
    DC_STUB();
}

void OutputSource_UI_StageBinocularMask(void)
{
    DC_STUB();
    m_Priv.binocular_mask = true;
}

void OutputSource_UI_StageSprite(const OUTPUT_UI_SPRITE sprite)
{
    const SPRITE_TEXTURE *const tex = Output_GetSpriteTexture(sprite.sprite_idx);
    if (tex == nullptr) {
        return;
    }
    const float u0 = (tex->offset & 0xFF) / 256.0f;
    const float v0 = (tex->offset >> 8) / 256.0f;
    const float u1 = u0 + tex->width / 65536.0f;
    const float v1 = v0 + tex->height / 65536.0f;

    M_PRIM prim = { .page = tex->tex_page };
    const float x[4] = { sprite.x0, sprite.x1, sprite.x0, sprite.x1 };
    const float y[4] = { sprite.y0, sprite.y0, sprite.y1, sprite.y1 };
    const float u[4] = { u0, u1, u0, u1 };
    const float v[4] = { v0, v0, v1, v1 };
    // The sprite's colours go round the corners: tl, tr, br, bl.
    const int32_t color_idx[4] = { 0, 1, 3, 2 };
    for (int32_t i = 0; i < 4; i++) {
        M_ToScreen(x[i], y[i], sprite.z, &prim.v[i]);
        prim.v[i].u = u[i];
        prim.v[i].v = v[i];
        prim.v[i].argb = M_PackF(sprite.color[color_idx[i]]);
    }
    Vector_Add(m_Priv.prims, &prim);
}

void OutputSource_UI_StageQuad(const OUTPUT_UI_QUAD quad)
{
    M_PRIM prim = { .page = DC_PVR_NO_TEXTURE };
    const float x[4] = { quad.x0, quad.x1, quad.x0, quad.x1 };
    const float y[4] = { quad.y0, quad.y0, quad.y1, quad.y1 };
    const RGBA_8888 c[4] = { quad.tl, quad.tr, quad.bl, quad.br };
    for (int32_t i = 0; i < 4; i++) {
        M_ToScreen(x[i], y[i], quad.z, &prim.v[i]);
        prim.v[i].argb = M_Pack8(c[i]);
    }
    Vector_Add(m_Priv.prims, &prim);
}

void OutputSource_UI_StageCircle(const OUTPUT_UI_CIRCLE circle)
{
    const float ri = (float)circle.r_inner;
    const float ro = (float)circle.r_outer;
    if (ro <= ri) {
        return;
    }
    const uint32_t color = M_Pack8(circle.color);
    for (int32_t i = 0; i < M_RING_SEGMENTS; i++) {
        const float a0 = 2.0f * (float)M_PI * i / M_RING_SEGMENTS;
        const float a1 = 2.0f * (float)M_PI * (i + 1) / M_RING_SEGMENTS;
        M_PRIM prim = { .page = DC_PVR_NO_TEXTURE };
        const float x[4] = {
            circle.cx + ro * cosf(a0), circle.cx + ro * cosf(a1),
            circle.cx + ri * cosf(a0), circle.cx + ri * cosf(a1),
        };
        const float y[4] = {
            circle.cy + ro * sinf(a0), circle.cy + ro * sinf(a1),
            circle.cy + ri * sinf(a0), circle.cy + ri * sinf(a1),
        };
        for (int32_t j = 0; j < 4; j++) {
            M_ToScreen(x[j], y[j], circle.z, &prim.v[j]);
            prim.v[j].argb = color;
        }
        Vector_Add(m_Priv.prims, &prim);
    }
}

bool OutputSource_UI_HasStaged(void)
{
    return m_Priv.prims->count > 0 || m_Priv.binocular_mask;
}
