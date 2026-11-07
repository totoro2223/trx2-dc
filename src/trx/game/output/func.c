#include <trx/game/output/func.h>

#include <trx/config.h>
#include <trx/core/log.h>
#include <trx/core/math.h>
#include <trx/core/utils.h>
#include <trx/debug.h>
#include <trx/game/camera/common.h>
#include <trx/game/matrix.h>
#include <trx/game/output/const.h>
#include <trx/game/output/state.h>
#include <trx/game/output/utils.h>
#include <trx/game/output/vars.h>
#include <trx/gl/context.h>

#include <math.h>
#include <string.h>

typedef struct {
    int32_t num_z, x_min, x_max, y_min, y_max;
} M_BOX;

// The eight corners of the box through the matrix: the C as it was.
static void M_BoxC(
    const XYZ_32 vtx[8], const int32_t near_z, const int32_t far_z,
    M_BOX *const out)
{
    int32_t num_z = 0;
    int32_t x_min = INT32_MAX;
    int32_t y_min = INT32_MAX;
    int32_t x_max = INT32_MIN;
    int32_t y_max = INT32_MIN;

    for (int32_t i = 0; i < 8; i++) {
        // clang-format off
        const int32_t zv = (
            g_MatrixPtr->_20 * vtx[i].x +
            g_MatrixPtr->_21 * vtx[i].y +
            g_MatrixPtr->_22 * vtx[i].z +
            g_MatrixPtr->_23);
        // clang-format on

        if (zv <= near_z || zv >= far_z) {
            continue;
        }

        num_z++;
        const int32_t zp = zv / g_PhdPersp;
#if defined(__DREAMCAST__)
        // The SH-4 has no 64-bit division, which libgcc does in dozens of
        // steps; in floats it is one reciprocal, to within a pixel.
        const float inv_zp = 1.0f / (float)zp;
        // clang-format off
        const int32_t xv = (int32_t)(Output_MatrixToFloat(
            g_MatrixPtr->_00 * vtx[i].x +
            g_MatrixPtr->_01 * vtx[i].y +
            g_MatrixPtr->_02 * vtx[i].z +
            g_MatrixPtr->_03) * inv_zp);
        const int32_t yv = (int32_t)(Output_MatrixToFloat(
            g_MatrixPtr->_10 * vtx[i].x +
            g_MatrixPtr->_11 * vtx[i].y +
            g_MatrixPtr->_12 * vtx[i].z +
            g_MatrixPtr->_13) * inv_zp);
        // clang-format on
#else
        // clang-format off
        const int32_t xv = (
            g_MatrixPtr->_00 * vtx[i].x +
            g_MatrixPtr->_01 * vtx[i].y +
            g_MatrixPtr->_02 * vtx[i].z +
            g_MatrixPtr->_03) / zp;
        const int32_t yv = (
            g_MatrixPtr->_10 * vtx[i].x +
            g_MatrixPtr->_11 * vtx[i].y +
            g_MatrixPtr->_12 * vtx[i].z +
            g_MatrixPtr->_13) / zp;
        // clang-format on
#endif

        CLAMPG(x_min, xv);
        CLAMPL(x_max, xv);
        CLAMPG(y_min, yv);
        CLAMPL(y_max, yv);
    }

    out->num_z = num_z;
    out->x_min = x_min;
    out->x_max = x_max;
    out->y_min = y_min;
    out->y_max = y_max;
}

#if defined(__DREAMCAST__)
extern uint32_t g_DC_AsmOff;

// The same with the products as the SH-4 does them best: a matrix whose terms
// all fit 32 bits (they do, but for a far translation) takes a 32 by 32
// multiply into 64 in a single instruction where the 64-bit product of the
// C is a library call, nine a corner. The depth keeps only 32 bits, so a
// wrapping sum gives it; the sides keep the 64-bit sum for the float.
static bool M_BoxNarrow(void)
{
    const int64_t *const m = &g_MatrixPtr->_00;
    for (int32_t i = 0; i < 12; i++) {
        if (m[i] != (int64_t)(int32_t)m[i]) {
            return false;
        }
    }
    return true;
}

static void M_BoxNarrowRun(
    const XYZ_32 vtx[8], const int32_t near_z, const int32_t far_z,
    M_BOX *const out)
{
    const int32_t m00 = (int32_t)g_MatrixPtr->_00;
    const int32_t m01 = (int32_t)g_MatrixPtr->_01;
    const int32_t m02 = (int32_t)g_MatrixPtr->_02;
    const int32_t m03 = (int32_t)g_MatrixPtr->_03;
    const int32_t m10 = (int32_t)g_MatrixPtr->_10;
    const int32_t m11 = (int32_t)g_MatrixPtr->_11;
    const int32_t m12 = (int32_t)g_MatrixPtr->_12;
    const int32_t m13 = (int32_t)g_MatrixPtr->_13;
    const int32_t m20 = (int32_t)g_MatrixPtr->_20;
    const int32_t m21 = (int32_t)g_MatrixPtr->_21;
    const int32_t m22 = (int32_t)g_MatrixPtr->_22;
    const int32_t m23 = (int32_t)g_MatrixPtr->_23;
    int32_t num_z = 0;
    int32_t x_min = INT32_MAX;
    int32_t y_min = INT32_MAX;
    int32_t x_max = INT32_MIN;
    int32_t y_max = INT32_MIN;
    for (int32_t i = 0; i < 8; i++) {
        const int32_t x = vtx[i].x;
        const int32_t y = vtx[i].y;
        const int32_t z = vtx[i].z;
        const int32_t zv = (int32_t)((uint32_t)m20 * (uint32_t)x
                                     + (uint32_t)m21 * (uint32_t)y
                                     + (uint32_t)m22 * (uint32_t)z
                                     + (uint32_t)m23);
        if (zv <= near_z || zv >= far_z) {
            continue;
        }
        num_z++;
        const int32_t zp = zv / g_PhdPersp;
        const float inv_zp = 1.0f / (float)zp;
        const int64_t sx = (int64_t)m00 * x + (int64_t)m01 * y
            + (int64_t)m02 * z + m03;
        const int64_t sy = (int64_t)m10 * x + (int64_t)m11 * y
            + (int64_t)m12 * z + m13;
        const int32_t xv = (int32_t)(Output_MatrixToFloat(sx) * inv_zp);
        const int32_t yv = (int32_t)(Output_MatrixToFloat(sy) * inv_zp);
        CLAMPG(x_min, xv);
        CLAMPL(x_max, xv);
        CLAMPG(y_min, yv);
        CLAMPL(y_max, yv);
    }
    out->num_z = num_z;
    out->x_min = x_min;
    out->x_max = x_max;
    out->y_min = y_min;
    out->y_max = y_max;
}
#endif

CLIP Output_CheckBoundsClip(const BOUNDS_16 *const bounds)
{
    const int32_t near_z = Output_GetNearZ();
    const int32_t far_z = Output_GetFarZ();
    if (g_MatrixPtr->_23 >= far_z) {
        return CLIP_NOT_VISIBLE;
    }

    const XYZ_32 vtx[8] = {
        { .x = bounds->min.x, .y = bounds->min.y, .z = bounds->min.z },
        { .x = bounds->max.x, .y = bounds->min.y, .z = bounds->min.z },
        { .x = bounds->max.x, .y = bounds->max.y, .z = bounds->min.z },
        { .x = bounds->min.x, .y = bounds->max.y, .z = bounds->min.z },
        { .x = bounds->min.x, .y = bounds->min.y, .z = bounds->max.z },
        { .x = bounds->max.x, .y = bounds->min.y, .z = bounds->max.z },
        { .x = bounds->max.x, .y = bounds->max.y, .z = bounds->max.z },
        { .x = bounds->min.x, .y = bounds->max.y, .z = bounds->max.z },
    };

    M_BOX box;
#if defined(__DREAMCAST__)
    if ((g_DC_AsmOff & 33554432) == 0 && M_BoxNarrow()) {
        M_BoxNarrowRun(vtx, near_z, far_z, &box);
    #if defined(DC_EMIT_VERIFY)
        static int32_t s_runs = 0;
        static int32_t s_bad = 0;
        M_BOX ref;
        M_BoxC(vtx, near_z, far_z, &ref);
        s_runs++;
        if (memcmp(&ref, &box, sizeof(box)) != 0) {
            s_bad++;
        }
        if (s_runs % 20000 == 0) {
            LOG_INFO("[boxclip-verify] %d runs, %d bad", s_runs, s_bad);
        }
    #endif
    } else {
        M_BoxC(vtx, near_z, far_z, &box);
    }
#else
    M_BoxC(vtx, near_z, far_z, &box);
#endif
    const int32_t num_z = box.num_z;
    int32_t x_min = box.x_min;
    int32_t x_max = box.x_max;
    int32_t y_min = box.y_min;
    int32_t y_max = box.y_max;

    if (num_z == 0) {
        return CLIP_NOT_VISIBLE; // out of screen
    }

    const VIEWPORT_RECT vp = Viewport_GetRect(VIEWPORT_GAME);
    x_min += vp.w / 2;
    x_max += vp.w / 2;
    y_min += vp.h / 2;
    y_max += vp.h / 2;

    // The corners left out above are the ones that would have widened the
    // rectangle, so what remains is smaller than the box covers and cannot be
    // rejected on. It can sit off to one side while the box still crosses the
    // view.
    if (num_z < 8) {
        return CLIP_PARTIALLY_VISIBLE;
    }

    // clang-format off
    if (x_min > g_PhdRight
        || y_min > g_PhdBottom
        || x_max < g_PhdLeft
        || y_max < g_PhdTop) {
        return CLIP_NOT_VISIBLE; // outside the room's clip window
    }
    // clang-format on

    // clang-format off
    if (x_min < 0 ||
        y_min < 0 ||
        x_max >= vp.w ||
        y_max >= vp.h) {
        return CLIP_PARTIALLY_VISIBLE;
    }
    // clang-format on

    return CLIP_FULLY_VISIBLE;
}

void Output_MakeScreenshot(const char *const path)
{
    LOG_INFO("Taking screenshot");
    TRX_GL_Context_ScheduleScreenshot(path);
}

void Output_ApplyFOV(void)
{
    int32_t fov = Camera_GetInterpolatedFOV();
    const int32_t sw = Viewport_GetWidth(VIEWPORT_GAME);
    const int32_t sh = Viewport_GetHeight(VIEWPORT_GAME);
    const float aspect = sw / (float)sh;

    int32_t fov_width;
    switch (Viewport_GetFOVMode()) {
    case FOV_MODE_VERTICAL: {
        const float fov_rad_h = fov * M_PI / (180 * DEG_1);
        const float fov_rad_v = 2 * atan(aspect * tan(fov_rad_h / 2));
        fov = round((fov_rad_v / M_PI) * (180 * DEG_1));
        fov_width = sw;
        break;
    }
    case FOV_MODE_HORIZONTAL:
        fov_width = sw;
        break;
    case FOV_MODE_PC:
        fov_width = sw * ((4.0f / 3.0f) / aspect);
        break;
    case FOV_MODE_PS1:
        fov_width = sw * ((4.0f / 3.0f) / aspect) * 240 / 200;
        break;
    case FOV_MODE_PS1_FIT:
        fov_width =
            sw * ((4.0f / 3.0f) / MAX(aspect, 16.0f / 10.0f)) * 240 / 200;
        break;
    default:
        ASSERT_FAIL();
    }

    const int16_t c = Math_Cos(fov / 2);
    const int16_t s = Math_Sin(fov / 2);
    g_PhdPersp = fov_width / 2;
    if (s != 0) {
        g_PhdPersp *= c;
        g_PhdPersp /= s;
    }
}
