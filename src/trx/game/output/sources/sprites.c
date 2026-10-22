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
    #include <trx/game/matrix.h>
    #include <trx/game/output/sources/poly_fx.h>
    #include <trx/game/sparks/manager.h>
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
    // The world triangles' (see M_StageTri), by sprite, corners and blend.
    OUTPUT_MESH **tri_meshes;
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
    p->tri_meshes = Memory_Alloc(sizeof(*p->tri_meshes) * p->mesh_count * 4);
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
        for (size_t i = 0; i < p->mesh_count * 4; i++) {
            if (p->tri_meshes[i] != nullptr) {
                MeshBatcher_RemoveMesh(p->batcher, p->tri_meshes[i]);
                Output_Mesh_Destroy(p->tri_meshes[i]);
            }
        }
        Memory_FreePointer(&p->tri_meshes);
    }
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

// The desktop's PolyFX renderer is not built on the Dreamcast: a spark (TR3's
// blood, fire, smoke, splashes) is drawn as a sprite is, a quad facing the
// camera, its colour the instance's tint.
// shortcut: no interpolation between logic frames, no rotation, and a square
// of the mean of the spark's width and height; upgrade if smoke looks wrong
// or at 60 fps.
void OutputSource_PolyFX_StageSpark(const SPARK *const spark)
{
    M_PRIV *const p = &m_Priv;
    if (spark == nullptr || !spark->on || p->spark_meshes == nullptr) {
        return;
    }
    const XYZ_32 pos = Sparks_GetWorldPos(spark);
    const MATRIX *const v = &g_ViewMatrix;
    const int64_t zv = v->_20 * pos.x + v->_21 * pos.y + v->_22 * pos.z + v->_23;
    if (zv <= Output_GetNearZ() || zv >= Output_GetFarZ()) {
        return;
    }
    int32_t vpos_z = (int32_t)(zv >> W2V_SHIFT);
    if (vpos_z == 0) {
        vpos_z = 1;
    }

    int32_t sw = spark->size.width;
    int32_t sh = spark->size.height;
    if ((spark->flags & SPARK_F_SCALE) != 0U) {
        sw = (int32_t)((((int64_t)sw * REF_PERSP) << spark->scalar) / vpos_z);
        sh = (int32_t)((((int64_t)sh * REF_PERSP) << spark->scalar) / vpos_z);
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

    const bool add =
        draw_type == DRAW_BLEND_ADD || draw_type == DRAW_REFLECTIVE_BLEND_ADD;
    const bool sub = draw_type == DRAW_BLEND_SUB;
    Matrix_Push();
    Matrix_TranslateAbs(pos.x, pos.y, pos.z);
    Matrix_Scale((int32_t)(half * (1 << W2V_SHIFT)));
    MESH_INSTANCE inst = {
        .mesh = M_GetSparkMesh(p, sprite_idx, add || sub),
        .cwmatrix = *g_MatrixPtr,
        .wmatrix = *g_WMatrixPtr,
        .tint = tint,
        .room = Output_GetCurrentRoom(),
    };
    Matrix_Pop();
    if (add) {
        MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_BLEND_ADD);
    } else if (sub) {
        MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_BLEND_SUB);
    } else {
        MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_OPAQUE);
        MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_TRANSPARENT);
    }
}

// The unit triangle with a sprite's corners (0, 1, 2 or 0, 2, 3, a quad's
// two halves), double sided.
static OUTPUT_MESH *M_GetTriMesh(
    M_PRIV *const p, const int32_t sprite_idx, const int32_t half,
    const bool blended)
{
    OUTPUT_MESH **const slot =
        &p->tri_meshes[(sprite_idx * 2 + half) * 2 + blended];
    if (*slot != nullptr) {
        return *slot;
    }
    static const int32_t corners[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    static const float unit[3][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
    MESH_BUILDER *const builder = MeshBuilder_Create();
    for (int32_t j = 0; j < 3; j++) {
        const OUTPUT_MESH_VERTEX vertex = {
            .pos = { .x = unit[j][0], .y = unit[j][1], .z = 0.0f },
            .flags = VERT_NO_LIGHTING | VERT_NO_WIBBLE
                | Output_Textures_GetSpriteTextureFlags(sprite_idx),
            .color = { 255, 255, 255, 255 },
            .uvw_idx =
                Output_Textures_GetSpriteUVWIndex(sprite_idx, corners[half][j]),
        };
        MeshBuilder_AddVertex(builder, &vertex);
    }
    MeshBuilder_AddFan(
        builder, blended ? SCENE_PASS_BLEND_ADD : SCENE_PASS_TRANSPARENT, true,
        blended);
    *slot = MeshBuilder_Seal(builder);
    MeshBuilder_Destroy(builder);
    MeshBatcher_AddMesh(p->batcher, *slot);
    return *slot;
}

// The desktop's PolyFX draws a sprite over any three points of the world
// (TR3's splashes, ripples, footprints): here the unit triangle is stretched
// onto them by the instance's matrix, its columns two edges and their normal.
// shortcut: one colour per triangle, the corners' mean; upgrade if a splash's
// fade looks banded.
static void M_StageTri(
    const int32_t sprite_idx, const XYZ_32 pts[3], const int32_t half,
    const RGBA_8888 color[3], const float z_depth_adjust,
    const DRAW_TYPE draw_type)
{
    M_PRIV *const p = &m_Priv;
    if (p->tri_meshes == nullptr || sprite_idx < 0
        || sprite_idx >= (int32_t)p->mesh_count) {
        return;
    }
    // The desktop adds the adjustment to the clip space depth, which moves a
    // point towards the camera by about that over twice the near plane of
    // its distance: what keeps a footprint off the floor it lies on.
    const float pull = MAX(0.0f, -z_depth_adjust)
        / (2.0f * (float)(Output_GetNearZ() >> W2V_SHIFT));
    XYZ_32 q[3];
    for (int32_t i = 0; i < 3; i++) {
        q[i] = (XYZ_32) {
            .x = pts[i].x - (int32_t)((pts[i].x - g_ViewPos.x) * pull),
            .y = pts[i].y - (int32_t)((pts[i].y - g_ViewPos.y) * pull),
            .z = pts[i].z - (int32_t)((pts[i].z - g_ViewPos.z) * pull),
        };
    }
    const XYZ_32 e1 = { q[1].x - q[0].x, q[1].y - q[0].y, q[1].z - q[0].z };
    const XYZ_32 e2 = { q[2].x - q[0].x, q[2].y - q[0].y, q[2].z - q[0].z };
    const float nx = (float)e1.y * e2.z - (float)e1.z * e2.y;
    const float ny = (float)e1.z * e2.x - (float)e1.x * e2.z;
    const float nz = (float)e1.x * e2.y - (float)e1.y * e2.x;
    const float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if (len < 1.0f) {
        return;
    }
    const float s = (float)(1 << W2V_SHIFT) / len;
    const MATRIX basis = {
        ._00 = (int64_t)e1.x << W2V_SHIFT,
        ._01 = (int64_t)e2.x << W2V_SHIFT,
        ._02 = (int64_t)(nx * s),
        ._10 = (int64_t)e1.y << W2V_SHIFT,
        ._11 = (int64_t)e2.y << W2V_SHIFT,
        ._12 = (int64_t)(ny * s),
        ._20 = (int64_t)e1.z << W2V_SHIFT,
        ._21 = (int64_t)e2.z << W2V_SHIFT,
        ._22 = (int64_t)(nz * s),
    };
    const bool add =
        draw_type == DRAW_BLEND_ADD || draw_type == DRAW_REFLECTIVE_BLEND_ADD;
    const bool sub = draw_type == DRAW_BLEND_SUB;
    Matrix_Push();
    Matrix_TranslateAbs(q[0].x, q[0].y, q[0].z);
    Matrix_Mul3x3(&basis);
    MESH_INSTANCE inst = {
        .mesh = M_GetTriMesh(p, sprite_idx, half, add || sub),
        .cwmatrix = *g_MatrixPtr,
        .wmatrix = *g_WMatrixPtr,
        .tint = {
            .r = (color[0].r + color[1].r + color[2].r) / (3.0f * 255.0f),
            .g = (color[0].g + color[1].g + color[2].g) / (3.0f * 255.0f),
            .b = (color[0].b + color[1].b + color[2].b) / (3.0f * 255.0f),
            .a = 1.0f,
        },
        .room = Output_GetCurrentRoom(),
    };
    Matrix_Pop();
    if (add) {
        MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_BLEND_ADD);
    } else if (sub) {
        MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_BLEND_SUB);
    } else {
        MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_OPAQUE);
        MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_TRANSPARENT);
    }
}

void OutputSource_PolyFX_StageSpriteTriWorldDepth(
    const int32_t sprite_idx, const XYZ_32 world_pos[3],
    const RGBA_8888 color[3], const float z_depth_adjust,
    const DRAW_TYPE draw_type)
{
    M_StageTri(sprite_idx, world_pos, 0, color, z_depth_adjust, draw_type);
}

void OutputSource_PolyFX_StageSpriteTriWorld(
    const int32_t sprite_idx, const XYZ_32 world_pos[3],
    const RGBA_8888 color[3], const DRAW_TYPE draw_type)
{
    M_StageTri(sprite_idx, world_pos, 0, color, 0.0f, draw_type);
}

void OutputSource_PolyFX_StageSpriteTriWorldLight(
    const int32_t sprite_idx, const XYZ_32 world_pos[3],
    const RGBA_8888 color[3], const OUTPUT_LIGHT_INFO light_info,
    const DRAW_TYPE draw_type)
{
    M_StageTri(sprite_idx, world_pos, 0, color, 0.0f, draw_type);
}

void OutputSource_PolyFX_StageSpriteQuadWorldDepth(
    const int32_t sprite_idx, const XYZ_32 world_pos[4],
    const RGBA_8888 color[4], const float z_depth_adjust,
    const DRAW_TYPE draw_type)
{
    const XYZ_32 t0[3] = { world_pos[0], world_pos[1], world_pos[2] };
    const XYZ_32 t1[3] = { world_pos[0], world_pos[2], world_pos[3] };
    const RGBA_8888 c0[3] = { color[0], color[1], color[2] };
    const RGBA_8888 c1[3] = { color[0], color[2], color[3] };
    M_StageTri(sprite_idx, t0, 0, c0, z_depth_adjust, draw_type);
    M_StageTri(sprite_idx, t1, 1, c1, z_depth_adjust, draw_type);
}

void OutputSource_PolyFX_StageSpriteQuadWorld(
    const int32_t sprite_idx, const XYZ_32 world_pos[4],
    const RGBA_8888 color[4], const DRAW_TYPE draw_type)
{
    OutputSource_PolyFX_StageSpriteQuadWorldDepth(
        sprite_idx, world_pos, color, 0.0f, draw_type);
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
