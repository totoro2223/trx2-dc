#include <trx/game/output/sources/sprites.h>

#include <trx/core/memory.h>
#include <trx/core/utils.h>
#include <trx/game/output/const.h>
#include <trx/game/output/mesh_batcher/mesh_builder.h>
#include <trx/game/output/scene_compositor.h>
#include <trx/game/output/shaders/mesh.h>
#include <trx/game/output/state.h>
#include <trx/game/output/textures.h>

#include <string.h>

typedef struct {
    SCENE_SOURCE source;
    MESH_BATCHER *batcher;
    OUTPUT_MESH **meshes;
    OUTPUT_MESH **meshes_blend_add;
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
            .reflectivity = 1.0f,
            .shade = 0,
            .trapezoid_ratio = { 1.0f, 1.0f },
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
