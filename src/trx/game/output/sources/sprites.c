#include <trx/game/output/sources/sprites.h>

#include <trx/core/memory.h>
#include <trx/core/utils.h>
#include <trx/game/output/const.h>
#include <trx/game/output/mesh_batcher/mesh_builder.h>
#include <trx/game/output/scene_compositor.h>
#include <trx/game/output/shaders/mesh.h>
#include <trx/game/output/state.h>
#include <trx/game/output/textures.h>

#if defined(__DREAMCAST__)
    #include <trx/config.h>
    #include <trx/core/math.h>
    #include <trx/game/camera.h>
    #include <trx/game/interpolation.h>
    #include <trx/game/matrix.h>
    #include <trx/game/output/sources/lightnings.h>
    #include <trx/game/output/sources/poly_fx.h>
    #include <trx/game/sparks/manager.h>
    #include <trx/core/log.h>
    #include <platform/dreamcast/dc_mode.h>
    #include <trx/version.h>
#endif

#include <math.h>
#include <string.h>

typedef struct {
    SCENE_SOURCE source;
    MESH_BATCHER *batcher;
    OUTPUT_MESH **meshes;
    OUTPUT_MESH **meshes_blend_add;
#if defined(__DREAMCAST__)
    // The sparks' quads (see OutputSource_PolyFX_StageSpark), by sprite
    // index + 1; 0 is the untextured quad of a flat spark.
    OUTPUT_MESH **spark_meshes;
    OUTPUT_MESH **spark_meshes_blended;
    // The world triangles' (see M_StageTri), by sprite + 1 (0 is the flat
    // one), corners and blend.
    OUTPUT_MESH **tri_meshes;
    // The triangles that carry their own coordinates (debris, bats, water
    // particles), by the three corners' values: a few at a time, the oldest
    // replaced.
    struct {
        OUTPUT_UVW key[3];
        bool blended;
        OUTPUT_MESH *mesh;
        uint32_t stamp;
    } uv_meshes[16];
    uint32_t uv_stamp;
#endif
    size_t mesh_count;

    MATRIX last_matrix;
    int32_t stack;
} M_PRIV;

static M_PRIV m_Priv;

static void M_RenderBegin(const SCENE_SOURCE *const src)
{
    M_PRIV *const p = &m_Priv;
    memset(&p->last_matrix, 0, sizeof(MATRIX));
    p->stack = 0;
}

static void M_AddSpriteMesh(
    MESH_BUILDER *const builder, const int32_t texture_idx,
    const SCENE_PASS pass, const uint16_t extra_flags, const bool depth_write)
{
    const SPRITE_TEXTURE *const sprite = Output_GetSpriteTexture(texture_idx);
    const struct {
        float x, y;
    } normal[4] = {
        { .x = sprite->x0, .y = sprite->y0 },
        { .x = sprite->x1, .y = sprite->y0 },
        { .x = sprite->x1, .y = sprite->y1 },
        { .x = sprite->x0, .y = sprite->y1 },
    };
    for (int32_t j = 0; j < 4; j++) {
        const OUTPUT_MESH_VERTEX vertex = {
            .pos = { .x = 0.0f, .y = 0.0f, .z = 0.0f, .w = 0.0f },
            .normal = { .x = normal[j].x, .y = normal[j].y, .z = 0.0f },
            .flags = Output_Textures_GetSpriteTextureFlags(texture_idx)
                | extra_flags,
            .color = { 255, 255, 255, 255 },
            .uvw_idx = Output_Textures_GetSpriteUVWIndex(texture_idx, j),
            .shade = 0,
#if !defined(__DREAMCAST__)
            .reflectivity = 1.0f,
            .trapezoid_ratio = { 1.0f, 1.0f },
#endif
        };
        MeshBuilder_AddVertex(builder, &vertex);
    }
    MeshBuilder_AddFan(builder, pass, false, depth_write);
}

static void M_PrepareMeshes(M_PRIV *const p)
{
    p->mesh_count = Output_GetSpriteTextureCount();
    p->meshes = Memory_Alloc(sizeof(*p->meshes) * p->mesh_count);
    p->meshes_blend_add =
        Memory_Alloc(sizeof(*p->meshes_blend_add) * p->mesh_count);
#if defined(__DREAMCAST__)
    // Built as they are first drawn; see M_GetMesh.
    p->spark_meshes =
        Memory_Alloc(sizeof(*p->spark_meshes) * (p->mesh_count + 1));
    p->spark_meshes_blended =
        Memory_Alloc(sizeof(*p->spark_meshes_blended) * (p->mesh_count + 1));
    return;
#endif
    MESH_BUILDER *const builder = MeshBuilder_Create();
    for (int32_t i = 0; i < (int32_t)p->mesh_count; i++) {
        // The original engines painter-sorted every sprite instead of using
        // a depth buffer, so sprites never occlude other blended geometry;
        // skipping the depth prepass reproduces that in the sorted
        // transparent pass.
        M_AddSpriteMesh(
            builder, i, SCENE_PASS_TRANSPARENT, VERT_USE_OWN_LIGHT, false);
        OUTPUT_MESH *const mesh_transparent = MeshBuilder_Seal(builder);
        MeshBatcher_AddMesh(p->batcher, mesh_transparent);
        p->meshes[i] = mesh_transparent;

        M_AddSpriteMesh(
            builder, i, SCENE_PASS_BLEND_ADD, VERT_USE_OWN_LIGHT, true);
        OUTPUT_MESH *const mesh_blend_add = MeshBuilder_Seal(builder);
        MeshBatcher_AddMesh(p->batcher, mesh_blend_add);
        p->meshes_blend_add[i] = mesh_blend_add;
    }
    MeshBuilder_Destroy(builder);
}

static void M_FreeMeshes(M_PRIV *const p)
{
    if (p->meshes != nullptr) {
        for (size_t i = 0; i < p->mesh_count; i++) {
            if (p->meshes[i] != nullptr) {
                MeshBatcher_RemoveMesh(p->batcher, p->meshes[i]);
                Output_Mesh_Destroy(p->meshes[i]);
            }
            if (p->meshes_blend_add[i] != nullptr) {
                MeshBatcher_RemoveMesh(p->batcher, p->meshes_blend_add[i]);
                Output_Mesh_Destroy(p->meshes_blend_add[i]);
            }
        }
        Memory_FreePointer(&p->meshes);
        Memory_FreePointer(&p->meshes_blend_add);
    }
#if defined(__DREAMCAST__)
    if (p->spark_meshes != nullptr) {
        for (size_t i = 0; i <= p->mesh_count; i++) {
            OUTPUT_MESH **const both[2] = {
                &p->spark_meshes[i],
                &p->spark_meshes_blended[i],
            };
            for (int32_t j = 0; j < 2; j++) {
                if (*both[j] != nullptr) {
                    MeshBatcher_RemoveMesh(p->batcher, *both[j]);
                    Output_Mesh_Destroy(*both[j]);
                }
            }
        }
        Memory_FreePointer(&p->spark_meshes);
        Memory_FreePointer(&p->spark_meshes_blended);
    }
    if (p->tri_meshes != nullptr) {
        for (size_t i = 0; i < (p->mesh_count + 1) * 8; i++) {
            if (p->tri_meshes[i] != nullptr) {
                MeshBatcher_RemoveMesh(p->batcher, p->tri_meshes[i]);
                Output_Mesh_Destroy(p->tri_meshes[i]);
            }
        }
        Memory_FreePointer(&p->tri_meshes);
    }
    for (size_t i = 0; i < ARRAY_SIZE(p->uv_meshes); i++) {
        if (p->uv_meshes[i].mesh != nullptr) {
            MeshBatcher_RemoveMesh(p->batcher, p->uv_meshes[i].mesh);
            Output_Mesh_Destroy(p->uv_meshes[i].mesh);
        }
    }
    memset(p->uv_meshes, 0, sizeof(p->uv_meshes));
#endif
}

void OutputSource_Sprites_Init(MESH_BATCHER *batcher)
{
    m_Priv.batcher = batcher;
    m_Priv.source.render_begin = M_RenderBegin;
    SceneCompositor_AddSource(&m_Priv.source);
}

void OutputSource_Sprites_Shutdown(void)
{
    M_FreeMeshes(&m_Priv);
}

void OutputSource_Sprites_ObserveLevelLoad(void)
{
    M_FreeMeshes(&m_Priv);
    M_PrepareMeshes(&m_Priv);
}

void OutputSource_Sprites_ObserveLevelUnload(void)
{
    M_FreeMeshes(&m_Priv);
}

#if defined(__DREAMCAST__)
// Builds a sprite's mesh the first time it is drawn: a level names hundreds
// of sprites, and holding a mesh for each would not fit the Dreamcast's
// memory.
static OUTPUT_MESH *M_GetMesh(
    M_PRIV *const p, const int32_t sprite_idx, const bool blend_add)
{
    OUTPUT_MESH **const slot =
        blend_add ? &p->meshes_blend_add[sprite_idx] : &p->meshes[sprite_idx];
    if (*slot == nullptr) {
        MESH_BUILDER *const builder = MeshBuilder_Create();
        if (blend_add) {
            M_AddSpriteMesh(
                builder, sprite_idx, SCENE_PASS_BLEND_ADD, VERT_USE_OWN_LIGHT,
                true);
        } else {
            M_AddSpriteMesh(
                builder, sprite_idx, SCENE_PASS_TRANSPARENT,
                VERT_USE_OWN_LIGHT, false);
        }
        *slot = MeshBuilder_Seal(builder);
        MeshBuilder_Destroy(builder);
        MeshBatcher_AddMesh(p->batcher, *slot);
    }
    return *slot;
}

// A spark's quad: corners one unit out, sized by the instance's scale, with
// a sprite's texture or none (a flat spark). Blended faces go where the
// batcher draws both the added and the subtracted ones.
static OUTPUT_MESH *M_GetSparkMesh(
    M_PRIV *const p, const int32_t sprite_idx, const bool blended)
{
    OUTPUT_MESH **const slot = blended
        ? &p->spark_meshes_blended[sprite_idx + 1]
        : &p->spark_meshes[sprite_idx + 1];
    if (*slot != nullptr) {
        return *slot;
    }
    static const float corner[4][2] = {
        { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f },
    };
    MESH_BUILDER *const builder = MeshBuilder_Create();
    for (int32_t j = 0; j < 4; j++) {
        uint16_t flags = VERT_NO_LIGHTING | VERT_NO_WIBBLE | VERT_BILLBOARD;
        int32_t uvw_idx = -1;
        if (sprite_idx < 0) {
            flags |= VERT_FLAT_SHADED;
        } else {
            flags |= Output_Textures_GetSpriteTextureFlags(sprite_idx);
            uvw_idx = Output_Textures_GetSpriteUVWIndex(sprite_idx, j);
        }
        const OUTPUT_MESH_VERTEX vertex = {
            .normal = { .x = corner[j][0], .y = corner[j][1], .z = 0.0f },
            .flags = flags,
            .color = { 255, 255, 255, 255 },
            .uvw_idx = uvw_idx,
        };
        MeshBuilder_AddVertex(builder, &vertex);
    }
    MeshBuilder_AddFan(
        builder, blended ? SCENE_PASS_BLEND_ADD : SCENE_PASS_TRANSPARENT,
        false, blended);
    *slot = MeshBuilder_Seal(builder);
    MeshBuilder_Destroy(builder);
    MeshBatcher_AddMesh(p->batcher, *slot);
    return *slot;
}

// The camera's right and down, as unit vectors in the world (the view matrix's
// first two rows over the 1 << W2V_SHIFT of its scale).
// The view matrix in floats, made again when it changes: a spark converts
// nothing but its own numbers (the 64-bit products of the fixed point cost
// tens of microseconds each on the SH-4).
static struct {
    bool valid;
    MATRIX source;
    float right[3];
    float down[3];
    float depth[4]; // the third row, in the matrix's own scale
    float near_z;
    float far_z;
} m_View;

static void M_RefreshView(void)
{
    if (m_View.valid && memcmp(&m_View.source, &g_ViewMatrix, sizeof(MATRIX)) == 0) {
        return;
    }
    const MATRIX *const v = &g_ViewMatrix;
    const float k = 1.0f / (float)(1 << W2V_SHIFT);
    m_View.right[0] = (float)v->_00 * k;
    m_View.right[1] = (float)v->_01 * k;
    m_View.right[2] = (float)v->_02 * k;
    m_View.down[0] = (float)v->_10 * k;
    m_View.down[1] = (float)v->_11 * k;
    m_View.down[2] = (float)v->_12 * k;
    m_View.depth[0] = (float)v->_20;
    m_View.depth[1] = (float)v->_21;
    m_View.depth[2] = (float)v->_22;
    m_View.depth[3] = (float)v->_23;
    m_View.near_z = (float)Output_GetNearZ();
    m_View.far_z = (float)Output_GetFarZ();
    m_View.source = *v;
    m_View.valid = true;
}

static void M_CameraAxes(float right[3], float down[3])
{
    M_RefreshView();
    for (int32_t i = 0; i < 3; i++) {
        right[i] = m_View.right[i];
        down[i] = m_View.down[i];
    }
}

// A quad that faces the camera: each corner is a point of the world moved by
// its displacement along the camera's right and down, in world units (what
// the desktop's billboard shader does with the same numbers).
static void M_ExpandBillboard(
    const XYZ_32 pos[4], const float disp[4][2], XYZ_32 out[4])
{
    float right[3];
    float down[3];
    M_CameraAxes(right, down);
    for (int32_t i = 0; i < 4; i++) {
        const float dx = disp[i][0];
        const float dy = disp[i][1];
        out[i] = (XYZ_32) {
            .x = pos[i].x + (int32_t)(right[0] * dx + down[0] * dy),
            .y = pos[i].y + (int32_t)(right[1] * dx + down[1] * dy),
            .z = pos[i].z + (int32_t)(right[2] * dx + down[2] * dy),
        };
    }
}

// The desktop's PolyFX renderer is not built on the Dreamcast: a spark (TR3's
// blood, fire, smoke, splashes) is drawn as a sprite is, a quad facing the
// camera, its colour the instance's tint.
// A quad of the spark's own width and height, turned when it rotates.
static void M_StageSpark(const SPARK *spark);

void OutputSource_PolyFX_StageSpark(const SPARK *const spark)
{
    M_StageSpark(spark);
}

static void M_StageSpark(const SPARK *const spark)
{
    M_PRIV *const p = &m_Priv;
    if (spark == nullptr || !spark->on || p->batcher == nullptr) {
        return;
    }
    XYZ_32 pos = Sparks_GetWorldPos(spark);
    // Between ticks a free spark (one the world holds, not an item, an effect
    // or a joint) moves from where it was to where it is, as the desktop does.
    constexpr uint16_t M_ATTACHED =
        SPARK_F_ATTACHED_NODE | SPARK_F_FX | SPARK_F_ITEM;
    if ((spark->flags & M_ATTACHED) == 0U
        && (int32_t)spark->s_life - (int32_t)spark->life > 1) {
        const float rate = (float)Interpolation_GetWorldRate();
        pos = (XYZ_32) {
            .x = (int32_t)LERP(spark->prev_pos.x, spark->pos.x, rate),
            .y = (int32_t)LERP(spark->prev_pos.y, spark->pos.y, rate),
            .z = (int32_t)LERP(spark->prev_pos.z, spark->pos.z, rate),
        };
    }
    M_RefreshView();
    const float zv = m_View.depth[0] * (float)pos.x
        + m_View.depth[1] * (float)pos.y + m_View.depth[2] * (float)pos.z
        + m_View.depth[3];
    if (zv <= m_View.near_z || zv >= m_View.far_z) {
            return;
    }
    int32_t vpos_z = (int32_t)(zv * (1.0f / (float)(1 << W2V_SHIFT)));
    if (vpos_z == 0) {
        vpos_z = 1;
    }

    int32_t sw = spark->size.width;
    int32_t sh = spark->size.height;
    if ((spark->flags & SPARK_F_SCALE) != 0U) {
        const float k =
            (float)REF_PERSP * (float)(1 << spark->scalar) / (float)vpos_z;
        sw = (int32_t)((float)sw * k);
        sh = (int32_t)((float)sh * k);
    }
    const float half = (sw + sh) / 4.0f * (float)vpos_z / (float)REF_PERSP;
    if (half < 1.0f) {
            return;
    }

    DRAW_TYPE draw_type = spark->draw_type;
    int32_t sprite_idx = spark->sprite_idx;
    RGBA_F tint = {
        .r = spark->color.r / 255.0f,
        .g = spark->color.g / 255.0f,
        .b = spark->color.b / 255.0f,
        .a = 1.0f,
    };
    if ((spark->flags & SPARK_F_SPRITE) == 0U) {
        // As the desktop draws a flat spark: half blended, never added.
        sprite_idx = -1;
        if (draw_type == DRAW_BLEND_ADD
            || draw_type == DRAW_REFLECTIVE_BLEND_ADD) {
            tint.a = 0.5f;
        } else if (
            spark->context == SPARK_CONTEXT_BLOOD
            && g_Config.rendering.enable_ps1_blending) {
            tint.a = 0.5f;
        }
        draw_type = DRAW_BLEND;
    } else if (sprite_idx < 0 || sprite_idx >= (int32_t)p->mesh_count) {
            return;
    }

    // The four corners in the camera's plane, as the desktop gives them,
    // turned about the spark's centre when it rotates: a quad of the spark's
    // own width and height (not a square of their mean), as world quads that
    // the triangle path draws.
    const float w = ((sw / 2.0f) * (float)vpos_z) / (float)REF_PERSP;
    const float h = ((sh / 2.0f) * (float)vpos_z) / (float)REF_PERSP;
    float rot_sn = 0.0f;
    float rot_cs = 1.0f;
    if ((spark->flags & SPARK_F_ROTATE) != 0U) {
        const bool current =
            (int32_t)spark->s_life - (int32_t)spark->life <= 1;
        const int32_t rot_angle = current
            ? (int32_t)spark->rot_angle
            : Math_AngleMean(
                  (int32_t)spark->prev_rot_angle, (int32_t)spark->rot_angle,
                  Interpolation_GetWorldRate());
        const int32_t angle = rot_angle * DEG_360 / 0xFFF;
        rot_sn = Math_Sin(angle) / (float)(1 << W2V_SHIFT);
        rot_cs = Math_Cos(angle) / (float)(1 << W2V_SHIFT);
    }
    // TR3's spark colours are on the 128-neutral scale the desktop's shader
    // doubles; a smoke that starts at black and rises to 32 or 64 came out too
    // dim to see.
    const float gain = g_TRVersion == 3 && sprite_idx >= 0 ? 2.0f : 1.0f;
    const float blend_k = tint.a < 1.0f ? tint.a : 1.0f;
    const uint32_t cr = (uint32_t)MIN(255.0f, tint.r * 255.0f * gain * blend_k);
    const uint32_t cg = (uint32_t)MIN(255.0f, tint.g * 255.0f * gain * blend_k);
    const uint32_t cb = (uint32_t)MIN(255.0f, tint.b * 255.0f * gain * blend_k);
    // A flat spark that was blended at half its colour is added at half.
    if (sprite_idx < 0 && tint.a < 1.0f) {
        draw_type = DRAW_BLEND_ADD;
    }
    // One quad of the batcher's particle list, as the PSX draws it: no mesh
    // instance and no matrix. The sprite's corners are swapped (1 and 3) to the
    // particle's winding.
    DC_PARTICLE q = {};
    q.pos[0] = (float)pos.x;
    q.pos[1] = (float)pos.y;
    q.pos[2] = (float)pos.z;
    q.half = w;
    q.half_h = h;
    q.cs = 1.0f;
    if ((spark->flags & SPARK_F_ROTATE) != 0U) {
        q.sn = rot_sn;
        q.cs = rot_cs;
    }
    q.blend = draw_type == DRAW_BLEND_ADD
            || draw_type == DRAW_REFLECTIVE_BLEND_ADD
        ? 0
        : (draw_type == DRAW_BLEND_SUB ? 2 : 1);
    q.argb = 0xFF000000u | (cr << 16) | (cg << 8) | cb;
    if (sprite_idx < 0) {
        q.page = (-1) /* DC_PVR_NO_TEXTURE */;
    } else {
        static const int8_t k_swap[4] = { 0, 3, 2, 1 };
        for (int32_t k = 0; k < 4; k++) {
            const OUTPUT_UVW uvw = Output_Textures_GetUVW(
                Output_Textures_GetSpriteUVWIndex(sprite_idx, k_swap[k]));
            q.u[k] = uvw.u;
            q.v[k] = uvw.v;
            q.page = (int8_t)uvw.w;
        }
    }
    DC_MeshBatcher_StageParticle(p->batcher, &q);
}

// How far a quad is pulled towards the eye, as a fraction of its depth: the
// desktop adds z_depth_adjust to the clip space depth, about that over twice
// the near plane of the distance. It keeps a footprint off the floor it lies
// on, and puts the sun's flare over everything (at most three near planes from
// the eye, past which it would be behind the near plane).
static float M_DepthPull(const XYZ_32 *const pt, const float z_depth_adjust)
{
    const float near_w = (float)(Output_GetNearZ() >> W2V_SHIFT);
    float pull = MAX(0.0f, -z_depth_adjust) / (2.0f * near_w);
    if (pull > 0.0f) {
        M_RefreshView();
        const float depth = (m_View.depth[0] * (float)pt->x
                             + m_View.depth[1] * (float)pt->y
                             + m_View.depth[2] * (float)pt->z
                             + m_View.depth[3])
            * (1.0f / (float)(1 << W2V_SHIFT));
        const float limit =
            depth > 3.0f * near_w ? 1.0f - 3.0f * near_w / depth : 0.0f;
        pull = MIN(pull, limit);
    }
    return pull;
}

static int32_t M_BlendClass(const DRAW_TYPE draw_type)
{
    if (draw_type == DRAW_BLEND_ADD || draw_type == DRAW_REFLECTIVE_BLEND_ADD) {
        return 0;
    }
    return draw_type == DRAW_BLEND_SUB ? 2 : 1;
}

static uint32_t M_Argb(const RGBA_8888 c)
{
    return 0xFF000000u | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

// n points of the world (3 or 4) with a picture's corners, one colour per
// point; sprite_idx < 0 is flat. uvw, when given, are the corners' own
// coordinates in place of the sprite's.
static void M_StageQuadPts(
    const int32_t n, const XYZ_32 *const pts, const int32_t sprite_idx,
    const int8_t *const corner, const OUTPUT_UVW *const uvw,
    const RGBA_8888 *const color, const float z_depth_adjust,
    const DRAW_TYPE draw_type)
{
    M_PRIV *const p = &m_Priv;
    if (p->batcher == nullptr) {
        return;
    }
    DC_QUAD q = {};
    q.n = n;
    q.blend = M_BlendClass(draw_type);
    q.page = -1;
    q.pull = M_DepthPull(&pts[0], z_depth_adjust);
    for (int32_t k = 0; k < n; k++) {
        q.p[k][0] = (float)pts[k].x;
        q.p[k][1] = (float)pts[k].y;
        q.p[k][2] = (float)pts[k].z;
        q.argb[k] = M_Argb(color[k]);
        OUTPUT_UVW t;
        if (uvw != nullptr) {
            t = uvw[k];
        } else if (sprite_idx >= 0) {
            t = Output_Textures_GetUVW(
                Output_Textures_GetSpriteUVWIndex(sprite_idx, corner[k]));
        } else {
            continue;
        }
        q.u[k] = t.u;
        q.v[k] = t.v;
        q.page = (int8_t)t.w;
    }
    DC_MeshBatcher_StageQuad(p->batcher, &q);
}

static void M_StageTri(
    const int32_t sprite_idx, const XYZ_32 pts[3], const int32_t half,
    const RGBA_8888 color[3], const float z_depth_adjust,
    const DRAW_TYPE draw_type)
{
    static const int8_t k_corners[3][3] = { { 0, 1, 2 }, { 0, 2, 3 },
                                            { 0, 1, 3 } };
    if (sprite_idx >= (int32_t)m_Priv.mesh_count) {
        return;
    }
    M_StageQuadPts(
        3, pts, sprite_idx, k_corners[half], nullptr, color, z_depth_adjust,
        draw_type);
}

void OutputSource_PolyFX_StageSpriteTriWorldDepth(
    const int32_t sprite_idx, const XYZ_32 world_pos[3],
    const RGBA_8888 color[3], const float z_depth_adjust,
    const DRAW_TYPE draw_type)
{
    M_StageTri(sprite_idx, world_pos, 2, color, z_depth_adjust, draw_type);
}

void OutputSource_PolyFX_StageSpriteTriWorld(
    const int32_t sprite_idx, const XYZ_32 world_pos[3],
    const RGBA_8888 color[3], const DRAW_TYPE draw_type)
{
    M_StageTri(sprite_idx, world_pos, 2, color, 0.0f, draw_type);
}

void OutputSource_PolyFX_StageSpriteTriWorldLight(
    const int32_t sprite_idx, const XYZ_32 world_pos[3],
    const RGBA_8888 color[3], const OUTPUT_LIGHT_INFO light_info,
    const DRAW_TYPE draw_type)
{
    M_StageTri(sprite_idx, world_pos, 2, color, 0.0f, draw_type);
}

void OutputSource_PolyFX_StageSpriteQuadWorldDepth(
    const int32_t sprite_idx, const XYZ_32 world_pos[4],
    const RGBA_8888 color[4], const float z_depth_adjust,
    const DRAW_TYPE draw_type)
{
    static const int8_t k_corners[4] = { 0, 1, 2, 3 };
    if (sprite_idx >= (int32_t)m_Priv.mesh_count) {
        return;
    }
    M_StageQuadPts(
        4, world_pos, sprite_idx, k_corners, nullptr, color, z_depth_adjust,
        draw_type);
}

void OutputSource_PolyFX_StageSpriteQuadWorld(
    const int32_t sprite_idx, const XYZ_32 world_pos[4],
    const RGBA_8888 color[4], const DRAW_TYPE draw_type)
{
    OutputSource_PolyFX_StageSpriteQuadWorldDepth(
        sprite_idx, world_pos, color, 0.0f, draw_type);
}

// The desktop's extended PolyFX prims carry a screen-space displacement per
// corner (ring and wake widths, flare sizes) and shader flags; here they are
// the plain world quads and triangles above.
// shortcut: the displacements and flags are dropped, and a textured triangle
// (debris, bats, water particles) is flat in the mean of its corners; upgrade
// where rings, wakes or flares come out the wrong size.
void OutputSource_PolyFX_StageQuadExtDepth(
    const int32_t sprite_idx, const XYZ_32 world_pos[4],
    const float disp[4][2], const RGBA_8888 color[4], const uint16_t flags,
    const float z_depth_adjust, const DRAW_TYPE draw_type)
{
    // A billboard comes as four equal points and the size apart, in the
    // camera's plane (the sun's lens flares, the snow): opened out here.
    if (disp != nullptr && (flags & VERT_BILLBOARD) != 0) {
        XYZ_32 corners[4];
        M_ExpandBillboard(world_pos, disp, corners);
        OutputSource_PolyFX_StageSpriteQuadWorldDepth(
            sprite_idx, corners, color, z_depth_adjust, draw_type);
        return;
    }
    OutputSource_PolyFX_StageSpriteQuadWorldDepth(
        sprite_idx, world_pos, color, z_depth_adjust, draw_type);
}

#if defined(__DREAMCAST__)
// One quad of the sprite, added, at a world point: the PSX's particle
// (TOMB3.EXE 0x73560) rather than two instances of a mesh and a matrix each.
void OutputSource_PolyFX_StageParticle(
    const int32_t sprite_idx, const XYZ_32 *const pos, const float half,
    const RGBA_8888 color)
{
    M_PRIV *const p = &m_Priv;
    if (sprite_idx < 0 || p->batcher == nullptr) {
        return;
    }
    // The picture's corners, looked up once for a run of the same sprite.
    static int32_t s_sprite = -1;
    static DC_PARTICLE s_proto;
    if (sprite_idx != s_sprite) {
        for (int32_t k = 0; k < 4; k++) {
            const OUTPUT_UVW uvw = Output_Textures_GetUVW(
                Output_Textures_GetSpriteUVWIndex(sprite_idx, k));
            s_proto.u[k] = uvw.u;
            s_proto.v[k] = uvw.v;
            s_proto.page = (int8_t)uvw.w;
        }
        s_sprite = sprite_idx;
    }
    DC_PARTICLE q = s_proto;
    q.pos[0] = (float)pos->x;
    q.pos[1] = (float)pos->y;
    q.pos[2] = (float)pos->z;
    q.half = half;
    q.half_h = half;
    q.sn = 0.0f;
    q.cs = 1.0f;
    q.blend = 0;
    q.argb = 0xFF000000u | ((uint32_t)color.r << 16) | ((uint32_t)color.g << 8)
        | color.b;
    DC_MeshBatcher_StageParticle(p->batcher, &q);
}

void OutputSource_PolyFX_StageStreak(
    const XYZ_32 from, const RGBA_8888 from_color, const XYZ_32 to,
    const RGBA_8888 to_color)
{
    M_PRIV *const p = &m_Priv;
    if (p->batcher == nullptr) {
        return;
    }
    const DC_STREAK s = {
        .a = { (float)from.x, (float)from.y, (float)from.z },
        .b = { (float)to.x, (float)to.y, (float)to.z },
        .argb_a = 0xFF000000u | ((uint32_t)from_color.r << 16)
            | ((uint32_t)from_color.g << 8) | from_color.b,
        .argb_b = 0xFF000000u | ((uint32_t)to_color.r << 16)
            | ((uint32_t)to_color.g << 8) | to_color.b,
    };
    DC_MeshBatcher_StageStreak(p->batcher, &s);
}
#endif

void OutputSource_PolyFX_StageQuadExt(
    const int32_t sprite_idx, const XYZ_32 world_pos[4],
    const float disp[4][2], const RGBA_8888 color[4], const uint16_t flags,
    const DRAW_TYPE draw_type)
{
    OutputSource_PolyFX_StageQuadExtDepth(
        sprite_idx, world_pos, disp, color, flags, 0.0f, draw_type);
}

static void M_StageTriExtUV(
    const XYZ_32 world_pos[3], const OUTPUT_UVW uvw[3], const float disp[3][2],
    const RGBA_8888 color[3], const uint16_t flags, const float z_depth_adjust,
    const DRAW_TYPE draw_type)
{
    // A billboard comes as equal points and the size apart: opened out.
    XYZ_32 pts[3] = { world_pos[0], world_pos[1], world_pos[2] };
    if (disp != nullptr && (flags & VERT_BILLBOARD) != 0) {
        const XYZ_32 in[4] = { pts[0], pts[1], pts[2], pts[2] };
        const float d[4][2] = {
            { disp[0][0], disp[0][1] },
            { disp[1][0], disp[1][1] },
            { disp[2][0], disp[2][1] },
            { disp[2][0], disp[2][1] },
        };
        XYZ_32 out[4];
        M_ExpandBillboard(in, d, out);
        pts[0] = out[0];
        pts[1] = out[1];
        pts[2] = out[2];
    }
    M_StageQuadPts(3, pts, -1, nullptr, uvw, color, z_depth_adjust, draw_type);
}

void OutputSource_PolyFX_StageTriExtUVDepth(
    const XYZ_32 world_pos[3], const OUTPUT_UVW uvw[3],
    const OUTPUT_TEXTURE_SIZE texture_size[3], const float disp[3][2],
    const RGBA_8888 color[3], const uint16_t flags, const float z_depth_adjust,
    const DRAW_TYPE draw_type)
{
    M_StageTriExtUV(
        world_pos, uvw, disp, color, flags, z_depth_adjust, draw_type);
}

void OutputSource_PolyFX_StageTriExtUV(
    const XYZ_32 world_pos[3], const OUTPUT_UVW uvw[3],
    const OUTPUT_TEXTURE_SIZE texture_size[3], const float disp[3][2],
    const RGBA_8888 color[3], const uint16_t flags, const DRAW_TYPE draw_type)
{
    M_StageTriExtUV(world_pos, uvw, disp, color, flags, 0.0f, draw_type);
}

// A strip of half_width on each side, turned to face the camera: two flat
// triangles.
void OutputSource_PolyFX_StageLineSegment(
    const XYZ_32 from, const RGBA_8888 from_color, const XYZ_32 to,
    const RGBA_8888 to_color, const float half_width, const DRAW_TYPE draw_type)
{
    const float dx = (float)(to.x - from.x);
    const float dy = (float)(to.y - from.y);
    const float dz = (float)(to.z - from.z);
    const float vx = (float)((from.x + to.x) / 2 - g_Camera.pos.x);
    const float vy = (float)((from.y + to.y) / 2 - g_Camera.pos.y);
    const float vz = (float)((from.z + to.z) / 2 - g_Camera.pos.z);
    // The side is the direction crossed with the view.
    float sx = dy * vz - dz * vy;
    float sy = dz * vx - dx * vz;
    float sz = dx * vy - dy * vx;
    const float len = sqrtf(sx * sx + sy * sy + sz * sz);
    if (len < 0.00001f) {
        return;
    }
    // Half a screen pixel at least: a strip of a world unit is lost between
    // the PowerVR's pixels (and its corners, cut to whole units, collapse).
    const float dist = sqrtf(vx * vx + vy * vy + vz * vz);
    const float half = MAX(half_width, dist / (float)REF_PERSP);
    const float k = half / len;
    sx *= k;
    sy *= k;
    sz *= k;
    const XYZ_32 p[4] = {
        { from.x - (int32_t)lrintf(sx), from.y - (int32_t)lrintf(sy),
          from.z - (int32_t)lrintf(sz) },
        { from.x + (int32_t)lrintf(sx), from.y + (int32_t)lrintf(sy),
          from.z + (int32_t)lrintf(sz) },
        { to.x + (int32_t)lrintf(sx), to.y + (int32_t)lrintf(sy),
          to.z + (int32_t)lrintf(sz) },
        { to.x - (int32_t)lrintf(sx), to.y - (int32_t)lrintf(sy),
          to.z - (int32_t)lrintf(sz) },
    };
    const RGBA_8888 c[4] = { from_color, from_color, to_color, to_color };
    OutputSource_PolyFX_StageSpriteQuadWorldDepth(-1, p, c, 0.0f, draw_type);
}

// A bolt of lightning, as the desktop draws it: a blue glow with a white
// core, added to the scene.
// shortcut: two strips, not the four-quad cross the desktop builds.
void OutputSource_Lightnings_StageSegment(const LIGHTNING_SEGMENT *const segment)
{
    const RGBA_8888 blue = { 0, 0, 255, 128 };
    const RGBA_8888 white = { 255, 255, 255, 128 };
    const float w = segment->thickness / 2.0f;
    OutputSource_PolyFX_StageLineSegment(
        segment->from, blue, segment->to, blue, w, DRAW_BLEND_ADD);
    OutputSource_PolyFX_StageLineSegment(
        segment->from, white, segment->to, white, w / 4.0f, DRAW_BLEND_ADD);
}
#endif

void OutputSource_Sprites_Stage(
    const int32_t sprite_idx, const int16_t shade, const RGBA_F tint,
    const DRAW_TYPE draw_type)
{
    M_PRIV *const p = &m_Priv;
#if defined(__DREAMCAST__)
    const bool is_blend_add =
        draw_type == DRAW_BLEND_ADD || draw_type == DRAW_REFLECTIVE_BLEND_ADD;
    // Only the variant drawn is built; the blend path below reads its slot.
    OUTPUT_MESH *mesh = M_GetMesh(p, sprite_idx, is_blend_add);
#else
    OUTPUT_MESH *mesh = p->meshes[sprite_idx];
#endif

    if (memcmp(&p->last_matrix, g_WMatrixPtr, sizeof(MATRIX)) == 0) {
        p->stack++;
    } else {
        p->stack = 0;
    }
    p->last_matrix = *g_WMatrixPtr;

    float tr3_mul = 2.0f - (shade / (float)SHADE_NEUTRAL);
    CLAMP(tr3_mul, 0.0f, 1.0f);
    const OUTPUT_LIGHT_INFO light_info = {
        .ls_adder = shade,
        .ls_divider = 0,
        .ls_vector_view = {},
        .tr3_ambient = { tr3_mul, tr3_mul, tr3_mul },
        .tr3_light_color = {},
        .tr3_light_dir_view = {},
    };

    MESH_INSTANCE inst = {
        .mesh = mesh,
        .cwmatrix = *g_MatrixPtr,
        .wmatrix = *g_WMatrixPtr,
        .depth_adjust = p->stack * -0.005f,
        .tint = tint,
        .wibble = false,
        .water_effect = 0,
        .room = Output_GetCurrentRoom(),
        .light_info = light_info,
    };

    if (draw_type == DRAW_BLEND_ADD || draw_type == DRAW_REFLECTIVE_BLEND_ADD) {
        inst.mesh = p->meshes_blend_add[sprite_idx];
        MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_BLEND_ADD);
    } else {
        MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_OPAQUE);
        MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_TRANSPARENT);
    }
}
