#pragma once

#include <trx/core/math.h>
#include <trx/game/items/types.h>

#include <stdint.h>

typedef struct {
    union {
        struct {
            int32_t x;
            int32_t y;
            int32_t z;
        };
        XYZ_32 pos;
    };
    int16_t room_num;
} GAME_VECTOR;

typedef struct {
    union {
        struct {
            int32_t x;
            int32_t y;
            int32_t z;
        };
        XYZ_32 pos;
    };
    int16_t data;
    int16_t flags;
} OBJECT_VECTOR;

typedef struct {
    int32_t vertex_count;

    union {
        uint16_t texture_idx;
        uint16_t palette_idx;
    };
    uint16_t vertices[4];

#if !defined(__DREAMCAST__)
    // trapezoid ratios for textured quads that cannot be shared between
    // vertices (the PowerVR path reads none: 32 bytes of every face)
    TEXTURE_ZW_F texture_zw[4];
#endif

    uint16_t effects;
    bool double_sided;
    bool enable_reflections;
    // Draws the face at half opacity, like the PSX half blend mode.
    bool semi_transparent;
} FACE;
