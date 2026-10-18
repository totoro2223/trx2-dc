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
    UT_hash_handle hh;
} M_BINDING;

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
    float fog_start;
    float fog_scale;
    float fog_color[3];
    float min_shade;
    float time_in_game;
    float sunset_duration;
    bool lighting;
    bool fog;
    const DC_UNIFORM_LIGHTS *lights;
} M_FRAME;

static M_FRAME m_Frame = {};

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

static void M_PrepareRoomLights(
    const MESH_INSTANCE *const inst, M_INSTANCE_LIGHT *const out)
{
    out->has_room_lights = true;
    const float scale = 1.0f / (float)(1 << W2V_SHIFT);
    const MATRIX *const m = &inst->wmatrix;
    const int64_t rows[3][4] = {
        { m->_00, m->_01, m->_02, m->_03 },
        { m->_10, m->_11, m->_12, m->_13 },
        { m->_20, m->_21, m->_22, m->_23 },
    };
    for (int32_t r = 0; r < 3; r++) {
        for (int32_t c = 0; c < 4; c++) {
            out->world[r][c] = (float)rows[r][c] * scale;
        }
    }

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

static void M_EmitTriangle(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const uint32_t i0, const uint32_t i1, const uint32_t i2)
{
    const uint32_t idx[3] = { i0, i1, i2 };
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
            M_Project(
                &batcher->xverts[idx[k]], bind->uv[idx[k] * 2],
                bind->uv[idx[k] * 2 + 1], &out[k]);
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
            .u = bind->uv[idx[k] * 2],
            .v = bind->uv[idx[k] * 2 + 1],
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

static void M_EmitIndices(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const uint32_t *const indices, const int32_t count, const DC_BLEND blend)
{
    int32_t page = INT32_MIN;
    for (int32_t i = 0; i + 2 < count; i += 3) {
        const int32_t tri_page = bind->page[indices[i]];
        if (tri_page != page) {
            page = tri_page;
            DC_PVR_SetState(
                blend, page == M_NO_PAGE ? DC_PVR_NO_TEXTURE : page, true);
        }
        M_EmitTriangle(batcher, bind, indices[i], indices[i + 1], indices[i + 2]);
    }
}

static void M_DrawInstance(
    MESH_BATCHER *const batcher, const MESH_INSTANCE *const inst,
    const SCENE_PASS pass)
{
    const M_BINDING *const bind = M_GetBinding(batcher, inst->mesh);
    if (bind == nullptr || bind->vertex_count == 0) {
        return;
    }
    M_TransformInstance(batcher, inst, bind);

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
    VECTOR *const staged = batcher->staged[pass];
    for (int32_t i = 0; i < staged->count; i++) {
        M_DrawInstance(batcher, Vector_Get(staged, i), pass);
    }
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
    Vector_Add(batcher->staged[pass], inst);
}

const SCENE_SOURCE *MeshBatcher_AsSource(const MESH_BATCHER *const batcher)
{
    return &batcher->source;
}
