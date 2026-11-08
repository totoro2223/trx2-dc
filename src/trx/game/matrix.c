#include <trx/game/matrix.h>

#include <trx/config.h>
#include <trx/core/math.h>
#include <trx/core/utils.h>
#include <trx/game/const.h>
#include <trx/game/pathing/lot.h>

#include <trx/core/log.h>

#include <float.h>
#include <math.h>
#include <string.h>

#if defined(__DREAMCAST__)
    #include <dc/matrix.h>
#endif

#define MAX_MATRICES 40
#define MAX_NESTED_MATRICES 32
// How small cos(X) may be before Y and Z are read as one turn. Above the
// rounding a matrix carries from being held in fixed point.
#define M_ROT_LOCK_EPSILON 1e-3

static MATRIX m_MatrixStack[MAX_MATRICES] = {};
static MATRIX m_WMatrixStack[MAX_MATRICES] = {};
static int32_t m_IMRate = 0;
static int32_t m_IMFrac = 0;
static MATRIX *m_IMMatrixPtr = nullptr;
static MATRIX *m_WIMMatrixPtr = nullptr;
static MATRIX m_IMMatrixStack[MAX_NESTED_MATRICES] = {};
static MATRIX m_WIMMatrixStack[MAX_NESTED_MATRICES] = {};

MATRIX *g_MatrixPtr = &m_MatrixStack[0];
MATRIX *g_WMatrixPtr = &m_WMatrixStack[0];
bool g_MatrixWorldOnly = false;

XYZ_32 g_ViewPos = {};
MATRIX g_ViewMatrix = {};
MATRIX g_IDMatrix = {
    // clang-format off
    ._00 = 1 << W2V_SHIFT, ._01 = 0, ._02 = 0, ._03 = 0,
    ._10 = 0, ._11 = 1 << W2V_SHIFT, ._12 = 0, ._13 = 0,
    ._20 = 0, ._21 = 0, ._22 = 1 << W2V_SHIFT, ._23 = 0,
    // clang-format on
};

static inline void M_QuaternionNormalize(QUATERNION *q)
{
    const double n2 = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
    if (n2 > 0.0) {
        const double inv = 1.0 / sqrt(n2);
        q->x *= inv;
        q->y *= inv;
        q->z *= inv;
        q->w *= inv;
    } else {
        // fallback: identity
        q->x = q->y = q->z = 0.0;
        q->w = 1.0;
    }
}

// One inexpensive polar-decomposition iteration to orthonormalize R (3x3).
// R <- R * (3I - R^T R) / 2  (Newton step toward orthogonal)
// Works great if R is already close to rotation.
static void M_Double3x3Ortho(double r[3][3])
{
    double rt_r[3][3] = {};
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            for (int32_t k = 0; k < 3; k++) {
                rt_r[i][j] += r[k][i] * r[k][j];
            }
        }
    }

    double m[3][3];
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            m[i][j] = 3.0 * (i == j) - rt_r[i][j];
        }
    }

    double rn[3][3] = {};
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            for (int32_t k = 0; k < 3; k++) {
                rn[i][j] += r[i][k] * m[k][j];
            }
        }
    }

    // divide by 2
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            r[i][j] = 0.5 * rn[i][j];
        }
    }
}

// Extract 3x3 rotation (in doubles) from fixed-point MATRIX.
static void M_Double3x3FromMatrix(const MATRIX *const m, double e[3][3])
{
    const double s = (1 << W2V_SHIFT);
    e[0][0] = m->_00 / s;
    e[0][1] = m->_01 / s;
    e[0][2] = m->_02 / s;
    e[1][0] = m->_10 / s;
    e[1][1] = m->_11 / s;
    e[1][2] = m->_12 / s;
    e[2][0] = m->_20 / s;
    e[2][1] = m->_21 / s;
    e[2][2] = m->_22 / s;
}

// Remove uniform scale if present (estimate from row lengths).
// Use average row length to reduce noise.
static void M_Double3x3RemoveScale(double e[3][3])
{
    const double rl0 =
        sqrt(e[0][0] * e[0][0] + e[0][1] * e[0][1] + e[0][2] * e[0][2]);
    const double rl1 =
        sqrt(e[1][0] * e[1][0] + e[1][1] * e[1][1] + e[1][2] * e[1][2]);
    const double rl2 =
        sqrt(e[2][0] * e[2][0] + e[2][1] * e[2][1] + e[2][2] * e[2][2]);
    const double scale = (rl0 + rl1 + rl2) / 3.0;
    if (scale <= 0.0) {
        return;
    }
    const double inv = 1.0 / scale;
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            e[i][j] *= inv;
        }
    }
}

// Write 3x3 back to MATRIX as fixed-point, with rounding.
static void M_Double3x3ToMatrix(const double e[3][3], MATRIX *m)
{
    const double s = (double)(1 << W2V_SHIFT);
    m->_00 = (int32_t)llround(e[0][0] * s);
    m->_01 = (int32_t)llround(e[0][1] * s);
    m->_02 = (int32_t)llround(e[0][2] * s);
    m->_10 = (int32_t)llround(e[1][0] * s);
    m->_11 = (int32_t)llround(e[1][1] * s);
    m->_12 = (int32_t)llround(e[1][2] * s);
    m->_20 = (int32_t)llround(e[2][0] * s);
    m->_21 = (int32_t)llround(e[2][1] * s);
    m->_22 = (int32_t)llround(e[2][2] * s);
}

static void M_MatrixToQuaternion(const MATRIX *m, QUATERNION *q)
{
    double e[3][3];
    M_Double3x3FromMatrix(m, e);
    M_Double3x3RemoveScale(e);
    // Orthonormalize (fast, one Newton step is usually enough).
    M_Double3x3Ortho(e);

    const double tr = e[0][0] + e[1][1] + e[2][2];
    if (tr > 0.0) {
        const double s = sqrt(tr + 1.0) * 2.0; // 4*w
        q->w = 0.25 * s;
        q->x = (e[2][1] - e[1][2]) / s;
        q->y = (e[0][2] - e[2][0]) / s;
        q->z = (e[1][0] - e[0][1]) / s;
    } else {
        // Pick the biggest diagonal for numerical stability
        int32_t i = 0;
        if (e[1][1] > e[0][0]) {
            i = 1;
        }
        if (e[2][2] > e[i][i]) {
            i = 2;
        }
        const int32_t j = (i + 1) % 3;
        const int32_t k = (i + 2) % 3;
        const double s = sqrt(e[i][i] - e[j][j] - e[k][k] + 1.0) * 2.0;
        double qv[3];
        qv[i] = 0.25 * s;
        qv[j] = (e[j][i] + e[i][j]) / s;
        qv[k] = (e[k][i] + e[i][k]) / s;
        q->x = qv[0];
        q->y = qv[1];
        q->z = qv[2];
        q->w = (e[k][j] - e[j][k]) / s;
    }
    M_QuaternionNormalize(q);
}

static void M_MatrixFromQuaternion(const QUATERNION *const qin, MATRIX *const m)
{
    QUATERNION q = *qin;
    M_QuaternionNormalize(&q);

    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    double e[3][3];
    e[0][0] = 1.0 - 2.0 * (yy + zz);
    e[0][1] = 2.0 * (xy - wz);
    e[0][2] = 2.0 * (xz + wy);
    e[1][0] = 2.0 * (xy + wz);
    e[1][1] = 1.0 - 2.0 * (xx + zz);
    e[1][2] = 2.0 * (yz - wx);
    e[2][0] = 2.0 * (xz - wy);
    e[2][1] = 2.0 * (yz + wx);
    e[2][2] = 1.0 - 2.0 * (xx + yy);

    // Optional: one more orthonormalization step to crush rounding noise
    M_Double3x3Ortho(e);
    M_Double3x3ToMatrix(e, m);
}

static void M_QuaternionSlerp(
    const QUATERNION *const qa, const QUATERNION *const qb, const double t,
    QUATERNION *const out)
{
    QUATERNION a = *qa, b = *qb;
    M_QuaternionNormalize(&a);
    M_QuaternionNormalize(&b);

    double cosom = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cosom < 0.0) { // take shortest path
        cosom = -cosom;
        b.x = -b.x;
        b.y = -b.y;
        b.z = -b.z;
        b.w = -b.w;
    }

    // Guard acos input, and use nlerp for tiny angles
    CLAMP(cosom, -1.0, 1.0);

    // Threshold tuned for double precision
    const double EPS = 1e-12;
    double scale0;
    double scale1;
    if (1.0 - cosom > EPS) {
        const double omega = acos(cosom);
        const double sinom = 1.0 / sin(omega);
        scale0 = sin((1.0 - t) * omega) * sinom;
        scale1 = sin(t * omega) * sinom;
    } else {
        // Nearly parallel: nlerp, then normalize
        scale0 = 1.0 - t;
        scale1 = t;
    }

    out->x = scale0 * a.x + scale1 * b.x;
    out->y = scale0 * a.y + scale1 * b.y;
    out->z = scale0 * a.z + scale1 * b.z;
    out->w = scale0 * a.w + scale1 * b.w;
    M_QuaternionNormalize(out);
}

#if defined(__DREAMCAST__)
// SH4ZAM: sines and inverse roots on the SH-4's fsca and fsrra.
    #include <platform/dreamcast/dc_shz.h>
// The Dreamcast's FPU runs in single precision (-m4-single), so every double
// above is worked out in software: one blend of two matrices took ~60 us, and
// Lara alone blends two per mesh. The same steps in floats hold the 14-bit
// fixed point with room to spare.
typedef struct {
    float x, y, z, w;
} M_QUAT_F;

// A rotation entry this large carries a scale the floats below would not
// hold to the last unit; such a matrix takes the doubles.
    #define M_FLOAT_LIMIT (1 << 22)

// To the nearest integer: a conversion the SH-4 has (ftrc), where lrintf is
// a call into the library.
static inline int32_t M_RoundF(const float x)
{
    return (int32_t)(x + (x >= 0.0f ? 0.5f : -0.5f));
}

static inline void M_QuatFNormalize(M_QUAT_F *const q)
{
    const float n2 = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
    if (n2 > 0.0f) {
        const float inv = DC_Shz_InvSqrt(n2);
        q->x *= inv;
        q->y *= inv;
        q->z *= inv;
        q->w *= inv;
    } else {
        q->x = q->y = q->z = 0.0f;
        q->w = 1.0f;
    }
}

// As M_Double3x3Ortho.
static void M_Float3x3Ortho(float r[3][3])
{
    float rt_r[3][3] = {};
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            for (int32_t k = 0; k < 3; k++) {
                rt_r[i][j] += r[k][i] * r[k][j];
            }
        }
    }
    float rn[3][3] = {};
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            for (int32_t k = 0; k < 3; k++) {
                rn[i][j] += r[i][k] * ((k == j ? 3.0f : 0.0f) - rt_r[k][j]);
            }
        }
    }
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            r[i][j] = 0.5f * rn[i][j];
        }
    }
}

// As M_Double3x3FromMatrix and M_Double3x3RemoveScale. False for a matrix
// whose entries do not fit 32 bits.
static bool M_Float3x3FromMatrix(const MATRIX *const m, float e[3][3])
{
    const int64_t v[3][3] = {
        { m->_00, m->_01, m->_02 },
        { m->_10, m->_11, m->_12 },
        { m->_20, m->_21, m->_22 },
    };
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            if (v[i][j] >= M_FLOAT_LIMIT || v[i][j] <= -M_FLOAT_LIMIT) {
                return false;
            }
            e[i][j] = (float)(int32_t)v[i][j];
        }
    }
    float scale = 0.0f;
    for (int32_t i = 0; i < 3; i++) {
        scale += sqrtf(
            e[i][0] * e[i][0] + e[i][1] * e[i][1] + e[i][2] * e[i][2]);
    }
    if (scale <= 0.0f) {
        return false;
    }
    const float inv = 3.0f / scale;
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            e[i][j] *= inv;
        }
    }
    return true;
}

// As M_MatrixToQuaternion. TR's rotations are orthonormal but for the fixed
// point's rounding, which normalising the quaternion takes care of: only the
// blended result is made orthonormal again (M_InterpolateFloat).
static void M_Float3x3ToQuat(const float e[3][3], M_QUAT_F *const q)
{
    const float tr = e[0][0] + e[1][1] + e[2][2];
    if (tr > 0.0f) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        const float inv = 1.0f / s;
        q->w = 0.25f * s;
        q->x = (e[2][1] - e[1][2]) * inv;
        q->y = (e[0][2] - e[2][0]) * inv;
        q->z = (e[1][0] - e[0][1]) * inv;
    } else {
        int32_t i = 0;
        if (e[1][1] > e[0][0]) {
            i = 1;
        }
        if (e[2][2] > e[i][i]) {
            i = 2;
        }
        const int32_t j = (i + 1) % 3;
        const int32_t k = (i + 2) % 3;
        const float s = sqrtf(e[i][i] - e[j][j] - e[k][k] + 1.0f) * 2.0f;
        const float inv = 1.0f / s;
        float qv[3];
        qv[i] = 0.25f * s;
        qv[j] = (e[j][i] + e[i][j]) * inv;
        qv[k] = (e[k][i] + e[i][k]) * inv;
        q->x = qv[0];
        q->y = qv[1];
        q->z = qv[2];
        q->w = (e[k][j] - e[j][k]) * inv;
    }
    M_QuatFNormalize(q);
}

// As M_QuaternionSlerp. Two frames of an animation lie close together, where
// the straight blend, normalised, is the arc to within float rounding: the
// arc's sines are only worked out for a wider turn.
static void M_QuatFSlerp(
    const M_QUAT_F *const a, M_QUAT_F b, const float t, M_QUAT_F *const out)
{
    float cosom = a->x * b.x + a->y * b.y + a->z * b.z + a->w * b.w;
    if (cosom < 0.0f) {
        cosom = -cosom;
        b.x = -b.x;
        b.y = -b.y;
        b.z = -b.z;
        b.w = -b.w;
    }
    CLAMP(cosom, -1.0f, 1.0f);
    float scale0 = 1.0f - t;
    float scale1 = t;
    if (cosom < 0.999f) {
        const float omega = DC_Shz_Acos(cosom);
        const float sinom = 1.0f / DC_Shz_Sin(omega);
        scale0 = DC_Shz_Sin((1.0f - t) * omega) * sinom;
        scale1 = DC_Shz_Sin(t * omega) * sinom;
    }
    out->x = scale0 * a->x + scale1 * b.x;
    out->y = scale0 * a->y + scale1 * b.y;
    out->z = scale0 * a->z + scale1 * b.z;
    out->w = scale0 * a->w + scale1 * b.w;
    M_QuatFNormalize(out);
}

// One axis of the position, blended: the step between the two frames fits
// 32 bits unless the item jumped, which takes the doubles.
static bool M_LerpTranslation(
    const int64_t from, const int64_t to, const float t, int64_t *const out)
{
    const int64_t delta = to - from;
    if (delta >= INT32_MAX / 2 || delta <= -INT32_MAX / 2) {
        return false;
    }
    *out = from + M_RoundF((float)(int32_t)delta * t);
    return true;
}

// M_Interpolate in floats. False, with nothing written, for a matrix it
// cannot take.
[[maybe_unused]] static bool M_InterpolateFloat(
    const MATRIX *const m1, const MATRIX *const m2, MATRIX *const result)
{
    float t = (m_IMRate != 0) ? (float)m_IMFrac / (float)m_IMRate : 0.0f;
    CLAMP(t, 0.0f, 1.0f);

    float e1[3][3];
    float e2[3][3];
    int64_t pos[3];
    if (!M_Float3x3FromMatrix(m1, e1) || !M_Float3x3FromMatrix(m2, e2)
        || !M_LerpTranslation(m1->_03, m2->_03, t, &pos[0])
        || !M_LerpTranslation(m1->_13, m2->_13, t, &pos[1])
        || !M_LerpTranslation(m1->_23, m2->_23, t, &pos[2])) {
        return false;
    }

    M_QUAT_F q1;
    M_QUAT_F q2;
    M_QUAT_F q;
    M_Float3x3ToQuat(e1, &q1);
    M_Float3x3ToQuat(e2, &q2);
    M_QuatFSlerp(&q1, q2, t, &q);

    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    float e[3][3] = {
        { 1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy) },
        { 2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx) },
        { 2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy) },
    };
    M_Float3x3Ortho(e);

    const float s = (float)(1 << W2V_SHIFT);
    result->_00 = M_RoundF(e[0][0] * s);
    result->_01 = M_RoundF(e[0][1] * s);
    result->_02 = M_RoundF(e[0][2] * s);
    result->_10 = M_RoundF(e[1][0] * s);
    result->_11 = M_RoundF(e[1][1] * s);
    result->_12 = M_RoundF(e[1][2] * s);
    result->_20 = M_RoundF(e[2][0] * s);
    result->_21 = M_RoundF(e[2][1] * s);
    result->_22 = M_RoundF(e[2][2] * s);
    result->_03 = pos[0];
    result->_13 = pos[1];
    result->_23 = pos[2];
    return true;
}

// One entry of the blend the three original games use between two keyframes:
// a + (b - a) * frac / rate, with the plain average (TR2 and TR3 shift by one
// bit) when the keyframes lie two ticks apart. In 32 bits wherever the step
// allows it: the SH-4's 64-bit division is a call into the library.
static inline int64_t M_BlendLinear(
    const int64_t a, const int64_t b, const int32_t frac, const int32_t rate,
    const uint32_t inv_rate)
{
    if (rate == 2 && frac == 1) {
        return (a + b) >> 1;
    }
    const int64_t d = b - a;
    if (frac < 32 && d > -(1 << 26) && d < (1 << 26)) {
        // n / rate truncated toward zero, by the reciprocal (inv_rate is
        // 2^32 / rate rounded down, so the estimate falls short by at most
        // two) and not by the library's 32-bit division.
        const int32_t n = (int32_t)d * frac;
        const uint32_t u = n < 0 ? 0u - (uint32_t)n : (uint32_t)n;
        uint32_t q = (uint32_t)(((uint64_t)u * inv_rate) >> 32);
        uint32_t r = u - q * (uint32_t)rate;
        while (r >= (uint32_t)rate) {
            q++;
            r -= (uint32_t)rate;
        }
        return a + (n < 0 ? -(int64_t)q : (int64_t)q);
    }
    return a + (d * frac) / rate;
}

// The original games' pose blend (rendering.dc_linear_pose): every entry of
// the matrix goes the same fraction of the way from m1 to m2, with no
// quaternions and no trigonometry. The result stops being a pure rotation by
// a little between two keyframes far apart, as it did in the originals.
static void M_InterpolateLinearC(
    const MATRIX *m1, const MATRIX *m2, MATRIX *result, int32_t frac,
    int32_t rate, uint32_t inv_rate);
extern uint32_t g_DC_AsmOff;
extern int dc_blend12(
    MATRIX *out, const MATRIX *m1, const MATRIX *m2, const int32_t *p);

static void M_InterpolateLinear(
    const MATRIX *const m1, const MATRIX *const m2, MATRIX *const result)
{
    const int32_t rate = m_IMRate;
    if (rate <= 0) {
        if (result != m1) {
            *result = *m1;
        }
        return;
    }
    int32_t frac = m_IMFrac;
    CLAMP(frac, 0, rate);
    const uint32_t inv_rate = 0xFFFFFFFFu / (uint32_t)rate;

    // In assembly (game_sh4.S, dc_blend12) where the step is small; ASMOFF
    // bit 536870912 turns it off.
    if ((g_DC_AsmOff & 536870912) == 0
        && (frac < 32 || (rate == 2 && frac == 1))) {
        static const int32_t *p;
        static int32_t params[3];
        params[0] = frac;
        params[1] = rate;
        params[2] = (int32_t)inv_rate;
        p = params;
    #if defined(DC_EMIT_VERIFY)
        MATRIX ref;
        M_InterpolateLinearC(m1, m2, &ref, frac, rate, inv_rate);
        MATRIX out;
        if (dc_blend12(&out, m1, m2, p)) {
            static uint32_t runs;
            static uint32_t bad;
            if (memcmp(&ref, &out, sizeof(MATRIX)) != 0) {
                bad++;
                LOG_WARNING("[blend-verify] MISMATCH");
            }
            if (runs++ == 0 || runs % 2000 == 0) {
                LOG_WARNING(
                    "[blend-verify] %u runs, %u bad", (unsigned)runs,
                    (unsigned)bad);
            }
            *result = out;
            return;
        }
    #else
        if (dc_blend12(result, m1, m2, p)) {
            return;
        }
    #endif
    }
    M_InterpolateLinearC(m1, m2, result, frac, rate, inv_rate);
}

static void M_InterpolateLinearC(
    const MATRIX *const m1, const MATRIX *const m2, MATRIX *const result,
    const int32_t frac, const int32_t rate, const uint32_t inv_rate)
{
    result->_00 = M_BlendLinear(m1->_00, m2->_00, frac, rate, inv_rate);
    result->_01 = M_BlendLinear(m1->_01, m2->_01, frac, rate, inv_rate);
    result->_02 = M_BlendLinear(m1->_02, m2->_02, frac, rate, inv_rate);
    result->_03 = M_BlendLinear(m1->_03, m2->_03, frac, rate, inv_rate);
    result->_10 = M_BlendLinear(m1->_10, m2->_10, frac, rate, inv_rate);
    result->_11 = M_BlendLinear(m1->_11, m2->_11, frac, rate, inv_rate);
    result->_12 = M_BlendLinear(m1->_12, m2->_12, frac, rate, inv_rate);
    result->_13 = M_BlendLinear(m1->_13, m2->_13, frac, rate, inv_rate);
    result->_20 = M_BlendLinear(m1->_20, m2->_20, frac, rate, inv_rate);
    result->_21 = M_BlendLinear(m1->_21, m2->_21, frac, rate, inv_rate);
    result->_22 = M_BlendLinear(m1->_22, m2->_22, frac, rate, inv_rate);
    result->_23 = M_BlendLinear(m1->_23, m2->_23, frac, rate, inv_rate);
}
#endif

static void M_ScaleX(MATRIX *const m, const int32_t scale)
{
    m->_00 = ((int64_t)m->_00 * scale) >> W2V_SHIFT;
    m->_10 = ((int64_t)m->_10 * scale) >> W2V_SHIFT;
    m->_20 = ((int64_t)m->_20 * scale) >> W2V_SHIFT;
}

static void M_ScaleY(MATRIX *const m, const int32_t scale)
{
    m->_01 = ((int64_t)m->_01 * scale) >> W2V_SHIFT;
    m->_11 = ((int64_t)m->_11 * scale) >> W2V_SHIFT;
    m->_21 = ((int64_t)m->_21 * scale) >> W2V_SHIFT;
}

static void M_ScaleZ(MATRIX *const m, const int32_t angle)
{
    m->_02 = ((int64_t)m->_02 * angle) >> W2V_SHIFT;
    m->_12 = ((int64_t)m->_12 * angle) >> W2V_SHIFT;
    m->_22 = ((int64_t)m->_22 * angle) >> W2V_SHIFT;
}

// The low 32 bits of a product, as the rotations keep no more: 32-bit
// multiplies (mul.l) in place of the 64-bit ones the int64 entries invite.
#define M_MUL32(a_, b_) ((uint32_t)(int32_t)(a_) * (uint32_t)(int32_t)(b_))

static void M_RotXSC(MATRIX *m, int32_t sx, int32_t cx);
static void M_RotYSC(MATRIX *m, int32_t sy, int32_t cy);
static void M_RotZSC(MATRIX *m, int32_t sz, int32_t cz);
static void M_TranslateRel(MATRIX *m, XYZ_32 offset);

static void M_RotXSC_C(MATRIX *const m, const int32_t sx, const int32_t cx)
{

    int32_t r0, r1;
    r0 = (int32_t)(M_MUL32(m->_01, cx) + M_MUL32(m->_02, sx));
    r1 = (int32_t)(M_MUL32(m->_02, cx) - M_MUL32(m->_01, sx));
    m->_01 = r0 >> W2V_SHIFT;
    m->_02 = r1 >> W2V_SHIFT;

    r0 = (int32_t)(M_MUL32(m->_11, cx) + M_MUL32(m->_12, sx));
    r1 = (int32_t)(M_MUL32(m->_12, cx) - M_MUL32(m->_11, sx));
    m->_11 = r0 >> W2V_SHIFT;
    m->_12 = r1 >> W2V_SHIFT;

    r0 = (int32_t)(M_MUL32(m->_21, cx) + M_MUL32(m->_22, sx));
    r1 = (int32_t)(M_MUL32(m->_22, cx) - M_MUL32(m->_21, sx));
    m->_21 = r0 >> W2V_SHIFT;
    m->_22 = r1 >> W2V_SHIFT;
}

static void M_RotX(MATRIX *const m, const int16_t angle)
{
    if (angle != 0) {
        M_RotXSC(m, Math_Sin(angle), Math_Cos(angle));
    }
}

static void M_RotYSC_C(MATRIX *const m, const int32_t sy, const int32_t cy)
{

    int32_t r0, r1;
    r0 = (int32_t)(M_MUL32(m->_00, cy) - M_MUL32(m->_02, sy));
    r1 = (int32_t)(M_MUL32(m->_02, cy) + M_MUL32(m->_00, sy));
    m->_00 = r0 >> W2V_SHIFT;
    m->_02 = r1 >> W2V_SHIFT;

    r0 = (int32_t)(M_MUL32(m->_10, cy) - M_MUL32(m->_12, sy));
    r1 = (int32_t)(M_MUL32(m->_12, cy) + M_MUL32(m->_10, sy));
    m->_10 = r0 >> W2V_SHIFT;
    m->_12 = r1 >> W2V_SHIFT;

    r0 = (int32_t)(M_MUL32(m->_20, cy) - M_MUL32(m->_22, sy));
    r1 = (int32_t)(M_MUL32(m->_22, cy) + M_MUL32(m->_20, sy));
    m->_20 = r0 >> W2V_SHIFT;
    m->_22 = r1 >> W2V_SHIFT;
}

static void M_RotY(MATRIX *const m, const int16_t angle)
{
    if (angle != 0) {
        M_RotYSC(m, Math_Sin(angle), Math_Cos(angle));
    }
}

static void M_RotZSC_C(MATRIX *const m, const int32_t sz, const int32_t cz)
{

    int32_t r0, r1;
    r0 = (int32_t)(M_MUL32(m->_00, cz) + M_MUL32(m->_01, sz));
    r1 = (int32_t)(M_MUL32(m->_01, cz) - M_MUL32(m->_00, sz));
    m->_00 = r0 >> W2V_SHIFT;
    m->_01 = r1 >> W2V_SHIFT;

    r0 = (int32_t)(M_MUL32(m->_10, cz) + M_MUL32(m->_11, sz));
    r1 = (int32_t)(M_MUL32(m->_11, cz) - M_MUL32(m->_10, sz));
    m->_10 = r0 >> W2V_SHIFT;
    m->_11 = r1 >> W2V_SHIFT;

    r0 = (int32_t)(M_MUL32(m->_20, cz) + M_MUL32(m->_21, sz));
    r1 = (int32_t)(M_MUL32(m->_21, cz) - M_MUL32(m->_20, sz));
    m->_20 = r0 >> W2V_SHIFT;
    m->_21 = r1 >> W2V_SHIFT;
}

static void M_RotZ(MATRIX *const m, const int16_t angle)
{
    if (angle != 0) {
        M_RotZSC(m, Math_Sin(angle), Math_Cos(angle));
    }
}

#if defined(__DREAMCAST__)
// In assembly (game_sh4.S): the three turns and the translation of a matrix.
// TRX_DC_ASMOFF bit 67108864 turns them off; a DC_EMIT_VERIFY build runs the C
// on a copy and compares the twelve entries.
extern uint32_t g_DC_AsmOff;
extern void dc_rot_x(MATRIX *m, int32_t s, int32_t c);
extern void dc_rot_y(MATRIX *m, int32_t s, int32_t c);
extern void dc_rot_z(MATRIX *m, int32_t s, int32_t c);
extern int dc_mul3x3(MATRIX *out, const MATRIX *lhs, const MATRIX *rhs);
extern void dc_translate_rel(MATRIX *m, int32_t x, int32_t y, int32_t z);

#if defined(DC_EMIT_VERIFY)
static uint32_t m_MxRuns;
static uint32_t m_MxBad;
static void M_MxCheck(
    const MATRIX *const a, const MATRIX *const b, const MATRIX *const in,
    const int32_t s, const int32_t c)
{
    if (memcmp(a, b, sizeof(MATRIX)) != 0) {
        if (m_MxBad++ < 6) {
            const int64_t *const pi = &in->_00;
            const int64_t *const pa = &a->_00;
            const int64_t *const pb = &b->_00;
            for (int32_t k = 0; k < 12; k++) {
                if (pa[k] != pb[k]) {
                    LOG_WARNING(
                        "[matrix-verify] s %d c %d e%d in %lld c %lld asm %lld",
                        (int)s, (int)c, (int)k, (long long)pi[k],
                        (long long)pa[k], (long long)pb[k]);
                }
            }
        }
        LOG_WARNING("[matrix-verify] MISMATCH");
    }
    if (++m_MxRuns % 100000 == 0) {
        LOG_WARNING(
            "[matrix-verify] %u runs, %u bad", (unsigned)m_MxRuns,
            (unsigned)m_MxBad);
    }
}
#endif

typedef void (*M_ROT_FN)(MATRIX *, int32_t, int32_t);

static inline void M_RotRun(
    MATRIX *const m, const int32_t s, const int32_t c, const M_ROT_FN c_fn,
    const M_ROT_FN asm_fn)
{
    if ((g_DC_AsmOff & 67108864) != 0) {
        c_fn(m, s, c);
        return;
    }
#if defined(DC_EMIT_VERIFY)
    const MATRIX in = *m;
    MATRIX copy = *m;
    c_fn(&copy, s, c);
    asm_fn(m, s, c);
    M_MxCheck(&copy, m, &in, s, c);
#else
    asm_fn(m, s, c);
#endif
}

static void M_RotXSC(MATRIX *const m, const int32_t s, const int32_t c)
{
    M_RotRun(m, s, c, M_RotXSC_C, dc_rot_x);
}
static void M_RotYSC(MATRIX *const m, const int32_t s, const int32_t c)
{
    M_RotRun(m, s, c, M_RotYSC_C, dc_rot_y);
}
static void M_RotZSC(MATRIX *const m, const int32_t s, const int32_t c)
{
    M_RotRun(m, s, c, M_RotZSC_C, dc_rot_z);
}
#else
static void M_RotXSC(MATRIX *const m, const int32_t s, const int32_t c)
{
    M_RotXSC_C(m, s, c);
}
static void M_RotYSC(MATRIX *const m, const int32_t s, const int32_t c)
{
    M_RotYSC_C(m, s, c);
}
static void M_RotZSC(MATRIX *const m, const int32_t s, const int32_t c)
{
    M_RotZSC_C(m, s, c);
}
#endif

// Swaps a rotation for the one that undoes it. A rotation matrix's inverse is
// its transpose, and these carry no scale.
static void M_Transpose3x3(MATRIX *const m)
{
    SWAP(m->_01, m->_10);
    SWAP(m->_02, m->_20);
    SWAP(m->_12, m->_21);
}

static void M_RotYXZ(MATRIX *const m, const XYZ_16 rotation)
{
    M_RotY(m, rotation.y);
    M_RotX(m, rotation.x);
    M_RotZ(m, rotation.z);
}

// Sine and cosine are looked up once for all the matrices a call turns (the
// view and world stacks, and their interpolation twins).
#if defined(__DREAMCAST__)
// DOUBTFUL CHANGE (see PROBADO.md): the Y, X and Z turns of a rotation worked
// out as one float matrix and applied to each row of the matrix with ftrv,
// instead of three fixed point turns that each round down. Entries can differ
// by 1 in the last bit from M_RotYSC and the others; a bone is only ever
// turned this way while g_MatrixWorldOnly (the enemies' walk) is set.
static void M_RotYXZ_Float(
    MATRIX *const *const ms, const int32_t n, const XYZ_16 rotation)
{
    const float k = 1.0f / (float)(1 << W2V_SHIFT);
    // r[j][i]: the row vector v turned is sum_j v[j] * r[j][i].
    // mat_load reads it with 64-bit moves: aligned, as the stack gives no more
    // than 4 bytes to a float array.
    float r[4][4] __attribute__((aligned(32))) = {
        { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }
    };
    // Each turn is folded in by the columns it changes, in the order Y, X, Z.
    if (rotation.y != 0) {
        const float s = (float)Math_Sin(rotation.y) * k;
        const float c = (float)Math_Cos(rotation.y) * k;
        for (int32_t j = 0; j < 3; j++) {
            const float a = r[j][0];
            const float b = r[j][2];
            r[j][0] = a * c - b * s;
            r[j][2] = b * c + a * s;
        }
    }
    if (rotation.x != 0) {
        const float s = (float)Math_Sin(rotation.x) * k;
        const float c = (float)Math_Cos(rotation.x) * k;
        for (int32_t j = 0; j < 3; j++) {
            const float a = r[j][1];
            const float b = r[j][2];
            r[j][1] = a * c + b * s;
            r[j][2] = b * c - a * s;
        }
    }
    if (rotation.z != 0) {
        const float s = (float)Math_Sin(rotation.z) * k;
        const float c = (float)Math_Cos(rotation.z) * k;
        for (int32_t j = 0; j < 3; j++) {
            const float a = r[j][0];
            const float b = r[j][1];
            r[j][0] = a * c + b * s;
            r[j][1] = b * c - a * s;
        }
    }
    mat_load(&r);
    for (int32_t i = 0; i < n; i++) {
        MATRIX *const m = ms[i];
        // Rows of the rotation part; ftrc truncates, so the bias makes it the
        // floor the shifts of the fixed point turns were.
        float x = (float)(int32_t)m->_00;
        float y = (float)(int32_t)m->_01;
        float z = (float)(int32_t)m->_02;
        float w = 0.0f;
        mat_trans_nodiv(x, y, z, w);
        m->_00 = (int32_t)(x + 32768.0f) - 32768;
        m->_01 = (int32_t)(y + 32768.0f) - 32768;
        m->_02 = (int32_t)(z + 32768.0f) - 32768;
        x = (float)(int32_t)m->_10;
        y = (float)(int32_t)m->_11;
        z = (float)(int32_t)m->_12;
        w = 0.0f;
        mat_trans_nodiv(x, y, z, w);
        m->_10 = (int32_t)(x + 32768.0f) - 32768;
        m->_11 = (int32_t)(y + 32768.0f) - 32768;
        m->_12 = (int32_t)(z + 32768.0f) - 32768;
        x = (float)(int32_t)m->_20;
        y = (float)(int32_t)m->_21;
        z = (float)(int32_t)m->_22;
        w = 0.0f;
        mat_trans_nodiv(x, y, z, w);
        m->_20 = (int32_t)(x + 32768.0f) - 32768;
        m->_21 = (int32_t)(y + 32768.0f) - 32768;
        m->_22 = (int32_t)(z + 32768.0f) - 32768;
    }
}
#endif

static void M_RotYXZ_N(
    MATRIX *const *const ms, const int32_t n, const XYZ_16 rotation)
{
#if defined(__DREAMCAST__)
    if (g_MatrixWorldOnly) {
        M_RotYXZ_Float(ms, n, rotation);
        return;
    }
#endif
    if (rotation.y != 0) {
        const int32_t s = Math_Sin(rotation.y);
        const int32_t c = Math_Cos(rotation.y);
        for (int32_t i = 0; i < n; i++) {
            M_RotYSC(ms[i], s, c);
        }
    }
    if (rotation.x != 0) {
        const int32_t s = Math_Sin(rotation.x);
        const int32_t c = Math_Cos(rotation.x);
        for (int32_t i = 0; i < n; i++) {
            M_RotXSC(ms[i], s, c);
        }
    }
    if (rotation.z != 0) {
        const int32_t s = Math_Sin(rotation.z);
        const int32_t c = Math_Cos(rotation.z);
        for (int32_t i = 0; i < n; i++) {
            M_RotZSC(ms[i], s, c);
        }
    }
}

// Reads a rotation back out of a matrix in the Y, X, Z order M_RotYXZ puts it
// in. Both remaining pairs are scaled by cos(X), so at a quarter turn on X
// they say nothing apart: Y and Z describe a single turn between them there,
// and it reads out as Y alone.
static XYZ_16 M_ToRot16(const MATRIX *const m)
{
    double e[3][3];
    M_Double3x3FromMatrix(m, e);
    M_Double3x3RemoveScale(e);

    double sin_x = -e[1][2];
    CLAMP(sin_x, -1.0, 1.0);
    // Taken from the pair itself rather than from sin(X), which a matrix
    // rounded to fixed point leaves just short of one; the square root of that
    // shortfall then reads as a cos(X) far larger than the pair holds.
    const double cos_x = sqrt(e[0][2] * e[0][2] + e[2][2] * e[2][2]);
    double x = asin(sin_x);
    double y;
    double z;
    if (cos_x > M_ROT_LOCK_EPSILON) {
        y = atan2(e[0][2], e[2][2]);
        z = atan2(e[1][0], e[1][1]);
    } else {
        x = sin_x < 0.0 ? -M_PI / 2.0 : M_PI / 2.0;
        y = atan2(-e[2][0], e[0][0]);
        z = 0.0;
    }

    const double scale = DEG_360 / (2.0 * M_PI);
    return (XYZ_16) {
        .x = (int16_t)(int32_t)lround(x * scale),
        .y = (int16_t)(int32_t)lround(y * scale),
        .z = (int16_t)(int32_t)lround(z * scale),
    };
}

// A rotation entry is at most 1 << W2V_SHIFT, so each product is one 32 x 32
// -> 64 multiply (dmuls.l on the SH-4), not three 32-bit ones and their carries.
#define M_MUL64(a_, b_) ((int64_t)(int32_t)(a_) * (int32_t)(b_))

static void M_TranslateRel_C(MATRIX *const m, const XYZ_32 offset)
{
    m->_03 += M_MUL64(offset.x, m->_00) + M_MUL64(offset.y, m->_01)
        + M_MUL64(offset.z, m->_02);
    m->_13 += M_MUL64(offset.x, m->_10) + M_MUL64(offset.y, m->_11)
        + M_MUL64(offset.z, m->_12);
    m->_23 += M_MUL64(offset.x, m->_20) + M_MUL64(offset.y, m->_21)
        + M_MUL64(offset.z, m->_22);
}

static void M_TranslateRel(MATRIX *const m, const XYZ_32 offset)
{
#if defined(__DREAMCAST__)
    if ((g_DC_AsmOff & 67108864) == 0) {
    #if defined(DC_EMIT_VERIFY)
        MATRIX copy = *m;
        M_TranslateRel_C(&copy, offset);
        dc_translate_rel(m, offset.x, offset.y, offset.z);
        if (memcmp(&copy, m, sizeof(MATRIX)) != 0) {
            m_MxBad++;
            LOG_WARNING("[matrix-verify] MISMATCH translate");
        }
    #else
        dc_translate_rel(m, offset.x, offset.y, offset.z);
    #endif
        return;
    }
#endif
    M_TranslateRel_C(m, offset);
}

static void M_TranslateSet(MATRIX *const m, const XYZ_32 pos)
{
    const int64_t scale = (int64_t)(1 << W2V_SHIFT);
    m->_03 = (int64_t)pos.x * scale;
    m->_13 = (int64_t)pos.y * scale;
    m->_23 = (int64_t)pos.z * scale;
}

static void M_InterpolateArm(MATRIX *const m, const MATRIX *const mi)
{
    m->_00 = m[-2]._00;
    m->_01 = m[-2]._01;
    m->_02 = m[-2]._02;
    m->_03 += ((mi->_03 - m->_03) * m_IMFrac) / m_IMRate;
    m->_10 = m[-2]._10;
    m->_11 = m[-2]._11;
    m->_12 = m[-2]._12;
    m->_13 += ((mi->_13 - m->_13) * m_IMFrac) / m_IMRate;
    m->_20 = m[-2]._20;
    m->_21 = m[-2]._21;
    m->_22 = m[-2]._22;
    m->_23 += ((mi->_23 - m->_23) * m_IMFrac) / m_IMRate;
}

static void M_Interpolate(
    const MATRIX *const m1, const MATRIX *const m2, MATRIX *const result)
{
#if defined(__DREAMCAST__)
    // The original simulation also blends poses as the originals did.
    if (g_Config.rendering.dc_linear_pose || LOT_IsOriginalSim()) {
        M_InterpolateLinear(m1, m2, result);
        return;
    }
    if (M_InterpolateFloat(m1, m2, result)) {
        return;
    }
#endif
    double rate = (m_IMRate != 0) ? ((double)m_IMFrac / (double)m_IMRate) : 0.0;
    CLAMP(rate, 0.0, 1.0);

    QUATERNION q1, q2, q;
    M_MatrixToQuaternion(m1, &q1);
    M_MatrixToQuaternion(m2, &q2);
    M_QuaternionSlerp(&q1, &q2, rate, &q);
    M_MatrixFromQuaternion(&q, result);

    result->_03 = (int32_t)llround(m1->_03 + (m2->_03 - m1->_03) * rate);
    result->_13 = (int32_t)llround(m1->_13 + (m2->_13 - m1->_13) * rate);
    result->_23 = (int32_t)llround(m1->_23 + (m2->_23 - m1->_23) * rate);
}

static void M_Mul3x3_C(
    MATRIX *const out, const MATRIX *const lhs, const MATRIX *const rhs)
{
#define L_MUL(r_, c_)                                                          \
    (((lhs->_##r_##0 * rhs->_0##c_) + (lhs->_##r_##1 * rhs->_1##c_)            \
      + (lhs->_##r_##2 * rhs->_2##c_))                                         \
     >> W2V_SHIFT)

    const int64_t r00 = L_MUL(0, 0);
    const int64_t r01 = L_MUL(0, 1);
    const int64_t r02 = L_MUL(0, 2);
    const int64_t r10 = L_MUL(1, 0);
    const int64_t r11 = L_MUL(1, 1);
    const int64_t r12 = L_MUL(1, 2);
    const int64_t r20 = L_MUL(2, 0);
    const int64_t r21 = L_MUL(2, 1);
    const int64_t r22 = L_MUL(2, 2);

    out->_00 = r00;
    out->_01 = r01;
    out->_02 = r02;
    out->_10 = r10;
    out->_11 = r11;
    out->_12 = r12;
    out->_20 = r20;
    out->_21 = r21;
    out->_22 = r22;

#undef L_MUL
}

// In assembly (game_sh4.S, dc_mul3x3) when every entry is int32, else the C;
// ASMOFF bit 268435456 turns it off.
void Matrix_Mul3x3_M(
    MATRIX *const out, const MATRIX *const lhs, const MATRIX *const rhs)
{
#if defined(__DREAMCAST__)
    if ((g_DC_AsmOff & 268435456) == 0) {
    #if defined(DC_EMIT_VERIFY)
        MATRIX ref = *out;
        M_Mul3x3_C(&ref, lhs, rhs);
        if (dc_mul3x3(out, lhs, rhs)) {
            static uint32_t m_M3Runs;
            M_MxCheck(&ref, out, out, 0, 0);
            if (++m_M3Runs % 1000 == 0) {
                LOG_WARNING(
                    "[mul3x3-verify] %u asm runs", (unsigned)m_M3Runs);
            }
        } else {
            *out = ref;
        }
        return;
    #else
        if (dc_mul3x3(out, lhs, rhs)) {
            return;
        }
    #endif
    }
#endif
    M_Mul3x3_C(out, lhs, rhs);
}

void Matrix_ResetStack(void)
{
    g_MatrixPtr = &m_MatrixStack[0];
    g_WMatrixPtr = &m_WMatrixStack[0];
}

void Matrix_GenerateW2V(const XYZ_32 *pos, const XYZ_16 *rot)
{
    g_ViewPos = *pos;

    // The view takes the world into the camera's own axes, which is the
    // rotation the camera stands at, undone.
    g_ViewMatrix = g_IDMatrix;
    M_RotYXZ(&g_ViewMatrix, *rot);
    M_Transpose3x3(&g_ViewMatrix);
    M_TranslateRel(&g_ViewMatrix, (XYZ_32) { -pos->x, -pos->y, -pos->z });

    g_MatrixPtr = &m_MatrixStack[0];
    m_MatrixStack[0] = g_ViewMatrix;

    g_WMatrixPtr = &m_WMatrixStack[0];
    g_WMatrixPtr[0] = g_IDMatrix;
}

void Matrix_ScaleW2V(const XYZ_32 scale)
{
    // scale the columns (world axes), matching the original engine's
    // ScaleCurrentMatrix, then rebuild the translation through the scaled
    // rotation so the warp stays centered on the camera
    g_ViewMatrix._00 = (g_ViewMatrix._00 * scale.x) >> W2V_SHIFT;
    g_ViewMatrix._10 = (g_ViewMatrix._10 * scale.x) >> W2V_SHIFT;
    g_ViewMatrix._20 = (g_ViewMatrix._20 * scale.x) >> W2V_SHIFT;
    g_ViewMatrix._01 = (g_ViewMatrix._01 * scale.y) >> W2V_SHIFT;
    g_ViewMatrix._11 = (g_ViewMatrix._11 * scale.y) >> W2V_SHIFT;
    g_ViewMatrix._21 = (g_ViewMatrix._21 * scale.y) >> W2V_SHIFT;
    g_ViewMatrix._02 = (g_ViewMatrix._02 * scale.z) >> W2V_SHIFT;
    g_ViewMatrix._12 = (g_ViewMatrix._12 * scale.z) >> W2V_SHIFT;
    g_ViewMatrix._22 = (g_ViewMatrix._22 * scale.z) >> W2V_SHIFT;
    g_ViewMatrix._03 = 0;
    g_ViewMatrix._13 = 0;
    g_ViewMatrix._23 = 0;
    M_TranslateRel(
        &g_ViewMatrix, (XYZ_32) { -g_ViewPos.x, -g_ViewPos.y, -g_ViewPos.z });
    m_MatrixStack[0] = g_ViewMatrix;
}

void Matrix_SquashW2V(void)
{
    g_ViewMatrix._10 -= g_ViewMatrix._10 >> 2;
    g_ViewMatrix._11 -= g_ViewMatrix._11 >> 2;
    g_ViewMatrix._12 -= g_ViewMatrix._12 >> 2;
    g_ViewMatrix._03 = 0;
    g_ViewMatrix._13 = 0;
    g_ViewMatrix._23 = 0;
    M_TranslateRel(
        &g_ViewMatrix, (XYZ_32) { -g_ViewPos.x, -g_ViewPos.y, -g_ViewPos.z });
    m_MatrixStack[0] = g_ViewMatrix;
}

#if defined(__DREAMCAST__)
// In assembly (game_sh4.S, dc_mat_copy); ASMOFF bit 134217728 turns it off.
extern void dc_mat_copy(MATRIX *dst, const MATRIX *src);
static void M_MatCopy(MATRIX *const dst, const MATRIX *const src)
{
    if ((g_DC_AsmOff & 134217728) != 0) {
        *dst = *src;
        return;
    }
    dc_mat_copy(dst, src);
    #if defined(DC_EMIT_VERIFY)
    if (memcmp(dst, src, sizeof(MATRIX)) != 0) {
        LOG_WARNING("[matrix-verify] MISMATCH copy");
    }
    #endif
}
#else
static void M_MatCopy(MATRIX *const dst, const MATRIX *const src)
{
    *dst = *src;
}
#endif

bool Matrix_Push(void)
{
    if (g_MatrixPtr + 1 - m_MatrixStack >= MAX_MATRICES) {
        return false;
    }
    if (g_WMatrixPtr + 1 - m_WMatrixStack >= MAX_MATRICES) {
        return false;
    }
    g_MatrixPtr++;
    if (!g_MatrixWorldOnly) {
        M_MatCopy(&g_MatrixPtr[0], &g_MatrixPtr[-1]);
    }
    g_WMatrixPtr++;
    M_MatCopy(&g_WMatrixPtr[0], &g_WMatrixPtr[-1]);
    return true;
}

bool Matrix_PushUnit(void)
{
    if (g_MatrixPtr + 1 - m_MatrixStack >= MAX_MATRICES) {
        return false;
    }
    if (g_WMatrixPtr + 1 - m_WMatrixStack >= MAX_MATRICES) {
        return false;
    }
    g_MatrixPtr++;
    *g_MatrixPtr = g_IDMatrix;
    g_WMatrixPtr++;
    *g_WMatrixPtr = g_IDMatrix;
    return true;
}

void Matrix_Pop(void)
{
    g_MatrixPtr--;
    g_WMatrixPtr--;
}

void Matrix_Scale(const int32_t scale)
{
    Matrix_ScaleX(scale);
    Matrix_ScaleY(scale);
    Matrix_ScaleZ(scale);
}

void Matrix_Scale_I(const int32_t scale)
{
    Matrix_Scale(scale);
    M_ScaleX(m_IMMatrixPtr, scale);
    M_ScaleY(m_IMMatrixPtr, scale);
    M_ScaleZ(m_IMMatrixPtr, scale);
    M_ScaleX(m_WIMMatrixPtr, scale);
    M_ScaleY(m_WIMMatrixPtr, scale);
    M_ScaleZ(m_WIMMatrixPtr, scale);
}

void Matrix_ScaleXYZ(const XYZ_32 scale)
{
    Matrix_ScaleX(scale.x);
    Matrix_ScaleY(scale.y);
    Matrix_ScaleZ(scale.z);
}

void Matrix_ScaleX(const int32_t scale)
{
    M_ScaleX(g_MatrixPtr, scale);
    M_ScaleX(g_WMatrixPtr, scale);
}

void Matrix_ScaleY(const int32_t scale)
{
    M_ScaleY(g_MatrixPtr, scale);
    M_ScaleY(g_WMatrixPtr, scale);
}

void Matrix_ScaleZ(const int32_t scale)
{
    M_ScaleZ(g_MatrixPtr, scale);
    M_ScaleZ(g_WMatrixPtr, scale);
}

void Matrix_RotX(const int16_t angle)
{
    if (angle != 0) {
        const int32_t s = Math_Sin(angle);
        const int32_t c = Math_Cos(angle);
        if (!g_MatrixWorldOnly) {
            M_RotXSC(g_MatrixPtr, s, c);
        }
        M_RotXSC(g_WMatrixPtr, s, c);
    }
}

void Matrix_RotY(const int16_t angle)
{
    if (angle != 0) {
        const int32_t s = Math_Sin(angle);
        const int32_t c = Math_Cos(angle);
        if (!g_MatrixWorldOnly) {
            M_RotYSC(g_MatrixPtr, s, c);
        }
        M_RotYSC(g_WMatrixPtr, s, c);
    }
}

void Matrix_RotZ(const int16_t angle)
{
    if (angle != 0) {
        const int32_t s = Math_Sin(angle);
        const int32_t c = Math_Cos(angle);
        if (!g_MatrixWorldOnly) {
            M_RotZSC(g_MatrixPtr, s, c);
        }
        M_RotZSC(g_WMatrixPtr, s, c);
    }
}

void Matrix_Rot16(const XYZ_16 rotation)
{
    MATRIX *const ms[] = { g_WMatrixPtr, g_MatrixPtr };
    M_RotYXZ_N(ms, g_MatrixWorldOnly ? 1 : 2, rotation);
}

void Matrix_RotX_M(MATRIX *const m, const int16_t angle)
{
    M_RotX(m, angle);
}

void Matrix_RotY_M(MATRIX *const m, const int16_t angle)
{
    M_RotY(m, angle);
}

void Matrix_RotZ_M(MATRIX *const m, const int16_t angle)
{
    M_RotZ(m, angle);
}

XYZ_16 Matrix_SlerpRot16(
    const XYZ_16 rotation_1, const XYZ_16 rotation_2, const double t)
{
    MATRIX m1 = g_IDMatrix;
    M_RotYXZ(&m1, rotation_1);
    MATRIX m2 = g_IDMatrix;
    M_RotYXZ(&m2, rotation_2);
    Matrix_Slerp3x3_M(&m1, &m2, t);
    return M_ToRot16(&m1);
}

void Matrix_Slerp3x3_M(
    MATRIX *const lhs_out, const MATRIX *const rhs, const double t)
{
    QUATERNION q1, q2, q;
    M_MatrixToQuaternion(lhs_out, &q1);
    M_MatrixToQuaternion(rhs, &q2);

    double clamped_t = t;
    CLAMP(clamped_t, 0.0, 1.0);
    M_QuaternionSlerp(&q1, &q2, clamped_t, &q);
    M_MatrixFromQuaternion(&q, lhs_out);
}

void Matrix_Mul3x3(const MATRIX *const rhs)
{
    Matrix_Mul3x3_M(g_MatrixPtr, g_MatrixPtr, rhs);
    Matrix_Mul3x3_M(g_WMatrixPtr, g_WMatrixPtr, rhs);
}

void Matrix_TranslateRel(const int32_t dx, const int32_t dy, const int32_t dz)
{
    Matrix_TranslateRel32((XYZ_32) { dx, dy, dz });
}

void Matrix_TranslateRel16(const XYZ_16 offset)
{
    Matrix_TranslateRel32(XYZ_32_From16(offset));
}

void Matrix_TranslateRel32(const XYZ_32 offset)
{
    if (!g_MatrixWorldOnly) {
        M_TranslateRel(g_MatrixPtr, offset);
    }
    M_TranslateRel(g_WMatrixPtr, offset);
}

void Matrix_TranslateAbs(const int32_t x, const int32_t y, const int32_t z)
{
    MATRIX *const m = g_MatrixPtr;
    const MATRIX *const v = &g_ViewMatrix;
    m->_03 = x * v->_00 + y * v->_01 + z * v->_02 + v->_03;
    m->_13 = x * v->_10 + y * v->_11 + z * v->_12 + v->_13;
    m->_23 = x * v->_20 + y * v->_21 + z * v->_22 + v->_23;
    M_TranslateSet(g_WMatrixPtr, (XYZ_32) { x, y, z });
}

void Matrix_TranslateAbs16(const XYZ_16 offset)
{
    Matrix_TranslateAbs(offset.x, offset.y, offset.z);
}

void Matrix_TranslateAbs32(const XYZ_32 offset)
{
    Matrix_TranslateAbs(offset.x, offset.y, offset.z);
}

void Matrix_TranslateSet32(const XYZ_32 origin)
{
    M_TranslateSet(g_MatrixPtr, origin);
    M_TranslateSet(g_WMatrixPtr, origin);
}

void Matrix_TranslateSet32_M(MATRIX *const m, const XYZ_32 origin)
{
    M_TranslateSet(m, origin);
}

void Matrix_InitInterpolate(const int32_t frac, const int32_t rate)
{
    m_IMFrac = frac;
    m_IMRate = rate;
    m_IMMatrixPtr = &m_IMMatrixStack[0];
    if (!g_MatrixWorldOnly) {
        *m_IMMatrixPtr = *g_MatrixPtr;
    }
    m_WIMMatrixPtr = &m_WIMMatrixStack[0];
    *m_WIMMatrixPtr = *g_WMatrixPtr;
}

void Matrix_Interpolate(void)
{
    if (!g_MatrixWorldOnly) {
        M_Interpolate(g_MatrixPtr, m_IMMatrixPtr, g_MatrixPtr);
    }
    M_Interpolate(g_WMatrixPtr, m_WIMMatrixPtr, g_WMatrixPtr);
}

void Matrix_InterpolateArm(void)
{
    M_InterpolateArm(g_MatrixPtr, m_IMMatrixPtr);
    M_InterpolateArm(g_WMatrixPtr, m_WIMMatrixPtr);
}

void Matrix_Push_I(void)
{
    Matrix_Push();
    if (!g_MatrixWorldOnly) {
        M_MatCopy(&m_IMMatrixPtr[1], &m_IMMatrixPtr[0]);
    }
    m_IMMatrixPtr++;
    M_MatCopy(&m_WIMMatrixPtr[1], &m_WIMMatrixPtr[0]);
    m_WIMMatrixPtr++;
}

void Matrix_Pop_I(void)
{
    Matrix_Pop();
    m_IMMatrixPtr--;
    m_WIMMatrixPtr--;
}

void Matrix_TranslateRel_I(const int32_t x, const int32_t y, const int32_t z)
{
    Matrix_TranslateRel32_I((XYZ_32) { x, y, z });
}

void Matrix_TranslateRel16_I(const XYZ_16 offset)
{
    Matrix_TranslateRel32_I(XYZ_32_From16(offset));
}

void Matrix_TranslateRel32_I(const XYZ_32 offset)
{
    if (!g_MatrixWorldOnly) {
        M_TranslateRel(g_MatrixPtr, offset);
        M_TranslateRel(m_IMMatrixPtr, offset);
    }
    M_TranslateRel(g_WMatrixPtr, offset);
    M_TranslateRel(m_WIMMatrixPtr, offset);
}

void Matrix_TranslateRel_ID(
    const int32_t x, const int32_t y, const int32_t z, const int32_t x2,
    const int32_t y2, const int32_t z2)
{
    Matrix_TranslateRel32_ID((XYZ_32) { x, y, z }, (XYZ_32) { x2, y2, z2 });
}

void Matrix_TranslateRel16_ID(const XYZ_16 offset_1, const XYZ_16 offset_2)
{
    Matrix_TranslateRel32_ID(XYZ_32_From16(offset_1), XYZ_32_From16(offset_2));
}

void Matrix_TranslateRel32_ID(const XYZ_32 offset_1, const XYZ_32 offset_2)
{
    if (!g_MatrixWorldOnly) {
        M_TranslateRel(g_MatrixPtr, offset_1);
        M_TranslateRel(m_IMMatrixPtr, offset_2);
    }
    M_TranslateRel(g_WMatrixPtr, offset_1);
    M_TranslateRel(m_WIMMatrixPtr, offset_2);
}

void Matrix_RotY_I(const int16_t angle)
{
    if (angle != 0) {
        const int32_t s = Math_Sin(angle);
        const int32_t c = Math_Cos(angle);
        if (!g_MatrixWorldOnly) {
            M_RotYSC(g_MatrixPtr, s, c);
            M_RotYSC(m_IMMatrixPtr, s, c);
        }
        M_RotYSC(g_WMatrixPtr, s, c);
        M_RotYSC(m_WIMMatrixPtr, s, c);
    }
}

void Matrix_RotX_I(const int16_t angle)
{
    if (angle != 0) {
        const int32_t s = Math_Sin(angle);
        const int32_t c = Math_Cos(angle);
        if (!g_MatrixWorldOnly) {
            M_RotXSC(g_MatrixPtr, s, c);
            M_RotXSC(m_IMMatrixPtr, s, c);
        }
        M_RotXSC(g_WMatrixPtr, s, c);
        M_RotXSC(m_WIMMatrixPtr, s, c);
    }
}

void Matrix_RotZ_I(const int16_t angle)
{
    if (angle != 0) {
        const int32_t s = Math_Sin(angle);
        const int32_t c = Math_Cos(angle);
        if (!g_MatrixWorldOnly) {
            M_RotZSC(g_MatrixPtr, s, c);
            M_RotZSC(m_IMMatrixPtr, s, c);
        }
        M_RotZSC(g_WMatrixPtr, s, c);
        M_RotZSC(m_WIMMatrixPtr, s, c);
    }
}

void Matrix_Rot16_I(const XYZ_16 rotation)
{
    MATRIX *const ms[] = {
        g_WMatrixPtr, m_WIMMatrixPtr, g_MatrixPtr, m_IMMatrixPtr
    };
    M_RotYXZ_N(ms, g_MatrixWorldOnly ? 2 : 4, rotation);
}

void Matrix_Rot16_ID(const XYZ_16 rotation_1, const XYZ_16 rotation_2)
{
    MATRIX *const a[] = { g_WMatrixPtr, g_MatrixPtr };
    MATRIX *const b[] = { m_WIMMatrixPtr, m_IMMatrixPtr };
    const int32_t n = g_MatrixWorldOnly ? 1 : 2;
    M_RotYXZ_N(a, n, rotation_1);
    M_RotYXZ_N(b, n, rotation_2);
}

void Matrix_LookAt(
    const int32_t source_x, const int32_t source_y, const int32_t source_z,
    const int32_t target_x, const int32_t target_y, const int32_t target_z,
    const int16_t roll)
{
    int16_t angles[2];
    Math_GetVectorAngles(
        target_x - source_x, target_y - source_y, target_z - source_z, angles);

    const XYZ_32 view_pos = {
        .x = source_x,
        .y = source_y,
        .z = source_z,
    };
    const XYZ_16 view_rot = {
        .x = angles[1],
        .y = angles[0],
        .z = roll,
    };
    Matrix_GenerateW2V(&view_pos, &view_rot);
}

XYZ_32 Matrix_MulVec32_M(const MATRIX *const m, const XYZ_32 v)
{
    return (XYZ_32) {
        (m->_00 * v.x + m->_01 * v.y + m->_02 * v.z + m->_03) >> W2V_SHIFT,
        (m->_10 * v.x + m->_11 * v.y + m->_12 * v.z + m->_13) >> W2V_SHIFT,
        (m->_20 * v.x + m->_21 * v.y + m->_22 * v.z + m->_23) >> W2V_SHIFT,
    };
}

XYZ_32 Matrix_GetOffset_M(const MATRIX *const m)
{
    return (XYZ_32) {
        .x = m->_03 >> W2V_SHIFT,
        .y = m->_13 >> W2V_SHIFT,
        .z = m->_23 >> W2V_SHIFT,
    };
}
