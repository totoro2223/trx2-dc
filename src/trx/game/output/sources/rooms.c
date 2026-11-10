#include <trx/game/output/sources/rooms.h>

#include <trx/config.h>
#include <trx/core/log.h>
#include <trx/core/memory.h>
#include <trx/core/utils.h>
#include <trx/debug.h>
#include <trx/game/output.h>
#include <trx/game/output/bind.h>
#include <trx/game/output/mesh_batcher/mesh_builder.h>
#include <trx/game/output/water.h>
#include <trx/game/random.h>
#include <trx/game/rooms.h>
#include <trx/version.h>

#include <math.h>
#if defined(__DREAMCAST__)
    #include <trx/core/math.h>
    #include <trx/game/const.h>
    #include <trx/game/objects/common.h>
    #include <trx/game/output/const.h>
#endif

typedef struct {
    MESH_BATCHER *batcher;
    size_t mesh_count;
    OUTPUT_MESH **meshes;
} M_PRIV;

static M_PRIV m_Priv = {};

static SCENE_PASS M_GetScenePass(const FACE *const face)
{
    return Output_Textures_GetObjectTextureScenePass(face->texture_idx);
}

// Ports the OG CalcTriFaceNormal: N = cross(p3 - p2, p1 - p2).
static XYZ_F M_CalcTriNormal(const XYZ_F p1, const XYZ_F p2, const XYZ_F p3)
{
    const XYZ_F u = { p1.x - p2.x, p1.y - p2.y, p1.z - p2.z };
    const XYZ_F v = { p3.x - p2.x, p3.y - p2.y, p3.z - p2.z };
    return (XYZ_F) {
        .x = v.z * u.y - v.y * u.z,
        .y = v.x * u.z - v.z * u.x,
        .z = v.y * u.x - v.x * u.y,
    };
}

static XYZ_F M_NormalizeF(const XYZ_F v)
{
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (len2 <= 0.0f) {
        return (XYZ_F) {};
    }
    const float inv_len = 1.0f / sqrtf(len2);
    return (XYZ_F) { v.x * inv_len, v.y * inv_len, v.z * inv_len };
}

static XYZ_F M_GetVertexPos(const ROOM *const room, const int32_t idx)
{
    const XYZ_16 pos = room->mesh.vertices[idx].pos;
    return (XYZ_F) { pos.x, pos.y, pos.z };
}

// Port of the OG CreateVertexNormals (drawroom.cpp): each face normal is
// accumulated into all of its vertices, then normalized. Quads use the sum
// of their two triangle normals. Needed by the TR4 room dynamic light model.
static XYZ_F *M_CalcRoomVertexNormals(const ROOM *const room)
{
    XYZ_F *const normals =
        Memory_Alloc(sizeof(XYZ_F) * room->mesh.num_vertices);

    for (int32_t i = 0; i < room->mesh.all_faces.count; i++) {
        const FACE *const face = &room->mesh.all_faces.data[i];
        XYZ_F fn = M_CalcTriNormal(
            M_GetVertexPos(room, face->vertices[0]),
            M_GetVertexPos(room, face->vertices[1]),
            M_GetVertexPos(room, face->vertices[2]));
        if (face->vertex_count == 4) {
            const XYZ_F fn2 = M_CalcTriNormal(
                M_GetVertexPos(room, face->vertices[0]),
                M_GetVertexPos(room, face->vertices[2]),
                M_GetVertexPos(room, face->vertices[3]));
            fn.x += fn2.x;
            fn.y += fn2.y;
            fn.z += fn2.z;
        }
        fn = M_NormalizeF(fn);

        for (int32_t j = 0; j < face->vertex_count; j++) {
            const uint16_t vtx_idx = face->vertices[j];
            bool seen = false;
            for (int32_t k = 0; k < j; k++) {
                if (face->vertices[k] == vtx_idx) {
                    seen = true;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            normals[vtx_idx].x += fn.x;
            normals[vtx_idx].y += fn.y;
            normals[vtx_idx].z += fn.z;
        }
    }

    for (int32_t i = 0; i < room->mesh.num_vertices; i++) {
        normals[i] = M_NormalizeF(normals[i]);
    }
    return normals;
}

static void M_AddRoomFace(
    MESH_BUILDER *const builder, const FACE *const face, const ROOM *const room,
    const XYZ_F *const vertex_normals)
{
    OUTPUT_MESH_VERTEX vertices[4];

#if defined(__DREAMCAST__)
    // A face of no corners (TR1's St. Francis' Folly has one) took the game
    // down in the mesh builder as the level finished loading: left out.
    if (face->vertex_count <= 0) {
        LOG_WARNING(
            "a room face has no corners (room %d texture %d): left out",
            (int)(room - Room_Get(0)), (int)face->texture_idx);
        return;
    }
#endif
    ASSERT(face->vertex_count <= 4);

    for (int32_t i = 0; i < face->vertex_count; i++) {
        const ROOM_VERTEX *const room_vert =
            &room->mesh.vertices[face->vertices[i]];

        uint16_t flags = 0;
        const bool disable_wibble = g_TRVersion == 4
            ? room_vert->flags.move
            : room_vert->flags.disable_wibble;
        if (disable_wibble) {
            flags |= VERT_NO_WIBBLE;
        }
        if (room_vert->flags.move) {
            flags |= VERT_MOVE;
        }
        if (room_vert->flags.glow) {
            flags |= VERT_GLOW;
        }
        if (Output_Textures_GetObjectTextureScenePass(face->texture_idx)
            == SCENE_PASS_OPAQUE) {
            flags |= VERT_NO_ALPHA_DISCARD;
        }
        flags |= VERT_USE_DYNAMIC_LIGHT;
        if (g_TRVersion == 4) {
            // TR4 room colors are stored in the OG 128-neutral scale; the
            // shader doubles them and splits off the overbright excess.
            flags |= VERT_OVERBRIGHT;
        }

        // Match the object mesh normal convention (14-bit fixed point).
        XYZ_F normal = {};
        if (vertex_normals != nullptr) {
            normal = vertex_normals[face->vertices[i]];
            normal.x *= 1 << 14;
            normal.y *= 1 << 14;
            normal.z *= 1 << 14;
        }

        const XYZ_16 *const pos = &room_vert->pos;
        vertices[i] = (OUTPUT_MESH_VERTEX) {
            .pos = { .x = pos->x, .y = pos->y, .z = pos->z },
            .normal = { .x = normal.x, .y = normal.y, .z = normal.z },
            .flags = flags,
            .uvw_idx = Output_Textures_GetObjectUVWIndex(face->texture_idx, i),
            .shade = room_vert->light_base,
            .light_table_idx = room_vert->light_table_value,
            .color = room_vert->color,
#if !defined(__DREAMCAST__)
            .reflectivity = 1.0f,
            .trapezoid_ratio = {
                [0] = face->texture_zw[i].z,
                [1] = face->texture_zw[i].w,
            },
#endif
        };
    }
    MeshBuilder_AddVertices(builder, vertices, face->vertex_count);
    MeshBuilder_AddFan(builder, M_GetScenePass(face), face->double_sided, true);
}

#if defined(__DREAMCAST__)
// The statics merged into the room that draws them (PLAN_60_FIJO.md): their
// faces become the room's, lit once as the game lights a static (a constant
// light, tr3.c's and tr12.c's M_CalculateStaticMeshLight, over the mesh's own
// shades), so that they go through the room's chunks and the native batches
// instead of an instance each, every frame, in the staging and the pass. A
// light passing lights them by the vertex then, as it does the room. Per
// static draw_num: merged, and so not drawn as an object (rooms/draw.c).
static bool *m_MergedStatics = nullptr;
static int32_t m_MergedCapacity = 0;
static int32_t m_MergedVertices = 0;
// The vertices the merged statics may add over the level: each instance is a
// copy of its mesh, which the level's memory has to hold.
    #define M_MERGE_VERTEX_BUDGET 24000

bool OutputSource_Rooms_IsStaticMerged(const int32_t draw_num)
{
    return draw_num >= 0 && draw_num < m_MergedCapacity
        && m_MergedStatics[draw_num];
}

static int32_t m_Reject[8];
static const OBJECT_MESH *M_MergeableStatic(const STATIC_MESH *const sm)
{
    if (g_TRVersion > 3 || sm->draw_num < 0 || sm->draw_num >= m_MergedCapacity
        || m_MergedStatics[sm->draw_num]) {
        m_Reject[0]++;
        return nullptr;
    }
    // TR3's shatterable statics (none breaks in TRX yet: jeep.c).
    if (g_TRVersion == 3 && sm->static_num >= 50 && sm->static_num <= 59) {
        m_Reject[1]++;
        return nullptr;
    }
    // TR1 and TR2 light a static of a flickering or glowing room as it goes.
    if (g_TRVersion <= 2 && Room_Get(sm->room_num)->light_mode != RLM_NORMAL) {
        m_Reject[2]++;
        return nullptr;
    }
    const STATIC_OBJECT_3D *const obj = Object_Get3DStatic(sm->static_num);
    if (!obj->loaded || !obj->visible) {
        m_Reject[3]++;
        return nullptr;
    }
    const OBJECT_MESH *const mesh = Object_GetMesh(obj->mesh_idx);
    if (mesh == nullptr || mesh->flat_faces.count > 0
        || mesh->tex_faces.count == 0 || mesh->enable_solid_color
        || mesh->enable_reflections) {
        // (Its depth_adjustment is not kept: the Dreamcast's vertex has none.)
        m_Reject[4]++;
        return nullptr;
    }
    for (int32_t i = 0; i < mesh->tex_faces.count; i++) {
        const FACE *const face = &mesh->tex_faces.data[i];
        if (face->semi_transparent || face->enable_reflections
            || face->vertex_count < 3 || face->vertex_count > 4) {
            m_Reject[5]++;
        return nullptr;
        }
    }
    return mesh;
}

// Adds the static's faces to the room's mesh, in the room's space; false
// (nothing added) when a corner would lie past what a room vertex holds.
static bool M_MergeStatic(
    MESH_BUILDER *const builder, const ROOM *const room,
    const STATIC_MESH *const sm, const OBJECT_MESH *const mesh)
{
    const int32_t s = Math_Sin(sm->rot.y);
    const int32_t c = Math_Cos(sm->rot.y);
    const XYZ_32 d = {
        .x = sm->pos.x - room->pos.x,
        .y = sm->pos.y - room->pos.y,
        .z = sm->pos.z - room->pos.z,
    };
    // As Matrix_RotY turns it, then the static's place.
    for (int32_t i = 0; i < mesh->num_vertices; i++) {
        const XYZ_16 p = mesh->vertices[i];
        const int32_t x = ((c * p.x + s * p.z) >> W2V_SHIFT) + d.x;
        const int32_t y = p.y + d.y;
        const int32_t z = ((c * p.z - s * p.x) >> W2V_SHIFT) + d.z;
        if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX
            || z < INT16_MIN || z > INT16_MAX) {
            return false;
        }
    }

    // The static's light, as the game's lighting model makes it at rest.
    float ambient[3] = { 1.0f, 1.0f, 1.0f };
    int32_t adder = sm->shade.value_1;
    if (g_TRVersion == 3) {
        const int32_t rgb15 = sm->shade.value_1 & 0x7FFF;
        ambient[0] = ((rgb15 & 0x1F) << 3) / 255.0f;
        ambient[1] = ((rgb15 & 0x3E0) >> 2) / 255.0f;
        ambient[2] = ((rgb15 & 0x7C00) >> 7) / 255.0f;
    }
    const bool own = mesh->num_lights <= 0;

    for (int32_t f = 0; f < mesh->tex_faces.count; f++) {
        const FACE *const face = &mesh->tex_faces.data[f];
        OUTPUT_MESH_VERTEX vertices[4];
        for (int32_t k = 0; k < face->vertex_count; k++) {
            const int32_t vi = face->vertices[k];
            const XYZ_16 p = mesh->vertices[vi];
            const int32_t shade = own && vi < -mesh->num_lights
                ? mesh->lighting.lights[vi]
                : SHADE_NEUTRAL;
            RGBA_8888 color = { 255, 255, 255, 255 };
            int32_t room_shade = SHADE_NEUTRAL;
            if (g_TRVersion == 3) {
                // batcher.c M_LightVertex's TR3 own and object light: the
                // ambient, times the vertex's shade for its own.
                float shade8 = floorf((SHADE_MAX - shade) / 32.0f);
                shade8 = shade8 <= 0.0f ? 255.0f : shade8;
                uint8_t *const ch[3] = { &color.r, &color.g, &color.b };
                for (int32_t i = 0; i < 3; i++) {
                    float mul = own ? ambient[i] * shade8 / 255.0f : ambient[i];
                    mul = mul < 0.0f ? 0.0f : mul > 1.0f ? 1.0f : mul;
                    *ch[i] = (uint8_t)(mul * 255.0f + 0.5f);
                }
            } else {
                room_shade = own ? adder + shade : adder;
                CLAMP(room_shade, 0, SHADE_MAX);
            }
            uint16_t flags = VERT_USE_DYNAMIC_LIGHT;
            if (Output_Textures_GetObjectTextureScenePass(face->texture_idx)
                == SCENE_PASS_OPAQUE) {
                flags |= VERT_NO_ALPHA_DISCARD;
            }
            vertices[k] = (OUTPUT_MESH_VERTEX) {
                .pos = {
                    .x = ((c * p.x + s * p.z) >> W2V_SHIFT) + d.x,
                    .y = p.y + d.y,
                    .z = ((c * p.z - s * p.x) >> W2V_SHIFT) + d.z,
                },
                .normal = { .x = 1.0f },
                .flags = flags,
                .uvw_idx = Output_Textures_GetObjectUVWIndex(face->texture_idx, k),
                .shade = room_shade,
                // The middle of the flicker table: no flicker.
                .light_table_idx = 16,
                .color = color,
            };
        }
        MeshBuilder_AddVertices(builder, vertices, face->vertex_count);
        MeshBuilder_AddFan(builder, M_GetScenePass(face), face->double_sided, true);
        m_MergedVertices += face->vertex_count;
    }
    return true;
}

static void M_MergeStatics(MESH_BUILDER *const builder, const ROOM *const room)
{
    for (int32_t j = 0; j < room->num_static_meshes; j++) {
        const STATIC_MESH *const sm = &room->static_meshes[j];
        const OBJECT_MESH *const mesh = M_MergeableStatic(sm);
        if (mesh == nullptr
            || m_MergedVertices + mesh->tex_faces.count * 4
                > M_MERGE_VERTEX_BUDGET) {
            continue;
        }
        if (M_MergeStatic(builder, room, sm, mesh)) {
            m_MergedStatics[sm->draw_num] = true;
        }
    }
}

static void M_PrepareMergedStatics(void)
{
    Memory_FreePointer(&m_MergedStatics);
    m_MergedVertices = 0;
    int32_t top = 0;
    for (int32_t i = 0; i < Room_GetCount(); i++) {
        const ROOM *const room = Room_Get(i);
        for (int32_t j = 0; j < room->num_static_meshes; j++) {
            top = MAX(top, room->static_meshes[j].draw_num + 1);
        }
    }
    m_MergedCapacity = top;
    m_MergedStatics = Memory_Alloc(sizeof(bool) * MAX(top, 1));
}
#endif

static int32_t M_GetWaterEffect(const ROOM *const room)
{
    if (g_TRVersion >= 3) {
        return 2 + (int32_t)room->water_scheme;
    }
    return Output_Water_IsSubmerged() ? 1 : 0;
}

static void M_PrepareMeshes(M_PRIV *const p)
{
    p->mesh_count = Room_GetCount();
    p->meshes = Memory_Alloc(sizeof(OUTPUT_MESH *) * p->mesh_count);

    MESH_BUILDER *const builder = MeshBuilder_Create();
#if defined(__DREAMCAST__)
    M_PrepareMergedStatics();
#endif
    for (int32_t i = 0; i < Room_GetCount(); i++) {
        const ROOM *const room = Room_Get(i);
        XYZ_F *vertex_normals = nullptr;
        if (g_TRVersion == 4) {
            vertex_normals = M_CalcRoomVertexNormals(room);
        }
        for (int32_t j = 0; j < room->mesh.all_faces.count; j++) {
            M_AddRoomFace(
                builder, &room->mesh.all_faces.data[j], room, vertex_normals);
        }
        Memory_FreePointer(&vertex_normals);

        int32_t stack = 0;
        XYZ_16 prev_pos = { -1, -1, -1 };
        for (int32_t j = 0; j < room->mesh.sprites.count; j++) {
            const ROOM_SPRITE *const sprite = &room->mesh.sprites.data[j];
            const ROOM_VERTEX *const vert =
                &room->mesh.vertices[sprite->vertex];
            if (vert->pos.x == prev_pos.x && vert->pos.z == prev_pos.z) {
                stack++;
            } else {
                stack = 0;
            }
            MeshBuilder_AddRoomSprite(
                builder, sprite, room, stack * -0.005f, VERT_USE_DYNAMIC_LIGHT);
            prev_pos = vert->pos;
        }
#if defined(__DREAMCAST__)
        M_MergeStatics(builder, room);
#endif

        OUTPUT_MESH *const mesh = MeshBuilder_Seal(builder);
        if (mesh != nullptr) {
            MeshBatcher_AddMesh(p->batcher, mesh);
        }

        p->meshes[i] = mesh;
    }
    MeshBuilder_Destroy(builder);
#if defined(__DREAMCAST__)
    LOG_INFO("[statics] %d vertices of statics merged into their rooms",
             (int)m_MergedVertices);
    LOG_INFO("[statics] refused: %d %d %d %d %d %d (capacity %d)", (int)m_Reject[0], (int)m_Reject[1], (int)m_Reject[2], (int)m_Reject[3], (int)m_Reject[4], (int)m_Reject[5], (int)m_MergedCapacity);
#endif
}

static void M_FreeMeshes(M_PRIV *const p)
{
#if defined(__DREAMCAST__)
    Memory_FreePointer(&m_MergedStatics);
    m_MergedCapacity = 0;
#endif
    if (p->meshes != nullptr) {
        for (int32_t i = 0; i < (int32_t)p->mesh_count; i++) {
            MeshBatcher_RemoveMesh(p->batcher, p->meshes[i]);
            if (p->meshes[i] != nullptr) {
                Output_Mesh_Destroy(p->meshes[i]);
            }
        }
        Memory_FreePointer(&p->meshes);
    }
}

void OutputSource_Rooms_Init(MESH_BATCHER *const batcher)
{
    M_PRIV *const p = &m_Priv;
    p->batcher = batcher;
}

void OutputSource_Rooms_Shutdown(void)
{
    M_PRIV *const p = &m_Priv;
    M_FreeMeshes(p);
}

void OutputSource_Rooms_ObserveLevelLoad(void)
{
    M_PRIV *const p = &m_Priv;
    M_FreeMeshes(p);
    M_PrepareMeshes(p);
}

void OutputSource_Rooms_ObserveLevelUnload(void)
{
    M_PRIV *const p = &m_Priv;
    M_FreeMeshes(p);
}

void OutputSource_Rooms_ObserveRoomFlip(const ROOM *const room)
{
    if (room->flip_status == RFS_UNFLIPPED && room->flipped_room != NO_ROOM) {
        const int16_t room_1 = Room_GetIndex(room);
        const int16_t room_2 = room->flipped_room;
        SWAP(m_Priv.meshes[room_1], m_Priv.meshes[room_2]);
    }
}

#if defined(__DREAMCAST__)
bool OutputSource_Rooms_RayBlocked(
    const ROOM *const room, const XYZ_32 *const eye, const XYZ_32 *const to)
{
    const M_PRIV *const p = &m_Priv;
    return DC_MeshBatcher_RayBlocked(
        p->batcher, p->meshes[Room_GetIndex(room)], room, eye, to);
}
#endif

void OutputSource_Rooms_StageRoom(const ROOM *const room)
{
    M_PRIV *const p = &m_Priv;
    OUTPUT_MESH *const mesh = p->meshes[Room_GetIndex(room)];
    const OUTPUT_ROOM_BIND *const bind = Output_Bind_GetRoom(room);
    const MESH_INSTANCE inst = {
        .mesh = mesh,
        .cwmatrix = *g_MatrixPtr,
        .wmatrix = *g_WMatrixPtr,
        .tint = Output_GetRoomTint(),
        .wibble = Output_Water_IsWibbleEnabled(),
        .wibble_fill = g_TRVersion != 4,
        .water_effect = M_GetWaterEffect(room),
        .enable_scissor = Room_IsOverlapping(Room_GetIndex(room)),
        .scissor = {
            .x = bind->bound_left,
            .y = bind->bound_bottom,
            .width = bind->bound_right - bind->bound_left,
            .height = bind->bound_bottom - bind->bound_top,
        },
        .room = Output_GetCurrentRoom(),
    };
    MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_OPAQUE);
    MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_TRANSPARENT);
    MeshBatcher_Stage(p->batcher, &inst, SCENE_PASS_BLEND_ADD);
}
