#include <trx/game/fx/weather.h>

#include <trx/config.h>
#include <trx/core/json/util/read_io.h>
#include <trx/core/json/util/write_io.h>
#include <trx/core/math.h>
#include <trx/core/memory.h>
#include <trx/core/utils.h>
#include <trx/game/camera.h>
#include <trx/game/fx/common.h>
#include <trx/game/game/state.h>
#include <trx/game/interpolation.h>
#include <trx/game/lara.h>
#include <trx/game/matrix.h>
#include <trx/game/objects.h>
#include <trx/game/output/shaders/mesh.h>
#include <trx/game/output/sources/poly_fx.h>
#include <trx/game/output/state.h>
#include <trx/game/output/vars.h>
#include <trx/game/random.h>
#include <trx/game/rooms.h>
#include <trx/game/viewport.h>

#include <string.h>

#if defined(__DREAMCAST__)
    #include <platform/dreamcast/dc_mode.h>
#endif

#define M_BASE_WEATHER 256
#define M_PARTICLE_BYTES 72
#define M_BASE_WEATHER_ALIVE 16
#if defined(__DREAMCAST__)
    // Twice the base at most: 1024 drops and 1024 flakes took 98K of RAM
    // the PowerVR could not draw at speed anyway. Past severity 2 the
    // spawn just finds no free slot.
    #define M_MAX_WEATHER (M_BASE_WEATHER * 2)
#else
    #define M_MAX_WEATHER (M_BASE_WEATHER * WEATHER_SEVERITY_MAX)
#endif

#define M_RAIN_MAX_DISTANCE 6000
#define M_RAIN_BASE_Y_OFF (-WALL_L)
#define M_RAIN_BASE_YV 16
#define M_RAIN_YV_RND_MASK 7
#define M_RAIN_SPAWN_DIST_MASK 0xFFF
#define M_RAIN_SPAWN_ANGLE_MASK 0x1FFE
#define M_RAIN_Y_RND_MASK 0x7FF
#define M_RAIN_LIFE_BASE 88

#define M_SNOW_LIFE_BASE 96
#define M_SNOW_YV_MIN 8
#define M_SNOW_YV_RANGE 24

typedef struct {
    XYZ_32 pos;
    XYZ_32 prev_pos;
    int32_t prev_yv;
    XYZ_32 vel;
    uint8_t life;
} M_RAINDROP;

typedef struct {
    XYZ_32 pos;
    XYZ_32 prev_pos;
    bool stopped;
    int32_t prev_yv;
    int32_t prev_life;
    XYZ_32 vel;
    uint8_t life;
} M_SNOWFLAKE;

// A zero pos.x marks a free slot.
// Made when a level sets its weather and let go at the next reset, so levels
// without any carry none of it.
static M_RAINDROP *m_Raindrops = nullptr;
static M_SNOWFLAKE *m_Snowflakes = nullptr;
static WEATHER_TYPE m_WeatherType = WEATHER_NONE;
static float m_Severity = 1.0f;

static void M_ClearWeather(void)
{
    Memory_FreePointer(&m_Raindrops);
    Memory_FreePointer(&m_Snowflakes);
}

static void M_Ensure(const WEATHER_TYPE type)
{
    if (type == WEATHER_RAIN && m_Raindrops == nullptr) {
        m_Raindrops = Memory_Alloc(sizeof(M_RAINDROP) * M_MAX_WEATHER);
    } else if (type == WEATHER_SNOW && m_Snowflakes == nullptr) {
        m_Snowflakes = Memory_Alloc(sizeof(M_SNOWFLAKE) * M_MAX_WEATHER);
    }
}

// A slot past the count is left alone rather than emptied, so lowering the
// severity lets the particles already falling finish their lives.
static int32_t M_GetParticleCount(void)
{
    int32_t count = (int32_t)(M_BASE_WEATHER * m_Severity);
#if defined(__DREAMCAST__)
    // The PSX keeps 192 particles (TOMB3.EXE 0x73560).
    if (count > 192) {
        count = 192;
    }
    if (g_DC_TestSnow >= 0 && count > g_DC_TestSnow) {
        count = g_DC_TestSnow;
    }
#endif
    return count;
}

static int32_t M_GetSpawnCount(void)
{
    return (int32_t)(M_BASE_WEATHER_ALIVE * m_Severity);
}

static int64_t M_GetViewDepth(const XYZ_32 pos)
{
    // clang-format off
    return
        g_ViewMatrix._20 * pos.x +
        g_ViewMatrix._21 * pos.y +
        g_ViewMatrix._22 * pos.z +
        g_ViewMatrix._23;
    // clang-format on
}

static bool M_SpawnParticle(XYZ_32 *const pos)
{
    const ITEM *const lara_item = Lara_GetItem();
    if (lara_item == nullptr) {
        return false;
    }

    XYZ_32 base_pos = {};
    if (g_Camera.type == CAM_FIXED || g_Camera.type == CAM_FLYBY_MODE) {
        base_pos = g_Camera.pos.pos;
        if (g_Camera.target.y < g_Camera.pos.y) {
            base_pos.y += M_RAIN_BASE_Y_OFF;
        }
    } else {
        if (!Lara_GetMeshPos(LM_HIPS, &base_pos)) {
            base_pos = lara_item->pos;
        }
        base_pos.y += M_RAIN_BASE_Y_OFF;
    }

    const int32_t dist = Random_GetDraw() & M_RAIN_SPAWN_DIST_MASK;
    const int32_t angle = (Random_GetDraw() & M_RAIN_SPAWN_ANGLE_MASK) * 8;
    *pos = XYZ_32_OffsetYaw(base_pos, angle, dist);
    pos->y -= (Random_GetDraw() & M_RAIN_Y_RND_MASK);

    int16_t room_num = NO_ROOM;
    if (Room_GetOutsideStatus(*pos, &room_num, nullptr) != 1) {
        pos->x = 0;
        return false;
    }

    return true;
}

static void M_SpawnRainDrop(M_RAINDROP *const drop)
{
    if (!M_SpawnParticle(&drop->pos)) {
        return;
    }

    drop->vel.y =
        (uint8_t)((Random_GetDraw() & M_RAIN_YV_RND_MASK) + M_RAIN_BASE_YV);
    drop->vel.x = (int8_t)((Random_GetDraw() & 7) - 4);
    drop->vel.z = (int8_t)((Random_GetDraw() & 7) - 4);
    drop->life = (uint8_t)(M_RAIN_LIFE_BASE - ((int32_t)drop->vel.y << 1));
    drop->prev_pos = drop->pos;
    drop->prev_yv = drop->vel.y;
}

[[maybe_unused]] static void M_UpdateRainC(void)
{
    const XZ_32 wind = Sparks_GetSmokeWind();
    const int32_t count = M_GetParticleCount();
    const int32_t spawn_count = M_GetSpawnCount();

    int32_t num_alive = 0;
    for (int32_t i = 0; i < M_MAX_WEATHER; i++) {
        M_RAINDROP *const drop = &m_Raindrops[i];

        if (drop->pos.x == 0 && i < count && num_alive < spawn_count) {
            num_alive++;
            M_SpawnRainDrop(drop);
        }

        if (drop->pos.x == 0) {
            continue;
        }

        drop->prev_pos = drop->pos;
        drop->prev_yv = drop->vel.y;

        int16_t room_num = NO_ROOM;
        const int32_t outside =
            Room_GetOutsideStatus(drop->pos, &room_num, nullptr);
        if (outside == -2
            || (room_num != NO_ROOM && Room_Get(room_num)->flags.underwater)
            || drop->life > 240
            || ABS(g_Camera.pos.x - drop->pos.x) > M_RAIN_MAX_DISTANCE
            || ABS(g_Camera.pos.z - drop->pos.z) > M_RAIN_MAX_DISTANCE) {
            drop->pos.x = 0;
            continue;
        }

        drop->pos.x += (int32_t)drop->vel.x + 4 * wind.x;
        drop->pos.y += (int32_t)drop->vel.y * 8;
        drop->pos.z += (int32_t)drop->vel.z + 4 * wind.z;

        int32_t rnd = Random_GetDraw();
        if ((rnd & 3) != 3) {
            drop->vel.x += (int8_t)((rnd & 3) - 1);
            if (drop->vel.x < -4) {
                drop->vel.x = -4;
            } else if (drop->vel.x > 4) {
                drop->vel.x = 4;
            }
        }

        rnd = (rnd >> 2) & 3;
        if (rnd != 3) {
            drop->vel.z += (int8_t)(rnd - 1);
            if (drop->vel.z < -4) {
                drop->vel.z = -4;
            } else if (drop->vel.z > 4) {
                drop->vel.z = 4;
            }
        }

        drop->life -= 2;
        if (drop->life > 240) {
            drop->pos.x = 0;
        }
    }
}

[[maybe_unused]] static void M_DrawRainC(void)
{
    const XZ_32 wind = Sparks_GetSmokeWind();
    const TRX_REAL ratio = Interpolation_GetWorldRate();
    const bool do_interp =
        Interpolation_IsActive() && ratio > 0.0 && ratio < 1.0;

    // The PS1 adds its drops to the scene rather than blending them into it,
    // which is what makes them read as pale rather than blue.
    const bool ps1 = g_Config.visuals.enable_ps1_rain;
    const RGBA_8888 from_color =
        ps1 ? (RGBA_8888) { 0, 0, 0, 0xFF } : (RGBA_8888) { 0, 0, 0x20, 0x00 };
    const RGBA_8888 to_color = ps1 ? (RGBA_8888) { 0x30, 0x40, 0x40, 0xFF }
                                   : (RGBA_8888) { 0x30, 0x40, 0x60, 0x80 };
    const DRAW_TYPE draw_type = ps1 ? DRAW_BLEND_ADD : DRAW_BLEND;

    for (int32_t i = 0; i < M_MAX_WEATHER; i++) {
        const M_RAINDROP *const drop = &m_Raindrops[i];
        if (drop->pos.x == 0) {
            continue;
        }

        const int32_t yv = do_interp
            ? (int32_t)LERP(drop->prev_yv, drop->vel.y, ratio)
            : (int32_t)drop->vel.y;
        const XYZ_32 to = do_interp
            ? (XYZ_32) {
                  .x = (int32_t)LERP(drop->prev_pos.x, drop->pos.x, ratio),
                  .y = (int32_t)LERP(drop->prev_pos.y, drop->pos.y, ratio),
                  .z = (int32_t)LERP(drop->prev_pos.z, drop->pos.z, ratio),
              }
            : drop->pos;
        const XYZ_32 from = {
            to.x - (wind.x * 4),
            to.y - (yv * 8),
            to.z - (wind.z * 4),
        };

#if defined(__DREAMCAST__)
        // The PSX's drop: a line from black to pale, added, no mesh.
        OutputSource_PolyFX_StageStreak(
            from, (RGBA_8888) { 0, 0, 0, 0xFF },
            to, (RGBA_8888) { 0x30, 0x40, 0x40, 0xFF });
        continue;
#endif
        OutputSource_PolyFX_StageLineSegment(
            from, from_color, to, to_color, 1.0f, draw_type);
    }
}

static void M_SpawnSnowflake(M_SNOWFLAKE *const snow)
{
    if (!M_SpawnParticle(&snow->pos)) {
        return;
    }

    snow->stopped = false;
    snow->vel.x = (int8_t)((Random_GetDraw() & 7) - 4);
    snow->vel.y =
        (uint8_t)(((Random_GetDraw() % M_SNOW_YV_RANGE) + M_SNOW_YV_MIN) * 8);
    snow->vel.z = (int8_t)((Random_GetDraw() & 7) - 4);
    snow->life = (uint8_t)(M_SNOW_LIFE_BASE - ((int32_t)snow->vel.y << 1));
    snow->prev_pos = snow->pos;
    snow->prev_yv = snow->vel.y;
    snow->prev_life = snow->life;
}

[[maybe_unused]] static void M_UpdateSnowC(void)
{
    const int32_t count = M_GetParticleCount();
    const int32_t spawn_count = M_GetSpawnCount();

    int32_t num_alive = 0;
    for (int32_t i = 0; i < M_MAX_WEATHER; i++) {
        M_SNOWFLAKE *const snow = &m_Snowflakes[i];

        if (snow->pos.x == 0 && i < count && num_alive < spawn_count) {
            num_alive++;
            M_SpawnSnowflake(snow);
        }

        if (snow->pos.x == 0) {
            continue;
        }

        snow->prev_pos = snow->pos;
        snow->prev_yv = snow->vel.y;
        snow->prev_life = snow->life;

        const XYZ_32 old_pos = snow->pos;

        int16_t room_num = NO_ROOM;
        int32_t outside = 1;
        if (!snow->stopped) {
            snow->pos.x += snow->vel.x;
            snow->pos.y += (snow->vel.y & 0xF8) >> 2;
            snow->pos.z += snow->vel.z;

            outside = Room_GetOutsideStatus(snow->pos, &room_num, nullptr);
            if (outside == -3) {
                snow->pos.x = 0;
                continue;
            }

            if (outside == -2
                || (room_num != NO_ROOM
                    && Room_Get(room_num)->flags.underwater)) {
                snow->stopped = true;
                snow->pos = old_pos;

                if (snow->life > 16) {
                    snow->life = 16;
                }
                if (snow->vel.y > 16) {
                    snow->vel.y -= 16;
                }
            }
        }

        if (snow->life == 0) {
            snow->pos.x = 0;
            continue;
        }

        if ((ABS(g_Camera.pos.x - snow->pos.x) > M_RAIN_MAX_DISTANCE
             || ABS(g_Camera.pos.z - snow->pos.z) > M_RAIN_MAX_DISTANCE)
            && snow->life > 16) {
            snow->life = 16;
        }

        const XZ_32 wind = Sparks_GetSmokeWind();

        if (snow->vel.x < (wind.x * 2)) {
            snow->vel.x++;
        } else if (snow->vel.x > (wind.x * 2)) {
            snow->vel.x--;
        }

        if (snow->vel.z < (wind.z * 2)) {
            snow->vel.z++;
        } else if (snow->vel.z > (wind.z * 2)) {
            snow->vel.z--;
        }

        snow->life -= 2;
        if ((snow->vel.y & 7) != 7) {
            snow->vel.y++;
        }
    }
}

[[maybe_unused]] static void M_DrawSnowC(void)
{
    const OBJECT *const obj = Object_Get(O_SNOWFLAKE);
    if (!obj->loaded) {
        return;
    }

    const int32_t sprite_idx = obj->mesh_idx;
    const TRX_REAL ratio = Interpolation_GetWorldRate();
    const bool do_interp =
        Interpolation_IsActive() && ratio > 0.0 && ratio < 1.0;
    // The same for every flake: once, not per flake.
    const int64_t near_z = Output_GetNearZ();
    const int64_t far_z = Output_GetFarZ();
    const int32_t fog_start = Output_GetFogStart();
    const int32_t fog_end = Output_GetFogEnd();
    for (int32_t i = 0; i < M_MAX_WEATHER; i++) {
        M_SNOWFLAKE *const snow = &m_Snowflakes[i];
        if (snow->pos.x == 0) {
            continue;
        }

        const XYZ_32 center = do_interp
            ? (XYZ_32) {
                  .x = (int32_t)LERP(snow->prev_pos.x, snow->pos.x, ratio),
                  .y = (int32_t)LERP(snow->prev_pos.y, snow->pos.y, ratio),
                  .z = (int32_t)LERP(snow->prev_pos.z, snow->pos.z, ratio),
              }
            : snow->pos;
        const int64_t zv = M_GetViewDepth(center);
        const int32_t vpos_z = (int32_t)(zv >> W2V_SHIFT);

        if (vpos_z < 128 && snow->life > 16) {
            snow->life = 16;
        }

        if (zv <= near_z || zv >= far_z) {
            continue;
        }

        if (vpos_z < 128 || g_PhdPersp <= 0) {
            continue;
        }

        const int32_t yv = do_interp
            ? (int32_t)LERP(snow->prev_yv, snow->vel.y, ratio)
            : (int32_t)snow->vel.y;
        const int32_t life = do_interp
            ? (int32_t)LERP(snow->prev_life, snow->life, ratio)
            : (int32_t)snow->life;

        const XYZ_32 world_pos[4] = { center, center, center, center };
        const float s = 8.0f;
        const float disp[4][2] = {
            { -s, -s },
            { s, -s },
            { s, s },
            { -s, s },
        };

        uint32_t c;
        if ((yv & 7) < 7) {
            c = (uint32_t)(yv & 7);
        } else if (life > 18) {
            c = 15;
        } else {
            c = (uint32_t)life;
        }
        c <<= 3;
        CLAMPG(c, 255);

        {
            float fade = 1.0f;
            if (fog_end > fog_start && vpos_z > fog_start) {
                float t = (vpos_z - fog_start) / (float)(fog_end - fog_start);
                CLAMP(t, 0.0f, 1.0f);
                fade = 1.0f - t;
            }
            c = (uint32_t)(c * fade);
        }

        const RGBA_8888 color = { c, c, c, 255 };
#if defined(__DREAMCAST__)
        OutputSource_PolyFX_StageParticle(sprite_idx, &center, s, color);
        continue;
#endif
        const RGBA_8888 colors[4] = { color, color, color, color };

        OutputSource_PolyFX_StageQuadExt(
            sprite_idx, world_pos, disp, colors,
            VERT_NO_LIGHTING | VERT_NO_WIBBLE | VERT_BILLBOARD
                | VERT_ABS_SPRITE,
            DRAW_BLEND_ADD);
    }
}

#if defined(__DREAMCAST__)
// The assembly (weather_sh4.S) does the update and the staging; what follows
// is the control block it takes and the two C functions it calls back (a spawn
// and the room lookup). In a build with DC_EMIT_VERIFY the C functions above
// run too, and the assembly's result is compared with theirs.
extern void dc_rain_update(void *ctl);
extern void dc_snow_update(void *ctl);
extern void dc_rain_draw(void *ctl);
extern void dc_snow_draw(void *ctl);

typedef struct {
    void *particles;
    int32_t slots;
    int32_t spawn_below;
    int32_t spawn_max;
    int32_t wind_x;
    int32_t wind_z;
    int32_t cam_x;
    int32_t cam_z;
    uint32_t *rng;
    const bool *frozen;
    uint32_t mul;
    uint32_t add;
    void (*spawn)(void *);
    int32_t (*outside)(const XYZ_32 *);
    int32_t reach;
} M_WEATHER_CTL;

typedef struct {
    void *particles;
    int32_t slots;
    void *out;
    int32_t room;
    int32_t written;
    int32_t interp;
    float ratio;
    int32_t near_z;
    int32_t far_z;
    int32_t k36;
    int32_t k40;
    const float *picture;
    const void *matrix;
    int32_t k52;
    int32_t k56;
    int32_t persp;
    float fog_range;
} M_WEATHER_DRAW;

_Static_assert(sizeof(M_RAINDROP) == 44, "weather_sh4.S layout");
_Static_assert(offsetof(M_RAINDROP, prev_yv) == 24, "weather_sh4.S layout");
_Static_assert(offsetof(M_RAINDROP, vel) == 28, "weather_sh4.S layout");
_Static_assert(offsetof(M_RAINDROP, life) == 40, "weather_sh4.S layout");
_Static_assert(sizeof(M_SNOWFLAKE) == 52, "weather_sh4.S layout");
_Static_assert(offsetof(M_SNOWFLAKE, stopped) == 24, "weather_sh4.S layout");
_Static_assert(offsetof(M_SNOWFLAKE, prev_yv) == 28, "weather_sh4.S layout");
_Static_assert(offsetof(M_SNOWFLAKE, prev_life) == 32, "weather_sh4.S layout");
_Static_assert(offsetof(M_SNOWFLAKE, vel) == 36, "weather_sh4.S layout");
_Static_assert(offsetof(M_SNOWFLAKE, life) == 48, "weather_sh4.S layout");
_Static_assert(sizeof(M_WEATHER_CTL) == 60, "weather_sh4.S layout");
_Static_assert(offsetof(M_WEATHER_DRAW, picture) == 44, "weather_sh4.S layout");
_Static_assert(offsetof(M_WEATHER_DRAW, persp) == 60, "weather_sh4.S layout");
_Static_assert(sizeof(MATRIX) == 96, "weather_sh4.S layout");
_Static_assert(offsetof(MATRIX, _20) == 64, "weather_sh4.S layout");
_Static_assert(M_PARTICLE_BYTES == 72, "weather_sh4.S layout");

// Whether a point stops or kills a particle (bit 0: out of reach of the
// outside, or under water) and whether it is past the edge (bit 1).
static int32_t M_Outside(const XYZ_32 *const pos)
{
    int16_t room_num = NO_ROOM;
    const int32_t outside = Room_GetOutsideStatus(*pos, &room_num, nullptr);
    int32_t r = outside == -3 ? 2 : 0;
    if (outside == -2
        || (room_num != NO_ROOM && Room_Get(room_num)->flags.underwater)) {
        r |= 1;
    }
    return r;
}

static void M_SpawnRainDropV(void *const drop)
{
    M_SpawnRainDrop(drop);
}

static void M_SpawnSnowflakeV(void *const snow)
{
    M_SpawnSnowflake(snow);
}

static M_WEATHER_CTL M_MakeCtl(
    void *const particles, const int32_t wind_x, const int32_t wind_z,
    void (*const spawn)(void *))
{
    return (M_WEATHER_CTL) {
        .particles = particles,
        .slots = M_MAX_WEATHER,
        .spawn_below = M_GetParticleCount(),
        .spawn_max = M_GetSpawnCount(),
        .wind_x = wind_x,
        .wind_z = wind_z,
        .cam_x = g_Camera.pos.x,
        .cam_z = g_Camera.pos.z,
        .rng = Random_GetDrawState(),
        .frozen = Random_GetDrawFrozen(),
        .mul = 0x41C64E6DU,
        .add = 0x3039U,
        .spawn = spawn,
        .outside = M_Outside,
        .reach = M_RAIN_MAX_DISTANCE,
    };
}

    #if defined(DC_EMIT_VERIFY)
static void M_Report(
    const char *const tag, const bool ok, const char *const what)
{
    static uint32_t m_Runs[4];
    static uint32_t m_Bad[4];
    const int32_t k = tag[0] == 'r' ? 0 : tag[0] == 's' ? 1 : 2;
    m_Runs[k]++;
    if (!ok) {
        m_Bad[k]++;
        if (m_Bad[k] <= 6) {
            LOG_WARNING("[weather-verify] %s MISMATCH: %s", tag, what);
        }
    }
    if (m_Runs[k] % 300 == 0) {
        LOG_WARNING(
            "[weather-verify] %s %u runs, %u bad", tag, (unsigned)m_Runs[k],
            (unsigned)m_Bad[k]);
    }
}

static void M_VerifyUpdate(
    const char *const tag, void *const particles, const size_t size,
    void (*const asm_fn)(void *), M_WEATHER_CTL *const ctl,
    void (*const c_fn)(void))
{
    static uint8_t *before = nullptr;
    static uint8_t *after_asm = nullptr;
    const size_t bytes = size * M_MAX_WEATHER;
    if (before == nullptr) {
        // Only a level with weather pays for these: the verify build has
        // little heap to spare.
        before = Memory_Alloc(M_BASE_WEATHER * 2 * 52);
        after_asm = Memory_Alloc(M_BASE_WEATHER * 2 * 52);
    }
    memcpy(before, particles, bytes);
    const uint32_t seed0 = *Random_GetDrawState();
    asm_fn(ctl);
    memcpy(after_asm, particles, bytes);
    const uint32_t seed_asm = *Random_GetDrawState();
    memcpy(particles, before, bytes);
    *Random_GetDrawState() = seed0;
    c_fn();
    M_Report(
        tag,
        seed_asm == *Random_GetDrawState()
            && memcmp(after_asm, particles, bytes) == 0,
        "state or particles differ");
}

static void M_VerifyList(
    const char *const tag, const void *const asm_list, const int32_t asm_n,
    const void *const c_list, const int32_t c_n, const size_t size)
{
    M_Report(
        tag,
        asm_n == c_n && memcmp(asm_list, c_list, size * (size_t)c_n) == 0,
        "lists differ");
}
    #endif

static void M_UpdateRain(void)
{
    if ((g_DC_AsmOff & 8) != 0) {
        M_UpdateRainC();
        return;
    }
    const XZ_32 wind = Sparks_GetSmokeWind();
    M_WEATHER_CTL ctl =
        M_MakeCtl(m_Raindrops, 4 * wind.x, 4 * wind.z, M_SpawnRainDropV);
    #if defined(DC_EMIT_VERIFY)
    M_VerifyUpdate(
        "rain-update", m_Raindrops, sizeof(M_RAINDROP), dc_rain_update, &ctl,
        M_UpdateRainC);
    #else
    dc_rain_update(&ctl);
    #endif
}

static void M_UpdateSnow(void)
{
    if ((g_DC_AsmOff & 8) != 0) {
        M_UpdateSnowC();
        return;
    }
    const XZ_32 wind = Sparks_GetSmokeWind();
    M_WEATHER_CTL ctl =
        M_MakeCtl(m_Snowflakes, 2 * wind.x, 2 * wind.z, M_SpawnSnowflakeV);
    #if defined(DC_EMIT_VERIFY)
    M_VerifyUpdate(
        "snow-update", m_Snowflakes, sizeof(M_SNOWFLAKE), dc_snow_update, &ctl,
        M_UpdateSnowC);
    #else
    dc_snow_update(&ctl);
    #endif
}

static void M_DrawRain(void)
{
    if ((g_DC_AsmOff & 16) != 0) {
        M_DrawRainC();
        return;
    }
    const XZ_32 wind = Sparks_GetSmokeWind();
    const TRX_REAL ratio = Interpolation_GetWorldRate();
    int32_t room = 0;
    void *const slots = OutputSource_PolyFX_StreakSlots(&room);
    if (slots == nullptr) {
        return;
    }
    #if defined(DC_EMIT_VERIFY)
    static uint8_t *scratch = nullptr;
    if (scratch == nullptr) {
        scratch = Memory_Alloc(768 * 32);
    }
    const int32_t room_real = room;
    room = MIN(room, 768);
    #endif
    M_WEATHER_DRAW ctl = {
        .particles = m_Raindrops,
        .slots = M_MAX_WEATHER,
    #if defined(DC_EMIT_VERIFY)
        .out = scratch,
    #else
        .out = slots,
    #endif
        .room = room,
        .interp = Interpolation_IsActive() && ratio > 0.0 && ratio < 1.0,
        .ratio = (float)ratio,
        .k36 = (int32_t)0xFF000000u,
        .k40 = (int32_t)0xFF304040u,
        .k52 = wind.x * 4,
        .k56 = wind.z * 4,
    };
    dc_rain_draw(&ctl);
    #if defined(DC_EMIT_VERIFY)
    int32_t room_after = 0;
    M_DrawRainC();
    OutputSource_PolyFX_StreakSlots(&room_after);
    M_VerifyList(
        "rain-draw", scratch, ctl.written, slots, room_real - room_after,
        32);
    #else
    OutputSource_PolyFX_StreaksAdded(ctl.written);
    #endif
}

static void M_DrawSnow(void)
{
    if ((g_DC_AsmOff & 16) != 0) {
        M_DrawSnowC();
        return;
    }
    const OBJECT *const obj = Object_Get(O_SNOWFLAKE);
    if (!obj->loaded) {
        return;
    }
    const TRX_REAL ratio = Interpolation_GetWorldRate();
    int32_t room = 0;
    void *const slots = OutputSource_PolyFX_ParticleSlots(&room);
    if (slots == nullptr) {
        return;
    }
    float picture[9];
    OutputSource_PolyFX_GetPicture(obj->mesh_idx, picture);
    #if defined(DC_EMIT_VERIFY)
    static uint8_t *scratch = nullptr;
    if (scratch == nullptr) {
        scratch = Memory_Alloc(512 * M_PARTICLE_BYTES);
    }
    const int32_t room_real = room;
    room = MIN(room, 512);
    #endif
    const int32_t fog_start = Output_GetFogStart();
    const int32_t fog_end = Output_GetFogEnd();
    M_WEATHER_DRAW ctl = {
        .particles = m_Snowflakes,
        .slots = M_MAX_WEATHER,
    #if defined(DC_EMIT_VERIFY)
        .out = scratch,
    #else
        .out = slots,
    #endif
        .room = room,
        .interp = Interpolation_IsActive() && ratio > 0.0 && ratio < 1.0,
        .ratio = (float)ratio,
        .near_z = Output_GetNearZ(),
        .far_z = Output_GetFarZ(),
        .k36 = fog_start,
        .k40 = fog_end,
        .picture = picture,
        .matrix = &g_ViewMatrix,
        .persp = g_PhdPersp > 0,
    };
    dc_snow_draw(&ctl);
    #if defined(DC_EMIT_VERIFY)
    int32_t room_after = 0;
    M_DrawSnowC();
    OutputSource_PolyFX_ParticleSlots(&room_after);
    M_VerifyList(
        "snow-draw", scratch, ctl.written, slots, room_real - room_after,
        M_PARTICLE_BYTES);
    #else
    OutputSource_PolyFX_ParticlesAdded(ctl.written);
    #endif
}
#else
static void M_UpdateRain(void)
{
    M_UpdateRainC();
}

static void M_DrawRain(void)
{
    M_DrawRainC();
}

static void M_UpdateSnow(void)
{
    M_UpdateSnowC();
}

static void M_DrawSnow(void)
{
    M_DrawSnowC();
}
#endif

static void M_SaveRain(JSON_WRITE_IO *const io)
{
    JSONW_PUSH_ARRAY(io);
    for (int32_t i = 0; i < M_MAX_WEATHER; i++) {
        const M_RAINDROP *const drop = &m_Raindrops[i];
        if (drop->pos.x == 0) {
            continue;
        }

        JSONW_PUSH_OBJECT(io);
        JSONW_WRITE(io, "pos", drop->pos);
        JSONW_WRITE(io, "vel", drop->vel);
        JSONW_WRITE(io, "life", drop->life);
        JSONW_POP_AND_APPEND(io);
    }
    JSONW_POP_AND_SET_NZ(io, "rain");
}

static void M_SaveSnow(JSON_WRITE_IO *const io)
{
    JSONW_PUSH_ARRAY(io);
    for (int32_t i = 0; i < M_MAX_WEATHER; i++) {
        const M_SNOWFLAKE *const snow = &m_Snowflakes[i];
        if (snow->pos.x == 0) {
            continue;
        }

        JSONW_PUSH_OBJECT(io);
        JSONW_WRITE(io, "pos", snow->pos);
        JSONW_WRITE(io, "vel", snow->vel);
        JSONW_WRITE(io, "life", snow->life);
        JSONW_WRITE(io, "stopped", snow->stopped);
        JSONW_POP_AND_APPEND(io);
    }
    JSONW_POP_AND_SET_NZ(io, "snow");
}

static void M_Save(JSON_WRITE_IO *const io)
{
    JSONW_WRITE(io, "severity", m_Severity);

    // Particles of the inactive type are neither updated nor drawn.
    if (m_WeatherType == WEATHER_RAIN) {
        M_SaveRain(io);
    } else if (m_WeatherType == WEATHER_SNOW) {
        M_SaveSnow(io);
    }
}

static RESULT M_LoadVelocity(JSON_READ_IO *const io, XYZ_32 *const vel)
{
    if (JSON_ReadIO_HasKey(io, "vel")) {
        return JSON_READ(io, "vel", vel);
    }
    MUST(JSON_READ(io, "xv", &vel->x));
    MUST(JSON_READ(io, "yv", &vel->y));
    MUST(JSON_READ(io, "zv", &vel->z));
    return OK;
}

static RESULT M_LoadRain(JSON_READ_IO *const io)
{
    M_Ensure(WEATHER_RAIN);
    const int32_t count = JSON_ARRAY_LEN(io);
    for (int32_t i = 0; i < count; i++) {
        if (i >= M_MAX_WEATHER) {
            LOG_WARNING(
                "Malformed save: too many raindrops. Extra raindrops will be "
                "ignored.");
            break;
        }

        M_RAINDROP *const drop = &m_Raindrops[i];
        MUST(JSON_PUSH_INDEX(io, i));
        MUST(JSON_READ(io, "pos", &drop->pos));
        MUST(M_LoadVelocity(io, &drop->vel));
        MUST(JSON_READ(io, "life", &drop->life));
        MUST(JSON_POP(io));
        drop->prev_pos = drop->pos;
        drop->prev_yv = drop->vel.y;
    }
    return OK;
}

static RESULT M_LoadSnow(JSON_READ_IO *const io)
{
    M_Ensure(WEATHER_SNOW);
    const int32_t count = JSON_ARRAY_LEN(io);
    for (int32_t i = 0; i < count; i++) {
        if (i >= M_MAX_WEATHER) {
            LOG_WARNING(
                "Malformed save: too many snowflakes. Extra snowflakes will "
                "be ignored.");
            break;
        }

        M_SNOWFLAKE *const snow = &m_Snowflakes[i];
        MUST(JSON_PUSH_INDEX(io, i));
        MUST(JSON_READ(io, "pos", &snow->pos));
        MUST(M_LoadVelocity(io, &snow->vel));
        MUST(JSON_READ(io, "life", &snow->life));
        MUST(JSON_READ(io, "stopped", &snow->stopped));
        MUST(JSON_POP(io));
        snow->prev_pos = snow->pos;
        snow->prev_yv = snow->vel.y;
        snow->prev_life = snow->life;
    }
    return OK;
}

static RESULT M_Load(JSON_READ_IO *const io)
{
    float severity = 1.0f;
    MUST(JSON_READ_D(io, "severity", &severity, 1.0f));
    FX_Weather_SetSeverity(severity);

    if (JSON_ReadIO_HasKey(io, "rain")) {
        MUST(JSON_PUSH(io, "rain"));
        MUST(M_LoadRain(io));
        MUST(JSON_POP(io));
    }
    if (JSON_ReadIO_HasKey(io, "snow")) {
        MUST(JSON_PUSH(io, "snow"));
        MUST(M_LoadSnow(io));
        MUST(JSON_POP(io));
    }
    return OK;
}

static void M_Control(void)
{
    if (!g_Config.visuals.enable_weather) {
        return;
    }
    const WEATHER_TYPE weather_type = m_WeatherType;
    if (weather_type == WEATHER_RAIN) {
        M_UpdateRain();
    } else if (weather_type == WEATHER_SNOW) {
        M_UpdateSnow();
    }
}

static void M_Draw(void)
{
    if (!g_Config.visuals.enable_weather) {
        return;
    }
    switch (m_WeatherType) {
    case WEATHER_RAIN:
        M_DrawRain();
        break;
    case WEATHER_SNOW:
        M_DrawSnow();
        break;
    default:
        break;
    }
}

static void M_Reset(void)
{
    M_ClearWeather();
    m_Severity = 1.0f;
}

WEATHER_TYPE FX_Weather_GetWeather(void)
{
    return m_WeatherType;
}

void FX_Weather_SetWeather(const WEATHER_TYPE weather_type)
{
    m_WeatherType = weather_type;
    M_Ensure(weather_type);
}

float FX_Weather_GetSeverity(void)
{
    return m_Severity;
}

void FX_Weather_SetSeverity(const float severity)
{
    m_Severity = severity;
    CLAMP(m_Severity, 0.0f, (float)WEATHER_SEVERITY_MAX);
}

static const FX_MODULE m_Module = {
    .control_func = M_Control,
    .draw_func = M_Draw,
    .reset_func = M_Reset,
    .save_key = "weather",
    .save_func = M_Save,
    .load_func = M_Load,
};

REGISTER_FX(m_Module)
