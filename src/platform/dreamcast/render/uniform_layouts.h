#pragma once

// The layouts of the uniform buffers trx/game/output/uniforms.c fills, which
// the Dreamcast renderer reads back from the GL facade. They are private to
// that file, so they are repeated here and must follow it. The light source
// and fog bulb layouts are public, in trx/game/output/lights/priv.h.

#include <trx/game/output/lights.h>

#pragma pack(push, 4)
typedef struct {
    float fog_color[4];
    float fog_distance[2];
    float viewport_size[2];
    float vertex_snap_resolution[2];
    float time;
    float time_in_game;
    float brightness_multiplier;
    float ui_brightness_multiplier;
    float gamma;
    float sunset_duration;
    float min_shade;
    int billboard_lock_mode;
    int lighting_enabled;
    int static_lighting_enabled;
    int trapezoid_filter_enabled;
    int reflections_enabled;
    int textures_enabled;
    int vertex_snap_enabled;
    int ps1_fog_enabled;
    int lighting_curve;
    int affine_mapping_enabled;
    int tr_version;
    float uv_scroll_tick;
    int supersampling_factor;
    float _pad[1];
} DC_UNIFORM_GENERAL;

typedef struct {
    float mat_proj[4][4];
    float mat_view[4][4];
} DC_UNIFORM_MATRICES;

typedef struct {
    float pos[4];
    float color[4];
    float shade;
    float falloff;
    float kind;
    float _pad;
} DC_UNIFORM_LIGHT;

typedef struct {
    int num_lights;
    int room_light_mode;
    int _pad[2];
    DC_UNIFORM_LIGHT lights[OUTPUT_MAX_DYNAMIC_LIGHTS];
} DC_UNIFORM_LIGHTS;
#pragma pack(pop)
