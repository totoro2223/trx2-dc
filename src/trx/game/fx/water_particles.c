#include <trx/config.h>
#include <trx/core/json/util/read_io.h>
#include <trx/core/json/util/write_io.h>
#include <trx/core/math.h>
#include <trx/core/utils.h>
#include <trx/game/camera/vars.h>
#include <trx/game/fx/common.h>
#include <trx/game/game_flow/common.h>
#include <trx/game/interpolation.h>
#include <trx/game/matrix.h>
#include <trx/game/objects.h>
#include <trx/game/output/const.h>
#include <trx/game/output/sources/poly_fx.h>
#include <trx/game/output/state.h>
#include <trx/game/output/vars.h>
#include <trx/game/random.h>
#include <trx/game/rooms.h>
#include <trx/version.h>

#include <string.h>

#define M_MAX_WATER_PARTICLES 256
#define M_MAX_WATER_PARTICLES_ALIVE 16
#define M_SPAWN_DIST_MASK 0xFFF
#define M_SPAWN_ANGLE_MASK 0x1FFE
#define M_SPAWN_Y_MASK 0x7FF
#define M_BASE_Y_OFF (-1024)
#define M_SMALL_SIZE 6.0f
#define M_MAX_SIZE 12.0f
#define M_SIZE_DIV 3.0f

typedef struct {
    XYZ_32 pos;
    XYZ_32 prev_pos;
    uint8_t life;
    XYZ_32 vel __attribute__((aligned(4)));
} M_WATER_PARTICLE;

static M_WATER_PARTICLE m_WaterParticles[M_MAX_WATER_PARTICLES];

static bool M_IsEnabled(void)
{
    if (!g_Config.visuals.enable_weather) {
        return false;
    }

    if (g_TRVersion == 4) {
        return true;
    }

    const GF_LEVEL *const level = GF_GetCurrentLevel();
    return level != nullptr && level->water_particles;
}

static void M_Clear(void)
{
    memset(m_WaterParticles, 0, sizeof(m_WaterParticles));
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

static void M_Spawn(M_WATER_PARTICLE *const particle)
{
    const int32_t dist = Random_GetDraw() & M_SPAWN_DIST_MASK;
    const int32_t angle = (Random_GetDraw() & M_SPAWN_ANGLE_MASK) * 8;
    particle->pos = XYZ_32_OffsetYaw(g_Camera.pos.pos, angle, dist);
    particle->pos.y += (Random_GetDraw() & M_SPAWN_Y_MASK) + M_BASE_Y_OFF;

    int16_t room_num = NO_ROOM;
    Room_GetOutsideStatus(particle->pos, &room_num, nullptr);
    if (room_num == NO_ROOM || !Room_Get(room_num)->flags.underwater) {
        particle->pos.x = 0;
        return;
    }

    particle->life = (uint8_t)((Random_GetDraw() & 7) + 16);
    particle->vel.x = Random_GetDraw() & 3;
    if (particle->vel.x == 2) {
        particle->vel.x = -1;
    }

    particle->vel.y = ((Random_GetDraw() & 7) + 8) << 3;
    particle->vel.z = Random_GetDraw() & 3;
    if (particle->vel.z == 2) {
        particle->vel.z = -1;
    }

    particle->prev_pos = particle->pos;
}

static void M_Reset(void)
{
    M_Clear();
}

static void M_ControlC(void)
{
    if (!M_IsEnabled()) {
        M_Clear();
        return;
    }

    int32_t num_alive = 0;
    for (int32_t i = 0; i < M_MAX_WATER_PARTICLES; i++) {
        M_WATER_PARTICLE *const particle = &m_WaterParticles[i];

        if (particle->pos.x == 0 && num_alive < M_MAX_WATER_PARTICLES_ALIVE) {
            num_alive++;
            M_Spawn(particle);
        }

        if (particle->pos.x == 0) {
            continue;
        }

        particle->prev_pos = particle->pos;
        particle->pos.x += particle->vel.x;
        particle->pos.y += (particle->vel.y & 0xF8) >> 6;
        particle->pos.z += particle->vel.z;

        if (particle->life == 0) {
            particle->pos.x = 0;
            continue;
        }

        particle->life--;
        if ((particle->vel.y & 7) != 7) {
            particle->vel.y++;
        }
    }
}

static void M_DrawC(void)
{
    if (!M_IsEnabled()) {
        return;
    }

    const int32_t sprite_idx = Sparks_GetSpriteIndex(SPARK_TYPE_PARTICLE);
    if (sprite_idx == NO_ITEM) {
        return;
    }

    const int32_t atlas_idx = Output_Textures_GetSpriteUVWIndex(sprite_idx, 0);
    const OUTPUT_TEXTURE_SIZE atlas_size =
        Output_Textures_GetAtlasSize(atlas_idx / 4);
    const OUTPUT_TEXTURE_SIZE tri_size[3] = { atlas_size, atlas_size,
                                              atlas_size };
    const OUTPUT_UVW tri_uvw[3] = {
        Output_Textures_GetUVW(
            Output_Textures_GetSpriteUVWIndex(sprite_idx, 1)),
        Output_Textures_GetUVW(
            Output_Textures_GetSpriteUVWIndex(sprite_idx, 2)),
        Output_Textures_GetUVW(
            Output_Textures_GetSpriteUVWIndex(sprite_idx, 3)),
    };

    const TRX_REAL ratio = Interpolation_GetWorldRate();
    const bool do_interp =
        Interpolation_IsActive() && ratio > 0.0 && ratio < 1.0;

    for (int32_t i = 0; i < M_MAX_WATER_PARTICLES; i++) {
        M_WATER_PARTICLE *const particle = &m_WaterParticles[i];
        if (particle->pos.x == 0) {
            continue;
        }

        const XYZ_32 center = do_interp
            ? (XYZ_32) {
                  .x = (int32_t)LERP(
                      particle->prev_pos.x, particle->pos.x, ratio),
                  .y = (int32_t)LERP(
                      particle->prev_pos.y, particle->pos.y, ratio),
                  .z = (int32_t)LERP(
                      particle->prev_pos.z, particle->pos.z, ratio),
              }
            : particle->pos;

        const int64_t zv = M_GetViewDepth(center);
        const int64_t near_z = Output_GetNearZ();
        const int64_t far_z = Output_GetFarZ();
        const int32_t vpos_z = (int32_t)(zv >> W2V_SHIFT);

        if (vpos_z < 128) {
            if (particle->life > 16) {
                particle->life = 16;
            }
            continue;
        }

        if (zv <= near_z || zv >= far_z) {
            continue;
        }

        // The original sizes the speck in the screen pixels of its reference
        // viewport, so the clamp is applied there and the result taken back
        // into world units - a size in pixels of the rasterized picture would
        // follow the resolution and the supersampling factor.
        float size = (float)((REF_PERSP * (particle->vel.y >> 3)) / vpos_z);
        if (size < 1.0f) {
            size = M_SMALL_SIZE;
        } else {
            CLAMPG(size, M_MAX_SIZE);
        }
        size = (size * vpos_z) / (REF_PERSP * M_SIZE_DIV);

        uint32_t c;
        if ((particle->vel.y & 7) < 7) {
            c = (uint32_t)(particle->vel.y & 7);
        } else if (particle->life > 18) {
            c = 15;
        } else {
            c = particle->life;
        }
#if defined(__DREAMCAST__)
        // The PowerVR's additive blend leaves the desktop's faint specks
        // nearly black.
        c <<= 4;
#else
        c <<= 2;
#endif
        CLAMPG(c, 255);

        const RGBA_8888 color = { c, c, c, 255 };
        const RGBA_8888 colors[3] = { color, color, color };
        const XYZ_32 world_pos[3] = { center, center, center };
        const float disp[3][2] = {
            { size, -2.0f * size },
            { size, size },
            { -2.0f * size, size },
        };

#if defined(__DREAMCAST__)
        // One quad, no mesh: as the snow.
        OutputSource_PolyFX_StageParticle(sprite_idx, &center, size, color);
        continue;
#endif
        OutputSource_PolyFX_StageTriExtUV(
            world_pos, tri_uvw, tri_size, disp, colors,
            VERT_NO_LIGHTING | VERT_NO_WIBBLE | VERT_BILLBOARD
                | VERT_ABS_SPRITE,
            DRAW_BLEND_ADD);
    }
}

#if defined(__DREAMCAST__)
// The assembly (weather_sh4.S) does the update and the staging, as it does the
// weather's; the C above is its reference (DC_EMIT_VERIFY runs both and
// compares) and the other platforms' code.
extern void dc_water_update(void *ctl);
extern void dc_water_draw(void *ctl);

typedef struct {
    void *particles;
    int32_t slots;
    int32_t unused;
    int32_t spawn_max;
    int32_t pad[4];
    uint32_t *rng;
    const bool *frozen;
    uint32_t mul;
    uint32_t add;
    void (*spawn)(void *);
} M_WATER_CTL;

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
    int32_t pad[2];
    const float *picture;
    const void *matrix;
    float k52;
} M_WATER_DRAW;

_Static_assert(sizeof(M_WATER_PARTICLE) == 40, "weather_sh4.S layout");
_Static_assert(offsetof(M_WATER_PARTICLE, life) == 24, "weather_sh4.S layout");
_Static_assert(offsetof(M_WATER_PARTICLE, vel) == 28, "weather_sh4.S layout");
_Static_assert(offsetof(M_WATER_CTL, rng) == 32, "weather_sh4.S layout");
_Static_assert(offsetof(M_WATER_CTL, spawn) == 48, "weather_sh4.S layout");
_Static_assert(offsetof(M_WATER_DRAW, picture) == 44, "weather_sh4.S layout");
_Static_assert(offsetof(M_WATER_DRAW, k52) == 52, "weather_sh4.S layout");
_Static_assert(offsetof(MATRIX, _20) == 64, "weather_sh4.S layout");
_Static_assert(REF_PERSP == 381, "weather_sh4.S: 381");

static void M_SpawnV(void *const particle)
{
    M_Spawn(particle);
}

static void M_Control(void)
{
    if (!M_IsEnabled()) {
        M_Clear();
        return;
    }
    M_WATER_CTL ctl = {
        .particles = m_WaterParticles,
        .slots = M_MAX_WATER_PARTICLES,
        .spawn_max = M_MAX_WATER_PARTICLES_ALIVE,
        .rng = Random_GetDrawState(),
        .frozen = Random_GetDrawFrozen(),
        .mul = 0x41C64E6DU,
        .add = 0x3039U,
        .spawn = M_SpawnV,
    };
    #if defined(DC_EMIT_VERIFY)
    static M_WATER_PARTICLE before[M_MAX_WATER_PARTICLES];
    static M_WATER_PARTICLE after_asm[M_MAX_WATER_PARTICLES];
    static uint32_t m_Runs;
    static uint32_t m_Bad;
    memcpy(before, m_WaterParticles, sizeof(before));
    const uint32_t seed0 = *Random_GetDrawState();
    dc_water_update(&ctl);
    memcpy(after_asm, m_WaterParticles, sizeof(after_asm));
    const uint32_t seed_asm = *Random_GetDrawState();
    memcpy(m_WaterParticles, before, sizeof(before));
    *Random_GetDrawState() = seed0;
    M_ControlC();
    m_Runs++;
    if (seed_asm != *Random_GetDrawState()
        || memcmp(after_asm, m_WaterParticles, sizeof(after_asm)) != 0) {
        if (++m_Bad <= 6) {
            LOG_WARNING("[weather-verify] water-update MISMATCH");
        }
    }
    if (m_Runs % 300 == 0) {
        LOG_WARNING(
            "[weather-verify] water-update %u runs, %u bad", (unsigned)m_Runs,
            (unsigned)m_Bad);
    }
    #else
    dc_water_update(&ctl);
    #endif
}

static void M_Draw(void)
{
    if (!M_IsEnabled()) {
        return;
    }
    const int32_t sprite_idx = Sparks_GetSpriteIndex(SPARK_TYPE_PARTICLE);
    if (sprite_idx == NO_ITEM) {
        return;
    }
    const TRX_REAL ratio = Interpolation_GetWorldRate();
    int32_t room = 0;
    void *const slots = OutputSource_PolyFX_ParticleSlots(&room);
    if (slots == nullptr) {
        return;
    }
    float picture[9];
    OutputSource_PolyFX_GetPicture(sprite_idx, picture);
    #if defined(DC_EMIT_VERIFY)
    static uint8_t scratch[M_MAX_WATER_PARTICLES * 72];
    const int32_t room_real = room;
    room = MIN(room, M_MAX_WATER_PARTICLES);
    #endif
    M_WATER_DRAW ctl = {
        .particles = m_WaterParticles,
        .slots = M_MAX_WATER_PARTICLES,
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
        .picture = picture,
        .matrix = &g_ViewMatrix,
        .k52 = (float)REF_PERSP * M_SIZE_DIV,
    };
    dc_water_draw(&ctl);
    #if defined(DC_EMIT_VERIFY)
    static uint32_t m_Runs;
    static uint32_t m_Bad;
    int32_t room_after = 0;
    M_DrawC();
    OutputSource_PolyFX_ParticleSlots(&room_after);
    m_Runs++;
    if (ctl.written != room_real - room_after
        || memcmp(scratch, slots, 72 * (size_t)ctl.written) != 0) {
        if (++m_Bad <= 6) {
            LOG_WARNING(
                "[weather-verify] water-draw MISMATCH asm %d C %d",
                (int)ctl.written, (int)(room_real - room_after));
        }
    }
    if (m_Runs % 300 == 0) {
        LOG_WARNING(
            "[weather-verify] water-draw %u runs, %u bad", (unsigned)m_Runs,
            (unsigned)m_Bad);
    }
    #else
    OutputSource_PolyFX_ParticlesAdded(ctl.written);
    #endif
}
#else
static void M_Control(void)
{
    M_ControlC();
}

static void M_Draw(void)
{
    M_DrawC();
}
#endif

static void M_Save(JSON_WRITE_IO *const io)
{
    JSONW_PUSH_ARRAY(io);
    for (int32_t i = 0; i < M_MAX_WATER_PARTICLES; i++) {
        const M_WATER_PARTICLE *const particle = &m_WaterParticles[i];
        if (particle->life == 0) {
            continue;
        }
        JSONW_PUSH_OBJECT(io);
        JSONW_WRITE(io, "pos", particle->pos);
        JSONW_WRITE(io, "life", particle->life);
        JSONW_WRITE(io, "vel", particle->vel);
        JSONW_POP_AND_APPEND(io);
    }
    JSONW_POP_AND_SET_NZ(io, "particles");
}

static RESULT M_Load(JSON_READ_IO *const io)
{
    if (!JSON_ReadIO_HasKey(io, "particles")) {
        return OK;
    }
    MUST(JSON_PUSH(io, "particles"));

    const int32_t count = JSON_ARRAY_LEN(io);
    for (int32_t i = 0; i < count; i++) {
        if (i >= M_MAX_WATER_PARTICLES) {
            LOG_WARNING(
                "Malformed save: too many water particles. Extra particles "
                "will be ignored.");
            break;
        }

        M_WATER_PARTICLE *const particle = &m_WaterParticles[i];
        MUST(JSON_PUSH_INDEX(io, i));
        MUST(JSON_READ(io, "pos", &particle->pos));
        MUST(JSON_READ(io, "life", &particle->life));
        MUST(JSON_READ(io, "vel", &particle->vel));
        MUST(JSON_POP(io));
        particle->prev_pos = particle->pos;
    }

    MUST(JSON_POP(io));
    return OK;
}

static const FX_MODULE m_Module = {
    .control_func = M_Control,
    .draw_func = M_Draw,
    .reset_func = M_Reset,
    .save_key = "water_particles",
    .save_func = M_Save,
    .load_func = M_Load,
};

REGISTER_FX(m_Module)
