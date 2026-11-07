#include <trx/core/file.h>
#include <trx/core/vector.h>
#include <trx/debug.h>
#include <trx/game/gun/registry.h>
#include <trx/game/inject.h>
#include <trx/game/inject/canonical.h>
#include <trx/game/objects/common.h>

static VECTOR *m_ProcessedMeshes = nullptr;

#if defined(__DREAMCAST__)
// The long guns' injected objects carry placeholder meshes (a block for the
// hand), because the remaster draws the real model from the outfit's gun-swap
// object. The disc leaves that object out for memory, so the level's own
// object is the only source of the model: keep its meshes and take only the
// injection's animations.
static bool M_IsLaraGunObject(const OBJECT_ID obj_id)
{
    for (int32_t i = 0; i < Gun_Registry_GetCount(); i++) {
        const WEAPON_INFO *const weapon = Gun_Registry_GetByIndex(i);
        if (weapon != nullptr && weapon->anim_object_id == obj_id) {
            return true;
        }
    }
    return false;
}
#endif

static void M_ReadBounds16(BOUNDS_16 *const bounds, TRX_FILE *const file)
{
    bounds->min.x = File_ReadS16(file);
    bounds->max.x = File_ReadS16(file);
    bounds->min.y = File_ReadS16(file);
    bounds->max.y = File_ReadS16(file);
    bounds->min.z = File_ReadS16(file);
    bounds->max.z = File_ReadS16(file);
}

static void M_AlignTextureReferences(
    OBJECT_MESH *const mesh, const int32_t tex_info_base)
{
    if (Vector_Contains(m_ProcessedMeshes, (void *)mesh)) {
        return;
    }
    Vector_Add(m_ProcessedMeshes, (void *)mesh);

    for (int32_t j = 0; j < mesh->tex_faces.count; j++) {
        mesh->tex_faces.data[j].texture_idx += tex_info_base;
    }
    for (int32_t j = 0; j < mesh->flat_faces.count; j++) {
        FACE *const face = &mesh->flat_faces.data[j];
        face->palette_idx = Inject_GetPaletteIndex(face->palette_idx);
    }
}

static void M_ReadObject(const INJECTION_CHUNK chunk)
{
    const LEVEL_CONTEXT_INFO cached_info = Inject_GetCachedInfo();
    const INJECTION_OBJECT_INFO obj_info =
        Inject_ReadObjectPtr(chunk.injection);
    OBJECT *obj;
    if (!SHOULD(Inject_GetObject(obj_info, &obj), "object edit is skipped")) {
        File_Skip(chunk.injection->fp, 14);
        return;
    }

    const int16_t num_meshes = File_ReadS16(chunk.injection->fp);
    const int16_t mesh_idx = File_ReadS16(chunk.injection->fp);
    const int32_t bone_idx = File_ReadS32(chunk.injection->fp) / ANIM_BONE_SIZE;

    // Omitted mesh data indicates that we wish to retain what's already
    // defined in level data to avoid duplicate texture page usage.
#if defined(__DREAMCAST__)
    // A gun object the level already holds keeps the level's meshes: the
    // injected set is a placeholder and the real model comes from the outfit
    // object, which the disc leaves out.
    const bool keep_meshes = obj->loaded && M_IsLaraGunObject(obj_info.id);
#else
    const bool keep_meshes = false;
#endif
    if (!obj->loaded || num_meshes != 0) {
        if (!keep_meshes) {
            obj->mesh_count = num_meshes;
            obj->mesh_idx = mesh_idx + cached_info.mesh_ptr_count;
        }
        obj->bone_idx = bone_idx + cached_info.anims.bone_count;
    }

    // Ommitted animation data marks that existing related object data should be
    // retained i.e. mesh replacement only.
    uint32_t frame_ofs = File_ReadU32(chunk.injection->fp);
    const int16_t anim_idx = File_ReadS16(chunk.injection->fp);
    if ((int32_t)frame_ofs != -1) {
        if (chunk.injection->trxi) {
            // TRXI object records name a frame ordinal.
            frame_ofs = InjectCanonical_FrameOffset(frame_ofs);
        }
        obj->frame_ofs = frame_ofs;
        obj->frame_base = nullptr;
        obj->anim_idx = anim_idx;
        if (obj->anim_idx != -1) {
            obj->anim_idx += cached_info.anims.anim_count;
        } else if (chunk.injection->version >= INJ_VERSION_12) {
            // Match borrowed frames to an injected animation by frame offset.
            obj->frame_ofs += cached_info.anims.frame_count * 2;
        }
        obj->loaded = true;
    }

    if (!keep_meshes) {
        for (int32_t i = 0; i < num_meshes; i++) {
            OBJECT_MESH *const mesh = Object_GetMesh(obj->mesh_idx + i);
            M_AlignTextureReferences(mesh, cached_info.textures.object_count);
        }
    }
}

static void M_ReadStaticObject3D(const INJECTION_CHUNK chunk)
{
    const LEVEL_CONTEXT_INFO cached_info = Inject_GetCachedInfo();
    const int32_t static_id = File_ReadS32(chunk.injection->fp);

    if (static_id < 0 || static_id >= Object_GetStaticObjects3DCount()) {
        LOG_WARNING("Invalid static 3D %d", static_id);
        File_Skip(chunk.injection->fp, 2 + 12 + 12 + 2);
        return;
    }

    STATIC_OBJECT_3D *const obj = Object_Get3DStatic(static_id);
    obj->mesh_idx = File_ReadS16(chunk.injection->fp);
    obj->mesh_idx += cached_info.mesh_ptr_count;
    obj->loaded = true;

    M_ReadBounds16(&obj->draw_bounds, chunk.injection->fp);
    M_ReadBounds16(&obj->collision_bounds, chunk.injection->fp);

    const uint16_t flags = File_ReadU16(chunk.injection->fp);
    obj->collidable = (flags & 1) == 0;
    obj->visible = (flags & 2) != 0;

    OBJECT_MESH *const mesh = Object_GetMesh(obj->mesh_idx);
    M_AlignTextureReferences(mesh, cached_info.textures.object_count);
}

static void M_HandleObjectData(
    const INJECTION_CONTEXT *const ctx, const INJECTION_CHUNK chunk)
{
    m_ProcessedMeshes = Vector_Create(sizeof(OBJECT_MESH *));
    for (int32_t i = 0; i < chunk.num_blocks; i++) {
        const INJECTION_DATA_TYPE data_type = File_ReadS32(chunk.injection->fp);
        const int32_t data_count = File_ReadS32(chunk.injection->fp);
        const int32_t data_size = File_ReadS32(chunk.injection->fp);

        if (ctx->mode == INJECTION_MODE_STATS) {
            File_Skip(chunk.injection->fp, data_size);
            continue;
        }

        for (int32_t j = 0; j < data_count; j++) {
            switch (data_type) {
            case IDT_OBJECTS:
                M_ReadObject(chunk);
                break;
            case IDT_STATIC_OBJECTS:
                M_ReadStaticObject3D(chunk);
                break;
            default:
                LOG_WARNING("Unrecognised object data type %d", data_type);
                File_Skip(chunk.injection->fp, data_size);
                break;
            }
        }
    }

    Vector_Free(m_ProcessedMeshes);
}

REGISTER_INJECTOR(ICT_OBJECT_DATA, M_HandleObjectData)
