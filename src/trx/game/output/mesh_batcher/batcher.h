#pragma once

#include <trx/game/output/mesh_batcher/mesh.h>
#include <trx/game/output/scene_source.h>
#include <trx/game/output/uniforms.h>
#include <trx/game/rooms/types.h>
#include <trx/game/viewport.h>

#include <stdint.h>

typedef struct MESH_INSTANCE {
    OUTPUT_MESH *mesh;

    // TODO: use gl_InstanceID some day for this
    // and glMultiDrawArraysIndirect
    MATRIX cwmatrix;
    MATRIX wmatrix;
    const ROOM *room;
    RGBA_F tint;
    // Sets the water surface height so only the submerged part of the
    // instance is tinted; without a surface, the whole instance is tinted.
    bool has_water_line;
    float water_line;
    // Shifts the ambient light of the submerged part by this much, in place
    // of tinting it.
    bool has_submerged_ambient;
    RGB_F submerged_ambient_delta;
    // Sets the ambient light at the far end of an instance spanning two
    // rooms; without it, the instance uses one ambient light.
    bool has_ambient_span;
    RGB_F ambient_span_from;
    bool wibble;
    // Draws the instance a second time without the distortion, under the
    // distorted one and without writing depth. Faces that distort next to
    // faces that do not crack apart at the seam, and the plain copy fills
    // the cracks. An instance that distorts as a whole needs no fill.
    bool wibble_fill;
    int32_t water_effect;

    // Where the instance sits among the sorted pass's layers, drawn low first
    // and by depth within a layer. A shadow lies flat under the item it
    // belongs to, so no depth key puts it reliably behind every part of a body
    // resting on it; it takes a layer of its own instead.
    int32_t sort_layer;

    OUTPUT_LIGHT_INFO light_info;

    bool enable_scissor;
    float depth_adjust;
    VIEWPORT_RECT scissor;
#if defined(__DREAMCAST__)
    // A rig (the mesh's bone_count): its bones' world matrices are this many
    // in the frame's pool from rig_first (DC_MeshBatcher_RigBones), and
    // wmatrix is the object's own. 0 for any other instance.
    int16_t rig_bones;
    int16_t rig_first;
    // The bones left out (a far enemy's hands and feet), one bit each.
    uint32_t rig_skip;
#endif
} MESH_INSTANCE;

typedef struct MESH_BATCHER MESH_BATCHER;

MESH_BATCHER *MeshBatcher_Create(void);
void MeshBatcher_Destroy(struct MESH_BATCHER *batcher);
void MeshBatcher_AddMesh(struct MESH_BATCHER *batcher, OUTPUT_MESH *mesh);
void MeshBatcher_RemoveMesh(struct MESH_BATCHER *batcher, OUTPUT_MESH *mesh);
void MeshBatcher_Seal(MESH_BATCHER *batcher);

const SCENE_SOURCE *MeshBatcher_AsSource(const struct MESH_BATCHER *batcher);

void MeshBatcher_Stage(
    struct MESH_BATCHER *batcher, const MESH_INSTANCE *inst, SCENE_PASS pass);
#if defined(__DREAMCAST__)
// An object's mesh staged in place: Begin hands out the slot the instance
// lives in (the caller sets every field), Finish seats it (shadow caster, and
// the added faces' pass).
MESH_INSTANCE *DC_MeshBatcher_BeginObject(MESH_BATCHER *batcher);
void DC_MeshBatcher_FinishObject(
    MESH_BATCHER *batcher, const MESH_INSTANCE *inst);
// A particle as the PSX draws it (TOMB3.EXE 0x73560): one projection of its
// centre and one quad, no mesh instance and no matrix. half is the world
// size of the quad's half side at the camera's plane; the corners go round the
// picture (top left, top right, bottom right, bottom left).
typedef struct {
    float pos[3];
    float half;
    float half_h; // 0: a square of half
    float sn; // turn about the centre, as sin and cos (0 and 1: none)
    float cs;
    int32_t blend; // 0 added, 1 alpha, 2 subtracted
    uint32_t argb;
    int32_t page; // DC_PVR_NO_TEXTURE for a flat one
    float u[4];
    float v[4];
} DC_PARTICLE;
void DC_MeshBatcher_StageParticle(
    MESH_BATCHER *batcher, const DC_PARTICLE *particle);
// Particles made in bulk (the sparks' assembly): the free slots of the list,
// how many there are, and then how many were written, with how many of each
// blend among them.
DC_PARTICLE *DC_MeshBatcher_ParticleSlots(
    MESH_BATCHER *batcher, int32_t *room);
void DC_MeshBatcher_ParticlesAdded(
    MESH_BATCHER *batcher, int32_t count, const int32_t blends[3]);
const DC_PARTICLE *DC_MeshBatcher_Particles(
    MESH_BATCHER *batcher, int32_t *count);
// A streak (a raindrop, the PSX's line): two world points and a colour at
// each, drawn added as one thin untextured quad, no mesh instance.
typedef struct {
    float a[3];
    float b[3];
    uint32_t argb_a;
    uint32_t argb_b;
} DC_STREAK;
void DC_MeshBatcher_StageStreak(MESH_BATCHER *batcher, const DC_STREAK *streak);
// Streaks made in bulk (the weather's assembly): the free slots and how many,
// then how many were written.
DC_STREAK *DC_MeshBatcher_StreakSlots(MESH_BATCHER *batcher, int32_t *room);
void DC_MeshBatcher_StreaksAdded(MESH_BATCHER *batcher, int32_t count);
// A triangle (n 3) or quad (n 4) of the world, one picture and a colour per
// corner, no mesh instance. pull moves its depth towards the eye by that
// fraction (a footprint kept off the floor).
typedef struct {
    float p[4][3];
    float u[4];
    float v[4];
    uint32_t argb[4];
    int32_t page; // -1: flat
    int32_t blend; // 0 added, 1 alpha, 2 subtracted
    int32_t n;
    float pull;
} DC_QUAD;
void DC_MeshBatcher_StageQuad(MESH_BATCHER *batcher, const DC_QUAD *quad);
// Whether a rig of this mesh can be drawn (the plain object path), and room
// for a rig's bone matrices in the frame's pool (nullptr when full; *first is
// the first one's index).
bool DC_MeshBatcher_CanRig(const MESH_BATCHER *batcher, const OUTPUT_MESH *mesh);
MATRIX *DC_MeshBatcher_RigBones(
    MESH_BATCHER *batcher, int32_t count, int16_t *first);
// Whether a wall of the room, with nothing you can see through on it, lies
// between the eye and the point (both in the world).
bool DC_MeshBatcher_RayBlocked(
    const MESH_BATCHER *batcher, const OUTPUT_MESH *mesh, const ROOM *room,
    const XYZ_32 *eye, const XYZ_32 *to);
#endif

void MeshBatcher_UpdateMeshGeometry(
    const struct MESH_BATCHER *batcher, const OUTPUT_MESH *mesh);
