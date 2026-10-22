#pragma once

#include <trx/game/objects/types.h>
#include <trx/game/output/mesh_batcher/batcher.h>
#include <trx/game/output/scene_source.h>
#include <trx/game/output/uniforms.h>
#include <trx/game/types.h>

// Render-policy overrides that other modules can register for individual
// object meshes (e.g. the skybox), so that the generic mesh pipeline stays
// ignorant of their specifics. Individual hooks may be null. When several
// policies are registered for one mesh, they apply in registration order:
// vertex_flags are OR'ed together, the first get_* hook returning true wins,
// and every adjust_light_info runs.
typedef struct {
    // Extra VERT_* flags to apply to every vertex of the mesh.
    uint16_t vertex_flags;
    // Override the scene pass for a face; return false to keep the default.
    bool (*get_face_pass)(const FACE *face, SCENE_PASS *pass);
    // Override a face vertex color; return false to keep the default.
    bool (*get_vertex_color)(
        const FACE *face, int32_t vertex_idx, RGBA_8888 *color);
    // Override a face vertex shade; return false to keep the default.
    bool (*get_vertex_shade)(
        const FACE *face, int32_t vertex_idx, int32_t *shade);
    // Adjust lighting when staging the mesh.
    void (*adjust_light_info)(OUTPUT_LIGHT_INFO *info);
} OUTPUT_OBJECT_MESH_POLICY;

void OutputSource_Objects_Init(MESH_BATCHER *batcher);
void OutputSource_Objects_AddMeshPolicy(
    int32_t mesh_idx, const OUTPUT_OBJECT_MESH_POLICY *policy);
// Removes every registration of this policy, for any mesh index.
void OutputSource_Objects_RemoveMeshPolicy(
    const OUTPUT_OBJECT_MESH_POLICY *policy);
void OutputSource_Objects_Shutdown(void);
// Whether the object meshes have been batched for drawing. They are batched
// once the level is observed, so a mesh added before that needs no refresh.
bool OutputSource_Objects_HasMeshes(void);

void OutputSource_Objects_ObserveLevelLoad(void);
void OutputSource_Objects_ObserveLevelUnload(void);
void OutputSource_Objects_ObserveObjectMeshSwap(
    int32_t mesh_idx_1, int32_t mesh_idx_2);
void OutputSource_Objects_ObserveObjectMeshUpdate(int32_t mesh_idx);
// Re-uploads the mesh's vertex positions from the given array, indexed like
// the OBJECT_MESH's own vertices. Used for meshes whose geometry is rewritten
// at runtime, unlike the swap/flag paths above; float positions, so runtime
// deformation is not quantized to the int16 grid. When normals is non-null it
// is indexed the same way and replaces the baked normals, so a deformed seam
// can be lit to match the meshes it welds to.
void OutputSource_Objects_ObserveObjectMeshGeometry(
    int32_t mesh_idx, const XYZ_F *positions, const XYZ_F *normals,
    const float *tint_factors);

void OutputSource_Objects_StageObjectMesh(const OBJECT_MESH *mesh);

const SCENE_SOURCE *OutputSource_Objects_GetSource(void);

#if defined(__DREAMCAST__)
// Frames drawn so far (the Dreamcast's output counts them).
extern uint32_t g_DC_FrameCount;
// Lets go of the object meshes not drawn for max_age frames; they are built
// again when next drawn. Returns how many went.
int32_t OutputSource_Objects_EvictUnused(uint32_t max_age);
// Lets go of the meshes drawn longest ago (never the last frame's) until the
// kept ones hold at most max_vertices. Returns how many went.
int32_t OutputSource_Objects_EvictToBudget(int32_t max_vertices);
#endif
