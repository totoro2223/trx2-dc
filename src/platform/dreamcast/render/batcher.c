// Dreamcast mesh batcher: the MESH_BATCHER of trx/game/output/mesh_batcher
// drawn through the PowerVR.
//
// The sources build their meshes and stage instances exactly as on the
// desktop. What the desktop does in its mesh shader happens here on the SH-4,
// once per vertex of every staged instance: the model and view transform
// (ftrv), the TR1/TR2 shade lighting, and fog, which the PowerVR adds as the
// offset colour. Faces are clipped against the near plane and projected, then
// handed to pvr.c, which puts them in the list their blending asks for. The
// translucent list is sorted by the PowerVR, so the transparent faces need no
// sorting of their own.

#include "pvr.h"
#include "shadow.h"
#include "uniform_layouts.h"

#include <platform/dreamcast/gl_facade.h>
#include <trx/config.h>
#include <trx/core/memory.h>
#include <trx/core/vector.h>
#include <trx/debug.h>
#include <trx/game/const.h>
#include <trx/game/output.h>
#include <trx/game/output/lights/priv.h>
#include <trx/game/output/mesh_batcher/batcher.h>
#include <trx/game/output/shaders/mesh.h>
#include <trx/game/output/textures.h>
#include <trx/game/output/utils.h>
#include <trx/game/output/vertex_range.h>
#include <trx/game/viewport.h>

#include <dc/matrix.h>
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <uthash.h>

#define M_SHADE_NEUTRAL 0x1000
#define M_SHADE_MAX 0x1FFF
#define M_NO_PAGE (-1)

typedef struct M_BINDING {
    OUTPUT_MESH *mesh;
    int32_t vertex_count;
    // Per vertex: the texture coordinates and page the vertex samples, looked
    // up once instead of every frame.
    float *uv;
    int8_t *page;
    bool needs_room_lights;
    bool needs_object_light;
    bool needs_own_light;
    // Object space bounding box: minimum then maximum corner.
    float bounds[2][3];
    UT_hash_handle hh;
} M_BINDING;

// A mesh instance that casts a shadow this frame.
typedef struct {
    float bounds[2][3];
    float world[3][4];
    const ROOM *room;
} M_CASTER;

#define M_MAX_CASTERS 32
// How far a shadow volume reaches past its caster, in world units.
#define M_SHADOW_LENGTH 1024.0f
// How far a light may tilt a shadow off the vertical: a light level with or
// below the caster would throw it sideways or up.
#define M_SHADOW_MIN_DROP 0.6f
// How far a light may be from the casters and still turn their shadow.
#define M_SHADOW_LIGHT_RANGE (3.0f * WALL_L)

// A vertex once transformed and lit: eye space, and its colours.
typedef struct {
    float x;
    float y;
    float z;
    uint32_t argb;
    uint32_t oargb;
} M_XVERT;

typedef struct MESH_BATCHER {
    SCENE_SOURCE source;
    VECTOR *bindings; // M_BINDING *
    M_BINDING *map;
    VECTOR *staged[SCENE_PASS_COUNT];
    // Per staged instance: whether it casts a shadow (and so takes none).
    VECTOR *casters[SCENE_PASS_COUNT];
    M_XVERT *xverts;
    int32_t xverts_capacity;
} MESH_BATCHER;

// What a pass needs that holds for every instance in it.
typedef struct {
    float view[4][4];
    float proj_x;
    float proj_y;
    float center_x;
    float center_y;
    float near_z;
    // An orthographic projection (models drawn on the UI canvas): screen
    // position and depth straight from the eye position.
    bool ortho;
    float ortho_scale[3];
    float ortho_offset[3];
    float half_w;
    float half_h;
    float fog_start;
    float fog_scale;
    float fog_color[3];
    float min_shade;
    float time_in_game;
    float sunset_duration;
    bool lighting;
    // Bilinear filtering samples half a texel past a face's edge, into the
    // next texture on the page; the faces' coordinates are pulled in by that
    // much instead (the desktop clamps them to the texture in its shader).
    bool inset_uv;
    bool fog;
    const DC_UNIFORM_LIGHTS *lights;
} M_FRAME;

static M_FRAME m_Frame = {};

static bool m_Casting = false;
static int32_t m_CasterCount = 0;
static M_CASTER m_Casters[M_MAX_CASTERS];

// Running totals, for the watchdog.
int32_t g_DC_BatcherMeshCount = 0;
int32_t g_DC_BatcherVertexCount = 0;

static inline float M_Clamp(const float x, const float lo, const float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline uint32_t M_PackARGB(
    const float a, const float r, const float g, const float b)
{
    const uint32_t ia = (uint32_t)(M_Clamp(a, 0.0f, 1.0f) * 255.0f);
    const uint32_t ir = (uint32_t)(M_Clamp(r, 0.0f, 1.0f) * 255.0f);
    const uint32_t ig = (uint32_t)(M_Clamp(g, 0.0f, 1.0f) * 255.0f);
    const uint32_t ib = (uint32_t)(M_Clamp(b, 0.0f, 1.0f) * 255.0f);
    return (ia << 24) | (ir << 16) | (ig << 8) | ib;
}

static inline uint32_t M_LerpARGB(
    const uint32_t c0, const uint32_t c1, const float t)
{
    uint32_t result = 0;
    for (int32_t shift = 0; shift < 32; shift += 8) {
        const float a = (float)((c0 >> shift) & 0xFF);
        const float b = (float)((c1 >> shift) & 0xFF);
        const uint32_t c = (uint32_t)(a + (b - a) * t + 0.5f);
        result |= (c > 255 ? 255 : c) << shift;
    }
    return result;
}

static M_BINDING *M_GetBinding(
    const MESH_BATCHER *const batcher, const OUTPUT_MESH *const mesh)
{
    M_BINDING *bind = nullptr;
    HASH_FIND_PTR(batcher->map, &mesh, bind);
    return bind;
}

static void M_FillTexture(M_BINDING *const bind, const int32_t idx)
{
    const OUTPUT_MESH_VERTEX *const vertex =
        Vector_Get(bind->mesh->vertices, idx);
    if (vertex->uvw_idx < 0 || (vertex->flags & VERT_FLAT_SHADED) != 0) {
        bind->uv[idx * 2 + 0] = 0.0f;
        bind->uv[idx * 2 + 1] = 0.0f;
        bind->page[idx] = M_NO_PAGE;
        return;
    }
    const OUTPUT_UVW uvw = Output_Textures_GetUVW(vertex->uvw_idx);
    bind->uv[idx * 2 + 0] = uvw.u;
    bind->uv[idx * 2 + 1] = uvw.v;
    bind->page[idx] = (int8_t)uvw.w;
}

// The model-view matrix of an instance, as GL lays it out (column first),
// which is also how the SH-4's XMTRX takes it.
static void M_ModelView(
    const MATRIX *const wmatrix, float out[4][4], float *const out_scale)
{
    float model[4][4];
    Output_FillMatrix(model, wmatrix);
    for (int32_t c = 0; c < 4; c++) {
        for (int32_t r = 0; r < 4; r++) {
            out[c][r] = m_Frame.view[0][r] * model[c][0]
                + m_Frame.view[1][r] * model[c][1]
                + m_Frame.view[2][r] * model[c][2]
                + m_Frame.view[3][r] * model[c][3];
        }
    }
    *out_scale = sqrtf(
        model[0][0] * model[0][0] + model[0][1] * model[0][1]
        + model[0][2] * model[0][2]);
}

static int32_t M_FlickerIndex(const int32_t mode, const float time)
{
    switch (mode) {
    case RLM_FLICKER: {
        const float h = sinf(time * 593.123f) * 43758.5453f;
        return (int32_t)((h - floorf(h)) * 32.0f);
    }
    case RLM_GLOW: {
        const float phase = fmodf(time, 32.0f) / 32.0f;
        return (int32_t)((sinf(phase * 2.0f * M_PI) + 1.0f) * 0.5f * 31.0f);
    }
    case RLM_SUNSET: {
        const float duration =
            m_Frame.sunset_duration < 1.0f ? 1.0f : m_Frame.sunset_duration;
        return (int32_t)(M_Clamp(time / duration, 0.0f, 1.0f) * 31.0f);
    }
    default:
        return 0;
    }
}

// A room's point light, with what the shading asks of it worked out once.
typedef struct {
    float pos[3];
    float radius_sq;
    float max_shade;
    float inv_div;
} M_POINT_LIGHT;

#define M_MAX_POINT_LIGHTS 32

typedef struct {
    float l[3]; // object space light direction, for object lighting
    float adder;
    bool has_direction;
    // For vertices lit by the room's lights: the instance's world matrix in
    // world units, the lights, and the flicker the room's light mode adds
    // (times each vertex's light table offset).
    bool has_room_lights;
    float world[3][4];
    int32_t num_lights;
    M_POINT_LIGHT lights[M_MAX_POINT_LIGHTS];
    float flicker;
} M_INSTANCE_LIGHT;

static float M_DynamicLum(
    float shade, const float world[3], const M_INSTANCE_LIGHT *const il)
{
    for (int32_t i = 0; i < il->num_lights; i++) {
        const M_POINT_LIGHT *const light = &il->lights[i];
        const float dx = light->pos[0] - world[0];
        const float dy = light->pos[1] - world[1];
        const float dz = light->pos[2] - world[2];
        const float dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq > light->radius_sq) {
            continue;
        }
        shade -= light->max_shade - dist_sq * light->inv_div;
    }
    return shade < 0.0f ? 0.0f : shade;
}

// An instance's world matrix (fixed point in TR) in world units, by rows.
static void M_WorldMatrix(const MATRIX *const m, float out[3][4])
{
    const float scale = 1.0f / (float)(1 << W2V_SHIFT);
    const int64_t rows[3][4] = {
        { m->_00, m->_01, m->_02, m->_03 },
        { m->_10, m->_11, m->_12, m->_13 },
        { m->_20, m->_21, m->_22, m->_23 },
    };
    for (int32_t r = 0; r < 3; r++) {
        for (int32_t c = 0; c < 4; c++) {
            out[r][c] = (float)rows[r][c] * scale;
        }
    }
}

static void M_PrepareRoomLights(
    const MESH_INSTANCE *const inst, M_INSTANCE_LIGHT *const out)
{
    out->has_room_lights = true;
    M_WorldMatrix(&inst->wmatrix, out->world);

    const DC_UNIFORM_LIGHTS *const lights = m_Frame.lights;
    out->num_lights = 0;
    out->flicker = 0.0f;
    if (lights == nullptr) {
        return;
    }
    for (int32_t i = 0;
         i < lights->num_lights && out->num_lights < M_MAX_POINT_LIGHTS; i++) {
        const DC_UNIFORM_LIGHT *const light = &lights->lights[i];
        if (light->kind != 0.0f) {
            continue;
        }
        const float radius = exp2f(light->falloff);
        M_POINT_LIGHT *const pl = &out->lights[out->num_lights++];
        pl->pos[0] = light->pos[0];
        pl->pos[1] = light->pos[1];
        pl->pos[2] = light->pos[2];
        pl->radius_sq = radius * radius;
        pl->max_shade = exp2f(light->shade);
        pl->inv_div = 1.0f / exp2f(2.0f * light->falloff - light->shade);
    }
    const int32_t index =
        M_FlickerIndex(lights->room_light_mode, m_Frame.time_in_game);
    out->flicker = index * 512.0f / 31.0f;
}

static void M_PrepareInstanceLight(
    const MESH_INSTANCE *const inst, const M_BINDING *const bind,
    const float mv[4][4], M_INSTANCE_LIGHT *const out)
{
    out->has_direction = false;
    out->has_room_lights = false;
    out->adder = 0.0f;
    if (bind->needs_room_lights) {
        Output_Uniforms_UploadRoomLights(Output_GetUniforms(), inst->room);
        m_Frame.lights =
            DC_GL_GetUniformBlock(DC_GL_BINDING_LIGHTS, nullptr);
        M_PrepareRoomLights(inst, out);
    }
    if (bind->needs_object_light) {
        Output_Lights_UploadCPULight(&inst->light_info);
    } else if (bind->needs_own_light) {
        Output_Lights_UploadOwnLight(&inst->light_info);
    } else {
        return;
    }
    const OUTPUT_UNIFORM_LS_TR12 *const ls =
        DC_GL_GetUniformBlock(DC_GL_BINDING_LIGHT_SOURCE, nullptr);
    if (ls == nullptr) {
        return;
    }
    out->adder = ls->adder;
    if (ls->divider != 0.0f) {
        // The view-space light vector, taken into object space by the
        // transpose of the model-view rotation.
        const float *const v = ls->vector_view;
        for (int32_t c = 0; c < 3; c++) {
            out->l[c] =
                (mv[c][0] * v[0] + mv[c][1] * v[1] + mv[c][2] * v[2])
                / ls->divider;
        }
        out->has_direction = true;
    }
}

static void M_LightVertex(
    const OUTPUT_MESH_VERTEX *const vertex, const M_INSTANCE_LIGHT *const il,
    const MATRIX *const wmatrix, const RGBA_F tint, float out_rgba[4],
    float *const out_shade_mul)
{
    const uint16_t flags = vertex->flags;
    float shade = M_SHADE_NEUTRAL;
    if (m_Frame.lighting && (flags & VERT_NO_LIGHTING) == 0) {
        if ((flags & VERT_USE_OWN_LIGHT) != 0) {
            shade = il->adder + vertex->shade;
        } else if ((flags & VERT_USE_OBJECT_LIGHT) != 0) {
            shade = il->adder;
            if (il->has_direction) {
                const float n_dot_l = il->l[0] * vertex->normal.x
                    + il->l[1] * vertex->normal.y
                    + il->l[2] * vertex->normal.z;
                shade += n_dot_l / (float)(1 << 14) / 4.0f;
                shade = M_Clamp(shade, 0.0f, M_SHADE_MAX);
            }
        } else {
            shade = vertex->shade;
            if ((flags & VERT_USE_DYNAMIC_LIGHT) != 0 && il->has_room_lights) {
                const float x = vertex->pos.x;
                const float y = vertex->pos.y;
                const float z = vertex->pos.z;
                const float(*const w)[4] = il->world;
                const float world[3] = {
                    w[0][0] * x + w[0][1] * y + w[0][2] * z + w[0][3],
                    w[1][0] * x + w[1][1] * y + w[1][2] * z + w[1][3],
                    w[2][0] * x + w[2][1] * y + w[2][2] * z + w[2][3],
                };
                shade = M_DynamicLum(shade, world, il);
                const float j = (float)(vertex->light_table_idx & 31);
                shade += (j - 16.0f) * il->flicker;
            }
            shade = M_Clamp(shade, 0.0f, M_SHADE_MAX);
        }
    }

    float mul = 1.0f;
    if ((flags & VERT_NO_LIGHTING) == 0) {
        const float s = shade > m_Frame.min_shade ? shade : m_Frame.min_shade;
        mul = 2.0f - s / M_SHADE_NEUTRAL;
    }
    *out_shade_mul = mul;
    out_rgba[0] = vertex->color.r / 255.0f * mul * tint.r;
    out_rgba[1] = vertex->color.g / 255.0f * mul * tint.g;
    out_rgba[2] = vertex->color.b / 255.0f * mul * tint.b;
    out_rgba[3] = vertex->color.a / 255.0f * tint.a;
}

static void M_TransformInstance(
    MESH_BATCHER *const batcher, const MESH_INSTANCE *const inst,
    const M_BINDING *const bind)
{
    if (bind->vertex_count > batcher->xverts_capacity) {
        batcher->xverts_capacity = bind->vertex_count + 256;
        Memory_Free(batcher->xverts);
        batcher->xverts = memalign(32, batcher->xverts_capacity * sizeof(M_XVERT));
    }

    float mv[4][4];
    float scale;
    M_ModelView(&inst->wmatrix, mv, &scale);

    M_INSTANCE_LIGHT il;
    M_PrepareInstanceLight(inst, bind, mv, &il);

    mat_load((matrix_t *)mv);
    const OUTPUT_MESH_VERTEX *const vertices =
        Vector_GetData(bind->mesh->vertices);
    for (int32_t i = 0; i < bind->vertex_count; i++) {
        const OUTPUT_MESH_VERTEX *const vertex = &vertices[i];
        float x = vertex->pos.x;
        float y = vertex->pos.y;
        float z = vertex->pos.z;
        float w = 1.0f;
        mat_trans_nodiv(x, y, z, w);
        if ((vertex->flags & (VERT_BILLBOARD | VERT_ABS_SPRITE)) != 0) {
            x += vertex->normal.x * scale;
            y += vertex->normal.y * scale;
        }

        M_XVERT *const out = &batcher->xverts[i];
        out->x = x;
        out->y = y;
        out->z = z;

        float rgba[4];
        float mul;
        M_LightVertex(vertex, &il, &inst->wmatrix, inst->tint, rgba, &mul);

        float fog = 0.0f;
        if (m_Frame.fog && (vertex->flags & (VERT_NO_FOG | VERT_NO_LIGHTING)) == 0) {
            const float dist = sqrtf(x * x + y * y + z * z);
            fog = M_Clamp(
                (dist - m_Frame.fog_start) * m_Frame.fog_scale, 0.0f, 1.0f);
        }
        const float keep = 1.0f - fog;
        out->argb =
            M_PackARGB(rgba[3], rgba[0] * keep, rgba[1] * keep, rgba[2] * keep);
        out->oargb = fog > 0.0f
            ? M_PackARGB(
                  1.0f, m_Frame.fog_color[0] * fog * inst->tint.r,
                  m_Frame.fog_color[1] * fog * inst->tint.g,
                  m_Frame.fog_color[2] * fog * inst->tint.b)
            : 0;
    }
}

static inline void M_Project(
    const M_XVERT *const in, const float u, const float v,
    DC_PVR_VERTEX *const out)
{
    if (m_Frame.ortho) {
        const float(*const s) = m_Frame.ortho_scale;
        const float(*const o) = m_Frame.ortho_offset;
        out->x = m_Frame.center_x + (s[0] * in->x + o[0]) * m_Frame.half_w;
        out->y = m_Frame.center_y - (s[1] * in->y + o[1]) * m_Frame.half_h;
        // -1 at the near end; the PowerVR wants the nearer bigger.
        out->z = DC_PVR_Depth2D(0.5f - 0.5f * (s[2] * in->z + o[2]));
        out->u = u;
        out->v = v;
        out->argb = in->argb;
        out->oargb = in->oargb;
        return;
    }
    const float inv_z = 1.0f / in->z;
    out->x = m_Frame.center_x + m_Frame.proj_x * in->x * inv_z;
    out->y = m_Frame.center_y + m_Frame.proj_y * in->y * inv_z;
    out->z = DC_PVR_Depth3D(m_Frame.near_z * inv_z);
    out->u = u;
    out->v = v;
    out->argb = in->argb;
    out->oargb = in->oargb;
}

// A vertex of a face being clipped: transformed, with its texture
// coordinates alongside.
typedef struct {
    M_XVERT xv;
    float u;
    float v;
} M_CLIP_VERT;

static M_CLIP_VERT M_ClipEdge(
    const M_CLIP_VERT *const a, const M_CLIP_VERT *const b)
{
    const float t = (m_Frame.near_z - a->xv.z) / (b->xv.z - a->xv.z);
    return (M_CLIP_VERT) {
        .xv = {
            .x = a->xv.x + (b->xv.x - a->xv.x) * t,
            .y = a->xv.y + (b->xv.y - a->xv.y) * t,
            .z = m_Frame.near_z,
            .argb = M_LerpARGB(a->xv.argb, b->xv.argb, t),
            .oargb = M_LerpARGB(a->xv.oargb, b->xv.oargb, t),
        },
        .u = a->u + (b->u - a->u) * t,
        .v = a->v + (b->v - a->v) * t,
    };
}

// Half a texel of a texture page, in page coordinates.
#define M_HALF_TEXEL (0.5f / TEXTURE_PAGE_WIDTH)

// Reads a face's texture coordinates, pulled towards their middle by up to
// half a texel when filtering bilinearly.
static void M_GetFaceUVs(
    const M_BINDING *const bind, const uint32_t *const idx, const int32_t n,
    float uv[][2])
{
    float cu = 0.0f;
    float cv = 0.0f;
    for (int32_t k = 0; k < n; k++) {
        uv[k][0] = bind->uv[idx[k] * 2];
        uv[k][1] = bind->uv[idx[k] * 2 + 1];
        cu += uv[k][0];
        cv += uv[k][1];
    }
    if (!m_Frame.inset_uv || bind->page[idx[0]] == M_NO_PAGE) {
        return;
    }
    cu /= n;
    cv /= n;
    for (int32_t k = 0; k < n; k++) {
        for (int32_t a = 0; a < 2; a++) {
            const float d = (a == 0 ? cu : cv) - uv[k][a];
            uv[k][a] += d > M_HALF_TEXEL ? M_HALF_TEXEL
                : d < -M_HALF_TEXEL      ? -M_HALF_TEXEL
                                         : d * 0.5f;
        }
    }
}

static void M_EmitTriangle(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const uint32_t i0, const uint32_t i1, const uint32_t i2)
{
    const uint32_t idx[3] = { i0, i1, i2 };
    float uv[3][2];
    M_GetFaceUVs(bind, idx, 3, uv);
    const float near_z = m_Frame.near_z;
    int32_t inside = 0;
    for (int32_t k = 0; k < 3; k++) {
        if (batcher->xverts[idx[k]].z >= near_z) {
            inside++;
        }
    }
    if (inside == 0) {
        return;
    }

    if (inside == 3) {
        DC_PVR_VERTEX out[3];
        for (int32_t k = 0; k < 3; k++) {
            M_Project(&batcher->xverts[idx[k]], uv[k][0], uv[k][1], &out[k]);
        }
        DC_PVR_Triangle(&out[0], &out[1], &out[2]);
        return;
    }

    // Sutherland-Hodgman against the near plane: three corners in, at most
    // four out.
    M_CLIP_VERT in[3];
    for (int32_t k = 0; k < 3; k++) {
        in[k] = (M_CLIP_VERT) {
            .xv = batcher->xverts[idx[k]],
            .u = uv[k][0],
            .v = uv[k][1],
        };
    }
    M_CLIP_VERT poly[4];
    int32_t count = 0;
    for (int32_t k = 0; k < 3; k++) {
        const M_CLIP_VERT *const a = &in[k];
        const M_CLIP_VERT *const b = &in[(k + 1) % 3];
        const bool a_in = a->xv.z >= near_z;
        const bool b_in = b->xv.z >= near_z;
        if (a_in) {
            poly[count++] = *a;
        }
        if (a_in != b_in) {
            poly[count++] = M_ClipEdge(a, b);
        }
    }
    DC_PVR_VERTEX out[4];
    for (int32_t k = 0; k < count; k++) {
        M_Project(&poly[k].xv, poly[k].u, poly[k].v, &out[k]);
    }
    if (count == 3) {
        DC_PVR_Triangle(&out[0], &out[1], &out[2]);
    } else if (count == 4) {
        // Fan 0-1-2-3 as a strip: 1, 0, 2, 3.
        DC_PVR_Quad(&out[1], &out[0], &out[2], &out[3]);
    }
}

// Two triangles that share an edge (as a quad's halves do) go out as one
// strip of four vertices instead of six: TR's geometry is mostly quads, and
// the vertex buffers and the PowerVR both take a third less. Only when no
// corner needs clipping.
static bool M_TryEmitQuad(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const uint32_t *const t0, const uint32_t *const t1)
{
    int32_t shared[2];
    int32_t num_shared = 0;
    bool t0_shared[3] = {};
    bool t1_shared[3] = {};
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            if (t0[i] == t1[j] && !t1_shared[j]) {
                if (num_shared < 2) {
                    shared[num_shared] = (int32_t)t0[i];
                }
                num_shared++;
                t0_shared[i] = true;
                t1_shared[j] = true;
                break;
            }
        }
    }
    if (num_shared != 2) {
        return false;
    }
    uint32_t p = 0;
    uint32_t q = 0;
    for (int32_t i = 0; i < 3; i++) {
        if (!t0_shared[i]) {
            p = t0[i];
        }
        if (!t1_shared[i]) {
            q = t1[i];
        }
    }
    const uint32_t strip[4] = { p, (uint32_t)shared[0], (uint32_t)shared[1], q };
    for (int32_t k = 0; k < 4; k++) {
        if (batcher->xverts[strip[k]].z < m_Frame.near_z) {
            return false;
        }
    }
    // Pulled in about the middle of the whole quad, so that both halves
    // still meet along the diagonal.
    float uv[4][2];
    M_GetFaceUVs(bind, strip, 4, uv);
    DC_PVR_VERTEX out[4];
    for (int32_t k = 0; k < 4; k++) {
        M_Project(&batcher->xverts[strip[k]], uv[k][0], uv[k][1], &out[k]);
    }
    DC_PVR_Quad(&out[0], &out[1], &out[2], &out[3]);
    return true;
}

static void M_EmitIndices(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const uint32_t *const indices, const int32_t count, const DC_BLEND blend)
{
    int32_t page = INT32_MIN;
    int32_t i = 0;
    while (i + 2 < count) {
        const int32_t tri_page = bind->page[indices[i]];
        if (tri_page != page) {
            page = tri_page;
            DC_PVR_SetState(
                blend, page == M_NO_PAGE ? DC_PVR_NO_TEXTURE : page, true);
        }
        if (i + 5 < count && bind->page[indices[i + 3]] == page
            && M_TryEmitQuad(batcher, bind, &indices[i], &indices[i + 3])) {
            i += 6;
            continue;
        }
        M_EmitTriangle(batcher, bind, indices[i], indices[i + 1], indices[i + 2]);
        i += 3;
    }
}

static void M_DrawInstance(
    MESH_BATCHER *const batcher, const MESH_INSTANCE *const inst,
    const SCENE_PASS pass, const bool is_caster)
{
    const M_BINDING *const bind = M_GetBinding(batcher, inst->mesh);
    if (bind == nullptr || bind->vertex_count == 0) {
        return;
    }
    M_TransformInstance(batcher, inst, bind);
    // Boxes make a rough shadow; on the caster itself it would only look
    // wrong.
    DC_PVR_SetShadowReceiver(!is_caster);

    const OUTPUT_MESH *const mesh = inst->mesh;
    if (pass == SCENE_PASS_BLEND_ADD || pass == SCENE_PASS_BLEND_SUB) {
        M_EmitIndices(
            batcher, bind, Vector_GetData(mesh->blend_add_vertex_indices),
            mesh->blend_add_vertex_indices->count,
            pass == SCENE_PASS_BLEND_ADD ? DC_BLEND_ADD : DC_BLEND_SUB);
        return;
    }

    // An instance faded out as a whole draws every face blended.
    const bool faded = inst->tint.a < 1.0f;
    const uint32_t *const opaque = Vector_GetData(mesh->opaque_vertex_indices);
    const int32_t clean = mesh->opaque_nodiscard_count;
    M_EmitIndices(
        batcher, bind, opaque, clean, faded ? DC_BLEND_ALPHA : DC_BLEND_OPAQUE);
    M_EmitIndices(
        batcher, bind, opaque + clean,
        mesh->opaque_vertex_indices->count - clean,
        faded ? DC_BLEND_ALPHA : DC_BLEND_PUNCH);
    for (int32_t i = 0; i < mesh->transparent_faces->count; i++) {
        const OUTPUT_MESH_FACE *const face =
            Vector_Get(mesh->transparent_faces, i);
        M_EmitIndices(
            batcher, bind, (const uint32_t *)face->vertex_indices,
            face->vertex_count, DC_BLEND_ALPHA);
    }
}

static void M_PrepareFrame(void)
{
    const DC_UNIFORM_MATRICES *const matrices =
        DC_GL_GetUniformBlock(DC_GL_BINDING_MATRICES, nullptr);
    const DC_UNIFORM_GENERAL *const general =
        DC_GL_GetUniformBlock(DC_GL_BINDING_GENERAL, nullptr);
    if (matrices == nullptr || general == nullptr) {
        return;
    }
    memcpy(m_Frame.view, matrices->mat_view, sizeof(m_Frame.view));

    const VIEWPORT_RECT rect = Viewport_GetRect(VIEWPORT_GAME);
    const float half_w = rect.width * 0.5f;
    const float half_h = rect.height * 0.5f;
    m_Frame.center_x = rect.x + half_w;
    m_Frame.center_y = rect.y + half_h;
    m_Frame.proj_x = matrices->mat_proj[0][0] * half_w;
    m_Frame.proj_y = -matrices->mat_proj[1][1] * half_h;
    m_Frame.near_z = Output_GetNearZ() / (float)(1 << W2V_SHIFT);
    if (m_Frame.near_z < 1.0f) {
        m_Frame.near_z = 1.0f;
    }
    m_Frame.half_w = half_w;
    m_Frame.half_h = half_h;
    m_Frame.ortho = matrices->mat_proj[3][3] == 1.0f;
    if (m_Frame.ortho) {
        for (int32_t i = 0; i < 3; i++) {
            m_Frame.ortho_scale[i] = matrices->mat_proj[i][i];
            m_Frame.ortho_offset[i] = matrices->mat_proj[3][i];
        }
        // Nothing to clip: every vertex is in front.
        m_Frame.near_z = -1.0e30f;
    }

    m_Frame.lighting = general->lighting_enabled != 0;
    m_Frame.min_shade = general->min_shade;
    m_Frame.time_in_game = general->time_in_game;
    m_Frame.sunset_duration = general->sunset_duration;
    const float fog_start = general->fog_distance[0];
    const float fog_end = general->fog_distance[1];
    m_Frame.fog = m_Frame.lighting && fog_end > fog_start;
    m_Frame.fog_start = fog_start;
    m_Frame.fog_scale = m_Frame.fog ? 1.0f / (fog_end - fog_start) : 0.0f;
    m_Frame.fog_color[0] = general->fog_color[0];
    m_Frame.fog_color[1] = general->fog_color[1];
    m_Frame.fog_color[2] = general->fog_color[2];
    m_Frame.lights = DC_GL_GetUniformBlock(DC_GL_BINDING_LIGHTS, nullptr);
}

static void M_RenderBegin(const SCENE_SOURCE *const source)
{
    MESH_BATCHER *const batcher = source->priv;
    for (int32_t pass = 0; pass < SCENE_PASS_COUNT; pass++) {
        Vector_Clear(batcher->staged[pass]);
        Vector_Clear(batcher->casters[pass]);
    }
}

static void M_RenderPass(const SCENE_SOURCE *const source, const SCENE_PASS pass)
{
    MESH_BATCHER *const batcher = source->priv;
    if (pass != SCENE_PASS_OPAQUE && pass != SCENE_PASS_BLEND_ADD
        && pass != SCENE_PASS_BLEND_SUB) {
        return;
    }
    M_PrepareFrame();
    m_Frame.inset_uv =
        g_Config.rendering.texture_filter == TEXTURE_FILTER_BILINEAR;
    DC_PVR_SetFilter(m_Frame.inset_uv);
    VECTOR *const staged = batcher->staged[pass];
    const bool *const casters = Vector_GetData(batcher->casters[pass]);
    for (int32_t i = 0; i < staged->count; i++) {
        M_DrawInstance(batcher, Vector_Get(staged, i), pass, casters[i]);
    }
    DC_PVR_SetShadowReceiver(false);
}

static bool M_IsDirty(const SCENE_SOURCE *const source, const SCENE_PASS pass)
{
    const MESH_BATCHER *const batcher = source->priv;
    return batcher->staged[pass]->count > 0;
}

static void M_AnimateTextures(const SCENE_SOURCE *const source)
{
    MESH_BATCHER *const batcher = source->priv;
    for (int32_t i = 0; i < batcher->bindings->count; i++) {
        M_BINDING *const bind = *(M_BINDING **)Vector_Get(batcher->bindings, i);
        const VECTOR *const ranges = bind->mesh->animated_vertices;
        for (int32_t j = 0; j < ranges->count; j++) {
            const OUTPUT_VERTEX_RANGE *const range = Vector_Get(ranges, j);
            for (int32_t k = range->vertex_start;
                 k < range->vertex_start + range->vertex_count; k++) {
                M_FillTexture(bind, k);
            }
        }
    }
}

MESH_BATCHER *MeshBatcher_Create(void)
{
    MESH_BATCHER *const batcher = Memory_Alloc(sizeof(MESH_BATCHER));
    batcher->bindings = Vector_Create(sizeof(M_BINDING *));
    for (int32_t pass = 0; pass < SCENE_PASS_COUNT; pass++) {
        batcher->staged[pass] = Vector_Create(sizeof(MESH_INSTANCE));
        batcher->casters[pass] = Vector_Create(sizeof(bool));
    }
    batcher->source = (SCENE_SOURCE) {
        .render_begin = M_RenderBegin,
        .render_pass = M_RenderPass,
        .is_dirty = M_IsDirty,
        .animate_textures = M_AnimateTextures,
        .priv = batcher,
    };
    return batcher;
}

void MeshBatcher_Destroy(MESH_BATCHER *const batcher)
{
    if (batcher == nullptr) {
        return;
    }
    while (batcher->bindings->count > 0) {
        M_BINDING *const bind = *(M_BINDING **)Vector_Get(batcher->bindings, 0);
        MeshBatcher_RemoveMesh(batcher, bind->mesh);
    }
    Vector_Free(batcher->bindings);
    for (int32_t pass = 0; pass < SCENE_PASS_COUNT; pass++) {
        Vector_Free(batcher->staged[pass]);
        Vector_Free(batcher->casters[pass]);
    }
    Memory_Free(batcher->xverts);
    Memory_Free(batcher);
}

void MeshBatcher_AddMesh(MESH_BATCHER *const batcher, OUTPUT_MESH *const mesh)
{
    ASSERT(mesh->sealed == 1);
    // The mesh grew by doubling as it was built; it is kept for the level's
    // life, so the room it does not use goes back.
    Vector_ShrinkToFit(mesh->vertices);
    Vector_ShrinkToFit(mesh->opaque_vertex_indices);
    Vector_ShrinkToFit(mesh->blend_add_vertex_indices);
    Vector_ShrinkToFit(mesh->transparent_faces);
    Vector_ShrinkToFit(mesh->animated_vertices);
    M_BINDING *const bind = Memory_Alloc(sizeof(M_BINDING));
    bind->mesh = mesh;
    bind->vertex_count = mesh->vertices->count;
    bind->uv = Memory_Alloc(sizeof(float) * 2 * (bind->vertex_count + 1));
    bind->page = Memory_Alloc(bind->vertex_count + 1);
    const OUTPUT_MESH_VERTEX *const vertices = Vector_GetData(mesh->vertices);
    for (int32_t i = 0; i < bind->vertex_count; i++) {
        M_FillTexture(bind, i);
        const float pos[3] = {
            vertices[i].pos.x, vertices[i].pos.y, vertices[i].pos.z,
        };
        for (int32_t k = 0; k < 3; k++) {
            if (i == 0 || pos[k] < bind->bounds[0][k]) {
                bind->bounds[0][k] = pos[k];
            }
            if (i == 0 || pos[k] > bind->bounds[1][k]) {
                bind->bounds[1][k] = pos[k];
            }
        }
        const uint16_t flags = vertices[i].flags;
        bind->needs_room_lights |= (flags & VERT_USE_DYNAMIC_LIGHT) != 0;
        bind->needs_object_light |= (flags & VERT_USE_OBJECT_LIGHT) != 0;
        bind->needs_own_light |= (flags & VERT_USE_OWN_LIGHT) != 0;
    }
    mesh->sealed = 2;
    g_DC_BatcherMeshCount++;
    g_DC_BatcherVertexCount += bind->vertex_count;
    Vector_Add(batcher->bindings, &bind);
    HASH_ADD_PTR(batcher->map, mesh, bind);
}

void MeshBatcher_RemoveMesh(MESH_BATCHER *const batcher, OUTPUT_MESH *const mesh)
{
    M_BINDING *const bind = M_GetBinding(batcher, mesh);
    if (bind == nullptr) {
        return;
    }
    g_DC_BatcherMeshCount--;
    g_DC_BatcherVertexCount -= bind->vertex_count;
    Memory_Free(bind->uv);
    Memory_Free(bind->page);
    Vector_Remove(batcher->bindings, &bind);
    HASH_DEL(batcher->map, bind);
    Memory_Free(bind);
}

void MeshBatcher_Seal(MESH_BATCHER *const batcher)
{
    int32_t vertices = 0;
    for (int32_t i = 0; i < batcher->bindings->count; i++) {
        const M_BINDING *const bind =
            *(M_BINDING **)Vector_Get(batcher->bindings, i);
        vertices += bind->vertex_count;
    }
    LOG_INFO(
        "%d meshes, %d vertices batched", batcher->bindings->count, vertices);
}

void MeshBatcher_UpdateMeshGeometry(
    const MESH_BATCHER *const batcher, const OUTPUT_MESH *const mesh)
{
    // The vertices are read from the mesh as each frame is drawn.
}

void MeshBatcher_Stage(
    MESH_BATCHER *const batcher, const MESH_INSTANCE *const inst,
    const SCENE_PASS pass)
{
    if (inst->mesh == nullptr) {
        return;
    }
    bool is_caster = false;
    if (m_Casting && pass == SCENE_PASS_OPAQUE
        && m_CasterCount < M_MAX_CASTERS) {
        const M_BINDING *const bind = M_GetBinding(batcher, inst->mesh);
        if (bind != nullptr && bind->vertex_count > 0) {
            M_CASTER *const caster = &m_Casters[m_CasterCount++];
            memcpy(caster->bounds, bind->bounds, sizeof(caster->bounds));
            M_WorldMatrix(&inst->wmatrix, caster->world);
            caster->room = inst->room;
            is_caster = true;
        }
    }
    Vector_Add(batcher->staged[pass], inst);
    Vector_Add(batcher->casters[pass], &is_caster);
}

void DC_Shadow_SetCasting(const bool casting)
{
    m_Casting = casting;
}

bool DC_Shadow_IsCasting(void)
{
    return m_Casting;
}

static void M_Transform34(
    const float m[3][4], const float in[3], float out[3])
{
    for (int32_t r = 0; r < 3; r++) {
        out[r] = m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2] + m[r][3];
    }
}

// Takes a light for the shadow if it reaches the casters' middle, lies within
// M_SHADOW_LIGHT_RANGE of it and is nearer than the best so far.
static void M_ConsiderLight(
    const float center[3], const float x, const float y, const float z,
    float reach, float *const best_dist_sq, float out[3])
{
    if (reach > M_SHADOW_LIGHT_RANGE) {
        reach = M_SHADOW_LIGHT_RANGE;
    }
    const float d[3] = { center[0] - x, center[1] - y, center[2] - z };
    const float dist_sq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (dist_sq < 1.0f || dist_sq > reach * reach || dist_sq >= *best_dist_sq) {
        return;
    }
    *best_dist_sq = dist_sq;
    const float inv = 1.0f / sqrtf(dist_sq);
    out[0] = d[0] * inv;
    out[1] = d[1] * inv;
    out[2] = d[2] * inv;
}

// Picks the direction the casters' shadows fall in: away from the light
// nearest their middle within reach, the room's own or a passing one (a
// muzzle flash, a flare), or straight down without one.
static void M_ShadowDirection(const float center[3], float out[3])
{
    out[0] = 0.0f;
    out[1] = 1.0f;
    out[2] = 0.0f;
    float best = M_SHADOW_LIGHT_RANGE * M_SHADOW_LIGHT_RANGE + 1.0f;

    const ROOM *const room = m_Casters[0].room;
    if (room != nullptr) {
        for (int32_t i = 0; i < room->num_lights; i++) {
            const LIGHT *const light = &room->lights[i];
            if (light->type == LIGHT_TYPE_SUN
                || light->type == LIGHT_TYPE_FOG_BULB) {
                continue;
            }
            const float reach = light->layout == LIGHT_LAYOUT_TR4
                ? (float)light->u.tr4.outer_radius
                : (float)light->u.legacy.falloff.value_1;
            M_ConsiderLight(
                center, light->pos.x, light->pos.y, light->pos.z, reach, &best,
                out);
        }
    }

    // The passing lights, as the shading has them this frame.
    Output_Uniforms_UploadRoomLights(Output_GetUniforms(), room);
    const DC_UNIFORM_LIGHTS *const lights =
        DC_GL_GetUniformBlock(DC_GL_BINDING_LIGHTS, nullptr);
    m_Frame.lights = lights;
    for (int32_t i = 0; lights != nullptr && i < lights->num_lights; i++) {
        const DC_UNIFORM_LIGHT *const light = &lights->lights[i];
        const float reach = light->kind == 0.0f
            ? exp2f(light->falloff)
            : light->falloff / (float)(1 << (OUTPUT_DYNAMIC_FALLOFF_SHIFT
                                             - OUTPUT_DYNAMIC_RADIUS_SHIFT));
        M_ConsiderLight(
            center, light->pos[0], light->pos[1], light->pos[2], reach, &best,
            out);
    }
    // TR's y axis points down.
    if (out[1] < M_SHADOW_MIN_DROP) {
        out[1] = M_SHADOW_MIN_DROP;
        const float inv = 1.0f
            / sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        out[0] *= inv;
        out[1] *= inv;
        out[2] *= inv;
    }
}

// Projects a world point; false when it lies before the near plane.
static bool M_ProjectWorld(const float p[3], float out[3])
{
    const float(*const v)[4] = m_Frame.view;
    const float x = v[0][0] * p[0] + v[1][0] * p[1] + v[2][0] * p[2] + v[3][0];
    const float y = v[0][1] * p[0] + v[1][1] * p[1] + v[2][1] * p[2] + v[3][1];
    const float z = v[0][2] * p[0] + v[1][2] * p[1] + v[2][2] * p[2] + v[3][2];
    if (z < m_Frame.near_z) {
        return false;
    }
    const float inv_z = 1.0f / z;
    out[0] = m_Frame.center_x + m_Frame.proj_x * x * inv_z;
    out[1] = m_Frame.center_y + m_Frame.proj_y * y * inv_z;
    out[2] = DC_PVR_Depth3D(m_Frame.near_z * inv_z);
    return true;
}

// A box's faces by corner (bit 0: x, bit 1: y, bit 2: z; set for the maximum).
static const int8_t m_BoxFaces[6][4] = {
    { 0, 2, 6, 4 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 },
    { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 5, 7, 6 },
};

// The volume a box throws: the faces towards the light where they are, the
// ones away from it pushed along the shadow, and the box's outline joining
// the two, which closes it.
static int32_t M_BuildVolume(
    const M_CASTER *const caster, const float dir[3],
    DC_PVR_TRIANGLE *const out)
{
    float near[8][3];
    float far[8][3];
    float corners[8][3];
    float center[3] = {};
    for (int32_t i = 0; i < 8; i++) {
        const float local[3] = {
            caster->bounds[(i >> 0) & 1][0],
            caster->bounds[(i >> 1) & 1][1],
            caster->bounds[(i >> 2) & 1][2],
        };
        M_Transform34(caster->world, local, corners[i]);
        for (int32_t k = 0; k < 3; k++) {
            center[k] += corners[i][k] / 8.0f;
        }
    }

    // A shadow falling towards the camera is cut short of the near plane: a
    // volume crossing it would not be closed.
    const float(*const v)[4] = m_Frame.view;
    const float dir_z = v[0][2] * dir[0] + v[1][2] * dir[1] + v[2][2] * dir[2];
    float length = M_SHADOW_LENGTH;
    for (int32_t i = 0; i < 8; i++) {
        const float *const p = corners[i];
        const float z =
            v[0][2] * p[0] + v[1][2] * p[1] + v[2][2] * p[2] + v[3][2];
        const float margin = m_Frame.near_z * 1.5f;
        if (z < margin) {
            return 0;
        }
        if (dir_z < 0.0f && z + dir_z * length < margin) {
            length = (margin - z) / dir_z;
        }
    }
    if (length < 32.0f) {
        return 0;
    }
    for (int32_t i = 0; i < 8; i++) {
        float pushed[3];
        for (int32_t k = 0; k < 3; k++) {
            pushed[k] = corners[i][k] + dir[k] * length;
        }
        if (!M_ProjectWorld(corners[i], near[i])
            || !M_ProjectWorld(pushed, far[i])) {
            return 0;
        }
    }

    bool lit[6];
    for (int32_t f = 0; f < 6; f++) {
        // The face's outward direction, from the box's middle to the face's.
        const int8_t *const q = m_BoxFaces[f];
        float world[4][3];
        float face_center[3] = {};
        for (int32_t j = 0; j < 4; j++) {
            const float local[3] = {
                caster->bounds[(q[j] >> 0) & 1][0],
                caster->bounds[(q[j] >> 1) & 1][1],
                caster->bounds[(q[j] >> 2) & 1][2],
            };
            M_Transform34(caster->world, local, world[j]);
            for (int32_t k = 0; k < 3; k++) {
                face_center[k] += world[j][k] / 4.0f;
            }
        }
        const float n_dot_d = (face_center[0] - center[0]) * dir[0]
            + (face_center[1] - center[1]) * dir[1]
            + (face_center[2] - center[2]) * dir[2];
        lit[f] = n_dot_d < 0.0f;
    }

    int32_t count = 0;
    for (int32_t f = 0; f < 6; f++) {
        const int8_t *const q = m_BoxFaces[f];
        float(*const cap)[3] = lit[f] ? near : far;
        const int8_t tris[2][3] = { { q[0], q[1], q[2] }, { q[0], q[2], q[3] } };
        for (int32_t t = 0; t < 2; t++) {
            for (int32_t j = 0; j < 3; j++) {
                memcpy(out[count].v[j], cap[tris[t][j]], sizeof(float) * 3);
            }
            count++;
        }
    }
    // The outline: edges between a face towards the light and one away.
    for (int32_t f = 0; f < 6; f++) {
        for (int32_t g = f + 1; g < 6; g++) {
            if (lit[f] == lit[g]) {
                continue;
            }
            int8_t shared[2];
            int32_t num_shared = 0;
            for (int32_t i = 0; i < 4; i++) {
                for (int32_t j = 0; j < 4; j++) {
                    if (m_BoxFaces[f][i] == m_BoxFaces[g][j]
                        && num_shared < 2) {
                        shared[num_shared++] = m_BoxFaces[f][i];
                    }
                }
            }
            if (num_shared != 2) {
                continue;
            }
            const int8_t a = shared[0];
            const int8_t b = shared[1];
            memcpy(out[count].v[0], near[a], sizeof(float) * 3);
            memcpy(out[count].v[1], near[b], sizeof(float) * 3);
            memcpy(out[count].v[2], far[b], sizeof(float) * 3);
            count++;
            memcpy(out[count].v[0], near[a], sizeof(float) * 3);
            memcpy(out[count].v[1], far[b], sizeof(float) * 3);
            memcpy(out[count].v[2], far[a], sizeof(float) * 3);
            count++;
        }
    }
    return count;
}

void DC_Shadow_DrawVolumes(void)
{
    if (m_CasterCount == 0) {
        return;
    }
    float center[3] = {};
    for (int32_t i = 0; i < m_CasterCount; i++) {
        float local[3];
        for (int32_t k = 0; k < 3; k++) {
            local[k] =
                (m_Casters[i].bounds[0][k] + m_Casters[i].bounds[1][k]) * 0.5f;
        }
        float world[3];
        M_Transform34(m_Casters[i].world, local, world);
        for (int32_t k = 0; k < 3; k++) {
            center[k] += world[k] / m_CasterCount;
        }
    }
    float dir[3];
    M_ShadowDirection(center, dir);

    // 12 cap triangles and at most 12 outline ones.
    DC_PVR_TRIANGLE tris[24];
    int32_t volumes = 0;
    for (int32_t i = 0; i < m_CasterCount; i++) {
        const int32_t count = M_BuildVolume(&m_Casters[i], dir, tris);
        DC_PVR_ShadowVolume(tris, count);
        volumes += count > 0;
    }
#if defined(TRX_DC_WATCHDOG)
    static int32_t frame = 0;
    static int32_t lit_frames = 0;
    lit_frames += dir[1] < 0.999f;
    const DC_UNIFORM_LIGHTS *const passing = m_Frame.lights;
    if (++frame % 120 == 0) {
        LOG_DEBUG(
            "shadow: %d casters, %d volumes, lit %d/120, room lights %d, "
            "passing %d, dir %.2f %.2f %.2f",
            m_CasterCount, volumes, lit_frames,
            m_Casters[0].room != nullptr ? m_Casters[0].room->num_lights : -1,
            passing != nullptr ? (int)passing->num_lights : -1, dir[0], dir[1],
            dir[2]);
        lit_frames = 0;
    }
#endif
    m_CasterCount = 0;
}

const SCENE_SOURCE *MeshBatcher_AsSource(const MESH_BATCHER *const batcher)
{
    return &batcher->source;
}
