// Dreamcast: TR4 lighting is left out of the build (TR4 does not run on the
// Dreamcast). The shared lighting code still names its model, which does
// nothing here.

#include <trx/game/output/lights.h>
#include <trx/game/output/lights/priv.h>

static void M_Nothing(void)
{
}

static void M_CalculateLight(const XYZ_32 pos, const int16_t room_num)
{
}

static void M_CalculateObjectLightingAt(
    const ITEM *const item, const GAME_VECTOR sample_pos)
{
}

static void M_CalculateStaticLight(const int16_t adder)
{
}

static void M_CalculateStaticMeshLight(
    const XYZ_32 pos, const SHADE shade, const ROOM *const room)
{
}

static void M_AddDynamicLight(
    const XYZ_32 pos, const int32_t intensity, const int32_t falloff)
{
}

static void M_UploadLight(
    const OUTPUT_UNIFORMS *const uniforms, const OUTPUT_LIGHT_INFO *const info)
{
}

const LIGHTING_MODEL g_LightingModelTR4 = {
    .init = M_Nothing,
    .shutdown = M_Nothing,
    .observe_level_load = M_Nothing,
    .calculate_light = M_CalculateLight,
    .calculate_object_lighting_at = M_CalculateObjectLightingAt,
    .calculate_static_light = M_CalculateStaticLight,
    .calculate_static_mesh_light = M_CalculateStaticMeshLight,
    .add_dynamic_light = M_AddDynamicLight,
    .upload_cpu_light = M_UploadLight,
    .upload_own_light = M_UploadLight,
};

void Output_SetInventoryLightingMode(const bool enabled)
{
}
