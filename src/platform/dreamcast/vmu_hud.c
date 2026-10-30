// Dreamcast platform layer: what the VMU's screen shows while a level plays.
//
// L + R + B steps through three screens: the logo (the default, nothing is
// sent), the level's statistics and the secret finder. The finder points an
// arrow, from the middle of the screen, at the nearest secret not yet found,
// relative to where the camera looks, and beeps slower or faster with the
// distance. The screen and the buzzer only queue their command and return
// (a busy card drops it), so nothing here holds the game up.

#include "vmu.h"

#include <trx/core/math.h>
#include <trx/game/camera.h>
#include <trx/game/console.h>
#include <trx/game/const.h>
#include <trx/game/game.h>
#include <trx/game/items.h>
#include <trx/game/lara.h>
#include <trx/game/objects/families.h>
#include <trx/game/rooms.h>
#include <trx/game/stats.h>

#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <dc/vmu_fb.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>

// Logic frames between two pictures: twice a second for the statistics, five
// times for the arrow.
#define M_STATS_PERIOD (LOGIC_FPS / 2)
#define M_FINDER_PERIOD (LOGIC_FPS / 5)
// How long a beep sounds, and what a save leaves on the screen.
#define M_BEEP_LENGTH 3
#define M_HOLD_LENGTH (LOGIC_FPS * 3)
#define M_BEEP_TONE 0x000065F0
// The finder's three ranges and the seconds between beeps in each; further
// than the last it keeps quiet. Calibrate by ear: the secret is a sector
// (WALL_L, about 2.3 m) wide.
#define M_NEAR (WALL_L * 3)
#define M_MIDDLE (WALL_L * 8)
#define M_FAR (WALL_L * 16)
#define M_NEAR_PERIOD 2
#define M_MIDDLE_PERIOD 3
#define M_FAR_PERIOD 5
// Floor sectors that trigger a secret (TR1 and TR3); one secret may have a
// few. The secret items (TR2's dragons) are looked up among the items.
#define M_MAX_SPOTS 64

typedef enum {
    M_MODE_LOGO,
    M_MODE_STATS,
    M_MODE_FINDER,
    M_MODE_NUMBER_OF,
} M_MODE;

typedef struct {
    XYZ_32 pos;
    int16_t secret_num;
} M_SPOT;

static M_MODE m_Mode = M_MODE_LOGO;
static int32_t m_Tick = 0;
static int32_t m_Hold = 0;
static int32_t m_NextBeep = 0;
static int32_t m_BeepOff = -1;

static const GF_LEVEL *m_SpotsLevel = nullptr;
static int32_t m_SpotCount = 0;
static M_SPOT m_Spots[M_MAX_SPOTS];

void DC_VMU_Hud_Hold(void)
{
    m_Hold = M_HOLD_LENGTH;
}

static void M_SetPixel(vmufb_t *const fb, const int x, const int y)
{
    if (x < 0 || y < 0 || x >= VMU_SCREEN_WIDTH || y >= VMU_SCREEN_HEIGHT) {
        return;
    }
    const int bit = y * VMU_SCREEN_WIDTH + x;
    ((uint8_t *)fb->data)[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
}

static void M_Line(
    vmufb_t *const fb, const float x0, const float y0, const float x1,
    const float y1)
{
    const float steps = fmaxf(fabsf(x1 - x0), fabsf(y1 - y0));
    for (int i = 0; i <= (int)steps; i++) {
        const float t = steps > 0.0f ? i / steps : 0.0f;
        M_SetPixel(
            fb, (int)lrintf(x0 + (x1 - x0) * t), (int)lrintf(y0 + (y1 - y0) * t));
    }
}

static void M_ShowStats(maple_device_t *const vmu)
{
    const GF_LEVEL *const level = Game_GetCurrentLevel();
    const LEVEL_STATS *const stats = Stats_GetLevelStats(level);
    if (stats == nullptr) {
        return;
    }
    const uint32_t seconds = stats->timer / LOGIC_FPS;
    char time[12];
    if (seconds >= 3600) {
        snprintf(
            time, sizeof(time), "%lu:%02lu:%02lu",
            (unsigned long)(seconds / 3600), (unsigned long)(seconds / 60 % 60),
            (unsigned long)(seconds % 60));
    } else {
        snprintf(
            time, sizeof(time), "%lu:%02lu", (unsigned long)(seconds / 60),
            (unsigned long)(seconds % 60));
    }
    // As the statistics screen counts it (counts.lua, format_distance).
    const uint32_t metres = stats->distance_travelled / 445;
    char distance[12];
    if (metres < 1000) {
        snprintf(distance, sizeof(distance), "%lum", (unsigned long)metres);
    } else {
        snprintf(
            distance, sizeof(distance), "%lu.%02lukm",
            (unsigned long)(metres / 1000), (unsigned long)(metres % 1000 / 10));
    }
    STATS_CATEGORY secrets = {};
    Stats_GetCategory(level, STATS_CAT_SECRETS, &secrets);

    // Hits of shots fired, and secrets found of the level's.
    char shots[12];
    char found[12];
    snprintf(
        shots, sizeof(shots), "%lu/%lu", (unsigned long)stats->ammo_hits,
        (unsigned long)stats->ammo_used);
    snprintf(
        found, sizeof(found), "%lu/%lu",
        (unsigned long)stats->counts[STATS_CAT_SECRETS],
        (unsigned long)secrets.max);
    // Four lines of the VMU's 4x6 font, 12 columns each.
    char text[80];
    snprintf(
        text, sizeof(text), "TIME%8s\nDIST%8s\nSHOT%8s\nSECR%8s", time,
        distance, shots, found);

    vmufb_t fb;
    vmufb_clear(&fb);
    vmufb_print_string_into(
        &fb, nullptr, 0, 2, VMU_SCREEN_WIDTH, VMU_SCREEN_HEIGHT - 2, 2, text);
    vmufb_present(&fb, vmu);
}

static void M_FindSpots(const GF_LEVEL *const level)
{
    m_SpotsLevel = level;
    m_SpotCount = 0;
    for (int32_t i = 0; i < Room_GetCount(); i++) {
        const ROOM *const room = Room_Get(i);
        for (int32_t z = 0; z < room->size.z; z++) {
            for (int32_t x = 0; x < room->size.x; x++) {
                const SECTOR *const sector = Room_GetUnitSector(room, x, z);
                if (sector->trigger == nullptr) {
                    continue;
                }
                for (const TRIGGER_CMD *cmd = sector->trigger->command;
                     cmd != nullptr; cmd = cmd->next_cmd) {
                    if (cmd->type != TO_SECRET || m_SpotCount == M_MAX_SPOTS) {
                        continue;
                    }
                    m_Spots[m_SpotCount++] = (M_SPOT) {
                        .pos = {
                            .x = room->pos.x + x * WALL_L + WALL_L / 2,
                            .y = sector->floor.height,
                            .z = room->pos.z + z * WALL_L + WALL_L / 2,
                        },
                        .secret_num = (int16_t)(intptr_t)cmd->parameter,
                    };
                }
            }
        }
    }
}

// The nearest secret still to be found, or false if there is none.
static bool M_FindNearest(
    const XYZ_32 from, XYZ_32 *const out_pos, int32_t *const out_distance)
{
    const GF_LEVEL *const level = Game_GetCurrentLevel();
    if (level != m_SpotsLevel) {
        M_FindSpots(level);
    }
    bool found = false;
    *out_distance = INT32_MAX;
    for (int32_t i = 0; i < m_SpotCount; i++) {
        if (Stats_HasSecret(level, m_Spots[i].secret_num)) {
            continue;
        }
        const int32_t distance = XYZ_32_GetDistance(from, m_Spots[i].pos);
        if (distance < *out_distance) {
            *out_distance = distance;
            *out_pos = m_Spots[i].pos;
            found = true;
        }
    }
    for (int32_t i = 0; i < Item_GetTotalCount(); i++) {
        const ITEM *const item = Item_Get(i);
        if (item->is_destroyed || item->room_num == NO_ROOM
            || !ObjectFamily_Has(item->object_id, OBJ_FAMILY_SECRET)) {
            continue;
        }
        const int32_t distance = XYZ_32_GetDistance(from, item->pos);
        if (distance < *out_distance) {
            *out_distance = distance;
            *out_pos = item->pos;
            found = true;
        }
    }
    return found;
}

static void M_RunFinder(maple_device_t *const vmu)
{
    const ITEM *const lara = Lara_GetItem();
    if (lara == nullptr) {
        return;
    }
    XYZ_32 target;
    int32_t distance;
    const bool found = M_FindNearest(lara->pos, &target, &distance);

    if (found && distance <= M_FAR && m_Tick >= m_NextBeep && m_BeepOff < 0) {
        if (vmu_beep_raw(vmu, M_BEEP_TONE) != MAPLE_EAGAIN) {
            const int32_t period = distance <= M_NEAR ? M_NEAR_PERIOD
                : distance <= M_MIDDLE                ? M_MIDDLE_PERIOD
                                                      : M_FAR_PERIOD;
            m_NextBeep = m_Tick + period * LOGIC_FPS;
            m_BeepOff = m_Tick + M_BEEP_LENGTH;
        }
    }

    vmufb_t fb;
    vmufb_clear(&fb);
    if (!found) {
        vmufb_print_string_into(
            &fb, nullptr, 0, 8, VMU_SCREEN_WIDTH, 16, 2, "NO SECRETS\n   LEFT");
        vmufb_present(&fb, vmu);
        return;
    }

    // Up the screen is where the camera looks; a positive angle turns right.
    const int16_t camera_yaw = (int16_t)Math_Atan(
        g_Camera.target.z - g_Camera.pos.z, g_Camera.target.x - g_Camera.pos.x);
    const int16_t secret_yaw =
        (int16_t)Math_Atan(target.z - lara->pos.z, target.x - lara->pos.x);
    const float angle =
        (int16_t)(secret_yaw - camera_yaw) * (float)M_PI / 32768.0f;
    const float cx = VMU_SCREEN_WIDTH / 2;
    const float cy = VMU_SCREEN_HEIGHT / 2;
    const float tip_x = cx + sinf(angle) * 13.0f;
    const float tip_y = cy - cosf(angle) * 13.0f;
    M_Line(&fb, cx, cy, tip_x, tip_y);
    for (int side = -1; side <= 1; side += 2) {
        const float head = angle + (float)M_PI + side * 0.5f;
        M_Line(
            &fb, tip_x, tip_y, tip_x + sinf(head) * 5.0f,
            tip_y - cosf(head) * 5.0f);
    }

    // The arrow is flat; above or below, and how far, go in the corners.
    char metres[12];
    snprintf(metres, sizeof(metres), "%ldm", (long)(distance / 445));
    vmufb_print_string_into(&fb, nullptr, 0, 26, 24, 6, 0, metres);
    const int32_t rise = lara->pos.y - target.y;
    if (rise > WALL_L) {
        vmufb_print_string_into(&fb, nullptr, 0, 0, 8, 6, 0, "UP");
    } else if (rise < -WALL_L) {
        vmufb_print_string_into(&fb, nullptr, 0, 0, 8, 6, 0, "DN");
    }
    vmufb_present(&fb, vmu);
}

void DC_VMU_Hud_Update(void)
{
    m_Tick++;
    if (DC_Input_TakeVMUMode()) {
        m_Mode = (m_Mode + 1) % M_MODE_NUMBER_OF;
        static const char *const names[M_MODE_NUMBER_OF] = {
            [M_MODE_LOGO] = "VMU: logo",
            [M_MODE_STATS] = "VMU: level statistics",
            [M_MODE_FINDER] = "VMU: secret finder",
        };
        Console_Info("%s", names[m_Mode]);
        m_Hold = 0;
        m_NextBeep = m_Tick;
        if (m_Mode == M_MODE_LOGO) {
            DC_VMU_DrawLogo();
        }
    }

    maple_device_t *const vmu = maple_enum_type(0, MAPLE_FUNC_LCD);
    if (vmu == nullptr) {
        return;
    }
    // A buzzer left on keeps sounding, so the stop is sent until it goes.
    if (m_BeepOff >= 0 && m_Tick >= m_BeepOff
        && vmu_beep_raw(vmu, 0) != MAPLE_EAGAIN) {
        m_BeepOff = -1;
    }
    if (m_Hold > 0) {
        m_Hold--;
        return;
    }

    switch (m_Mode) {
    case M_MODE_STATS:
        if (m_Tick % M_STATS_PERIOD == 0) {
            M_ShowStats(vmu);
        }
        break;
    case M_MODE_FINDER:
        if (m_Tick % M_FINDER_PERIOD == 0) {
            M_RunFinder(vmu);
        }
        break;
    default:
        break;
    }
}
