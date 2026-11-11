// The inventory ring without Lua: what ui/ring/screen.lua, ring/motion.lua,
// ring/entries.lua, ring/init.lua and ring_hints.lua did. The ring takes the
// engine's menu (UI_TAKEOVER_MENU) in every mode: in play, at a lock, to save
// or load, when Lara dies and on the title screen. It runs on the UI tick and
// draws its entries as models in a world of their own.
#include "nolua.h"

#include <arch/timer.h>
#include <platform/dreamcast/dc_games.h>
#include <platform/dreamcast/dc_mode.h>

bool DC_Input_TakeEndLevelCombo(void);
#include <trx/config.h>
#include <trx/core/json/util/file.h>
#include <trx/core/json/util/read_io.h>
#include <trx/core/log.h>
#include <trx/core/math/const.h>
#include <trx/core/math/trig.h>
#include <trx/core/strings.h>
#include <trx/game/const.h>
#include <trx/game/demo.h>
#include <trx/game/game_flow.h>
#include <trx/game/game_flow/common.h>
#include <trx/game/game_strings/entries.h>
#include <trx/game/gun/common.h>
#include <trx/game/gun/registry.h>
#include <trx/game/input.h>
#include <trx/game/interpolation.h>
#include <trx/game/inventory.h>
#include <trx/game/lara.h>
#include <trx/game/lara/common.h>
#include <trx/game/lua/events/ui.h>
#include <trx/game/objects/common.h>
#include <trx/game/objects/families.h>
#include <trx/game/objects/names.h>
#include <trx/game/paths.h>
#include <trx/game/savegame.h>
#include <trx/game/shell/common.h>
#include <trx/game/sound.h>
#include <trx/game/stats/common.h>
#include <trx/game/ui/draw.h>
#include <trx/game/ui/regions.h>
#include <trx/game/ui/scaler.h>
#include <trx/game/ui/text.h>
#include <trx/game/ui/tween.h>
#include <trx/game/viewport.h>
#include <trx/version.h>

#include <math.h>
#include <stdio.h>

// --- Entries (ring/entries.lua, inv_ring.json5) ---

#define M_MAX_SPECS 160
#define M_MAX_LIST 40
#define M_MAX_COUNT 999999

typedef struct {
    OBJECT_ID object;
    int32_t frames_total;
    int32_t open_frame;
    int32_t anim_speed;
    int32_t x_rot_pt_sel;
    int32_t x_rot_sel;
    int32_t x_rot_nosel;
    int32_t y_rot_sel;
    int32_t y_trans_sel;
    int32_t z_trans_sel;
    double scale;
    int32_t y_offset;
    int32_t base_rot_x;
    int32_t base_rot_y;
    int32_t base_rot_z;
    bool draws_at_pivot;
    bool is_used_at_once;
    int32_t meshes_sel;
    int32_t inv_pos;
} M_SPEC;

typedef enum {
    M_RING_MAIN,
    M_RING_KEYS,
    M_RING_OPTION,
    M_RING_NUMBER_OF,
} M_RING;

typedef struct {
    OBJECT_ID object;
    int32_t count;
    const M_SPEC *spec;
} M_CONTENT;

static M_SPEC m_Specs[M_MAX_SPECS];
static int32_t m_SpecCount = 0;

static M_SPEC *M_FindSpec(const OBJECT_ID object)
{
    for (int32_t i = 0; i < m_SpecCount; i++) {
        if (m_Specs[i].object == object) {
            return &m_Specs[i];
        }
    }
    return nullptr;
}

static RESULT M_ReadSpec(JSON_READ_IO *const io)
{
    const char *key = nullptr;
    MUST(JSON_READ_OPT(io, "object_id", &key));
    const OBJECT_ID object = key != nullptr ? Object_IdFromKey(key) : NO_OBJECT;
    if (object == NO_OBJECT) {
        LOG_WARNING("ring: unknown object '%s'", key != nullptr ? key : "");
        return OK;
    }
    M_SPEC *spec = M_FindSpec(object);
    if (spec == nullptr) {
        if (m_SpecCount >= M_MAX_SPECS) {
            return JSON_ReadIO_Fail(io, "too many ring entries");
        }
        spec = &m_Specs[m_SpecCount++];
        *spec = (M_SPEC) {
            .object = object,
            .frames_total = 1,
            .scale = 1.0,
            .meshes_sel = -1,
        };
    }
#define M_INT(name) MUST(JSON_READ_OPT(io, #name, &spec->name))
    M_INT(frames_total);
    M_INT(open_frame);
    M_INT(anim_speed);
    M_INT(x_rot_pt_sel);
    M_INT(x_rot_sel);
    M_INT(x_rot_nosel);
    M_INT(y_rot_sel);
    M_INT(y_trans_sel);
    M_INT(z_trans_sel);
    M_INT(y_offset);
    M_INT(base_rot_x);
    M_INT(base_rot_y);
    M_INT(base_rot_z);
    M_INT(meshes_sel);
    M_INT(inv_pos);
#undef M_INT
    MUST(JSON_READ_OPT(io, "scale", &spec->scale));
    MUST(JSON_READ_OPT(io, "draws_at_pivot", &spec->draws_at_pivot));
    MUST(JSON_READ_OPT(io, "is_used_at_once", &spec->is_used_at_once));
    if (spec->frames_total < 1) {
        spec->frames_total = 1;
    }
    return OK;
}

static RESULT M_ReadSpecs(void)
{
    const char *path = nullptr;
    MUST(GamePath_Resolve(
        GAME_DYNAMIC_PATH_COMMON_CONFIG, "inv_ring.json5", &path));
    JSON_VALUE *root = nullptr;
    MUST(JSONFile_ReadRequired(path, &root));
    JSON_READ_IO *const io = JSON_ReadIO_Create(root, 0, path);
    RESULT result = OK;
    const int32_t count = JSON_ReadIO_GetArrayLength(io);
    for (int32_t i = 0; i < count && IS_OK(result); i++) {
        result = JSON_ReadIO_PushArrayElem(io, (size_t)i);
        if (IS_OK(result)) {
            result = M_ReadSpec(io);
            const RESULT popped = JSON_POP(io);
            if (IS_OK(result)) {
                result = popped;
            }
        }
    }
    JSON_ReadIO_Destroy(io);
    JSON_ValueFree(root);
    return result;
}

// The passport rests on the ring as its closed cover, which takes what the
// open passport is declared with unless the cover is declared itself.
static const M_SPEC *M_SpecOf(const OBJECT_ID object)
{
    const M_SPEC *const spec = M_FindSpec(object);
    if (spec == nullptr && object == O_PASSPORT_CLOSED) {
        return M_FindSpec(O_PASSPORT_OPTION);
    }
    return spec;
}

static M_RING M_RingOf(const M_SPEC *const spec)
{
    if (spec->inv_pos < 100) {
        return M_RING_MAIN;
    } else if (spec->inv_pos < 200) {
        return M_RING_KEYS;
    } else if (spec->inv_pos < 300) {
        return M_RING_OPTION;
    }
    return M_RING_NUMBER_OF;
}

static bool M_IsHidden(const OBJECT_ID object)
{
    return object == O_BINOCULARS_OPTION
        && !g_Config.gameplay.enable_binoculars;
}

static void M_Add(
    M_CONTENT *const list, int32_t *const count, const M_RING ring,
    const OBJECT_ID object, const int32_t qty)
{
    const M_SPEC *const spec = M_SpecOf(object);
    if (spec == nullptr || M_RingOf(spec) != ring || M_IsHidden(object)
        || *count >= M_MAX_LIST) {
        return;
    }
    list[(*count)++] = (M_CONTENT) {
        .object = object,
        .count = MIN(qty, M_MAX_COUNT),
        .spec = spec,
    };
}

// The entries a ring holds, in the order they are declared to sit in; those
// that sit in the same place keep the order they were added in. The
// Dreamcast's menu holds the passport and the graphics (its rendering page).
static int32_t M_Contents(const M_RING ring, M_CONTENT *const list)
{
    int32_t count = 0;
    if (ring == M_RING_OPTION) {
        M_Add(list, &count, ring, O_PASSPORT_CLOSED, 1);
        M_Add(list, &count, ring, O_GRAPHICS_OPTION, 1);
        M_Add(list, &count, ring, O_SOUND_OPTION, 1);
    } else {
        INVENTORY_ENTRY entries[INV_MAX_ENTRIES];
        const int32_t n = Inv_GetDrawnEntries(entries, INV_MAX_ENTRIES);
        for (int32_t i = 0; i < n; i++) {
            M_Add(list, &count, ring, entries[i].object_id, entries[i].qty);
        }
    }
    for (int32_t i = 1; i < count; i++) {
        const M_CONTENT c = list[i];
        int32_t j = i - 1;
        for (; j >= 0 && list[j].spec->inv_pos > c.spec->inv_pos; j--) {
            list[j + 1] = list[j];
        }
        list[j + 1] = c;
    }
    return count;
}

static bool M_IsAvailable(const M_RING ring)
{
    if (ring == M_RING_OPTION && g_Config.flow.lockout_option_ring) {
        return false;
    }
    M_CONTENT list[M_MAX_LIST];
    return M_Contents(ring, list) > 0;
}

// --- Motion (ring/motion.lua) ---

#define M_RADIUS 688
#define M_CAMERA_GAP 598
#define M_CAMERA_START (-1536)
#define M_CAMERA_HEIGHT (-256)
#define M_LOOK_Y (-96)
#define M_FOV (80 * DEG_1)
#define M_TURN (-DEG_180)
#define M_TURN_TICKS 12
#define M_TITLE_PITCH 1024
// How far a hand turn comes back each tick once the player lets go.
#define M_UNTURN_RATE (1.0f - (1.0f - 0.15f) * (1.0f - 0.15f))

typedef enum {
    M_MOTION_OPEN,
    M_MOTION_CLOSE,
    M_MOTION_LEAVE_UP,
    M_MOTION_LEAVE_DOWN,
    M_MOTION_ARRIVE,
    M_MOTION_SELECT,
    M_MOTION_DESELECT,
} M_MOTION;

typedef enum {
    M_SPIN_IDLE,
    M_SPIN_HELD,
    M_SPIN_STILL,
} M_SPIN;

typedef struct {
    float w;
    float x;
    float y;
    float z;
} M_QUAT;

static const M_QUAT m_Identity = { .w = 1.0f };

typedef struct {
    UI_TWEEN x_rot_pt;
    UI_TWEEN x_rot;
    UI_TWEEN y_trans;
    UI_TWEEN z_trans;
    // The turn about its own axis, unwrapped, as the last two ticks left it.
    float y_rot_last;
    float y_rot_now;
    M_QUAT hand_last;
    M_QUAT hand_now;
    float turn_yaw;
    float turn_pitch;
    bool turned;
    bool has_bounds;
    BOUNDS_16 bounds;
} M_ENTRY;

typedef struct {
    UI_TWEEN radius;
    UI_TWEEN camera_y;
    UI_TWEEN pitch;
    UI_TWEEN rot;
    M_ENTRY entries[M_MAX_LIST];
    int32_t count;
    int32_t current;
    int32_t base_pitch;
    int32_t left_by;
    int32_t left;
    struct {
        bool active;
        int32_t from;
        int32_t to;
        int32_t left;
        float start;
        float by;
    } turning;
} M_VIEW;

static int32_t M_Wrap(const float angle)
{
    const int32_t a = (int32_t)floorf(angle + 32768.0f);
    return ((a % 65536) + 65536) % 65536 - 32768;
}

static int32_t M_Whole(const float value)
{
    return (int32_t)floorf(value + 0.5f);
}

static void M_TweenTo(UI_TWEEN *const tween, const float to, const int32_t ticks)
{
    UI_Tween_To(tween, to, ticks, 0, UI_EASE_LINEAR);
}

static int32_t M_Spacing(const M_VIEW *const view)
{
    return DEG_360 / MAX(view->count, 1);
}

static float M_RestAngle(const M_VIEW *const view, const int32_t index)
{
    return -DEG_90 - index * M_Spacing(view);
}

static float M_NearestRestAngle(const M_VIEW *const view)
{
    const float heading = UI_Tween_GetTarget(&view->rot);
    return heading + M_Wrap(M_RestAngle(view, view->current) - heading);
}

// How high above the camera's view an entry reaches once the ring has folded
// onto its centre, over every angle the entry turns through about its own
// axis.
static int32_t M_Elevation(const M_ENTRY *const entry, const int32_t below)
{
    if (!entry->has_bounds) {
        return 0;
    }
    const XYZ_16 ends[2] = { entry->bounds.min, entry->bounds.max };
    int32_t result = 0;
    for (int32_t corner = 0; corner < 8; corner++) {
        const XYZ_32 pos = XYZ_32_Rotate(
            (XYZ_32) {
                .x = ends[corner & 1].x,
                .y = ends[(corner >> 1) & 1].y,
                .z = ends[(corner >> 2) & 1].z,
            },
            (XYZ_16) { .x = (int16_t)M_Wrap(
                           UI_Tween_GetTarget(&entry->x_rot)) });
        const float reach = sqrtf((float)pos.x * pos.x + (float)pos.z * pos.z);
        const int32_t depth = M_Whole(fmaxf(
            M_CAMERA_GAP - UI_Tween_GetTarget(&entry->z_trans) - reach, 1.0f));
        const int32_t height =
            M_Whole(below + UI_Tween_GetTarget(&entry->y_trans) + pos.y);
        result = MAX(result, M_Wrap(Math_Atan(depth, height)));
    }
    return result;
}

// The angle the camera tilts away from a ring that leaves, which takes the
// highest point any entry reaches past the edge of the screen.
static int32_t M_LeavePitch(const M_VIEW *const view)
{
    const float aspect = (float)Viewport_GetWidth(VIEWPORT_UI)
        / (float)MAX(Viewport_GetHeight(VIEWPORT_UI), 1);
    const float half_fov = M_FOV * (float)M_PI / DEG_360;
    const float focal = 4.0f / 3.0f / fmaxf(aspect, 16.0f / 10.0f)
        * (240.0f / 200.0f) / tanf(half_fov) * aspect;
    const float half_view = atanf(1.0f / focal) * DEG_180 / (float)M_PI;
    const int32_t below = -M_Whole(UI_Tween_GetTarget(&view->camera_y));
    const int32_t aim = Math_Atan(M_CAMERA_GAP, below + M_LOOK_Y);
    int32_t highest = 0;
    for (int32_t i = 0; i < view->count; i++) {
        highest = MAX(highest, M_Elevation(&view->entries[i], below));
    }
    return (int32_t)floorf(half_view + highest - aim);
}

static void M_StartMotion(
    M_VIEW *const view, const M_MOTION motion, const M_SPEC *const spec)
{
    M_ENTRY *const entry = &view->entries[view->current];
    const float rest = M_RestAngle(view, view->current);
    const float rot = UI_Tween_GetTarget(&view->rot);
    switch (motion) {
    case M_MOTION_OPEN:
        view->left = 16;
        UI_Tween_Set(&view->radius, 0);
        M_TweenTo(&view->radius, M_RADIUS, 16);
        UI_Tween_Set(&view->camera_y, M_CAMERA_START);
        M_TweenTo(&view->camera_y, M_CAMERA_HEIGHT, 16);
        UI_Tween_Set(&view->rot, rest - M_TURN);
        M_TweenTo(&view->rot, rest, 16);
        break;
    case M_MOTION_CLOSE:
        view->left = 16;
        M_TweenTo(&view->radius, 0, 16);
        M_TweenTo(&view->camera_y, M_CAMERA_START, 16);
        M_TweenTo(&view->rot, rot + M_TURN, 16);
        break;
    case M_MOTION_LEAVE_UP:
    case M_MOTION_LEAVE_DOWN:
        view->left = 12;
        view->left_by = motion == M_MOTION_LEAVE_UP ? M_LeavePitch(view)
                                                    : -M_LeavePitch(view);
        M_TweenTo(&view->radius, 0, 12);
        M_TweenTo(&view->pitch, view->base_pitch + view->left_by, 12);
        M_TweenTo(&view->rot, rot + M_TURN, 12);
        break;
    case M_MOTION_ARRIVE:
        view->left = 12;
        UI_Tween_Set(&view->radius, 0);
        M_TweenTo(&view->radius, M_RADIUS, 12);
        UI_Tween_Set(&view->pitch, view->base_pitch - view->left_by);
        M_TweenTo(&view->pitch, view->base_pitch, 12);
        UI_Tween_Set(&view->rot, rest - M_TURN);
        M_TweenTo(&view->rot, rest, 12);
        break;
    case M_MOTION_SELECT:
        view->left = 8;
        M_TweenTo(&view->rot, M_NearestRestAngle(view), 8);
        M_TweenTo(&entry->x_rot_pt, spec->x_rot_pt_sel, 8);
        M_TweenTo(&entry->x_rot, spec->x_rot_sel, 8);
        M_TweenTo(&entry->y_trans, spec->y_trans_sel, 8);
        M_TweenTo(&entry->z_trans, spec->z_trans_sel, 8);
        break;
    case M_MOTION_DESELECT:
        view->left = 8;
        M_TweenTo(&view->rot, M_NearestRestAngle(view), 8);
        M_TweenTo(&entry->x_rot_pt, 0, 8);
        M_TweenTo(&entry->x_rot, spec->x_rot_nosel, 8);
        M_TweenTo(&entry->y_trans, 0, 8);
        M_TweenTo(&entry->z_trans, 0, 8);
        break;
    }
}

static void M_TurnTo(M_VIEW *const view, const int32_t by)
{
    const int32_t count = view->count;
    const float start = UI_Tween_GetTarget(&view->rot);
    view->turning.active = true;
    view->turning.from = view->current;
    view->turning.to = ((view->current + by) % count + count) % count;
    view->turning.left = M_TURN_TICKS;
    view->turning.start = start;
    view->turning.by = (float)(-by * M_Spacing(view));
    M_TweenTo(&view->rot, start - by * M_Spacing(view), M_TURN_TICKS);
}

typedef enum {
    M_STEP_ZERO_256,
    M_STEP_ZERO_512,
    M_STEP_TOWARDS,
    M_STEP_ROUND,
    M_STEP_STILL,
} M_STEP;

static int32_t M_StepAngle(
    const int32_t angle, const M_STEP step, const int32_t target)
{
    switch (step) {
    case M_STEP_ZERO_256:
    case M_STEP_ZERO_512: {
        const int32_t by = step == M_STEP_ZERO_256 ? 256 : 512;
        if (angle > 0) {
            return MAX(angle - by, 0);
        } else if (angle < 0) {
            return MIN(angle + by, 0);
        }
        return angle;
    }
    case M_STEP_TOWARDS: {
        const int32_t delta = M_Wrap(target - angle);
        if (delta == 0) {
            return angle;
        }
        return (delta > 0 ? angle + 1024 : angle - 1024) & ~1023;
    }
    case M_STEP_ROUND:
        return angle + 256;
    case M_STEP_STILL:
        break;
    }
    return angle;
}

// Moves an entry's turn about its own axis by up to a step towards its
// target, two steps a tick.
static void M_Spin(M_ENTRY *const entry, const M_STEP step, const int32_t target)
{
    const float now = entry->y_rot_now;
    const int32_t at = M_Wrap(now);
    int32_t to = at;
    for (int32_t i = 0; i < 2; i++) {
        to = M_StepAngle(to, step, target);
    }
    entry->y_rot_last = now;
    entry->y_rot_now = now + M_Wrap(to - at);
}

static M_QUAT M_QuatMul(const M_QUAT a, const M_QUAT b)
{
    return (M_QUAT) {
        .w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        .x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        .y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        .z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

static M_QUAT M_QuatAbout(const int32_t axis, const float angle)
{
    const float half = angle * (float)M_PI / 32768.0f / 2.0f;
    M_QUAT q = { .w = cosf(half) };
    float *const parts[3] = { &q.x, &q.y, &q.z };
    *parts[axis] = sinf(half);
    return q;
}

static M_QUAT M_Slerp(const M_QUAT a, M_QUAT b, const float t)
{
    float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot < 0.0f) {
        b = (M_QUAT) { -b.w, -b.x, -b.y, -b.z };
        dot = -dot;
    }
    float ka = 1.0f - t;
    float kb = t;
    if (dot < 0.9995f) {
        const float theta = acosf(dot);
        const float s = sinf(theta);
        ka = sinf((1.0f - t) * theta) / s;
        kb = sinf(t * theta) / s;
    }
    M_QUAT q = {
        .w = ka * a.w + kb * b.w,
        .x = ka * a.x + kb * b.x,
        .y = ka * a.y + kb * b.y,
        .z = ka * a.z + kb * b.z,
    };
    const float len = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    q.w /= len;
    q.x /= len;
    q.y /= len;
    q.z /= len;
    return q;
}

// The engine's yaw, pitch and roll that turn as a rotation does.
static XYZ_32 M_QuatToRot(const M_QUAT q)
{
    const float m02 = 2.0f * (q.x * q.z + q.w * q.y);
    const float m10 = 2.0f * (q.x * q.y + q.w * q.z);
    const float m11 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    const float m12 = 2.0f * (q.y * q.z - q.w * q.x);
    const float m22 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, -m12)));
    const float k = 32768.0f / (float)M_PI;
    return (XYZ_32) {
        .x = M_Wrap(floorf(pitch * k + 0.5f)),
        .y = M_Wrap(floorf(atan2f(m02, m22) * k + 0.5f)),
        .z = M_Wrap(floorf(atan2f(m10, m11) * k + 0.5f)),
    };
}

// Runs a tick of the view. Returns whether the motion ended on this tick.
static bool M_TickView(
    M_VIEW *const view, const M_SPIN how, const M_CONTENT *const list)
{
    for (int32_t i = 0; i < view->count; i++) {
        M_ENTRY *const entry = &view->entries[i];
        M_QUAT hand = entry->hand_now;
        if (entry->turn_yaw != 0.0f || entry->turn_pitch != 0.0f) {
            hand = M_QuatMul(
                M_QuatMul(
                    M_QuatAbout(1, entry->turn_yaw),
                    M_QuatAbout(0, -entry->turn_pitch)),
                hand);
        }
        entry->turn_yaw = 0.0f;
        entry->turn_pitch = 0.0f;
        if (i != view->current) {
            M_Spin(entry, M_STEP_ZERO_256, 0);
        } else if (view->turning.active) {
            M_Spin(entry, M_STEP_ZERO_512, 0);
        } else if (how == M_SPIN_HELD) {
            if (entry->turned) {
                M_Spin(entry, M_STEP_STILL, 0);
            } else {
                M_Spin(entry, M_STEP_TOWARDS, list[i].spec->y_rot_sel);
            }
        } else if (how == M_SPIN_IDLE) {
            M_Spin(entry, M_STEP_ROUND, 0);
        } else {
            M_Spin(entry, M_STEP_STILL, 0);
        }
        if (!entry->turned) {
            hand = M_Slerp(hand, m_Identity, M_UNTURN_RATE);
        }
        entry->hand_last = entry->hand_now;
        entry->hand_now = hand;
    }

    if (view->turning.active && --view->turning.left == 0) {
        view->current = view->turning.to;
        view->turning.active = false;
    }

    if (view->left == 0) {
        return false;
    }
    view->left--;
    return view->left == 0;
}

// How far an entry stands from being the one the ring rests on, from 0 for
// that one to 1 for any other. The shade moves as the ring turns.
static float M_Shade(const M_VIEW *const view, const int32_t index)
{
    if (!view->turning.active) {
        return index == view->current ? 0.0f : 1.0f;
    }
    float done =
        (UI_Tween_Get(&view->rot) - view->turning.start) / view->turning.by;
    done = fmaxf(0.0f, fminf(1.0f, done));
    if (index == view->turning.from) {
        return done;
    } else if (index == view->turning.to) {
        return 1.0f - done;
    }
    return 1.0f;
}

static void M_SetCamera(const M_VIEW *const view, UI_MODEL_WORLD *const world)
{
    const int32_t radius = M_Whole(UI_Tween_Get(&view->radius));
    const XYZ_32 pos = {
        .y = M_Whole(UI_Tween_Get(&view->camera_y)),
        .z = radius + M_CAMERA_GAP,
    };
    const float dy = (float)(M_LOOK_Y - pos.y);
    const float dz = (float)(radius - pos.z);
    const float length = sqrtf(dy * dy + dz * dz);
    const float pitch = Math_Atan(-(int32_t)dz, -(int32_t)dy)
        + UI_Tween_Get(&view->pitch);
    const float angle = pitch * (float)M_PI / DEG_180;
    world->camera_pos = pos;
    world->camera_target = (XYZ_32) {
        .y = M_Whole(pos.y - sinf(angle) * length),
        .z = M_Whole(pos.z - cosf(angle) * length),
    };
    world->fov = M_FOV;
}

static void M_Step(
    UI_MODEL_WORLD *const world, const UI_MODEL_STEP kind, const int32_t x,
    const int32_t y, const int32_t z)
{
    if (world->step_count < UI_MODEL_STEPS) {
        world->steps[world->step_count].kind = kind;
        world->steps[world->step_count].vec = (XYZ_32) { x, y, z };
        world->step_count++;
    }
}

static void M_Pose(
    const M_VIEW *const view, const int32_t index, const M_SPEC *const spec,
    UI_MODEL_WORLD *const world)
{
    const M_ENTRY *const entry = &view->entries[index];
    const int32_t x_rot_pt = M_Wrap(UI_Tween_Get(&entry->x_rot_pt));
    const float rate = Interpolation_GetUIRate();
    const float y_rot =
        entry->y_rot_last + (entry->y_rot_now - entry->y_rot_last) * rate;
    M_Step(
        world, UI_MODEL_STEP_TURN, 0,
        M_Wrap(UI_Tween_Get(&view->rot) + index * M_Spacing(view)), 0);
    M_Step(
        world, UI_MODEL_STEP_MOVE, M_Whole(UI_Tween_Get(&view->radius)), 0, 0);
    M_Step(world, UI_MODEL_STEP_TURN, 0, DEG_90, 0);
    M_Step(world, UI_MODEL_STEP_TURN, x_rot_pt, 0, 0);
    M_Step(
        world, UI_MODEL_STEP_MOVE, 0,
        M_Whole(UI_Tween_Get(&entry->y_trans) + spec->y_offset),
        M_Whole(UI_Tween_Get(&entry->z_trans)));
    M_Step(world, UI_MODEL_STEP_TURN, M_Wrap(-x_rot_pt), 0, 0);
    const XYZ_32 hand =
        M_QuatToRot(M_Slerp(entry->hand_last, entry->hand_now, rate));
    M_Step(world, UI_MODEL_STEP_TURN, hand.x, hand.y, hand.z);
    M_Step(world, UI_MODEL_STEP_TURN, x_rot_pt, 0, 0);
    M_Step(
        world, UI_MODEL_STEP_TURN,
        M_Wrap(UI_Tween_Get(&entry->x_rot) + spec->base_rot_x),
        M_Wrap(y_rot + spec->base_rot_y), M_Wrap(spec->base_rot_z));
    if (spec->scale != 1.0) {
        M_Step(
            world, UI_MODEL_STEP_SCALE, (int32_t)lround(spec->scale * 1024.0),
            0, 0);
    }
}

static void M_InitView(
    M_VIEW *const view, const M_CONTENT *const list, const int32_t count,
    const int32_t current, const int32_t pitch)
{
    *view = (M_VIEW) {
        .count = count,
        .current = current,
        .base_pitch = pitch,
    };
    UI_Tween_Set(&view->radius, 0);
    UI_Tween_Set(&view->camera_y, M_CAMERA_START);
    UI_Tween_Set(&view->pitch, pitch);
    UI_Tween_Set(&view->rot, 0);
    for (int32_t i = 0; i < count; i++) {
        M_ENTRY *const entry = &view->entries[i];
        *entry = (M_ENTRY) {
            .hand_last = m_Identity,
            .hand_now = m_Identity,
        };
        UI_Tween_Set(&entry->x_rot_pt, 0);
        UI_Tween_Set(&entry->x_rot, list[i].spec->x_rot_nosel);
        UI_Tween_Set(&entry->y_trans, 0);
        UI_Tween_Set(&entry->z_trans, 0);
        const OBJECT *const object = Object_Get(list[i].object);
        if (object->loaded) {
            const ANIM *const anim = Object_GetAnim(object, 0);
            if (anim != nullptr && anim->frame_ptr != nullptr) {
                entry->has_bounds = true;
                entry->bounds = anim->frame_ptr->bounds;
            }
        }
    }
}

// --- The ring (ring/screen.lua) ---

// How long the backdrop takes to come in and to go, and how long the screen
// takes to go black when the game leaves, in ticks.
#define M_FADE_TICKS 16
#define M_BLACK_TICKS 8
#define M_BLACK_HOLD_TICKS 1
// How dark the ring goes behind the screen of the picked entry, where that
// screen asks for it.
#define M_DARKEN 0.75f

typedef enum {
    M_OPENING,
    M_OPEN,
    M_LEAVING,
    M_SELECTING,
    M_SELECTED,
    M_CLOSING_ITEM,
    M_DESELECTING,
    M_EXITING,
    M_CLOSING,
    M_FADING_OUT,
    M_DONE,
} M_STATUS;

typedef enum {
    M_ACTION_USE,
    M_ACTION_EXAMINE,
    M_ACTION_COMBINE,
} M_ACTION;

typedef struct {
    int32_t frame;
    int32_t last;
    int32_t goal;
    int32_t direction;
    int32_t count;
} M_ANIM;

typedef struct {
    int32_t index;
    OBJECT_ID object;
} M_PLACE;

typedef struct {
    OBJECT_ID object;
    M_ANIM anim;
    float y_rot;
} M_KEPT;

static struct {
    bool active;
    INVENTORY_MODE mode;
    M_STATUS status;
    M_STATUS after_close;
    M_STATUS close_to;
    M_RING ring_type;
    M_RING next_ring;
    M_CONTENT list[M_MAX_LIST];
    M_ANIM anims[M_MAX_LIST];
    M_VIEW view;
    M_KEPT kept[M_MAX_LIST * M_RING_NUMBER_OF];
    int32_t kept_count;
    int32_t needle;
    int32_t needle_speed;
    int32_t idle_ticks;
    int32_t demo_ticks;
    bool turning_held;
    bool passport_open;
    bool passport_shown;
    bool demo_needed;
    bool waiting;
    bool hidden;
    bool offered;
    bool unhandled;
    bool busy;
    bool left_by_screen;
    bool darkened;
    bool health_bar;
    OBJECT_ID chosen;
    M_ACTION action;
    INV_ITEM_ACTIONS actions;
    UI_TAKEOVER screen;
    struct {
        bool valid;
        int32_t index;
        OBJECT_ID object;
        M_PLACE place;
    } stale;
    UI_TWEEN backdrop;
    BACKGROUND_TYPE style;
    const char *background;
    bool is_live;
    int32_t fade_id;
    // What the ring says: the caption, the key hints, the count and the
    // heading. The caption is the one the ring set, or the script's.
    const char *caption;
    bool caption_ours;
    bool arrow_up;
    bool arrow_down;
    char hints[2][96];
    int32_t hint_count;
    char count_text[32];
    char heading[64];
    int32_t hint_slot;
    int32_t count_slot;
    int32_t heading_slot;
    int32_t dc_slots[3];
} m_Ring = { .screen = UI_TAKEOVER_NUMBER_OF };

// The entry each ring rests on, kept from one opening to the next, and the
// entry each ring was left on.
static M_PLACE m_Places[M_RING_NUMBER_OF];
static bool m_HasPlace[M_RING_NUMBER_OF];
static OBJECT_ID m_LeftOn[M_RING_NUMBER_OF];
// The entry a script, or the engine, asked the ring to rest on next.
static OBJECT_ID m_Requested = NO_OBJECT;

static void M_Forget(void)
{
    for (int32_t i = 0; i < M_RING_NUMBER_OF; i++) {
        m_HasPlace[i] = false;
        m_LeftOn[i] = NO_OBJECT;
    }
}

static bool M_IsRemembered(const M_RING ring)
{
    switch (g_Config.gameplay.ring_memory_mode) {
    case RING_MEMORY_MAIN:
        return ring == M_RING_MAIN;
    case RING_MEMORY_ALL:
        return ring == M_RING_MAIN || ring == M_RING_KEYS;
    default:
        return false;
    }
}

static int32_t M_IndexOf(
    const M_CONTENT *const list, const int32_t count, const OBJECT_ID object)
{
    for (int32_t i = 0; i < count; i++) {
        if (list[i].object == object) {
            return i;
        }
    }
    return -1;
}

// Where a ring rests as it opens: where it was left, where the player asked
// for that, or else the place it last rested at.
static int32_t M_RestingPlace(
    const M_RING ring, const M_CONTENT *const list, const int32_t count)
{
    if (M_IsRemembered(ring) && m_LeftOn[ring] != NO_OBJECT) {
        const int32_t index = M_IndexOf(list, count, m_LeftOn[ring]);
        if (index >= 0) {
            return index;
        }
    }
    if (!m_HasPlace[ring]) {
        return 0;
    }
    const M_PLACE place = m_Places[ring];
    if (g_Config.gameplay.fix_item_duplication_glitch && place.index < count
        && list[place.index].object != place.object) {
        return 0;
    }
    return MAX(0, MIN(place.index, count - 1));
}

static void M_KeepPlace(void)
{
    if (m_Ring.stale.valid) {
        m_Places[m_Ring.ring_type] = m_Ring.stale.place;
        m_HasPlace[m_Ring.ring_type] = true;
        return;
    }
    const int32_t current = m_Ring.view.current;
    m_Places[m_Ring.ring_type] = (M_PLACE) {
        .index = current,
        .object = current < m_Ring.view.count ? m_Ring.list[current].object
                                              : NO_OBJECT,
    };
    m_HasPlace[m_Ring.ring_type] = true;
}

// The place the keys ring rests on when the main ring moves up to it, where
// the duplication glitch is left unfixed and an entry used up at the end of
// the list left the place past the end: the ring rests on the entry the
// place wraps round to, as the original game does.
static bool M_StalePlace(const M_RING ring)
{
    m_Ring.stale.valid = false;
    if (ring != M_RING_KEYS || g_Config.gameplay.fix_item_duplication_glitch
        || !m_HasPlace[ring] || m_Places[ring].object == NO_OBJECT) {
        return false;
    }
    M_CONTENT list[M_MAX_LIST];
    const int32_t count = M_Contents(ring, list);
    const M_PLACE place = m_Places[ring];
    if (count == 0 || place.index < count) {
        return false;
    }
    m_Ring.stale.valid = true;
    m_Ring.stale.index = place.index % count;
    m_Ring.stale.object = place.object;
    m_Ring.stale.place = place;
    return true;
}

// --- Drawing ---

// The meshes of the passport, one bit each, and which show as its pages turn.
#define M_PASS_SPINE 1
#define M_PASS_FRONT 2
#define M_PASS_IN_FRONT 4
#define M_PASS_PAGE_2 8
#define M_PASS_BACK 16
#define M_PASS_IN_BACK 32
#define M_PASS_PAGE_1 64
#define M_PASS_COMMON (M_PASS_SPINE | M_PASS_BACK | M_PASS_IN_BACK | M_PASS_FRONT)

static const struct {
    int32_t frame;
    uint32_t meshes;
} m_PassPages[] = {
    { 14, M_PASS_IN_FRONT | M_PASS_PAGE_1 },
    { 18, M_PASS_IN_FRONT | M_PASS_PAGE_1 | M_PASS_PAGE_2 },
    { 19, M_PASS_PAGE_1 | M_PASS_PAGE_2 },
    { 23, M_PASS_PAGE_1 | M_PASS_PAGE_2 | M_PASS_IN_BACK },
    { 28, M_PASS_PAGE_2 | M_PASS_IN_BACK },
    { 29, 0 },
};

static uint32_t M_CrystalMeshes(void)
{
    const bool heal = g_Config.gameplay.save_crystal_mode == SAVE_CRYSTAL_HEAL;
    int32_t index;
    if (g_TRVersion == 3) {
        index = heal ? 0 : 1;
    } else if (heal) {
        index = 2;
    } else {
        index = g_Config.visuals.enable_ps1_crystals ? 1 : 0;
    }
    const OBJECT *const object = Object_Get(O_SAVE_CRYSTAL_OPTION);
    if (index >= object->mesh_count) {
        index = 0;
    }
    return 1u << index;
}

static uint32_t M_MeshesOf(
    const OBJECT_ID object, const M_SPEC *const spec, const int32_t frame)
{
    if (object == O_PASSPORT_OPTION) {
        for (size_t i = 0; i < ARRAY_SIZE(m_PassPages); i++) {
            if (frame <= m_PassPages[i].frame) {
                return M_PASS_COMMON | m_PassPages[i].meshes;
            }
        }
        return M_PASS_COMMON;
    } else if (object == O_COMPASS_OPTION || object == O_STOPWATCH_OPTION) {
        return frame == 0 || frame >= 18 ? (uint32_t)spec->meshes_sel
                                         : UINT32_MAX;
    } else if (object == O_SAVE_CRYSTAL_OPTION) {
        return M_CrystalMeshes();
    }
    return (uint32_t)spec->meshes_sel;
}

static RGB_F M_Grey(const float value)
{
    return (RGB_F) { value / 255.0f, value / 255.0f, value / 255.0f };
}

// The light an entry is drawn in, each game the way it lit its own ring, and
// the entry the ring rests on brighter than the rest.
static UI_MODEL_LIGHT M_LightOf(const float shade)
{
    UI_MODEL_LIGHT light = {};
    if (g_TRVersion == 3) {
        const float unit = 1.0f / 4096.0f;
        light.ambient = M_Grey(32.0f);
        light.light_count = 3;
        light.lights[0].dir = (XYZ_32) { 0x4000, -0x4000, 0x3000 };
        light.lights[0].color = (RGB_F) { 3312 * unit, 1664 * unit, 0 };
        light.lights[1].dir = (XYZ_32) { -0x4000, -0x4000, 0x3000 };
        light.lights[1].color =
            (RGB_F) { 3312 * unit, 3312 * unit, 3312 * unit };
        light.lights[2].dir = (XYZ_32) { 0, 0x2000, 0x3000 };
        light.lights[2].color = (RGB_F) { 0, 0, 3072 * unit };
        return light;
    }
    // TR1 and TR2 shade a model darker by its ambient light and lighten it
    // towards the ring's one light.
    light.ambient = M_Grey(128.0f - 32.0f * shade);
    light.light_count = 1;
    light.lights[0].dir = (XYZ_32) { -1536, 256, 1024 };
    light.lights[0].color = M_Grey(170.67f);
    return light;
}

// The needle of the compass swings towards north as Lara faces, and settles.
static void M_SwingNeedle(const M_ENTRY *const entry)
{
    const ITEM *const lara = Lara_GetItem();
    if (lara == nullptr) {
        return;
    }
    const int32_t delta =
        M_Wrap(-entry->y_rot_now - lara->rot.y - m_Ring.needle);
    m_Ring.needle_speed = m_Ring.needle_speed * 19 / 20 + delta / 50;
    m_Ring.needle = M_Wrap(m_Ring.needle + m_Ring.needle_speed);
}

static void M_StopwatchBones(UI_MODEL_WORLD *const world)
{
    const GF_LEVEL *const level = GF_GetCurrentLevel();
    const LEVEL_STATS *const stats =
        level != nullptr ? Stats_GetLevelStats(level) : nullptr;
    const int32_t seconds =
        stats != nullptr ? (int32_t)(stats->timer / LOGIC_FPS) : 0;
    const int32_t periods[3] = { 43200, 3600, 60 };
    for (int32_t i = 0; i < 3; i++) {
        world->bones[i].bone = 3 + i;
        world->bones[i].axes.z = true;
        world->bones[i].rot.z =
            (int16_t)M_Wrap((seconds % periods[i]) * DEG_1 * -360 / periods[i]);
    }
    world->bone_count = 3;
}

// The object an entry draws now: the passport opens out of its closed cover
// once the player picks it.
static OBJECT_ID M_DrawnObject(const int32_t index)
{
    const OBJECT_ID object = m_Ring.list[index].object;
    if (object == O_PASSPORT_CLOSED && index == m_Ring.view.current
        && m_Ring.passport_open) {
        return O_PASSPORT_OPTION;
    }
    return object;
}

static void M_PaintEntry(const int32_t index)
{
    const M_VIEW *const view = &m_Ring.view;
    const M_CONTENT *const content = &m_Ring.list[index];
    const M_ANIM *const anim = &m_Ring.anims[index];
    const OBJECT_ID object = M_DrawnObject(index);
    UI_MODEL_WORLD world = {
        .frame = anim->frame,
        .next_frame = anim->frame,
        .at_pivot = content->spec->draws_at_pivot,
    };
    M_SetCamera(view, &world);
    M_Pose(view, index, content->spec, &world);
    // The entry moves from the frame the tick started on to the one it
    // reached.
    if (anim->last != anim->frame) {
        world.frame = anim->last;
        world.next_frame = anim->frame;
        world.blend = Interpolation_GetUIRate();
    }
    if (object == O_COMPASS_OPTION) {
        world.bones[0].bone = 0;
        world.bones[0].axes.y = true;
        world.bones[0].rot.y = (int16_t)m_Ring.needle;
        world.bone_count = 1;
    } else if (object == O_STOPWATCH_OPTION) {
        M_StopwatchBones(&world);
    }
    const UI_MODEL_LIGHT light = M_LightOf(M_Shade(view, index));
    UI_ScheduleDrawModelInWorld(
        object, M_MeshesOf(object, content->spec, anim->frame), &world, &light);
}

// The entries from the back of the ring to the front, so that a nearer entry
// stands over the ones behind it.
static void M_Paint(void)
{
    if (m_Ring.mode != INV_TITLE_MODE) {
        const float progress = UI_Tween_Get(&m_Ring.backdrop);
        UI_ScheduleDrawBackdrop(
            m_Ring.style, m_Ring.style == BK_IMAGE ? 1.0f - progress : progress,
            nullptr);
    } else if (m_Ring.background != nullptr && !m_Ring.is_live) {
        UI_ScheduleDrawBackground(
            GamePath_PeekResolve(
                GAME_DYNAMIC_PATH_IMAGE_FILE, m_Ring.background),
            false, 0.0f);
    }
    if (m_Ring.hidden) {
        return;
    }
    const M_VIEW *const view = &m_Ring.view;
    int32_t order[M_MAX_LIST];
    float depth[M_MAX_LIST];
    for (int32_t i = 0; i < view->count; i++) {
        const float angle = UI_Tween_Get(&view->rot) + i * M_Spacing(view);
        order[i] = i;
        depth[i] = sinf(angle * (float)M_PI / (2 * DEG_90));
    }
    for (int32_t i = 1; i < view->count; i++) {
        const int32_t o = order[i];
        int32_t j = i - 1;
        for (; j >= 0 && depth[order[j]] < depth[o]; j--) {
            order[j + 1] = order[j];
        }
        order[j + 1] = o;
    }
    for (int32_t i = 0; i < view->count; i++) {
        M_PaintEntry(order[i]);
    }
    if (m_Ring.screen != UI_TAKEOVER_NUMBER_OF && m_Ring.darkened) {
        UI_ScheduleDrawDarken(M_DARKEN);
    }
}

// --- What the ring says ---

// Puts a ring of a type on show, resting on an entry, with the camera where
// the ring before it left it. Each entry keeps its animation frame and its
// turn about its own axis until the ring closes, as it moves between rings.
static void M_Fill(const M_RING ring, const int32_t current)
{
    M_VIEW *const before = &m_Ring.view;
    const bool had = m_Ring.view.count > 0;
    for (int32_t i = 0; had && i < before->count; i++) {
        int32_t k = 0;
        for (; k < m_Ring.kept_count; k++) {
            if (m_Ring.kept[k].object == m_Ring.list[i].object) {
                break;
            }
        }
        if (k == m_Ring.kept_count) {
            if (k >= (int32_t)ARRAY_SIZE(m_Ring.kept)) {
                continue;
            }
            m_Ring.kept_count++;
        }
        m_Ring.kept[k] = (M_KEPT) {
            .object = m_Ring.list[i].object,
            .anim = m_Ring.anims[i],
            .y_rot = before->entries[i].y_rot_now,
        };
    }
    const float camera_y = UI_Tween_Get(&before->camera_y);
    const float pitch = UI_Tween_Get(&before->pitch);
    const int32_t left_by = before->left_by;

    const int32_t count = M_Contents(ring, m_Ring.list);
    M_InitView(
        &m_Ring.view, m_Ring.list, count, MAX(0, MIN(current, count - 1)),
        m_Ring.mode == INV_TITLE_MODE ? M_TITLE_PITCH : 0);
    for (int32_t i = 0; i < count; i++) {
        m_Ring.anims[i] = (M_ANIM) { .direction = 1 };
        for (int32_t k = 0; k < m_Ring.kept_count; k++) {
            if (m_Ring.kept[k].object == m_Ring.list[i].object) {
                m_Ring.anims[i] = m_Ring.kept[k].anim;
                m_Ring.view.entries[i].y_rot_last = m_Ring.kept[k].y_rot;
                m_Ring.view.entries[i].y_rot_now = m_Ring.kept[k].y_rot;
            }
        }
    }
    if (had) {
        UI_Tween_Set(&m_Ring.view.camera_y, camera_y);
        UI_Tween_Set(&m_Ring.view.pitch, pitch);
        m_Ring.view.left_by = left_by;
    }
    m_Ring.ring_type = ring;
}

// Rests the ring on the entry asked for, where the ring holds it and Lara
// carries it.
static void M_RestOnRequest(void)
{
    const OBJECT_ID object = m_Requested;
    m_Requested = NO_OBJECT;
    if (object == NO_OBJECT) {
        return;
    }
    const int32_t index = M_IndexOf(m_Ring.list, m_Ring.view.count, object);
    if (index >= 0
        && (m_Ring.ring_type == M_RING_OPTION || Inv_HasItem(object))) {
        m_Ring.view.current = index;
        UI_Tween_Set(&m_Ring.view.rot, M_RestAngle(&m_Ring.view, index));
    }
}

// The weapon whose pickup or ammunition an entry is, if any.
static const WEAPON_INFO *M_WeaponOf(const OBJECT_ID object, bool *const ammo)
{
    *ammo = false;
    const int32_t count = Gun_Registry_GetCount();
    for (int32_t i = 0; i < count; i++) {
        if (!Gun_Registry_IsValidType(i) || i <= LGT_UNARMED) {
            continue;
        }
        const WEAPON_INFO *const w = Gun_Registry_Get(i);
        if (w->gun_object_id != NO_OBJECT
            && Inv_GetItemOption(w->gun_object_id) == object) {
            return w;
        }
        if (w->ammo_object_id != NO_OBJECT
            && Inv_GetItemOption(w->ammo_object_id) == object) {
            *ammo = true;
            return w;
        }
    }
    return nullptr;
}

static void M_FormatCount(const char *const fmt, const int32_t count)
{
    char inner[32];
    snprintf(
        inner, sizeof(inner), GS("general/inventory_ring/item_count_fmt"), fmt);
    snprintf(m_Ring.count_text, sizeof(m_Ring.count_text), inner, count);
}

// The count that stands with the caption: what a weapon has left in shots,
// the rounds a box of ammunition holds, and how many there are of anything
// else Lara carries more than one of.
static void M_CountOf(const M_CONTENT *const content)
{
    m_Ring.count_text[0] = '\0';
    bool is_ammo;
    const WEAPON_INFO *const w = M_WeaponOf(content->object, &is_ammo);
    if (w != nullptr) {
        if (Gun_HasInfiniteAmmo(w->gun_type)) {
            return;
        }
        if (is_ammo) {
            M_FormatCount("%d", content->count * w->ammo.box_label_qty);
            return;
        }
        char fmt[24] = "%5d";
        if (g_TRVersion == 1 && w->ammo_icon != nullptr) {
            snprintf(fmt, sizeof(fmt), "%%5d %s", w->ammo_icon);
        }
        M_FormatCount(
            fmt,
            Inv_GetAmmo(w->gun_type) / MAX(Gun_GetRoundsPerShot(w->gun_type), 1));
        return;
    }
    if (content->object == O_FLARES_BOX_OPTION) {
        if (Gun_Registry_IsValidType(LGT_FLARE)
            && Gun_HasInfiniteAmmo(LGT_FLARE)) {
            return;
        }
        M_FormatCount("%d", content->count);
        return;
    }
    if (content->count > 1) {
        M_FormatCount("%d", content->count);
    }
}

static const char *M_NameOf(OBJECT_ID object)
{
    if (object == O_PASSPORT_OPTION) {
        return nullptr;
    }
    // In the save pickup mode the crystal in the inventory is the save
    // crystal Lara picked up, rather than a plain collectible.
    if (object == O_SAVE_CRYSTAL_OPTION
        && g_Config.gameplay.save_crystal_mode == SAVE_CRYSTAL_SAVE_PICKUP) {
        object = O_SAVE_CRYSTAL_ITEM;
    }
    const char *const name = Object_GetName(object);
    return name != nullptr && name[0] != '\0' ? name : nullptr;
}

static void M_SetHints(void)
{
    m_Ring.hint_count = 0;
    const INV_ITEM_ACTIONS a = m_Ring.actions;
    if (!(a.can_examine || a.can_combine)) {
        return;
    }
    if (a.can_examine) {
        snprintf(
            m_Ring.hints[m_Ring.hint_count++], sizeof(m_Ring.hints[0]),
            "\\{input menu_show_info} %s", GS("general/actions/examine_item"));
    }
    if (a.can_use || a.can_combine) {
        snprintf(
            m_Ring.hints[m_Ring.hint_count++], sizeof(m_Ring.hints[0]),
            "\\{input menu_confirm} %s",
            a.can_combine ? GS("general/actions/combine_item")
                          : GS("general/actions/use_item"));
    }
}

static const char *M_HeadingOf(void)
{
    if (m_Ring.ring_type == M_RING_MAIN) {
        return GS("general/inventory_ring/heading_inventory");
    } else if (m_Ring.ring_type == M_RING_KEYS) {
        return GS("general/inventory_ring/heading_items");
    } else if (m_Ring.mode == INV_DEATH_MODE) {
        return GS("general/inventory_ring/heading_game_over");
    }
    return GS("general/inventory_ring/heading_option");
}

static void M_Caption(const char *const text)
{
    if (!m_Ring.caption_ours || text != m_Ring.caption) {
        m_Ring.caption = text;
        m_Ring.caption_ours = true;
        DC_NoLua_SetCaption(text, true, false);
    }
}

static void M_ShowArrows(const bool shown)
{
    bool up = false;
    bool down = false;
    if (shown && m_Ring.mode == INV_GAME_MODE) {
        up = m_Ring.ring_type == M_RING_OPTION
            || (m_Ring.ring_type == M_RING_MAIN && M_IsAvailable(M_RING_KEYS));
        down = m_Ring.ring_type == M_RING_KEYS
            || (m_Ring.ring_type == M_RING_MAIN
                && M_IsAvailable(M_RING_OPTION));
    }
    if (up == m_Ring.arrow_up && down == m_Ring.arrow_down) {
        return;
    }
    m_Ring.arrow_up = up;
    m_Ring.arrow_down = down;
    DC_NoLua_ShowArrow(DC_ARROW_TOP_LEFT, up);
    DC_NoLua_ShowArrow(DC_ARROW_TOP_RIGHT, up);
    DC_NoLua_ShowArrow(DC_ARROW_BOTTOM_LEFT, down);
    DC_NoLua_ShowArrow(DC_ARROW_BOTTOM_RIGHT, down);
}

static bool M_IsShown(const M_STATUS status)
{
    return status == M_OPEN || status == M_SELECTING || status == M_SELECTED
        || status == M_CLOSING_ITEM || status == M_DESELECTING;
}

static void M_Quiet(void)
{
    m_Ring.hint_count = 0;
    m_Ring.count_text[0] = '\0';
    m_Ring.health_bar = false;
}

// Names the entry the ring rests on and what can be done with it, while the
// ring stands still. The caption is the screen's while it holds the entry,
// and once a screen the player left the ring from closes.
static void M_Say(void)
{
    const bool shown = M_IsShown(m_Ring.status);
    m_Ring.heading[0] = '\0';
    if (shown && m_Ring.mode != INV_TITLE_MODE) {
        snprintf(
            m_Ring.heading, sizeof(m_Ring.heading),
            GS("general/inventory_ring/heading_fmt"), M_HeadingOf());
    }
    M_ShowArrows(shown);

    const M_VIEW *const view = &m_Ring.view;
    const bool quiet = !shown || view->turning.active
        || (m_Ring.turning_held && view->count > 1);
    const bool scripted = (m_Ring.screen != UI_TAKEOVER_NUMBER_OF
                           && m_Ring.action != M_ACTION_EXAMINE)
        || (m_Ring.status == M_SELECTED && m_Ring.action == M_ACTION_COMBINE)
        || m_Ring.left_by_screen;
    if (quiet) {
        M_Caption(nullptr);
        M_Quiet();
        return;
    }
    if (scripted) {
        // The screen sets the caption meanwhile, so the ring sets its own
        // again once it takes it back.
        m_Ring.caption_ours = false;
        m_Ring.health_bar = false;
        return;
    }

    const M_CONTENT *const content = &m_Ring.list[view->current];
    M_Caption(M_NameOf(M_DrawnObject(view->current)));
    m_Ring.health_bar = content->object == O_SMALL_MEDIPACK_OPTION
        || content->object == O_LARGE_MEDIPACK_OPTION;
    // A screen open on the entry, such as the examine screen, shows hints of
    // its own.
    if (m_Ring.screen == UI_TAKEOVER_NUMBER_OF) {
        if (m_Ring.status == M_OPEN) {
            M_SetHints();
        } else {
            m_Ring.hint_count = 0;
        }
    }
    M_CountOf(content);
}

// --- Statuses ---

static void M_SetStatus(const M_STATUS status)
{
    m_Ring.status = status;
}

static void M_Start(const M_STATUS status, const M_MOTION motion)
{
    m_Ring.status = status;
    M_StartMotion(
        &m_Ring.view, motion, m_Ring.list[m_Ring.view.current].spec);
}

static bool M_IsForcedMode(const INVENTORY_MODE mode)
{
    return mode == INV_SAVE_MODE || mode == INV_SAVE_CRYSTAL_MODE
        || mode == INV_LOAD_MODE || mode == INV_DEATH_MODE;
}

static bool M_LeavesAfterSave(const INVENTORY_MODE mode)
{
    return mode == INV_SAVE_MODE || mode == INV_SAVE_CRYSTAL_MODE
        || mode == INV_LOAD_MODE;
}

static bool M_IsLeaving(void)
{
    return Shell_IsExiting() || GF_GetOverrideCommand().action != GF_NOOP;
}

// Whether the game leaves once the ring closes. Lara's Home is started only
// as the ring finishes, so the game is not leaving for it yet.
static bool M_LeavesGame(void)
{
    return M_IsLeaving() || m_Ring.demo_needed
        || (m_Ring.chosen == O_PHOTO_OPTION && GF_GetGymLevel() != nullptr);
}

static int32_t M_BackdropTicks(void)
{
    return DC_NoLua_FadesEnabled() && g_Config.ui.inventory_fade_effects
        ? M_FADE_TICKS
        : 0;
}

static void M_Close(void)
{
    M_Start(M_CLOSING, M_MOTION_CLOSE);
    if (M_LeavesGame()) {
        m_Ring.after_close = M_FADING_OUT;
    } else {
        m_Ring.after_close = M_DONE;
        const int32_t ticks = M_BackdropTicks();
        if (ticks > 0) {
            M_TweenTo(&m_Ring.backdrop, 0.0f, ticks);
        }
    }
}

static void M_Finish(void)
{
    if (m_Ring.status == M_DONE) {
        return;
    }
    m_Ring.status = M_DONE;
    M_Quiet();
    if (m_Ring.demo_needed) {
        const int32_t idx = Demo_ChooseLevel(-1);
        if (idx >= 0) {
            GF_OverrideCommand(
                (GF_COMMAND) {
                    .action = GF_START_DEMO,
                    .param = GF_GetLevelTable(GFLT_DEMOS)->levels[idx].num,
                },
                true);
        }
    } else if (m_Ring.chosen == O_PHOTO_OPTION) {
        const GF_LEVEL *const gym = GF_GetGymLevel();
        if (gym != nullptr) {
            SG_Manager_UnbindSlot();
            GF_OverrideCommand(
                (GF_COMMAND) { .action = GF_SELECT_GAME, .param = gym->num },
                true);
        }
    } else if (m_Ring.chosen != NO_OBJECT && !M_IsLeaving()) {
        Lara_UseItem(m_Ring.chosen);
    }
    DC_Screens_Close(UI_TAKEOVER_MENU, UI_TAKEOVER_CHOICE_CONFIRM);
}

static void M_FadedOut(const int32_t id)
{
    M_Finish();
}

static void M_BlackRest(const int32_t id)
{
    DC_NoLua_FadeMove(id, 1.0f, M_BLACK_HOLD_TICKS, false, M_FadedOut);
}

static int32_t M_PickSound(const OBJECT_ID object)
{
    if (object == O_COMPASS_OPTION) {
        return SFX_MENU_COMPASS;
    } else if (object == O_STOPWATCH_OPTION) {
        return -1;
    } else if (object == O_PHOTO_OPTION) {
        return SFX_MENU_LARA_HOME;
    } else if (object == O_CONTROLS_OPTION) {
        return g_TRVersion == 1 ? SFX_MENU_GAMEBOY : SFX_MENU_CHOOSE;
    }
    bool is_ammo;
    if (M_WeaponOf(object, &is_ammo) != nullptr && !is_ammo) {
        return SFX_MENU_GUNS;
    }
    return SFX_MENU_CHOOSE;
}

static void M_Sound(const int32_t sample)
{
    if (sample >= 0) {
        Sound_Effect(sample, nullptr, SPM_ALWAYS);
    }
}

// Picks the entry the ring rests on, to use it or to examine it.
static void M_Pick(const bool examine)
{
    if (M_IsForcedMode(m_Ring.mode)) {
        m_Ring.passport_shown = true;
    }
    M_KeepPlace();
    const int32_t current = m_Ring.view.current;
    const M_CONTENT *const content = &m_Ring.list[current];
    M_ANIM *const anim = &m_Ring.anims[current];
    if (examine) {
        m_Ring.action = M_ACTION_EXAMINE;
        anim->goal = 0;
    } else if (m_Ring.actions.can_combine) {
        m_Ring.action = M_ACTION_COMBINE;
        anim->goal = 0;
    } else {
        m_Ring.action = M_ACTION_USE;
        anim->goal = content->spec->open_frame;
    }
    anim->direction = 1;
    m_Ring.offered = false;
    M_Start(M_SELECTING, M_MOTION_SELECT);
    M_Sound(M_PickSound(content->object));
}

static void M_CloseItem(const bool exits)
{
    m_Ring.close_to = exits ? M_EXITING : M_DESELECTING;
    m_Ring.status = M_CLOSING_ITEM;
}

static void M_LeaveFor(const M_RING ring, const bool up)
{
    m_Ring.next_ring = ring;
    M_Start(M_LEAVING, up ? M_MOTION_LEAVE_UP : M_MOTION_LEAVE_DOWN);
}

// In the expansion, B held for this long on the title goes back to the game.
// The next game is chosen (dc_games.c): the ring closes and the background
// fades, as for a new game, and the game then ends.
static void M_LeaveForGame(void)
{
    GF_OverrideCommand((GF_COMMAND) { .action = GF_EXIT_GAME }, false);
    m_Ring.chosen = NO_OBJECT;
    M_Sound(SFX_MENU_SPINOUT);
    M_Close();
}

// The game that Up (-1) or Down (1) goes to from here: the expansions keep
// to the expansions.
static int M_Neighbour(const int delta)
{
    if (DC_Games_InExpansion()) {
        return DC_Games_StepExpansionId(delta);
    }
    return delta < 0 ? DC_Games_PrevId() : DC_Games_NextId();
}

#define M_ORIGINAL_HOLD_MS 2000
static uint64_t m_BackSince = 0;

static void M_ControlOpen(void)
{
    M_VIEW *const view = &m_Ring.view;
    if (view->turning.active) {
        return;
    }
    m_Ring.turning_held = g_Input.menu_left || g_Input.menu_right;
    if (g_Input.menu_right && view->count > 1) {
        m_Ring.stale.valid = false;
        M_TurnTo(view, -1);
        M_Sound(SFX_MENU_ROTATE);
        return;
    }
    if (g_Input.menu_left && view->count > 1) {
        m_Ring.stale.valid = false;
        M_TurnTo(view, 1);
        M_Sound(SFX_MENU_ROTATE);
        return;
    }

    if (g_InputDB.menu_back && m_Ring.mode != INV_TITLE_MODE) {
        m_Ring.chosen = NO_OBJECT;
        M_KeepPlace();
        M_Sound(SFX_MENU_SPINOUT);
        M_Close();
        return;
    }

    const bool examine = g_InputDB.menu_show_info && m_Ring.actions.can_examine;
    if (g_InputDB.menu_confirm || examine) {
        M_Pick(examine);
        return;
    }

    if (m_Ring.mode == INV_TITLE_MODE && DC_Games_InExpansion()) {
        // B held for a while goes back to the original game.
        if (!g_Input.menu_back) {
            m_BackSince = 0;
        } else if (m_BackSince == 0) {
            m_BackSince = timer_ms_gettime64();
        } else if (
            timer_ms_gettime64() - m_BackSince >= M_ORIGINAL_HOLD_MS
            && DC_Games_ChooseOriginal(-1)) {
            m_BackSince = 0;
            M_LeaveForGame();
            return;
        }
    }
    if (m_Ring.mode == INV_TITLE_MODE && DC_Input_TakeEndLevelCombo()
        && DC_Games_ChooseLevelList()) {
        // L + R + X + Y: the list of levels of this game or expansion.
        M_LeaveForGame();
        return;
    }
    if (m_Ring.mode == INV_TITLE_MODE) {
        // A Dreamcast disc of several games: Up goes to the game before this
        // one and Down to the one after.
        const bool in_expansion = DC_Games_InExpansion();
        int32_t delta = 0;
        if (g_InputDB.menu_up && M_Neighbour(-1) != 0) {
            delta = -1;
        } else if (g_InputDB.menu_down && M_Neighbour(1) != 0) {
            delta = 1;
        }
        // A test disc (SWITCH.TXT "100 <frame>") presses Down by itself.
        static int32_t test_frames = 0;
        if (g_DC_TestSwitchMode == 100 && ++test_frames == g_DC_TestSwitchFrame) {
            test_frames = 0;
            delta = M_Neighbour(1) != 0 ? 1 : -1;
        }
        // A test disc (SWITCH.TXT "101 <frame>") starts the expansion.
        static int32_t expansion_frames = 0;
        if (g_DC_TestSwitchMode == 101
            && ++expansion_frames == g_DC_TestSwitchFrame
            && DC_Games_ChooseExpansion(-1)) {
            M_LeaveForGame();
            return;
        }
        if (delta != 0
            && (in_expansion ? DC_Games_ChooseExpansionStep(delta)
                             : DC_Games_Choose(delta, true))) {
            M_LeaveForGame();
        }
        return;
    }
    if (m_Ring.mode == INV_KEYS_MODE) {
        return;
    }
    if (g_InputDB.menu_up) {
        if (m_Ring.ring_type == M_RING_MAIN && M_IsAvailable(M_RING_KEYS)) {
            M_LeaveFor(M_RING_KEYS, true);
        } else if (
            m_Ring.ring_type == M_RING_OPTION && M_IsAvailable(M_RING_MAIN)) {
            M_LeaveFor(M_RING_MAIN, true);
        }
    } else if (g_InputDB.menu_down) {
        if (m_Ring.ring_type == M_RING_MAIN && M_IsAvailable(M_RING_OPTION)) {
            M_LeaveFor(M_RING_OPTION, false);
        } else if (
            m_Ring.ring_type == M_RING_KEYS && M_IsAvailable(M_RING_MAIN)) {
            M_LeaveFor(M_RING_MAIN, false);
        }
    }
}

// The entries that do what they are for at once when no screen draws them:
// a weapon or a medipack is used, and so are the keys and the plain items.
static bool M_IsUsedAtOnce(const M_CONTENT *const content)
{
    if (content->spec->is_used_at_once) {
        return true;
    }
    bool is_ammo;
    if (M_WeaponOf(content->object, &is_ammo) != nullptr) {
        return !is_ammo;
    }
    return content->object == O_SMALL_MEDIPACK_OPTION
        || content->object == O_LARGE_MEDIPACK_OPTION
        || (content->spec->inv_pos >= 100 && content->spec->inv_pos < 200)
        || content->object == O_SCION_OPTION
        || ObjectFamily_Has(content->object, OBJ_FAMILY_GENERIC_INV_OPTION);
}

// What the player does with an entry that no screen holds.
static void M_ControlEntry(const bool busy)
{
    const int32_t current = m_Ring.view.current;
    const M_CONTENT *const content = &m_Ring.list[current];
    M_ANIM *const anim = &m_Ring.anims[current];
    bool confirm = g_InputDB.menu_confirm;
    bool back = g_InputDB.menu_back;

    if (m_Ring.action != M_ACTION_USE) {
        // Nothing examines or combines it, so any key puts it away.
        if (!busy && (confirm || g_InputDB.menu_show_info)) {
            back = true;
            confirm = false;
        }
    } else if (M_IsUsedAtOnce(content)) {
        confirm = !busy;
    } else {
        bool is_ammo;
        if (M_WeaponOf(content->object, &is_ammo) == nullptr && !busy
            && (confirm || back)) {
            anim->goal = 0;
            anim->direction = -1;
        }
    }

    if (busy) {
        return;
    }
    if (back) {
        M_CloseItem(M_LeavesAfterSave(m_Ring.mode));
    } else if (confirm) {
        m_Ring.chosen =
            m_Ring.stale.valid ? m_Ring.stale.object : content->object;
        M_KeepPlace();
        const bool stays = m_Ring.mode == INV_TITLE_MODE
            && (content->object == O_GRAPHICS_OPTION
                || content->object == O_SOUND_OPTION
                || content->object == O_PDA_OPTION
                || content->object == O_CONTROLS_OPTION);
        M_CloseItem(!stays);
    }
}

static void M_EntryScreenClosed(const bool confirmed)
{
    const UI_TAKEOVER screen = m_Ring.screen;
    m_Ring.screen = UI_TAKEOVER_NUMBER_OF;
    m_Ring.darkened = false;
    if (screen == UI_TAKEOVER_NUMBER_OF) {
        return;
    }
    if (m_Ring.action == M_ACTION_EXAMINE) {
        m_Ring.view.entries[m_Ring.view.current].turned = false;
    }
    if (m_Ring.status != M_SELECTED) {
        return;
    }
    // A ring opened to save or load holds nothing else worth going back to,
    // so putting the entry away leaves it. A combined entry goes back into
    // the ring, which builds itself again around what the screen made.
    const bool leaves = (confirmed && m_Ring.action != M_ACTION_COMBINE)
        || M_LeavesAfterSave(m_Ring.mode);
    if (confirmed) {
        M_KeepPlace();
    }
    m_Ring.left_by_screen = leaves;
    M_CloseItem(leaves);
}

static UI_TAKEOVER M_ScreenOf(const M_ACTION action)
{
    switch (action) {
    case M_ACTION_EXAMINE:
        return UI_TAKEOVER_EXAMINE;
    case M_ACTION_COMBINE:
        return UI_TAKEOVER_COMBINE;
    default:
        return UI_TAKEOVER_RING_ENTRY;
    }
}

// Offers the screen of what the player does with the entry, once.
static void M_Offer(void)
{
    if (m_Ring.offered) {
        return;
    }
    m_Ring.offered = true;
    const UI_TAKEOVER screen = M_ScreenOf(m_Ring.action);
    const UI_TAKEOVER_ARGS args = {
        .object_id = M_DrawnObject(m_Ring.view.current),
        .mode = m_Ring.mode,
    };
    if (DC_Screens_OpenEntry(screen, &args, M_EntryScreenClosed)) {
        m_Ring.screen = screen;
        m_Ring.unhandled = false;
    } else {
        m_Ring.unhandled = true;
    }
}

// Runs the selected entry's animation two steps a tick. Returns whether it
// is still on its way.
static bool M_Animate(void)
{
    const int32_t index = m_Ring.view.current;
    M_ANIM *const anim = &m_Ring.anims[index];
    const M_SPEC *const spec = m_Ring.list[index].spec;
    bool busy = false;
    for (int32_t i = 0; i < 2; i++) {
        busy = anim->frame != anim->goal;
        if (!busy) {
            continue;
        }
        if (anim->count > 0) {
            anim->count--;
        } else {
            anim->count = spec->anim_speed;
            anim->frame += anim->direction;
            if (anim->frame >= spec->frames_total) {
                anim->frame = 0;
            } else if (anim->frame < 0) {
                anim->frame = spec->frames_total - 1;
            }
        }
    }
    return busy;
}

// Builds the ring again where the entry the player put back is gone, as
// combining two takes them, and rests it on what was asked for.
static void M_Refresh(void)
{
    const M_CONTENT *const content = &m_Ring.list[m_Ring.view.current];
    if (Inv_HasItem(content->object) || m_Ring.ring_type == M_RING_OPTION) {
        return;
    }
    M_Fill(m_Ring.ring_type, m_Ring.view.current);
    M_VIEW *const view = &m_Ring.view;
    UI_Tween_Set(&view->radius, M_RADIUS);
    UI_Tween_Set(&view->camera_y, M_CAMERA_HEIGHT);
    M_RestOnRequest();
    UI_Tween_Set(&view->rot, M_RestAngle(view, view->current));
}

static bool M_IsPassport(void)
{
    return m_Ring.view.count > 0
        && m_Ring.list[m_Ring.view.current].object == O_PASSPORT_CLOSED;
}

static void M_Run(void)
{
    M_VIEW *view = &m_Ring.view;
    for (int32_t i = 0; i < view->count; i++) {
        m_Ring.anims[i].last = m_Ring.anims[i].frame;
    }

    const M_STATUS status = m_Ring.status;
    if (status == M_SELECTED) {
        if (M_IsPassport() && !m_Ring.passport_open) {
            m_Ring.passport_open = true;
        }
        M_Offer();
        if (m_Ring.screen != UI_TAKEOVER_NUMBER_OF && M_IsLeaving()) {
            m_Ring.chosen = m_Ring.list[view->current].object;
            M_KeepPlace();
            const UI_TAKEOVER screen = m_Ring.screen;
            m_Ring.screen = UI_TAKEOVER_NUMBER_OF;
            DC_Screens_Cancel(screen);
            M_CloseItem(true);
        }
    } else if (status == M_CLOSING_ITEM) {
        if (!M_Animate()) {
            const int32_t index = view->current;
            if (m_Ring.passport_open) {
                m_Ring.passport_open = false;
                m_Ring.anims[index].frame = 0;
                m_Ring.anims[index].last = 0;
            }
            if (m_Ring.close_to == M_DESELECTING) {
                M_Refresh();
                M_Sound(SFX_MENU_SPINOUT);
                M_Start(M_DESELECTING, M_MOTION_DESELECT);
            } else {
                M_Start(M_EXITING, M_MOTION_DESELECT);
            }
        }
    } else if (status == M_EXITING && view->left == 0) {
        M_Close();
    }
    view = &m_Ring.view;

    if (m_Ring.status == M_SELECTED) {
        const M_ENTRY *const entry = &view->entries[view->current];
        const M_SPEC *const spec = m_Ring.list[view->current].spec;
        m_Ring.busy = M_Wrap(entry->y_rot_now) == M_Wrap(spec->y_rot_sel)
            ? M_Animate()
            : false;
    }

    M_SPIN spin = M_SPIN_IDLE;
    if (m_Ring.status == M_OPENING || m_Ring.status == M_DONE
        || m_Ring.status == M_FADING_OUT) {
        spin = M_SPIN_STILL;
    } else if (
        m_Ring.status == M_SELECTING || m_Ring.status == M_SELECTED
        || m_Ring.status == M_CLOSING_ITEM || m_Ring.status == M_DESELECTING) {
        spin = M_SPIN_HELD;
    } else if (m_Ring.turning_held && view->count > 1) {
        spin = M_SPIN_STILL;
    }

    if (m_Ring.status != M_DONE && !m_Ring.waiting
        && M_TickView(view, spin, m_Ring.list)) {
        const M_STATUS ended = m_Ring.status;
        if (ended == M_OPENING || ended == M_DESELECTING) {
            M_SetStatus(M_OPEN);
        } else if (ended == M_SELECTING) {
            M_SetStatus(M_SELECTED);
        } else if (ended == M_LEAVING) {
            M_KeepPlace();
            const M_RING next = m_Ring.next_ring;
            int32_t index = m_HasPlace[next] ? m_Places[next].index : 0;
            if (M_StalePlace(next)) {
                index = m_Ring.stale.index;
            }
            const bool stale = m_Ring.stale.valid;
            M_Fill(next, index);
            m_Ring.stale.valid = stale;
            M_Start(M_OPENING, M_MOTION_ARRIVE);
        } else if (ended == M_CLOSING) {
            if (m_Ring.after_close == M_FADING_OUT) {
                M_SetStatus(M_FADING_OUT);
                const int32_t id = DC_NoLua_FadeOpen(0.0f, false);
                DC_NoLua_FadeMove(id, 1.0f, M_BLACK_TICKS, false, M_BlackRest);
                if (id == 0) {
                    M_Finish();
                }
            } else {
                M_Finish();
            }
        }
    }
    view = &m_Ring.view;

    // The compass needle swings while Lara's facing shows.
    if (m_Ring.mode != INV_TITLE_MODE && !m_Ring.waiting
        && m_Ring.status != M_OPENING) {
        const int32_t index =
            M_IndexOf(m_Ring.list, view->count, O_COMPASS_OPTION);
        if (index >= 0) {
            M_SwingNeedle(&view->entries[index]);
        }
    }

    if (m_Ring.status != M_DONE && view->count > 0) {
        if (M_IsShown(m_Ring.status)) {
            m_Ring.actions =
                Inv_GetItemActions(m_Ring.list[view->current].object);
        }
        M_Say();
    }
}

static void M_Control(void)
{
    if (m_Ring.status == M_OPEN) {
        if (M_IsForcedMode(m_Ring.mode) && !m_Ring.passport_shown) {
            M_Pick(false);
            return;
        }
        M_ControlOpen();
    } else if (m_Ring.status == M_SELECTED && m_Ring.unhandled) {
        M_ControlEntry(m_Ring.busy);
    }

    // The title opens on the next demo once nobody has touched it for a
    // while.
    if (m_Ring.mode == INV_TITLE_MODE) {
        if (InputState_IsAnyPressed(g_Input)) {
            m_Ring.idle_ticks = 0;
        } else if (m_Ring.status == M_OPEN) {
            m_Ring.idle_ticks++;
            if (m_Ring.demo_ticks > 0
                && m_Ring.idle_ticks >= m_Ring.demo_ticks) {
                m_Ring.demo_needed = true;
                m_Ring.chosen = NO_OBJECT;
                M_Sound(SFX_MENU_SPINOUT);
                M_Close();
            }
        }
    }
}

// --- The screen ---

static int32_t M_DemoTicks(void)
{
    if (!g_Config.gameplay.enable_demo
        || GF_GetLevelTable(GFLT_DEMOS)->count == 0) {
        return 0;
    }
    return (int32_t)floorf(g_Config.flow.demo_delay * LOGIC_FPS);
}

static void M_FirstRing(M_RING *const ring, int32_t *const current)
{
    M_CONTENT list[M_MAX_LIST];
    *ring = M_RING_OPTION;
    *current = 0;
    if (m_Ring.mode == INV_KEYS_MODE) {
        *ring = M_RING_KEYS;
        *current =
            M_RestingPlace(M_RING_KEYS, list, M_Contents(M_RING_KEYS, list));
    } else if (m_Ring.mode == INV_GAME_MODE) {
        const int32_t count = M_Contents(M_RING_MAIN, list);
        if (count > 0) {
            *ring = M_RING_MAIN;
            *current = M_RestingPlace(M_RING_MAIN, list, count);
        }
    }
}

static void M_SpinIn(void)
{
    m_Ring.waiting = false;
    M_Sound(SFX_MENU_SPININ);
    M_StartMotion(
        &m_Ring.view, M_MOTION_OPEN, m_Ring.list[m_Ring.view.current].spec);
}

static void M_TitleFadedIn(const int32_t id)
{
    m_Ring.fade_id = 0;
    m_Ring.hidden = false;
    M_SpinIn();
}

static void M_End(void)
{
    if (!m_Ring.active) {
        return;
    }
    // The engine ends the ring before it is done where something else starts
    // a level at once. The ring has had no time to fade out, so it fades to
    // black as it goes.
    if (m_Ring.status != M_DONE && DC_NoLua_FadesEnabled()) {
        DC_NoLua_FadeLeaveToBlack(M_BLACK_TICKS, M_BLACK_HOLD_TICKS);
    }
    if (m_Ring.screen != UI_TAKEOVER_NUMBER_OF) {
        const UI_TAKEOVER screen = m_Ring.screen;
        m_Ring.screen = UI_TAKEOVER_NUMBER_OF;
        DC_Screens_Cancel(screen);
    }
    if (m_Ring.fade_id != 0) {
        DC_NoLua_FadeClose(m_Ring.fade_id);
        m_Ring.fade_id = 0;
    }
    if (m_Ring.view.count > 0) {
        m_LeftOn[m_Ring.ring_type] = m_Ring.list[m_Ring.view.current].object;
    }
    M_ShowArrows(false);
    DC_NoLua_SetCaption(nullptr, false, false);
    M_Quiet();
    m_Ring.heading[0] = '\0';
    DC_NoLua_ShowVersion(false);
    m_Ring.active = false;
}

static bool M_Open(const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    if (m_Ring.active) {
        M_End();
    }
    m_Ring = (typeof(m_Ring)) {
        .mode = args->mode,
        .status = M_OPENING,
        .chosen = NO_OBJECT,
        .screen = UI_TAKEOVER_NUMBER_OF,
        .background = args->background,
        .is_live = args->is_live,
        .demo_ticks = args->mode == INV_TITLE_MODE ? M_DemoTicks() : 0,
    };
    if (args->mode == INV_KEYS_MODE && !M_IsAvailable(M_RING_KEYS)) {
        return false;
    }

    // The ring opens where it was left, and the keys ring is sent back to
    // its first entry where the duplication glitch is fixed and the player
    // did not ask the rings to remember.
    if (g_Config.gameplay.fix_item_duplication_glitch
        && !M_IsRemembered(M_RING_KEYS)) {
        m_HasPlace[M_RING_KEYS] = false;
    }
    m_HasPlace[M_RING_MAIN] = false;
    m_HasPlace[M_RING_OPTION] = false;

    if (args->object_id != NO_OBJECT) {
        m_Requested = args->object_id;
    }
    M_RING ring;
    int32_t current;
    M_FirstRing(&ring, &current);
    M_CONTENT list[M_MAX_LIST];
    if (M_Contents(ring, list) == 0) {
        return false;
    }
    M_Fill(ring, current);
    M_RestOnRequest();
    m_Ring.active = true;

    m_Ring.style = g_Config.ui.inventory_background_style;
    UI_Tween_Set(&m_Ring.backdrop, 0.0f);
    M_TweenTo(&m_Ring.backdrop, 1.0f, M_BackdropTicks());

    if (args->mode == INV_TITLE_MODE) {
        DC_NoLua_ShowVersion(true);
        // The title comes in from black before its ring spins in.
        m_Ring.waiting = true;
        m_Ring.hidden = true;
        m_Ring.fade_id = DC_NoLua_FadeOpen(1.0f, false);
        DC_NoLua_FadeMove(
            m_Ring.fade_id, 0.0f, DC_NoLua_FadesEnabled() ? M_FADE_TICKS : 0,
            true, M_TitleFadedIn);
        if (m_Ring.fade_id == 0) {
            M_TitleFadedIn(0);
        }
    } else {
        M_SpinIn();
    }
    return true;
}

static void M_Release(const UI_TAKEOVER screen)
{
    M_End();
}

// --- Events ---

static bool M_OnTick(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (!m_Ring.active) {
        return false;
    }
    // The ring holds still as the game exits, under the fade to black.
    if (Shell_IsExiting()) {
        return false;
    }
    if (m_Ring.status != M_DONE) {
        M_Control();
    }
    if (m_Ring.active && m_Ring.status != M_DONE) {
        M_Run();
    }
    return false;
}

#define M_SPACING 20
#define M_PART_SPACING 60
#define M_COUNT_SHIFT 64
#define M_CAPTION_GAP 8

static void M_Measure(const char *const text, float *const w, float *const h)
{
    UI_Text_Measure(text, w, h, (UI_TEXT_SETTINGS) { .scale = 1.0f });
}

static void M_Text(const char *const text, const float x, const float y)
{
    UI_Text_Draw(text, x, y, (UI_TEXT_SETTINGS) { .scale = 1.0f });
}

#define M_HOLD_BAR_GAP 2.0f
#define M_HOLD_BAR_H 4.0f

// The bar under the hint fills as B is held: a dark frame and a fill in the
// game's gold, as the passport's hold button draws it.
static void M_DrawHoldBar(const float x, const float y, const float w)
{
    const RGBA_8888 frame = { 0x06, 0x06, 0x06, 0xFF };
    const RGBA_8888 fill = { 0xA1, 0x83, 0x3C, 0xFF };
    float done = (float)(timer_ms_gettime64() - m_BackSince)
        / (float)M_ORIGINAL_HOLD_MS;
    done = done < 0.0f ? 0.0f : done > 1.0f ? 1.0f : done;
    const int32_t x0 = (int32_t)floorf(UI_ScaleX(x) + 0.5f);
    const int32_t x1 = (int32_t)floorf(UI_ScaleX(x + w) + 0.5f);
    const int32_t y0 = (int32_t)floorf(UI_ScaleY(y) + 0.5f);
    const int32_t y1 = (int32_t)floorf(UI_ScaleY(y + M_HOLD_BAR_H) + 0.5f);
    UI_ScheduleDrawScreenFlatQuad(x0, y0, 0, x1 - x0, y1 - y0, frame);
    UI_ScheduleDrawScreenFlatQuad(
        x0 + 1, y0 + 1, 0, (int32_t)((x1 - x0 - 2) * done), y1 - y0 - 2, fill);
}

static bool M_HasDCHints(void)
{
    return m_Ring.active && m_Ring.mode == INV_TITLE_MODE && !m_Ring.hidden
        && m_Ring.status != M_SELECTED && m_Ring.status != M_CLOSING_ITEM;
}

static void M_DCHintText(const int32_t i, char *const buf, const size_t size)
{
    if (i == 2) {
        buf[0] = '\0';
        if (DC_Games_InExpansion()) {
            snprintf(
                buf, size,
                m_BackSince != 0 ? "Returning..."
                                 : "Hold B: return to base");
        }
        return;
    }
    const int32_t game = M_Neighbour(i == 0 ? -1 : 1);
    const char *const kind = DC_Games_InExpansion() ? " Expansion" : "";
    buf[0] = 0;
    if (game != 0) {
        snprintf(
            buf, size,
            i == 0 ? "Press Up to go to TR%d%s" : "Press Down to go to TR%d%s",
            (int)game, kind);
    }
}

// The hints and the count stand over the caption at the foot of the screen,
// and the name of the ring at the top.
static bool M_OnReserve(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    m_Ring.hint_slot = -1;
    m_Ring.count_slot = -1;
    m_Ring.heading_slot = -1;
    m_Ring.dc_slots[0] = -1;
    m_Ring.dc_slots[1] = -1;
    m_Ring.dc_slots[2] = -1;
    if (!m_Ring.active) {
        return false;
    }
    const float scale = UI_Scaler_GetTextScale();
    float w;
    float h;
    if (m_Ring.heading[0] != '\0') {
        M_Measure(m_Ring.heading, &w, &h);
        m_Ring.heading_slot = UI_Region_Reserve(UI_REGION_TOP_CENTER, w, h);
    }
    const bool hints = m_Ring.hint_count > 0;
    const bool count = m_Ring.count_text[0] != '\0';
    if (hints) {
        float total = 0.0f;
        float height = 0.0f;
        for (int32_t i = 0; i < m_Ring.hint_count; i++) {
            M_Measure(m_Ring.hints[i], &w, &h);
            total += w + (i > 0 ? M_PART_SPACING * scale : 0.0f);
            height = fmaxf(height, h);
        }
        m_Ring.hint_slot = UI_Region_Reserve(
            UI_REGION_BOTTOM_CENTER, total,
            height + (count ? 0.0f : M_CAPTION_GAP * scale));
    }
    if (count) {
        M_Measure(m_Ring.count_text, &w, &h);
        m_Ring.count_slot = UI_Region_Reserve(
            UI_REGION_BOTTOM_CENTER, w, h + M_CAPTION_GAP * scale);
    }
    if (M_HasDCHints()) {
        for (int32_t i = 0; i < 3; i++) {
            char text[48];
            M_DCHintText(i, text, sizeof(text));
            if (text[0] != '\0') {
                M_Measure(text, &w, &h);
                m_Ring.dc_slots[i] = UI_Region_Reserve(
                    i == 0       ? UI_REGION_TOP_RIGHT
                        : i == 1 ? UI_REGION_BOTTOM_RIGHT
                                 : UI_REGION_TOP_LEFT,
                    w, i == 2 ? h + M_HOLD_BAR_GAP + M_HOLD_BAR_H : h);
            }
        }
    }
    return false;
}

static bool M_OnPaint(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (!m_Ring.active) {
        return false;
    }
    M_Paint();
    const float scale = UI_Scaler_GetTextScale();
    float x;
    float y;
    float w;
    float h;
    if (UI_Region_GetSlotBox(m_Ring.heading_slot, &x, &y, &w, &h)) {
        M_Text(m_Ring.heading, x, y);
    }
    if (UI_Region_GetSlotBox(m_Ring.hint_slot, &x, &y, &w, &h)) {
        for (int32_t i = 0; i < m_Ring.hint_count; i++) {
            float pw;
            float ph;
            M_Measure(m_Ring.hints[i], &pw, &ph);
            M_Text(m_Ring.hints[i], x, y);
            x += pw + M_PART_SPACING * scale;
        }
    }
    if (UI_Region_GetSlotBox(m_Ring.count_slot, &x, &y, &w, &h)) {
        M_Text(m_Ring.count_text, x + M_COUNT_SHIFT * scale, y);
    }
    for (int32_t i = 0; i < 3; i++) {
        if (UI_Region_GetSlotBox(m_Ring.dc_slots[i], &x, &y, &w, &h)) {
            char text[48];
            M_DCHintText(i, text, sizeof(text));
            M_Text(text, x, y);
            if (i == 2 && m_BackSince != 0) {
                M_DrawHoldBar(x, y + h + M_HOLD_BAR_GAP, w);
            }
        }
    }
    return false;
}

static bool M_OnGameStart(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    M_Forget();
    return false;
}

void DC_NoLua_SetupRing(void)
{
    if (!IS_OK(M_ReadSpecs())) {
        LOG_ERROR("ring: inv_ring.json5 could not be read");
    }
    DC_Screens_Define(UI_TAKEOVER_MENU, M_Open, M_Release);
    DC_Events_Listen(LUA_EVENT_TICK, M_OnTick);
    DC_Events_Listen(LUA_EVENT_UI_RESERVE_ALL, M_OnReserve);
    DC_Events_Listen(LUA_EVENT_UI_PAINT, M_OnPaint);
    DC_Events_Listen(LUA_EVENT_GAME_START, M_OnGameStart);
}

bool DC_Ring_ForcesHealthBar(void)
{
    return m_Ring.active && m_Ring.health_bar;
}

void DC_Ring_Request(const OBJECT_ID object)
{
    m_Requested = object;
}

void DC_Ring_SetDarkened(const bool darkened)
{
    m_Ring.darkened = darkened;
}

void DC_Ring_TurnSelection(const int32_t yaw, const int32_t pitch)
{
    if (!m_Ring.active || m_Ring.view.count == 0 || (yaw == 0 && pitch == 0)) {
        return;
    }
    M_ENTRY *const entry = &m_Ring.view.entries[m_Ring.view.current];
    entry->turned = true;
    entry->turn_yaw += yaw;
    entry->turn_pitch += pitch;
}

bool DC_Ring_GetSelectionAnim(
    int32_t *const frame, int32_t *const goal, int32_t *const open_frame,
    int32_t *const frame_count)
{
    if (!m_Ring.active || m_Ring.view.count == 0) {
        return false;
    }
    const int32_t index = m_Ring.view.current;
    *frame = m_Ring.anims[index].frame;
    *goal = m_Ring.anims[index].goal;
    *open_frame = m_Ring.list[index].spec->open_frame;
    *frame_count = m_Ring.list[index].spec->frames_total;
    return true;
}

void DC_Ring_AnimateSelection(const int32_t goal, const int32_t direction)
{
    if (!m_Ring.active || m_Ring.view.count == 0) {
        return;
    }
    const int32_t index = m_Ring.view.current;
    if (goal < 0 || goal >= m_Ring.list[index].spec->frames_total) {
        return;
    }
    m_Ring.anims[index].goal = goal;
    m_Ring.anims[index].direction = direction < 0 ? -1 : 1;
}

// The frame the picked entry rests open on, or -1 while it moves or no entry
// is picked.
int32_t DC_Ring_GetRestFrame(void)
{
    if (!m_Ring.active || m_Ring.status != M_SELECTED
        || m_Ring.view.count == 0) {
        return -1;
    }
    const M_ANIM *const anim = &m_Ring.anims[m_Ring.view.current];
    return anim->frame == anim->goal ? anim->frame : -1;
}

INVENTORY_MODE DC_Ring_GetMode(void)
{
    return m_Ring.mode;
}

bool DC_Ring_IsOpen(void)
{
    return m_Ring.active;
}

void DC_Ring_SetHints(const char *const *const parts, const int32_t count)
{
    m_Ring.hint_count = 0;
    for (int32_t i = 0; i < count && i < 2; i++) {
        snprintf(
            m_Ring.hints[i], sizeof(m_Ring.hints[i]), "%s", parts[i]);
        m_Ring.hint_count++;
    }
}
