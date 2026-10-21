#include <trx/config.h>
#include <trx/debug.h>
#include <trx/game/camera.h>
#include <trx/game/lara.h>
#include <trx/game/output.h>
#include <trx/game/output/state.h>
#include <trx/game/output/water/priv.h>
#include <trx/game/rooms.h>

static RGBA_F M_GetWaterTint(void)
{
    return g_Camera.underwater ? Output_GetTint()
                               : Color_RGBToRGBA(Output_GetWaterColor());
}

// Selects a mesh by its origin, as TR3 does because underwater views already
// carry the water colour.
static RGBA_F M_GetPerMeshTint(const GAME_VECTOR pos)
{
    if (g_Camera.underwater) {
        return Output_GetTint();
    }

    int16_t room_num = pos.room_num;
    Room_GetSector(pos.pos, &room_num);
    if (!Room_Get(room_num)->flags.underwater) {
        return COLOR_RGBA_F_WHITE;
    }
    const int32_t water_height = Room_GetWaterHeight(pos.pos, room_num);
    if (water_height == NO_HEIGHT || pos.y > water_height) {
        return M_GetWaterTint();
    }
    return COLOR_RGBA_F_WHITE;
}

// Returns the world-space water surface height above a point, or NO_HEIGHT
// when no surface exists.
static int32_t M_ProbeWaterSurface(const GAME_VECTOR pos)
{
    int16_t room_num = pos.room_num;
    Room_GetSector(pos.pos, &room_num);
    return Room_GetWaterHeight(pos.pos, room_num);
}

#if defined(__DREAMCAST__)
// How far a mesh of Lara's reaches from her origin, sideways.
    #define M_LARA_REACH 640
// Lara's position is taken in cells of this size.
    #define M_CELL_SHIFT 9

// Each of Lara's meshes looks for water under it, two sector walks a mesh.
// The answer belongs to the column the mesh is in, so where no column she can
// reach holds water, none of her meshes can find any. That is worked out from
// a grid of columns round the cell she stands in, once while she stays in it.
static struct {
    bool valid;
    int32_t cell_x;
    int32_t cell_z;
    int16_t room_num;
    bool flipped;
    bool has_water;
} m_Near = {};

static bool M_IsWaterNearLara(const XYZ_32 mesh_pos)
{
    const ITEM *const lara_item = Lara_GetItem();
    if (lara_item == nullptr) {
        return true;
    }
    const int32_t cell_x = lara_item->pos.x >> M_CELL_SHIFT;
    const int32_t cell_z = lara_item->pos.z >> M_CELL_SHIFT;
    // A mesh past her reach (one not hers) is looked up on its own.
    if (ABS(mesh_pos.x - lara_item->pos.x) > M_LARA_REACH
        || ABS(mesh_pos.z - lara_item->pos.z) > M_LARA_REACH) {
        return true;
    }
    const bool flipped = Room_GetFlipStatus();
    if (m_Near.valid && m_Near.cell_x == cell_x && m_Near.cell_z == cell_z
        && m_Near.room_num == lara_item->room_num
        && m_Near.flipped == flipped) {
        return m_Near.has_water;
    }

    // Columns at most 896 apart cover every 1024-wide sector of the area.
    const int32_t cell = 1 << M_CELL_SHIFT;
    const int32_t x0 = (cell_x << M_CELL_SHIFT) - M_LARA_REACH;
    const int32_t z0 = (cell_z << M_CELL_SHIFT) - M_LARA_REACH;
    const int32_t span = cell + 2 * M_LARA_REACH;
    bool has_water = false;
    for (int32_t i = 0; i < 3 && !has_water; i++) {
        for (int32_t j = 0; j < 3 && !has_water; j++) {
            const GAME_VECTOR column = {
                .pos = {
                    .x = x0 + span * i / 2,
                    .y = lara_item->pos.y - WALL_L / 2,
                    .z = z0 + span * j / 2,
                },
                .room_num = lara_item->room_num,
            };
            has_water = M_ProbeWaterSurface(column) != NO_HEIGHT;
        }
    }
    m_Near = (typeof(m_Near)) {
        .valid = true,
        .cell_x = cell_x,
        .cell_z = cell_z,
        .room_num = lara_item->room_num,
        .flipped = flipped,
        .has_water = has_water,
    };
    return has_water;
}
#endif

// Finds water reached by a mesh by testing its lowest possible position first,
// including meshes whose centres remain above water.
static int32_t M_FindMeshWaterSurface(
    const GAME_VECTOR pos, const int32_t radius)
{
#if defined(__DREAMCAST__)
    if (!M_IsWaterNearLara(pos.pos)) {
        return NO_HEIGHT;
    }
#endif
    GAME_VECTOR deepest = pos;
    deepest.pos.y += radius;
    const int32_t height = M_ProbeWaterSurface(deepest);
    if (height != NO_HEIGHT) {
        return height;
    }
    return M_ProbeWaterSurface(pos);
}

static WATER_EFFECTS M_GetEffects(
    const bool is_below_water, const bool is_camera_underwater)
{
    if (is_below_water) {
        return (WATER_EFFECTS) {
            .shade = true,
            .room_shade = true,
            .wibble = !is_camera_underwater,
        };
    }
    return (WATER_EFFECTS) {
        .shade = is_camera_underwater,
        .room_shade = is_camera_underwater,
        .wibble = is_camera_underwater,
    };
}

static bool M_IsObjectWibbleEnabled(void)
{
    return false;
}

static WATER_LARA_MESH M_DecideLaraMesh(
    const LARA_MESH mesh, const GAME_VECTOR pos, const int32_t radius)
{
    switch (g_Config.visuals.water_tint_mode) {
    case WATER_TINT_MODE_WHOLE:
        return (WATER_LARA_MESH) { .tint = Output_GetTint() };

    case WATER_TINT_MODE_PER_MESH:
        return (WATER_LARA_MESH) { .tint = M_GetPerMeshTint(pos) };

    case WATER_TINT_MODE_RESPONSIVE: {
        const int32_t surface = M_FindMeshWaterSurface(pos, radius);
        if (surface == NO_HEIGHT) {
            return (WATER_LARA_MESH) { .tint = COLOR_RGBA_F_WHITE };
        }
        return (WATER_LARA_MESH) {
            .tint = M_GetWaterTint(),
            .has_surface = true,
            .surface = surface,
        };
    }

    case WATER_TINT_MODE_NUMBER_OF:
        break;
    }
    ASSERT_FAIL();
    return (WATER_LARA_MESH) { .tint = COLOR_RGBA_F_WHITE };
}

const WATER_MODEL g_WaterModelTR123 = {
    .get_effects = M_GetEffects,
    .is_object_wibble_enabled = M_IsObjectWibbleEnabled,
    .decide_lara_mesh = M_DecideLaraMesh,
};
