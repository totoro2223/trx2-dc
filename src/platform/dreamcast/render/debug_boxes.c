// Dreamcast output: the debug bounding boxes (trx/game/output/sources/misc.c
// draws them as wireframe cuboids). Each box staged during the frame is
// outlined in the transparent pass, as thin flat quads along its twelve edges,
// over the scene whatever lies in front of it.

#include "pvr.h"

#include <trx/core/vector.h>
#include <trx/game/matrix.h>
#include <trx/game/output/scene_compositor.h>
#include <trx/game/output/scene_source.h>
#include <trx/game/output/sources/misc.h>

#include <stdint.h>

// In batcher.c, which holds the frame's camera.
bool DC_Batcher_ProjectWorld(const float p[3], float out[3]);
void DC_Batcher_WorldMatrix(const MATRIX *m, float out[3][4]);

// Half a line's width, in the 640x480 screen's pixels.
#define M_HALF_WIDTH 1.0f
// More boxes than a frame has are dropped.
#define M_MAX_BOXES 256

typedef struct {
    float matrix[3][4];
    uint32_t argb;
} M_BOX;

static SCENE_SOURCE m_Source;
static VECTOR *m_Boxes = nullptr;

// The corners of the box, by bit 0 for x, 1 for y, 2 for z (set: the maximum).
// A cuboid is a unit one, [-1, 1], that the staged matrix sizes and places.
static void M_Corner(const M_BOX *const box, const int32_t i, float out[3])
{
    const float local[3] = {
        (i & 1) ? 1.0f : -1.0f,
        (i & 2) ? 1.0f : -1.0f,
        (i & 4) ? 1.0f : -1.0f,
    };
    for (int32_t r = 0; r < 3; r++) {
        out[r] = box->matrix[r][0] * local[0] + box->matrix[r][1] * local[1]
            + box->matrix[r][2] * local[2] + box->matrix[r][3];
    }
}

static void M_Line(const float a[3], const float b[3], const uint32_t argb)
{
    const float dx = b[0] - a[0];
    const float dy = b[1] - a[1];
    const float length_sq = dx * dx + dy * dy;
    if (length_sq < 0.25f) {
        return;
    }
    // The line's sideways, a half width long.
    const float inv = M_HALF_WIDTH / __builtin_sqrtf(length_sq);
    const float nx = -dy * inv;
    const float ny = dx * inv;
    const float z = a[2] > b[2] ? a[2] : b[2];
    const DC_PVR_VERTEX v[4] = {
        { .x = a[0] + nx, .y = a[1] + ny, .z = z, .argb = argb },
        { .x = a[0] - nx, .y = a[1] - ny, .z = z, .argb = argb },
        { .x = b[0] + nx, .y = b[1] + ny, .z = z, .argb = argb },
        { .x = b[0] - nx, .y = b[1] - ny, .z = z, .argb = argb },
    };
    DC_PVR_Quad(&v[0], &v[1], &v[2], &v[3]);
}

static void M_RenderBegin(const SCENE_SOURCE *const source)
{
    Vector_Clear(m_Boxes);
}

static bool M_IsDirty(const SCENE_SOURCE *const source, const SCENE_PASS pass)
{
    return pass == SCENE_PASS_TRANSPARENT && m_Boxes->count > 0;
}

static void M_RenderPass(const SCENE_SOURCE *const source, const SCENE_PASS pass)
{
    if (pass != SCENE_PASS_TRANSPARENT || m_Boxes->count == 0) {
        return;
    }
    DC_PVR_SetState(DC_BLEND_ALPHA, DC_PVR_NO_TEXTURE, false);
    for (int32_t i = 0; i < m_Boxes->count; i++) {
        const M_BOX *const box = Vector_Get(m_Boxes, i);
        float p[8][3];
        bool in_view[8];
        for (int32_t k = 0; k < 8; k++) {
            float world[3];
            M_Corner(box, k, world);
            in_view[k] = DC_Batcher_ProjectWorld(world, p[k]);
        }
        // Each edge joins two corners that differ in one bit.
        for (int32_t k = 0; k < 8; k++) {
            for (int32_t bit = 1; bit <= 4; bit <<= 1) {
                const int32_t other = k | bit;
                if (other != k && in_view[k] && in_view[other]) {
                    M_Line(p[k], p[other], box->argb);
                }
            }
        }
    }
}

void OutputSource_Misc_Init(void)
{
    m_Boxes = Vector_Create(sizeof(M_BOX));
    m_Source.render_begin = M_RenderBegin;
    m_Source.render_pass = M_RenderPass;
    m_Source.is_dirty = M_IsDirty;
    SceneCompositor_AddSource(&m_Source);
}

void OutputSource_Misc_Shutdown(void)
{
    if (m_Boxes != nullptr) {
        Vector_Free(m_Boxes);
        m_Boxes = nullptr;
    }
}

void OutputSource_Misc_StageCuboid(const RGBA_8888 color)
{
    if (m_Boxes == nullptr || m_Boxes->count >= M_MAX_BOXES) {
        return;
    }
    M_BOX box = {
        .argb = ((uint32_t)color.a << 24) | ((uint32_t)color.r << 16)
            | ((uint32_t)color.g << 8) | color.b,
    };
    DC_Batcher_WorldMatrix(g_WMatrixPtr, box.matrix);
    Vector_Add(m_Boxes, &box);
}
