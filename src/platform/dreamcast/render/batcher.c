// Dreamcast mesh batcher: the MESH_BATCHER of trx/game/output/mesh_batcher
// drawn through the PowerVR.
//
// The sources build their meshes and stage instances exactly as on the
// desktop. What the desktop does in its mesh shader happens here on the SH-4,
// once per vertex of every staged instance: the model and view transform
// (ftrv), the TR1/TR2 shade lighting, and fog, which the PowerVR adds as the
// offset colour. Faces are clipped against the near plane and projected, then
// handed to pvr.c, which puts them in the list their blending asks for. The
// translucent list is sorted by the PowerVR, so the transparent faces need no
// sorting of their own.

#include "cel.h"
#include "pvr.h"
#include "shadow.h"
#include "uniform_layouts.h"

#include <platform/dreamcast/dc_mode.h>
#include <platform/dreamcast/gl_facade.h>
#include <trx/config.h>
#include <trx/core/memory.h>
#include <trx/core/vector.h>
#include <trx/debug.h>
#include <trx/game/const.h>
#include <trx/game/output.h>
#include <trx/game/output/lights/priv.h>
#include <trx/game/output/mesh_batcher/batcher.h>
#include <trx/game/output/shaders/mesh.h>
#include <trx/game/output/textures.h>
#include <trx/game/output/utils.h>
#include <trx/game/output/vertex_range.h>
#include <trx/game/viewport.h>

#include <arch/timer.h>
#include <dc/fmath.h>
#include <dc/pvr.h>
#include <dc/matrix.h>
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <uthash.h>

// The build is optimised for size, which leaves inlining to GCC; the helpers
// run per vertex and per face must be inlined all the same.
#define M_HOT static inline __attribute__((always_inline))
#define M_SHADE_NEUTRAL 0x1000
#define M_SHADE_MAX 0x1FFF
#define M_NO_PAGE (-1)

typedef struct M_BINDING {
    OUTPUT_MESH *mesh;
    int32_t vertex_count;
    // Per vertex: the texture coordinates and page the vertex samples, looked
    // up once instead of every frame.
    // (As fractions of the page in 16 bits, 0 to M_UV_ONE.)
    uint16_t *uv;
    int8_t *page;
    bool needs_room_lights;
    bool needs_object_light;
    bool needs_own_light;
    // Every vertex lit by the object's light alone, as Lara, enemies and
    // pickups are (M_TransformInstance's object loop).
    bool plain_object;
    // Has sprites, whose corners stand off the box.
    bool billboard;
    // A room's: each vertex's colour lit without fog and with no light
    // passing, kept from frame to frame while what it is lit by holds.
    uint32_t *lit;
    bool lit_valid;
    bool lit_lighting;
    float lit_min_shade;
    RGBA_F lit_tint;
    // Object space bounding box: minimum then maximum corner.
    float bounds[2][3];
    // The longest of its normals, for the cel shading (0 until it is told).
    float normal_len;
    // The opaque triangles that reflect (VERT_REFLECTIVE on every corner),
    // drawn again with the environment map over them; quads' halves stay
    // side by side as M_PairQuads left them.
    OUTPUT_MESH_INDEX *env;
    int32_t env_count;
    UT_hash_handle hh;
} M_BINDING;

// A mesh instance that casts a shadow this frame.
typedef struct {
    const struct M_BINDING *bind;
    float bounds[2][3];
    float world[3][4];
    const ROOM *room;
} M_CASTER;

#define M_MAX_CASTERS 32
// How far a shadow volume reaches past its caster, in world units.
#define M_SHADOW_LENGTH 1024.0f
// How far a light may tilt a shadow off the vertical: a light level with or
// below the caster would throw it sideways or up.
#define M_SHADOW_MIN_DROP 0.6f
// How far a light may be from the casters and still turn their shadow.
#define M_SHADOW_LIGHT_RANGE (3.0f * WALL_L)

// The soft shadows (see shadow.h): one cell of the shadow map per light.
#define M_SOFT_MAX_LIGHTS 3
// Self-shadowing, from the nearest light only: the casters are cut into this
// many slices by how far they lie along the light, and each slice but the
// nearest takes the silhouette of the slices nearer the light, never its
// own (the PowerVR cannot compare depths, so a caster would shadow all of
// itself).
#define M_SOFT_SLICES 3 // see how M_KeepSoftCasters cuts them
// Where the cells lie in the map: the self-shadowing slices first, then the
// lights with a white cell before each. A big face of the world reaches past
// its light's cell, into the white either side or the map's clamped edge;
// Lara's own faces stay inside theirs.
#define M_SOFT_SELF_SLOT(slice) ((slice) - 1)
#define M_SOFT_LIGHT_SLOT(l) (M_SOFT_SLICES + 2 * (l))
_Static_assert(
    M_SOFT_LIGHT_SLOT(M_SOFT_MAX_LIGHTS - 1) == DC_PVR_SHADOW_MAP_SLOTS - 1,
    "the last light's cell lies against the map's clamped edge");
// The world units a silhouette spans about the casters' middle: room for
// Lara reaching up or out.
#define M_SOFT_SPAN 1024.0f
// How far from the casters' middle, along the light, faces take the shadow;
// it fades out on the way.
#define M_SOFT_REACH 1536.0f
// How much of a face's colour the shadow takes away, from a light at the end
// of its reach to one up close: the nearer the light, the darker.
#define M_SOFT_STRENGTH_MIN 0.5f
#define M_SOFT_STRENGTH_MAX 0.95f
// How strong a shadow is without a light near, as a light's weight.
#define M_SOFT_NO_LIGHT_WEIGHT 0.3f
// The same for the shadow volumes, which the PowerVR darkens all as much.
#define M_VOLUME_STRENGTH_MIN 0.2f
#define M_VOLUME_STRENGTH_MAX 0.95f
// How much of Lara's colour her own shadow takes away.
#define M_SOFT_SELF_STRENGTH 0.7f
// Caster meshes with more vertices than this are left out of the map.
#define M_SOFT_MAX_VERTICES 512

// Cel shading, as Jet Set Radio's: the meshes lit by the object light (Lara,
// the enemies) keep their light as it is, and their side turned from it is
// drawn again over them darker, cut with a hard edge. The PowerVR has no shaders;
// the edge comes of the punch-through alpha test, which cuts each pixel where
// the alpha the vertices hand on falls under half (DC_BLEND_PUNCH_SHADE).
// How much of its colour the shaded side keeps: as the shadows, the nearer
// the light, the darker; from a light at the end of its reach to one up
// close (and M_SOFT_NO_LIGHT_WEIGHT's worth with no light near).
#define M_CEL_SHADE_FAR 0.8f
#define M_CEL_SHADE_NEAR 0.45f
// Where the edge lies: the cosine to the light past which a vertex is in
// shade (0 at right angles to it; above 0, the shade narrows).
#define M_CEL_EDGE 0.0f
// A face whose corners all lie under half the alpha is lit all over: the
// alpha runs straight across it. (A colour under this has its alpha so.)
#define M_CEL_CUT 0x80000000u

// Relief (bump mapping): the room faces nearer the camera than this take
// their page's relief (DC_BLEND_BUMP), lit from the room's light nearest
// them, fading out over the last stretch.
#define M_BUMP_RANGE (2.5f * WALL_L)
#define M_BUMP_FADE (1.0f * WALL_L)
// How deep the relief's shade runs, of the whole colour.
#define M_BUMP_DEPTH 0.9f
// The lowest a light stands over a face, in radians: one level with it or
// behind it would shade every slope.
#define M_BUMP_MIN_ELEVATION 0.35f
#define M_BUMP_MAX_LIGHTS 16
// The relief goes off while the frame rate falls this far under the mode's,
// and comes back once it is whole again in a scene this much lighter (in
// vertices) than the one it went off in: back in the same scene, it would
// only go off again.
#define M_BUMP_SLOW_FPS 2
#define M_BUMP_LIGHTER 0.85f

// Reflections: the faces marked reflective take the environment map
// (DC_PVR_ENV_MAP) added over them, where their normal mirrors the view to,
// in their lit colour times this.
#define M_ENV_STRENGTH 0.4f

// A vertex once transformed and lit: eye space, and its colours.
typedef struct {
    float x;
    float y;
    float z;
    uint32_t argb;
    uint32_t oargb;
    // The cel shading's shaded side (see M_CEL_*): its colour, and in its
    // alpha how far the vertex is turned from the light, half at the edge.
    uint32_t sargb;
    // Where the vertex samples the environment map, and the colour it adds
    // it in, when the instance reflects.
    float eu;
    float ev;
    uint32_t eargb;
    // M_CLIP_*: what the vertex lies past; a face whose corners all lie past
    // one same thing is not drawn.
    uint8_t clip;
    // Where it lands on screen, when in front of the near plane: which way
    // a face turns is told from its corners.
    float sx;
    float sy;
    float inv_z;
} M_XVERT;

// Wholly in the fog.
#define M_CLIP_FOG 0x01
// Behind the near plane; the screen's sides are only told in front of it.
#define M_CLIP_NEAR 0x02
// Past a side of the screen, or of the room's portal window for its own
// faces (as the original game clipped them).
#define M_CLIP_LEFT 0x04
#define M_CLIP_RIGHT 0x08
#define M_CLIP_TOP 0x10
#define M_CLIP_BOTTOM 0x20
// Room left around a window: the game's bounds are worked out in whole
// pixels from its own projection.
#define M_CLIP_MARGIN 4.0f

typedef struct MESH_BATCHER {
    SCENE_SOURCE source;
    VECTOR *bindings; // M_BINDING *
    M_BINDING *map;
    VECTOR *staged[SCENE_PASS_COUNT];
    // Per staged instance: its index among the casters, or -1 (int16_t).
    VECTOR *casters[SCENE_PASS_COUNT];
    M_XVERT *xverts;
    // The model-view matrix of the instance just transformed.
    matrix_t mv;
    // Whether the instance just transformed is cel shaded.
    bool cel;
    // Whether the instance just transformed reflects.
    bool env;
    // Per vertex of the instance just transformed, for the soft shadows.
    float (*soft)[3];
    int32_t xverts_capacity;
} MESH_BATCHER;

// What a pass needs that holds for every instance in it.
typedef struct {
    float view[4][4];
    float proj_x;
    float proj_y;
    float center_x;
    float center_y;
    float near_z;
    // An orthographic projection (models drawn on the UI canvas): screen
    // position and depth straight from the eye position.
    bool ortho;
    float ortho_scale[3];
    float ortho_offset[3];
    float half_w;
    float half_h;
    float fog_start;
    float fog_end;
    float fog_scale;
    float fog_color[3];
    float min_shade;
    float time_in_game;
    float sunset_duration;
    bool lighting;
    // Bilinear filtering samples half a texel past a face's edge, into the
    // next texture on the page; the faces' coordinates are pulled in by that
    // much instead (the desktop clamps them to the texture in its shader).
    bool inset_uv;
    // What pvr.c scales screen positions by (DC_PVR_GetScreenScale).
    float screen_scale;
    bool fog;
    // Faces turned away are not drawn, as the desktop has OpenGL cull them
    // (double sided faces come twice, once each way round).
    bool cull;
    // The faces going out are the cel shading's shaded side: their vertices
    // take its colours.
    bool cel_pass;
    // The faces going out are the reflections: the environment map, at the
    // vertices' own coordinates on it and in their colours.
    bool env_pass;
    // The faces going out are the relief: white, with the offset colour
    // lighting it.
    bool bump_pass;
    uint32_t bump_k;
    // The mode draws the relief.
    bool bump;
    // The cel shading is on: the player's setting, where the mode allows it.
    bool cel;
    const DC_UNIFORM_LIGHTS *lights;
} M_FRAME;

static M_FRAME m_Frame = {};

// Whether the load leaves room for the relief (M_UpdateBumpLoad).
static bool m_BumpOn = true;
static uint32_t m_BumpOffVertices = 0;
// Vertices transformed this frame.
static uint32_t m_FrameVertices = 0;
// Whether this frame drew rooms: the menus' frames say nothing of the load.
static bool m_FrameRooms = false;

// Whether what is drawn now may be cel shaded (DC_Cel_SetAllowed).
static bool m_CelAllowed = true;

void DC_Cel_SetAllowed(const bool allowed)
{
    m_CelAllowed = allowed;
}

// The cel shading: the player's setting, where the mode allows it.
static bool M_CelOn(void)
{
    return m_CelAllowed && g_Config.rendering.dc_cel_shading
        && DC_Mode_Get()->cel_shading;
}

// Lara's shadow on herself: the cel shading shades her side turned from the
// light already, and the two together darken it twice.
static bool M_SelfShadowOn(void)
{
    return DC_Mode_Get()->self_shadow && !M_CelOn();
}

// How much of its colour the cel shading's shaded side keeps this frame, by
// the light nearest the shadow casters the last frame found (M_SetCelShade).
static float m_CelShade = M_CEL_SHADE_FAR;

static bool m_Casting = false;
static int32_t m_CasterCount = 0;
static M_CASTER m_Casters[M_MAX_CASTERS];

typedef struct {
    // From the light towards the casters, and two directions across it (the
    // silhouette's x and y), in world space.
    float dir[3];
    float right[3];
    float up[3];
    float strength;
} M_SOFT_LIGHT;

static struct {
    // The casters the last frame kept, and their middle, for the map.
    int32_t caster_count;
    M_CASTER casters[M_MAX_CASTERS];
    float center[3];
    // The lights the map is (or is to be) drawn for, one per cell.
    int32_t light_count;
    M_SOFT_LIGHT lights[M_SOFT_MAX_LIGHTS];
    // How many cells the map holds this frame.
    int32_t map_count;
    // The casters' slices, by their index, for the map drawn this frame.
    bool self;
    int32_t self_count;
    int8_t slice[M_MAX_CASTERS];
    // This frame's projection, in eye space: the casters' middle, and per
    // light its three directions, each divided by its squared length so
    // that a dot product with an eye space offset gives world units.
    bool active;
    float center_eye[3];
    float axes_eye[M_SOFT_MAX_LIGHTS][3][3];
} m_Soft = {};

static float m_SoftXY[M_SOFT_MAX_VERTICES][2];

// The shadows drawn: the player's choice, with soft ones drawn as volumes
// where the mode asks.
static SHADOW_TYPE M_ShadowType(void)
{
    const SHADOW_TYPE type = g_Config.visuals.shadow_type;
    return type == SHADOW_TYPE_SOFT && DC_Mode_Get()->volume_shadows
        ? SHADOW_TYPE_VOLUME
        : type;
}
// Faces drawn with a soft shadow, for the debug console.
static int32_t m_SoftFaces = 0;

// Running totals, for the watchdog.
int32_t g_DC_BatcherMeshCount = 0;
int32_t g_DC_BatcherVertexCount = 0;
// This frame's: instances and vertices transformed (the debug console's).
uint32_t g_DC_BatcherFrameInstances = 0;
uint32_t g_DC_BatcherFrameVertices = 0;
uint32_t g_DC_BatcherXformUs = 0;

M_HOT float M_Clamp(const float x, const float lo, const float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

// 1 / sqrt(x) for x > 0 by the SH-4's own approximation (to 2^-21), in
// place of a root and a division, each of which holds up the FPU for about
// a dozen cycles.
M_HOT float M_InvSqrt(float x)
{
    __asm__("fsrra %0" : "+f"(x));
    return x;
}

M_HOT uint32_t M_PackARGB(
    const float a, const float r, const float g, const float b)
{
    const uint32_t ia = (uint32_t)(M_Clamp(a, 0.0f, 1.0f) * 255.0f);
    const uint32_t ir = (uint32_t)(M_Clamp(r, 0.0f, 1.0f) * 255.0f);
    const uint32_t ig = (uint32_t)(M_Clamp(g, 0.0f, 1.0f) * 255.0f);
    const uint32_t ib = (uint32_t)(M_Clamp(b, 0.0f, 1.0f) * 255.0f);
    return (ia << 24) | (ir << 16) | (ig << 8) | ib;
}

// The colour of a packed ARGB scaled by k in [0, 1], its alpha kept.
M_HOT uint32_t M_ScaleRGB(const uint32_t c, const float k)
{
    const uint32_t m = (uint32_t)(k * 256.0f);
    return (c & 0xFF000000) | ((((c >> 16) & 0xFF) * m >> 8) << 16)
        | ((((c >> 8) & 0xFF) * m >> 8) << 8) | (((c & 0xFF) * m) >> 8);
}

// Each channel of c0 towards c1 by t (in 1/256ths), in integers.
static inline uint32_t M_LerpARGB(
    const uint32_t c0, const uint32_t c1, const int32_t t)
{
    uint32_t result = 0;
    for (int32_t shift = 0; shift < 32; shift += 8) {
        const int32_t a = (int32_t)((c0 >> shift) & 0xFF);
        const int32_t b = (int32_t)((c1 >> shift) & 0xFF);
        result |= (uint32_t)(a + (((b - a) * t + 128) >> 8)) << shift;
    }
    return result;
}

static M_BINDING *M_GetBinding(
    const MESH_BATCHER *const batcher, const OUTPUT_MESH *const mesh)
{
    M_BINDING *bind = nullptr;
    HASH_FIND_PTR(batcher->map, &mesh, bind);
    return bind;
}

#define M_UV_ONE 65535.0f

static inline uint16_t M_PackUV(const float x)
{
    return (uint16_t)(M_Clamp(x, 0.0f, 1.0f) * M_UV_ONE + 0.5f);
}

static void M_FillTexture(M_BINDING *const bind, const int32_t idx)
{
    const OUTPUT_MESH_VERTEX *const vertex =
        Vector_Get(bind->mesh->vertices, idx);
    if (vertex->uvw_idx < 0 || (vertex->flags & VERT_FLAT_SHADED) != 0) {
        bind->uv[idx * 2 + 0] = 0;
        bind->uv[idx * 2 + 1] = 0;
        bind->page[idx] = M_NO_PAGE;
        return;
    }
    const OUTPUT_UVW uvw = Output_Textures_GetUVW(vertex->uvw_idx);
    bind->uv[idx * 2 + 0] = M_PackUV(uvw.u);
    bind->uv[idx * 2 + 1] = M_PackUV(uvw.v);
    bind->page[idx] = (int8_t)uvw.w;
}

// The model-view matrix of an instance, as GL lays it out (column first),
// which is also how the SH-4's XMTRX takes it.
static void M_ModelView(
    const MATRIX *const wmatrix, float out[4][4], float *const out_scale)
{
    float model[4][4];
    Output_FillMatrix(model, wmatrix);
    for (int32_t c = 0; c < 4; c++) {
        for (int32_t r = 0; r < 4; r++) {
            out[c][r] = m_Frame.view[0][r] * model[c][0]
                + m_Frame.view[1][r] * model[c][1]
                + m_Frame.view[2][r] * model[c][2]
                + m_Frame.view[3][r] * model[c][3];
        }
    }
    *out_scale = sqrtf(
        model[0][0] * model[0][0] + model[0][1] * model[0][1]
        + model[0][2] * model[0][2]);
}

static int32_t M_FlickerIndex(const int32_t mode, const float time)
{
    switch (mode) {
    case RLM_FLICKER: {
        const float h = sinf(time * 593.123f) * 43758.5453f;
        return (int32_t)((h - floorf(h)) * 32.0f);
    }
    case RLM_GLOW: {
        const float phase = fmodf(time, 32.0f) / 32.0f;
        return (int32_t)((sinf(phase * 2.0f * M_PI) + 1.0f) * 0.5f * 31.0f);
    }
    case RLM_SUNSET: {
        const float duration =
            m_Frame.sunset_duration < 1.0f ? 1.0f : m_Frame.sunset_duration;
        return (int32_t)(M_Clamp(time / duration, 0.0f, 1.0f) * 31.0f);
    }
    default:
        return 0;
    }
}

// A room's point light, with what the shading asks of it worked out once.
typedef struct {
    float pos[3];
    float radius_sq;
    float max_shade;
    float inv_div;
} M_POINT_LIGHT;

#define M_MAX_POINT_LIGHTS 32

typedef struct {
    float l[3]; // object space light direction, for object lighting
    float adder;
    bool has_direction;
    // For vertices lit by the room's lights: the instance's world matrix in
    // world units, the lights, and the flicker the room's light mode adds
    // (times each vertex's light table offset).
    bool has_room_lights;
    float world[3][4];
    int32_t num_lights;
    M_POINT_LIGHT lights[M_MAX_POINT_LIGHTS];
    float flicker;
} M_INSTANCE_LIGHT;

M_HOT float M_DynamicLum(
    float shade, const float world[3], const M_INSTANCE_LIGHT *const il)
{
    for (int32_t i = 0; i < il->num_lights; i++) {
        const M_POINT_LIGHT *const light = &il->lights[i];
        const float dx = light->pos[0] - world[0];
        const float dy = light->pos[1] - world[1];
        const float dz = light->pos[2] - world[2];
        const float dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq > light->radius_sq) {
            continue;
        }
        shade -= light->max_shade - dist_sq * light->inv_div;
    }
    return shade < 0.0f ? 0.0f : shade;
}

// An instance's world matrix (fixed point in TR) in world units, by rows.
static void M_WorldMatrix(const MATRIX *const m, float out[3][4])
{
    const float scale = 1.0f / (float)(1 << W2V_SHIFT);
    const int64_t rows[3][4] = {
        { m->_00, m->_01, m->_02, m->_03 },
        { m->_10, m->_11, m->_12, m->_13 },
        { m->_20, m->_21, m->_22, m->_23 },
    };
    for (int32_t r = 0; r < 3; r++) {
        for (int32_t c = 0; c < 4; c++) {
            out[r][c] = Output_MatrixToFloat(rows[r][c]) * scale;
        }
    }
}

static void M_PrepareRoomLights(
    const MESH_INSTANCE *const inst, const M_BINDING *const bind,
    M_INSTANCE_LIGHT *const out)
{
    out->has_room_lights = true;
    M_WorldMatrix(&inst->wmatrix, out->world);

    const DC_UNIFORM_LIGHTS *const lights = m_Frame.lights;
    out->num_lights = 0;
    out->flicker = 0.0f;
    if (lights == nullptr) {
        return;
    }
    // The instance's bounding sphere in the world: a light that does not
    // reach it lights none of its vertices, and is left out.
    const float(*const w)[4] = out->world;
    float local[3];
    float half_sq = 0.0f;
    for (int32_t k = 0; k < 3; k++) {
        const float half = (bind->bounds[1][k] - bind->bounds[0][k]) * 0.5f;
        local[k] = bind->bounds[0][k] + half;
        half_sq += half * half;
    }
    float center[3];
    for (int32_t r = 0; r < 3; r++) {
        center[r] = w[r][0] * local[0] + w[r][1] * local[1]
            + w[r][2] * local[2] + w[r][3];
    }
    const float scale_sq =
        w[0][0] * w[0][0] + w[1][0] * w[1][0] + w[2][0] * w[2][0];
    const float inst_radius = sqrtf(half_sq * scale_sq);
    for (int32_t i = 0;
         i < lights->num_lights && out->num_lights < M_MAX_POINT_LIGHTS; i++) {
        const DC_UNIFORM_LIGHT *const light = &lights->lights[i];
        if (light->kind != 0.0f) {
            continue;
        }
        const float radius = exp2f(light->falloff);
        const float dx = light->pos[0] - center[0];
        const float dy = light->pos[1] - center[1];
        const float dz = light->pos[2] - center[2];
        const float reach = radius + inst_radius;
        if (dx * dx + dy * dy + dz * dz > reach * reach) {
            continue;
        }
        M_POINT_LIGHT *const pl = &out->lights[out->num_lights++];
        pl->pos[0] = light->pos[0];
        pl->pos[1] = light->pos[1];
        pl->pos[2] = light->pos[2];
        pl->radius_sq = radius * radius;
        pl->max_shade = exp2f(light->shade);
        pl->inv_div = 1.0f / exp2f(2.0f * light->falloff - light->shade);
    }
    const int32_t index =
        M_FlickerIndex(lights->room_light_mode, m_Frame.time_in_game);
    out->flicker = index * 512.0f / 31.0f;
}

static void M_PrepareInstanceLight(
    const MESH_INSTANCE *const inst, const M_BINDING *const bind,
    const float mv[4][4], M_INSTANCE_LIGHT *const out)
{
    out->has_direction = false;
    out->has_room_lights = false;
    out->adder = 0.0f;
    if (bind->needs_room_lights) {
        Output_Uniforms_UploadRoomLights(Output_GetUniforms(), inst->room);
        m_Frame.lights =
            DC_GL_GetUniformBlock(DC_GL_BINDING_LIGHTS, nullptr);
        M_PrepareRoomLights(inst, bind, out);
    }
    if (bind->needs_object_light) {
        Output_Lights_UploadCPULight(&inst->light_info);
    } else if (bind->needs_own_light) {
        Output_Lights_UploadOwnLight(&inst->light_info);
    } else {
        return;
    }
    const OUTPUT_UNIFORM_LS_TR12 *const ls =
        DC_GL_GetUniformBlock(DC_GL_BINDING_LIGHT_SOURCE, nullptr);
    if (ls == nullptr) {
        return;
    }
    out->adder = ls->adder;
    if (ls->divider != 0.0f) {
        // The view-space light vector, taken into object space by the
        // transpose of the model-view rotation.
        const float *const v = ls->vector_view;
        for (int32_t c = 0; c < 3; c++) {
            out->l[c] =
                (mv[c][0] * v[0] + mv[c][1] * v[1] + mv[c][2] * v[2])
                / ls->divider;
        }
        out->has_direction = true;
    }
}

M_HOT void M_LightVertex(
    const OUTPUT_MESH_VERTEX *const vertex, const M_INSTANCE_LIGHT *const il,
    const MATRIX *const wmatrix, const RGBA_F tint, float out_rgba[4],
    float *const out_shade_mul)
{
    const uint16_t flags = vertex->flags;
    float shade = M_SHADE_NEUTRAL;
    if (m_Frame.lighting && (flags & VERT_NO_LIGHTING) == 0) {
        if ((flags & VERT_USE_OWN_LIGHT) != 0) {
            shade = il->adder + vertex->shade;
        } else if ((flags & VERT_USE_OBJECT_LIGHT) != 0) {
            shade = il->adder;
            if (il->has_direction) {
                const float n_dot_l = fipr(
                    il->l[0], il->l[1], il->l[2], 0.0f, vertex->normal.x,
                    vertex->normal.y, vertex->normal.z, 0.0f);
                shade += n_dot_l * (1.0f / (float)(1 << 16));
                shade = M_Clamp(shade, 0.0f, M_SHADE_MAX);
            }
        } else {
            shade = vertex->shade;
            if ((flags & VERT_USE_DYNAMIC_LIGHT) != 0 && il->has_room_lights) {
                // Most rooms have no light passing and do not flicker.
                if (il->num_lights > 0) {
                    const float x = vertex->pos.x;
                    const float y = vertex->pos.y;
                    const float z = vertex->pos.z;
                    const float(*const w)[4] = il->world;
                    const float world[3] = {
                        w[0][0] * x + w[0][1] * y + w[0][2] * z + w[0][3],
                        w[1][0] * x + w[1][1] * y + w[1][2] * z + w[1][3],
                        w[2][0] * x + w[2][1] * y + w[2][2] * z + w[2][3],
                    };
                    shade = M_DynamicLum(shade, world, il);
                }
                if (il->flicker != 0.0f) {
                    const float j = (float)(vertex->light_table_idx & 31);
                    shade += (j - 16.0f) * il->flicker;
                }
            }
            shade = M_Clamp(shade, 0.0f, M_SHADE_MAX);
        }
    }

    float mul = 1.0f;
    if ((flags & VERT_NO_LIGHTING) == 0) {
        const float s = shade > m_Frame.min_shade ? shade : m_Frame.min_shade;
        mul = 2.0f - s * (1.0f / M_SHADE_NEUTRAL);
    }
    *out_shade_mul = mul;
    // Products, not quotients: the build keeps a division a division, and
    // the SH-4 takes several times as long over one.
    const float m = mul * (1.0f / 255.0f);
    out_rgba[0] = vertex->color.r * m * tint.r;
    out_rgba[1] = vertex->color.g * m * tint.g;
    out_rgba[2] = vertex->color.b * m * tint.b;
    out_rgba[3] = vertex->color.a * (1.0f / 255.0f) * tint.a;
}

static bool M_TransformInstance(
    MESH_BATCHER *const batcher, const MESH_INSTANCE *const inst,
    M_BINDING *const bind)
{
    if (bind->vertex_count > batcher->xverts_capacity) {
        batcher->xverts_capacity = bind->vertex_count + 256;
        Memory_Free(batcher->xverts);
        batcher->xverts = memalign(32, batcher->xverts_capacity * sizeof(M_XVERT));
        Memory_Free(batcher->soft);
        batcher->soft =
            Memory_Alloc(batcher->xverts_capacity * sizeof(*batcher->soft));
    }

    float mv[4][4];
    float scale;
    M_ModelView(&inst->wmatrix, mv, &scale);

    // The sides faces are dropped past, about the screen's middle: the
    // screen's, or a room's own window onto it.
    float left = -m_Frame.half_w;
    float right = m_Frame.half_w;
    float top = -m_Frame.half_h;
    float bottom = m_Frame.half_h;
    if (bind->needs_room_lights && inst->scissor.width > 0) {
        const VIEWPORT_RECT *const s = &inst->scissor;
        left = MAX(left, s->x - m_Frame.center_x - M_CLIP_MARGIN);
        right = MIN(right, s->x + s->width - m_Frame.center_x + M_CLIP_MARGIN);
        top = MAX(top, s->y - s->height - m_Frame.center_y - M_CLIP_MARGIN);
        bottom = MIN(bottom, s->y - m_Frame.center_y + M_CLIP_MARGIN);
    }

    if (!m_Frame.ortho) {
        float center[3];
        float radius_sq = 0.0f;
        for (int32_t k = 0; k < 3; k++) {
            const float half = (bind->bounds[1][k] - bind->bounds[0][k]) * 0.5f;
            center[k] = bind->bounds[0][k] + half;
            radius_sq += half * half;
        }
        float eye[3];
        for (int32_t r = 0; r < 3; r++) {
            eye[r] = mv[0][r] * center[0] + mv[1][r] * center[1]
                + mv[2][r] * center[2] + mv[3][r];
        }
        const float radius = sqrtf(radius_sq) * scale;
        // Wholly past the end of the fog: nothing of it would show.
        const float reach = m_Frame.fog_end + radius;
        if (m_Frame.fog
            && eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]
                > reach * reach) {
            return false;
        }
        // Wholly past one of the sides. Each side is a plane through the
        // eye, sx = side for x: proj_x * x - side * z = 0, the same for y.
        // (Sprites' corners stand off their middle, past the box.)
        if (!bind->billboard) {
            const float px = m_Frame.proj_x;
            const float py = m_Frame.proj_y;
            // Per side: the factors of x (left, right) or y (top, bottom),
            // and of z; positive outside.
            const float planes[4][2] = {
                { -px, left }, { px, -right }, { -py, top }, { py, -bottom },
            };
            for (int32_t p = 0; p < 4; p++) {
                const float a = planes[p][0];
                const float b = planes[p][1];
                const float d = a * eye[p < 2 ? 0 : 1] + b * eye[2];
                if (d > 0.0f && d * d > radius * radius * (a * a + b * b)) {
                    return false;
                }
            }
        }
    }

    g_DC_BatcherFrameInstances++;
    g_DC_BatcherFrameVertices += bind->vertex_count;
    m_FrameVertices += bind->vertex_count;
    M_INSTANCE_LIGHT il;
    M_PrepareInstanceLight(inst, bind, mv, &il);

    // Cel shaded: for each vertex how far it turns from the light, as a
    // cosine. The light runs across the lit side as before, so that it
    // keeps its roundness; only the shade is cut hard.
    batcher->cel = m_Frame.cel && m_Frame.lighting && !m_Frame.ortho
        && bind->needs_object_light && il.has_direction;
    float cel_scale = 0.0f;
    if (batcher->cel) {
        if (bind->normal_len == 0.0f) {
            const OUTPUT_MESH_VERTEX *const vs =
                Vector_GetData(bind->mesh->vertices);
            float len_sq = 0.0f;
            for (int32_t i = 0; i < bind->vertex_count; i++) {
                const float x = vs[i].normal.x;
                const float y = vs[i].normal.y;
                const float z = vs[i].normal.z;
                len_sq = MAX(len_sq, x * x + y * y + z * z);
            }
            bind->normal_len = len_sq > 0.0f ? sqrtf(len_sq) : -1.0f;
        }
        // The most the directional light adds, in the shade's units.
        const float reach =
            sqrtf(il.l[0] * il.l[0] + il.l[1] * il.l[1] + il.l[2] * il.l[2])
            * bind->normal_len;
        if (reach > 0.0f) {
            cel_scale = 1.0f / reach;
        } else {
            batcher->cel = false;
        }
    }

    const float fog_start_sq = m_Frame.fog_start * m_Frame.fog_start;
    // A room's colours stay as they were while no light passes, its light
    // does not flicker and the frame lights it as before.
    const bool use_lit =
        bind->lit != nullptr && il.num_lights == 0 && il.flicker == 0.0f;
    const bool fill_lit = use_lit
        && (!bind->lit_valid || bind->lit_lighting != m_Frame.lighting
            || bind->lit_min_shade != m_Frame.min_shade
            || memcmp(&bind->lit_tint, &inst->tint, sizeof(RGBA_F)) != 0);
    if (fill_lit) {
        bind->lit_valid = true;
        bind->lit_lighting = m_Frame.lighting;
        bind->lit_min_shade = m_Frame.min_shade;
        bind->lit_tint = inst->tint;
    }

    const bool black_fog = m_Frame.fog_color[0] * inst->tint.r == 0.0f
        && m_Frame.fog_color[1] * inst->tint.g == 0.0f
        && m_Frame.fog_color[2] * inst->tint.b == 0.0f;

    batcher->env = bind->env_count > 0 && !m_Frame.ortho;
    // Which way is right and which is down on the screen, in eye space.
    const float env_x = m_Frame.proj_x < 0.0f ? -1.0f : 1.0f;
    const float env_y = m_Frame.proj_y < 0.0f ? -1.0f : 1.0f;

    memcpy(batcher->mv, mv, sizeof(batcher->mv));
    mat_load(&batcher->mv);
    const OUTPUT_MESH_VERTEX *const vertices =
        Vector_GetData(bind->mesh->vertices);

    if (use_lit && !fill_lit && black_fog && !m_Frame.ortho && !batcher->env
        && !batcher->cel) {
        // Most of the world: a room's kept colours, faded into black fog.
        // Only what such a vertex needs, with what holds for the whole
        // instance asked once instead of per vertex.
        const bool fog_on = m_Frame.fog;
        const float fog_start = m_Frame.fog_start;
        const float fog_scale = m_Frame.fog_scale;
        const float near_z = m_Frame.near_z;
        const float proj_x = m_Frame.proj_x;
        const float proj_y = m_Frame.proj_y;
        const uint32_t *const lit = bind->lit;
        M_XVERT *const xverts = batcher->xverts;
        const int32_t count = bind->vertex_count;
        for (int32_t i = 0; i < count; i++) {
            const OUTPUT_MESH_VERTEX *const vertex = &vertices[i];
            // The SH-4 does not wait for a line it was told to fetch ahead.
            __builtin_prefetch(vertex + 2);
            const uint16_t flags = vertex->flags;
            float x = vertex->pos.x;
            float y = vertex->pos.y;
            float z = vertex->pos.z;
            float w = 1.0f;
            mat_trans_nodiv(x, y, z, w);
            if ((flags & (VERT_BILLBOARD | VERT_ABS_SPRITE)) != 0) {
                x += vertex->normal.x * scale;
                y += vertex->normal.y * scale;
            }
            M_XVERT *const out = &xverts[i];
            out->x = x;
            out->y = y;
            out->z = z;
            uint32_t argb = lit[i];
            uint32_t oargb = 0;
            uint8_t clip = 0;
            if (fog_on && (flags & (VERT_NO_FOG | VERT_NO_LIGHTING)) == 0) {
                const float dist_sq = fipr_magnitude_sqr(x, y, z, 0.0f);
                if (dist_sq > fog_start_sq) {
                    const float fog = M_Clamp(
                        (dist_sq * M_InvSqrt(dist_sq) - fog_start) * fog_scale,
                        0.0f, 1.0f);
                    if (fog > 0.0f) {
                        clip = fog >= 1.0f ? M_CLIP_FOG : 0;
                        argb = M_ScaleRGB(argb, 1.0f - fog);
                        oargb = 0xFF000000u;
                    }
                }
            }
            if (z >= near_z) {
                const float inv_z = M_InvSqrt(z * z);
                const float sx = proj_x * x * inv_z;
                const float sy = proj_y * y * inv_z;
                out->sx = sx;
                out->sy = sy;
                out->inv_z = inv_z;
                clip |= (sx < left ? M_CLIP_LEFT : 0)
                    | (sx > right ? M_CLIP_RIGHT : 0)
                    | (sy < top ? M_CLIP_TOP : 0)
                    | (sy > bottom ? M_CLIP_BOTTOM : 0);
            } else {
                clip |= M_CLIP_NEAR;
            }
            out->clip = clip;
            out->argb = argb;
            out->oargb = oargb;
        }
        return true;
    }

    if (bind->plain_object && m_Frame.lighting && black_fog && !m_Frame.ortho
        && !batcher->env) {
        // Lara, enemies, pickups: the object's light on each vertex, with
        // n.l found once for the light and the cel shading both.
        const bool fog_on = m_Frame.fog;
        const float fog_start = m_Frame.fog_start;
        const float fog_scale = m_Frame.fog_scale;
        const float near_z = m_Frame.near_z;
        const float proj_x = m_Frame.proj_x;
        const float proj_y = m_Frame.proj_y;
        const float min_shade = m_Frame.min_shade;
        const bool dir = il.has_direction;
        const bool cel = batcher->cel;
        const float cel_shade = m_CelShade;
        const float l0 = il.l[0];
        const float l1 = il.l[1];
        const float l2 = il.l[2];
        const float adder = il.adder;
        // Colour times tint, in 0..1 for 0..255.
        const float kr = inst->tint.r * (1.0f / 255.0f);
        const float kg = inst->tint.g * (1.0f / 255.0f);
        const float kb = inst->tint.b * (1.0f / 255.0f);
        const float ka = inst->tint.a * (1.0f / 255.0f);
        M_XVERT *const xverts = batcher->xverts;
        const int32_t count = bind->vertex_count;
        for (int32_t i = 0; i < count; i++) {
            const OUTPUT_MESH_VERTEX *const vertex = &vertices[i];
            __builtin_prefetch(vertex + 2);
            float x = vertex->pos.x;
            float y = vertex->pos.y;
            float z = vertex->pos.z;
            float w = 1.0f;
            mat_trans_nodiv(x, y, z, w);
            M_XVERT *const out = &xverts[i];
            out->x = x;
            out->y = y;
            out->z = z;

            float fog = 0.0f;
            if (fog_on && (vertex->flags & VERT_NO_FOG) == 0) {
                const float dist_sq = fipr_magnitude_sqr(x, y, z, 0.0f);
                if (dist_sq > fog_start_sq) {
                    fog = M_Clamp(
                        (dist_sq * M_InvSqrt(dist_sq) - fog_start) * fog_scale,
                        0.0f, 1.0f);
                }
            }
            uint8_t clip = fog >= 1.0f ? M_CLIP_FOG : 0;
            if (z >= near_z) {
                const float inv_z = M_InvSqrt(z * z);
                const float sx = proj_x * x * inv_z;
                const float sy = proj_y * y * inv_z;
                out->sx = sx;
                out->sy = sy;
                out->inv_z = inv_z;
                clip |= (sx < left ? M_CLIP_LEFT : 0)
                    | (sx > right ? M_CLIP_RIGHT : 0)
                    | (sy < top ? M_CLIP_TOP : 0)
                    | (sy > bottom ? M_CLIP_BOTTOM : 0);
            } else {
                clip |= M_CLIP_NEAR;
            }
            out->clip = clip;

            // As M_LightVertex lights a vertex of the object's light.
            float shade = adder;
            float n_dot_l = 0.0f;
            if (dir) {
                n_dot_l = fipr(
                    l0, l1, l2, 0.0f, vertex->normal.x, vertex->normal.y,
                    vertex->normal.z, 0.0f);
                shade = M_Clamp(
                    shade + n_dot_l * (1.0f / (float)(1 << 16)), 0.0f,
                    M_SHADE_MAX);
            }
            const float s = shade > min_shade ? shade : min_shade;
            const float mul = 2.0f - s * (1.0f / M_SHADE_NEUTRAL);
            const float r = vertex->color.r * mul * kr;
            const float g = vertex->color.g * mul * kg;
            const float b = vertex->color.b * mul * kb;
            const float a = vertex->color.a * ka;
            const float keep = 1.0f - fog;
            out->argb = M_PackARGB(a, r * keep, g * keep, b * keep);
            out->oargb = fog > 0.0f ? 0xFF000000u : 0;
            if (cel) {
                const float dark = keep * cel_shade;
                out->sargb = M_PackARGB(
                    0.5f + 0.5f * (n_dot_l * cel_scale - M_CEL_EDGE), r * dark,
                    g * dark, b * dark);
            }
        }
        return true;
    }

    for (int32_t i = 0; i < bind->vertex_count; i++) {
        const OUTPUT_MESH_VERTEX *const vertex = &vertices[i];
        float x = vertex->pos.x;
        float y = vertex->pos.y;
        float z = vertex->pos.z;
        float w = 1.0f;
        mat_trans_nodiv(x, y, z, w);
        if ((vertex->flags & (VERT_BILLBOARD | VERT_ABS_SPRITE)) != 0) {
            x += vertex->normal.x * scale;
            y += vertex->normal.y * scale;
        }

        M_XVERT *const out = &batcher->xverts[i];
        out->x = x;
        out->y = y;
        out->z = z;

        float fog = 0.0f;
        if (m_Frame.fog && (vertex->flags & (VERT_NO_FOG | VERT_NO_LIGHTING)) == 0) {
            // Nearer than the fog starts, as most is, it needs no root.
            const float dist_sq = fipr_magnitude_sqr(x, y, z, 0.0f);
            if (dist_sq > fog_start_sq) {
                fog = M_Clamp(
                    (dist_sq * M_InvSqrt(dist_sq) - m_Frame.fog_start)
                        * m_Frame.fog_scale,
                    0.0f, 1.0f);
            }
        }
        uint8_t clip = fog >= 1.0f ? M_CLIP_FOG : 0;
        // Kept for the projection, so that a vertex is divided once.
        if (m_Frame.ortho) {
        } else if (z >= m_Frame.near_z) {
            // z is past the near plane, so positive.
            const float inv_z = M_InvSqrt(z * z);
            const float sx = m_Frame.proj_x * x * inv_z;
            const float sy = m_Frame.proj_y * y * inv_z;
            out->sx = sx;
            out->sy = sy;
            out->inv_z = inv_z;
            clip |= (sx < left ? M_CLIP_LEFT : 0)
                | (sx > right ? M_CLIP_RIGHT : 0)
                | (sy < top ? M_CLIP_TOP : 0)
                | (sy > bottom ? M_CLIP_BOTTOM : 0);
        } else {
            clip |= M_CLIP_NEAR;
        }
        out->clip = clip;
        // The kept colour, faded into black fog as the lighting below
        // would fade it.
        if (use_lit && !fill_lit && (fog == 0.0f || black_fog)) {
            out->argb =
                fog == 0.0f ? bind->lit[i] : M_ScaleRGB(bind->lit[i], 1.0f - fog);
            out->oargb = fog > 0.0f ? 0xFF000000u : 0;
            continue;
        }

        float rgba[4];
        float mul;
        M_LightVertex(vertex, &il, &inst->wmatrix, inst->tint, rgba, &mul);
        if (fill_lit) {
            bind->lit[i] = M_PackARGB(rgba[3], rgba[0], rgba[1], rgba[2]);
        }
        const float keep = 1.0f - fog;
        out->argb =
            M_PackARGB(rgba[3], rgba[0] * keep, rgba[1] * keep, rgba[2] * keep);
        out->oargb = fog > 0.0f
            ? M_PackARGB(
                  1.0f, m_Frame.fog_color[0] * fog * inst->tint.r,
                  m_Frame.fog_color[1] * fog * inst->tint.g,
                  m_Frame.fog_color[2] * fog * inst->tint.b)
            : 0;
        if (batcher->cel) {
            // Half at the edge, more into the shade; the PowerVR runs it
            // across each face, and the alpha test cuts it there.
            float shade = 0.0f;
            if ((vertex->flags & (VERT_USE_OBJECT_LIGHT | VERT_NO_LIGHTING))
                == VERT_USE_OBJECT_LIGHT) {
                const float cos = fipr(il.l[0], il.l[1], il.l[2], 0.0f,
                                      vertex->normal.x, vertex->normal.y,
                                      vertex->normal.z, 0.0f)
                    * cel_scale;
                shade = 0.5f + 0.5f * (cos - M_CEL_EDGE);
            }
            const float dark = keep * m_CelShade;
            out->sargb = M_PackARGB(
                shade, rgba[0] * dark, rgba[1] * dark, rgba[2] * dark);
        }
        if (batcher->env && (vertex->flags & VERT_REFLECTIVE) != 0) {
            // The normal in eye space (the camera at the origin, looking
            // down z), turned to face the camera, mirrors the way to the
            // vertex; the sphere map holds what lies that way.
            const float nx = vertex->normal.x;
            const float ny = vertex->normal.y;
            const float nz = vertex->normal.z;
            float n[3] = {
                mv[0][0] * nx + mv[1][0] * ny + mv[2][0] * nz,
                mv[0][1] * nx + mv[1][1] * ny + mv[2][1] * nz,
                mv[0][2] * nx + mv[1][2] * ny + mv[2][2] * nz,
            };
            const float d_len_sq = x * x + y * y + z * z;
            const float n_len_sq = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
            float eu = 0.5f;
            float ev = 0.5f;
            if (d_len_sq > 0.0f && n_len_sq > 0.0f) {
                const float d[3] = {
                    x / sqrtf(d_len_sq), y / sqrtf(d_len_sq),
                    z / sqrtf(d_len_sq),
                };
                const float inv_n = 1.0f / sqrtf(n_len_sq);
                float d_n = 0.0f;
                for (int32_t k = 0; k < 3; k++) {
                    n[k] *= inv_n;
                    d_n += d[k] * n[k];
                }
                if (d_n > 0.0f) {
                    for (int32_t k = 0; k < 3; k++) {
                        n[k] = -n[k];
                    }
                    d_n = -d_n;
                }
                const float r[3] = {
                    d[0] - 2.0f * d_n * n[0],
                    d[1] - 2.0f * d_n * n[1],
                    d[2] - 2.0f * d_n * n[2],
                };
                // Straight back at the camera is the map's middle, away
                // from it its rim.
                const float m2 =
                    r[0] * r[0] + r[1] * r[1] + (r[2] - 1.0f) * (r[2] - 1.0f);
                if (m2 > 0.0f) {
                    const float inv_m = 0.5f / sqrtf(m2);
                    eu = 0.5f + env_x * r[0] * inv_m;
                    ev = 0.5f + env_y * r[1] * inv_m;
                }
            }
            out->eu = eu;
            out->ev = ev;
            // Half its own, half the light on it: metal shows its
            // surroundings even in the shade, if less.
            float e[3];
            for (int32_t k = 0; k < 3; k++) {
                e[k] = keep * M_ENV_STRENGTH
                    * (0.5f + 0.5f * M_Clamp(rgba[k], 0.0f, 1.0f));
            }
            out->eargb = M_PackARGB(1.0f, e[0], e[1], e[2]);
        }
    }
    return true;
}

M_HOT void M_Project(
    const M_XVERT *const in, const float u, const float v,
    DC_PVR_VERTEX *const out)
{
    if (m_Frame.ortho) {
        const float(*const s) = m_Frame.ortho_scale;
        const float(*const o) = m_Frame.ortho_offset;
        out->x = m_Frame.center_x + (s[0] * in->x + o[0]) * m_Frame.half_w;
        out->y = m_Frame.center_y - (s[1] * in->y + o[1]) * m_Frame.half_h;
        // -1 at the near end; the PowerVR wants the nearer bigger.
        out->z = DC_PVR_Depth2D(0.5f - 0.5f * (s[2] * in->z + o[2]));
        out->u = u;
        out->v = v;
        out->argb = m_Frame.bump_pass ? 0xFFFFFFFFu
            : m_Frame.env_pass        ? in->eargb
            : m_Frame.cel_pass        ? in->sargb
                                      : in->argb;
        out->oargb = m_Frame.bump_pass ? m_Frame.bump_k
            : m_Frame.env_pass         ? 0
                                       : in->oargb;
        return;
    }
    out->x = m_Frame.center_x + in->sx;
    out->y = m_Frame.center_y + in->sy;
    out->z = DC_PVR_Depth3D(m_Frame.near_z * in->inv_z);
    out->u = u;
    out->v = v;
    // Most faces are in none of the extra passes: one test for them.
    if (!(m_Frame.bump_pass | m_Frame.env_pass | m_Frame.cel_pass)) {
        out->argb = in->argb;
        out->oargb = in->oargb;
        return;
    }
    out->argb = m_Frame.bump_pass ? 0xFFFFFFFFu
        : m_Frame.env_pass        ? in->eargb
        : m_Frame.cel_pass        ? in->sargb
                                  : in->argb;
    out->oargb = m_Frame.bump_pass ? m_Frame.bump_k
        : m_Frame.env_pass         ? 0
                                   : in->oargb;
}

// M_Project straight into a list's buffer (DC_PVR_ReservePoly); inlined, the
// vertex in between is never stored.
M_HOT void M_ProjectInto(
    const M_XVERT *const in, const float u, const float v, const uint32_t flags,
    pvr_vertex_t *const out)
{
    DC_PVR_VERTEX p;
    M_Project(in, u, v, &p);
    out->flags = flags;
    out->x = p.x * m_Frame.screen_scale;
    out->y = p.y * m_Frame.screen_scale;
    out->z = p.z;
    out->u = p.u;
    out->v = p.v;
    out->argb = p.argb;
    out->oargb = p.oargb;
}

// A vertex of a face being clipped: transformed, with its texture
// coordinates alongside.
typedef struct {
    M_XVERT xv;
    float u;
    float v;
} M_CLIP_VERT;

static M_CLIP_VERT M_ClipEdge(
    const M_CLIP_VERT *const a, const M_CLIP_VERT *const b)
{
    const float t = (m_Frame.near_z - a->xv.z) / (b->xv.z - a->xv.z);
    const float x = a->xv.x + (b->xv.x - a->xv.x) * t;
    const float y = a->xv.y + (b->xv.y - a->xv.y) * t;
    const float inv_z = 1.0f / m_Frame.near_z;
    const int32_t ti = (int32_t)(t * 256.0f + 0.5f);
    // Only the colours M_Project takes in this pass.
    return (M_CLIP_VERT) {
        .xv = {
            .x = x,
            .y = y,
            .z = m_Frame.near_z,
            .sx = m_Frame.proj_x * x * inv_z,
            .sy = m_Frame.proj_y * y * inv_z,
            .inv_z = inv_z,
            .argb = M_LerpARGB(a->xv.argb, b->xv.argb, ti),
            .oargb = M_LerpARGB(a->xv.oargb, b->xv.oargb, ti),
            .sargb = m_Frame.cel_pass
                ? M_LerpARGB(a->xv.sargb, b->xv.sargb, ti)
                : 0,
            .eargb = m_Frame.env_pass
                ? M_LerpARGB(a->xv.eargb, b->xv.eargb, ti)
                : 0,
        },
        .u = a->u + (b->u - a->u) * t,
        .v = a->v + (b->v - a->v) * t,
    };
}

// Half a texel of a texture page, in page coordinates.
#define M_HALF_TEXEL (0.5f / TEXTURE_PAGE_WIDTH)

// How far a corner moves towards its face's middle, d away.
M_HOT float M_Inset(const float d)
{
    return d > M_HALF_TEXEL ? M_HALF_TEXEL
        : d < -M_HALF_TEXEL ? -M_HALF_TEXEL
                            : d * 0.5f;
}

// Reads a face's texture coordinates, pulled towards their middle by up to
// half a texel when filtering bilinearly.
static void M_GetFaceUVs(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const OUTPUT_MESH_INDEX *const idx, const int32_t n, float uv[][2])
{
    if (m_Frame.env_pass) {
        for (int32_t k = 0; k < n; k++) {
            uv[k][0] = batcher->xverts[idx[k]].eu;
            uv[k][1] = batcher->xverts[idx[k]].ev;
        }
        return;
    }
    float cu = 0.0f;
    float cv = 0.0f;
    for (int32_t k = 0; k < n; k++) {
        uv[k][0] = bind->uv[idx[k] * 2] * (1.0f / M_UV_ONE);
        uv[k][1] = bind->uv[idx[k] * 2 + 1] * (1.0f / M_UV_ONE);
        cu += uv[k][0];
        cv += uv[k][1];
    }
    if (!m_Frame.inset_uv || bind->page[idx[0]] == M_NO_PAGE) {
        return;
    }
    const float inv_n = n == 4 ? 0.25f : 1.0f / 3.0f;
    cu *= inv_n;
    cv *= inv_n;
    for (int32_t k = 0; k < n; k++) {
        uv[k][0] += M_Inset(cu - uv[k][0]);
        uv[k][1] += M_Inset(cv - uv[k][1]);
    }
}

// OpenGL's front faces wind anticlockwise with y up; on the screen, where y
// grows downwards, they wind clockwise. Only for corners all in front of the
// near plane.
M_HOT bool M_IsBackFacing(
    const M_XVERT *const a, const M_XVERT *const b, const M_XVERT *const c)
{
    return (b->sx - a->sx) * (c->sy - a->sy) - (c->sx - a->sx) * (b->sy - a->sy)
        >= 0.0f;
}

static void M_EmitTriangle(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const uint32_t i0, const uint32_t i1, const uint32_t i2)
{
    const OUTPUT_MESH_INDEX idx[3] = { i0, i1, i2 };
    const M_XVERT *const xv = batcher->xverts;
    if ((xv[i0].clip & xv[i1].clip & xv[i2].clip) != 0) {
        return;
    }
    if (m_Frame.cel_pass
        && (xv[i0].sargb | xv[i1].sargb | xv[i2].sargb) < M_CEL_CUT) {
        return;
    }
    // Read only for faces that are drawn: those turned away go first.
    float uv[3][2];
    const float near_z = m_Frame.near_z;

    if (((xv[i0].clip | xv[i1].clip | xv[i2].clip) & M_CLIP_NEAR) == 0) {
        if (m_Frame.cull
            && M_IsBackFacing(
                &batcher->xverts[i0], &batcher->xverts[i1],
                &batcher->xverts[i2])) {
            return;
        }
        M_GetFaceUVs(batcher, bind, idx, 3, uv);
        pvr_vertex_t *const out = DC_PVR_ReservePoly(3);
        if (out == nullptr) {
            return;
        }
        for (int32_t k = 0; k < 3; k++) {
            M_ProjectInto(
                &batcher->xverts[idx[k]], uv[k][0], uv[k][1],
                k == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, &out[k]);
        }
        DC_PVR_CommitPoly(3);
        return;
    }

    // Sutherland-Hodgman against the near plane: three corners in, at most
    // four out.
    M_GetFaceUVs(batcher, bind, idx, 3, uv);
    M_CLIP_VERT in[3];
    for (int32_t k = 0; k < 3; k++) {
        in[k] = (M_CLIP_VERT) {
            .xv = batcher->xverts[idx[k]],
            .u = uv[k][0],
            .v = uv[k][1],
        };
    }
    M_CLIP_VERT poly[4];
    int32_t count = 0;
    for (int32_t k = 0; k < 3; k++) {
        const M_CLIP_VERT *const a = &in[k];
        const M_CLIP_VERT *const b = &in[(k + 1) % 3];
        const bool a_in = a->xv.z >= near_z;
        const bool b_in = b->xv.z >= near_z;
        if (a_in) {
            poly[count++] = *a;
        }
        if (a_in != b_in) {
            poly[count++] = M_ClipEdge(a, b);
        }
    }
    DC_PVR_VERTEX out[4];
    for (int32_t k = 0; k < count; k++) {
        M_Project(&poly[k].xv, poly[k].u, poly[k].v, &out[k]);
    }
    if (count == 3) {
        DC_PVR_Triangle(&out[0], &out[1], &out[2]);
    } else if (count == 4) {
        // Fan 0-1-2-3 as a strip: 1, 0, 2, 3.
        DC_PVR_Quad(&out[1], &out[0], &out[2], &out[3]);
    }
}

// Whether two triangles are the halves of a quad as M_PairQuads lays them
// out: a, b, c then c, b, d.
M_HOT bool M_IsQuad(const OUTPUT_MESH_INDEX *const t)
{
    return t[3] == t[2] && t[4] == t[1];
}

// Turns the corners of each pair of triangles that share an edge (as a
// quad's halves do) so that M_IsQuad tells them apart with two comparisons;
// each triangle keeps its winding.
static void M_PairQuads(OUTPUT_MESH_INDEX *const indices, const int32_t count)
{
    for (int32_t i = 0; i + 5 < count; i += 3) {
        OUTPUT_MESH_INDEX *const t0 = &indices[i];
        OUTPUT_MESH_INDEX *const t1 = &indices[i + 3];
        for (int32_t r0 = 0; r0 < 3; r0++) {
            const OUTPUT_MESH_INDEX a[3] = { t0[r0], t0[(r0 + 1) % 3], t0[(r0 + 2) % 3] };
            int32_t r1 = 0;
            while (r1 < 3
                   && !(t1[r1] == a[2] && t1[(r1 + 1) % 3] == a[1])) {
                r1++;
            }
            if (r1 == 3 || t1[(r1 + 2) % 3] == a[0]) {
                continue;
            }
            const OUTPUT_MESH_INDEX b[3] = { t1[r1], t1[(r1 + 1) % 3], t1[(r1 + 2) % 3] };
            memcpy(t0, a, sizeof(a));
            memcpy(t1, b, sizeof(b));
            // The pair is taken: the next starts after it.
            i += 3;
            break;
        }
    }
}

// The halves of a quad go out as one strip of four vertices instead of six:
// TR's geometry is mostly quads, and the vertex buffers and the PowerVR both
// take a third less. Only when no corner needs clipping.
static bool M_TryEmitQuad(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const OUTPUT_MESH_INDEX *const t0, const OUTPUT_MESH_INDEX *const t1)
{
    if (!M_IsQuad(t0)) {
        return false;
    }
    const OUTPUT_MESH_INDEX strip[4] = { t0[0], t0[1], t0[2], t1[2] };
    const M_XVERT *const xv = batcher->xverts;
    const uint8_t clip_and = xv[strip[0]].clip & xv[strip[1]].clip
        & xv[strip[2]].clip & xv[strip[3]].clip;
    if (clip_and != 0) {
        return true;
    }
    if (m_Frame.cel_pass
        && (xv[strip[0]].sargb | xv[strip[1]].sargb | xv[strip[2]].sargb
            | xv[strip[3]].sargb)
            < M_CEL_CUT) {
        return true;
    }
    const uint8_t clip_or = xv[strip[0]].clip | xv[strip[1]].clip
        | xv[strip[2]].clip | xv[strip[3]].clip;
    if ((clip_or & M_CLIP_NEAR) != 0) {
        return false;
    }
    if (m_Frame.cull) {
        const bool back0 = M_IsBackFacing(&xv[t0[0]], &xv[t0[1]], &xv[t0[2]]);
        const bool back1 = M_IsBackFacing(&xv[t1[0]], &xv[t1[1]], &xv[t1[2]]);
        if (back0 && back1) {
            return true;
        }
        if (back0 || back1) {
            // Folded: each half on its own.
            return false;
        }
    }
    // Pulled in about the middle of the whole quad, so that both halves
    // still meet along the diagonal.
    float uv[4][2];
    M_GetFaceUVs(batcher, bind, strip, 4, uv);
    pvr_vertex_t *const out = DC_PVR_ReservePoly(4);
    if (out == nullptr) {
        return true;
    }
    for (int32_t k = 0; k < 4; k++) {
        M_ProjectInto(
            &batcher->xverts[strip[k]], uv[k][0], uv[k][1],
            k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, &out[k]);
    }
    DC_PVR_CommitPoly(4);
    return true;
}

static void M_EmitIndices(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const OUTPUT_MESH_INDEX *const indices, const int32_t count, const DC_BLEND blend)
{
    int32_t page = INT32_MIN;
    int32_t i = 0;
    while (i + 2 < count) {
        const int32_t tri_page =
            m_Frame.env_pass ? DC_PVR_ENV_MAP : bind->page[indices[i]];
        if (tri_page != page) {
            page = tri_page;
            DC_PVR_SetState(
                blend, page == M_NO_PAGE ? DC_PVR_NO_TEXTURE : page, true);
        }
        if (i + 5 < count
            && (m_Frame.env_pass || bind->page[indices[i + 3]] == page)
            && M_TryEmitQuad(batcher, bind, &indices[i], &indices[i + 3])) {
            i += 6;
            continue;
        }
        M_EmitTriangle(batcher, bind, indices[i], indices[i + 1], indices[i + 2]);
        i += 3;
    }
}

M_HOT float M_Dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Draws the faces of the instance just transformed that the silhouette in
// slot `slot` of the shadow map falls on again, with it projected on them
// from light l. The world's faces must also lie past the casters along the
// light and within reach; the casters' own (self-shadowing) lie among them
// anyway. The map is white but for the silhouettes, so each face multiplies
// what is there by 1 where lit and by 1 - strength in the shadow:
// texture * strength + (1 - strength), the vertex colour and the offset
// colour.
static void M_EmitShadowFaces(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const int32_t l, const int32_t slot, const bool world)
{
    const float *const c = m_Soft.center_eye;
    const float(*const axes)[3] = m_Soft.axes_eye[l];
    // Lara on herself darkens as much whatever the light's distance.
    const float strength =
        world ? m_Soft.lights[l].strength : M_SOFT_SELF_STRENGTH;

    // Where each vertex falls on the silhouette's cell (u, v), and how far
    // past the casters along the light; the instance is left alone when it
    // all lies off one side.
    // All three at once through the SH-4's matrix unit (ftrv), laid out by
    // columns as XMTRX takes them.
    const float inv_span = 1.0f / M_SOFT_SPAN;
    matrix_t m = {
        { axes[1][0] * inv_span, axes[2][0] * inv_span, axes[0][0], 0.0f },
        { axes[1][1] * inv_span, axes[2][1] * inv_span, axes[0][1], 0.0f },
        { axes[1][2] * inv_span, axes[2][2] * inv_span, axes[0][2], 0.0f },
        {
            0.5f - M_Dot3(c, axes[1]) * inv_span,
            0.5f - M_Dot3(c, axes[2]) * inv_span,
            -M_Dot3(c, axes[0]),
            1.0f,
        },
    };
    // First the corners of the instance's box, which hold all of it: most
    // instances lie off the cell, and are left before their vertices are.
    mat_load(&m);
    mat_apply(&batcher->mv);
    float lo[3];
    float hi[3];
    for (int32_t i = 0; i < 8; i++) {
        float x = bind->bounds[(i >> 0) & 1][0];
        float y = bind->bounds[(i >> 1) & 1][1];
        float z = bind->bounds[(i >> 2) & 1][2];
        float w = 1.0f;
        mat_trans_nodiv(x, y, z, w);
        const float p[3] = { x, y, z };
        for (int32_t k = 0; k < 3; k++) {
            lo[k] = i == 0 || p[k] < lo[k] ? p[k] : lo[k];
            hi[k] = i == 0 || p[k] > hi[k] ? p[k] : hi[k];
        }
    }
    if (hi[0] < 0.0f || lo[0] > 1.0f || hi[1] < 0.0f || lo[1] > 1.0f
        || (world && (lo[2] > M_SOFT_REACH || hi[2] < -M_SOFT_SPAN * 0.25f))) {
        return;
    }

    mat_load(&m);
    float(*const soft)[3] = batcher->soft;
    for (int32_t i = 0; i < bind->vertex_count; i++) {
        const M_XVERT *const xv = &batcher->xverts[i];
        float x = xv->x;
        float y = xv->y;
        float z = xv->z;
        float w = 1.0f;
        mat_trans_nodiv(x, y, z, w);
        soft[i][0] = x;
        soft[i][1] = y;
        soft[i][2] = z;
    }

    const OUTPUT_MESH *const mesh = bind->mesh;
    const OUTPUT_MESH_INDEX *const indices = Vector_GetData(mesh->opaque_vertex_indices);
    // TR2's rock and ground mostly carry see-through texels, so those faces
    // take the shadow too.
    // shortcut: the shadow also darkens what shows through their holes, and
    // faces crossing the near plane take none; split them out if it shows.
    const int32_t count = mesh->opaque_vertex_indices->count;
    for (int32_t i = 0; i + 2 < count; i += 3) {
        const OUTPUT_MESH_INDEX idx[3] = { indices[i], indices[i + 1], indices[i + 2] };
        const M_XVERT *const xv[3] = {
            &batcher->xverts[idx[0]],
            &batcher->xverts[idx[1]],
            &batcher->xverts[idx[2]],
        };
        const float *const s[3] = { soft[idx[0]], soft[idx[1]], soft[idx[2]] };
        // Faces are tested as a whole: a big one can lie under the casters
        // with every corner far off. Off the silhouette's cell, out of
        // reach, or between the light and the casters:
        bool off = false;
        for (int32_t k = 0; k < 2; k++) {
            off |= (s[0][k] < 0.0f && s[1][k] < 0.0f && s[2][k] < 0.0f)
                || (s[0][k] > 1.0f && s[1][k] > 1.0f && s[2][k] > 1.0f);
        }
        if (world) {
            off |= (s[0][2] > M_SOFT_REACH && s[1][2] > M_SOFT_REACH
                    && s[2][2] > M_SOFT_REACH)
                || (s[0][2] < -M_SOFT_SPAN * 0.25f
                    && s[1][2] < -M_SOFT_SPAN * 0.25f
                    && s[2][2] < -M_SOFT_SPAN * 0.25f);
        }
        if (off || (xv[0]->clip & xv[1]->clip & xv[2]->clip) != 0
            || ((xv[0]->clip | xv[1]->clip | xv[2]->clip) & M_CLIP_NEAR) != 0
            || M_IsBackFacing(xv[0], xv[1], xv[2])) {
            continue;
        }
        // The face's normal, turned towards the camera (at the origin); a
        // face turned away from the light takes no shadow from it.
        const float e1[3] = {
            xv[1]->x - xv[0]->x, xv[1]->y - xv[0]->y, xv[1]->z - xv[0]->z,
        };
        const float e2[3] = {
            xv[2]->x - xv[0]->x, xv[2]->y - xv[0]->y, xv[2]->z - xv[0]->z,
        };
        const float n[3] = {
            e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0],
        };
        const float p0[3] = { xv[0]->x, xv[0]->y, xv[0]->z };
        if ((M_Dot3(n, p0) > 0.0f) == (M_Dot3(n, axes[0]) < 0.0f)) {
            continue;
        }

        DC_PVR_SetState(DC_BLEND_MUL, DC_PVR_SHADOW_MAP, true);
        DC_PVR_VERTEX out[3];
        for (int32_t k = 0; k < 3; k++) {
            const float t = strength
                * M_Clamp(1.0f - s[k][2] * (1.0f / M_SOFT_REACH), 0.0f, 1.0f);
            M_Project(
                xv[k], (slot + s[k][0]) * (1.0f / DC_PVR_SHADOW_MAP_SLOTS),
                s[k][1], &out[k]);
            out[k].argb = M_PackARGB(1.0f, t, t, t);
            out[k].oargb = M_PackARGB(1.0f, 1.0f - t, 1.0f - t, 1.0f - t);
        }
        DC_PVR_Triangle(&out[0], &out[1], &out[2]);
        m_SoftFaces++;
    }
}

static void M_EmitSoftShadows(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind)
{
    for (int32_t l = 0; l < m_Soft.map_count; l++) {
        M_EmitShadowFaces(batcher, bind, l, M_SOFT_LIGHT_SLOT(l), true);
    }
}

// A caster's own share of the nearest light's shadow: the silhouette of the
// casters in the slices nearer the light than its own (see M_SOFT_SLICES).
static void M_EmitSelfShadow(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const int32_t caster)
{
    if (!m_Soft.self || caster >= m_Soft.self_count
        || m_Soft.slice[caster] <= 0) {
        return;
    }
    M_EmitShadowFaces(
        batcher, bind, 0, M_SOFT_SELF_SLOT(m_Soft.slice[caster]), false);
}

// Faces given their relief this frame, for the debug console.
static int32_t m_BumpFaces = 0;

M_HOT uint32_t M_PackUnit(const float x)
{
    return (uint32_t)(M_Clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// The offset colour that lights a face's relief (DC_BLEND_BUMP) from eye
// space point light, or 0 when the face takes none: K1, K2 and K3 so that a
// flat texel keeps the colour (K1 + K2 whole) and a slope turned from the
// light loses up to depth of it, and Q the way the light lies across the
// face, from u towards v.
static uint32_t M_BumpLight(
    const M_BINDING *const bind, const M_XVERT *const xv,
    const OUTPUT_MESH_INDEX *const idx, const float light[3], const float depth)
{
    const M_XVERT *const a = &xv[idx[0]];
    const M_XVERT *const b = &xv[idx[1]];
    const M_XVERT *const c = &xv[idx[2]];
    const float e1[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
    const float e2[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
    const float uv_scale = 1.0f / M_UV_ONE;
    const float u0 = bind->uv[idx[0] * 2] * uv_scale;
    const float v0 = bind->uv[idx[0] * 2 + 1] * uv_scale;
    const float du1 = bind->uv[idx[1] * 2] * uv_scale - u0;
    const float dv1 = bind->uv[idx[1] * 2 + 1] * uv_scale - v0;
    const float du2 = bind->uv[idx[2] * 2] * uv_scale - u0;
    const float dv2 = bind->uv[idx[2] * 2 + 1] * uv_scale - v0;
    const float det = du1 * dv2 - du2 * dv1;
    if (fabsf(det) < 1.0e-9f) {
        return 0;
    }
    // Which way u and v run across the face, and the face's normal, turned
    // towards the camera.
    float t[3];
    float bt[3];
    for (int32_t k = 0; k < 3; k++) {
        t[k] = e1[k] * dv2 - e2[k] * dv1;
        bt[k] = e2[k] * du1 - e1[k] * du2;
    }
    if (det < 0.0f) {
        for (int32_t k = 0; k < 3; k++) {
            t[k] = -t[k];
            bt[k] = -bt[k];
        }
    }
    float n[3] = {
        e1[1] * e2[2] - e1[2] * e2[1],
        e1[2] * e2[0] - e1[0] * e2[2],
        e1[0] * e2[1] - e1[1] * e2[0],
    };
    const float a_pos[3] = { a->x, a->y, a->z };
    if (M_Dot3(n, a_pos) > 0.0f) {
        n[0] = -n[0];
        n[1] = -n[1];
        n[2] = -n[2];
    }
    const float centre[3] = {
        (a->x + b->x + c->x) * (1.0f / 3.0f),
        (a->y + b->y + c->y) * (1.0f / 3.0f),
        (a->z + b->z + c->z) * (1.0f / 3.0f),
    };
    const float l[3] = {
        light[0] - centre[0], light[1] - centre[1], light[2] - centre[2],
    };
    const float tl = M_Dot3(t, t);
    const float bl = M_Dot3(bt, bt);
    const float nl = M_Dot3(n, n);
    const float ll = M_Dot3(l, l);
    if (tl <= 0.0f || bl <= 0.0f || nl <= 0.0f || ll <= 0.0f) {
        return 0;
    }
    const float lx = M_Dot3(l, t) / sqrtf(tl * ll);
    const float ly = M_Dot3(l, bt) / sqrtf(bl * ll);
    const float lz = M_Dot3(l, n) / sqrtf(nl * ll);
    float elevation = asinf(M_Clamp(lz, -1.0f, 1.0f));
    if (elevation < M_BUMP_MIN_ELEVATION) {
        elevation = M_BUMP_MIN_ELEVATION;
    }
    float q = atan2f(ly, lx);
    if (q < 0.0f) {
        q += 2.0f * (float)M_PI;
    }
    const float k2 = depth * sinf(elevation);
    const float k3 = depth * cosf(elevation);
    return (M_PackUnit(1.0f - k2) << 24) | (M_PackUnit(k2) << 16)
        | (M_PackUnit(k3) << 8)
        | ((uint32_t)(q * (256.0f / (2.0f * (float)M_PI))) & 0xFF);
}

// Draws the relief of a room's faces near the camera over them, each lit by
// the light nearest it, the room's or a passing one (or from above, with
// none).
static void M_EmitBump(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const MESH_INSTANCE *const inst)
{
    const float(*const v)[4] = m_Frame.view;
    float lights[M_BUMP_MAX_LIGHTS][3];
    int32_t light_count = 0;
    const ROOM *const room = inst->room;
    for (int32_t i = 0;
         room != nullptr && i < room->num_lights
         && light_count < M_BUMP_MAX_LIGHTS;
         i++) {
        const LIGHT *const light = &room->lights[i];
        if (light->type == LIGHT_TYPE_SUN
            || light->type == LIGHT_TYPE_FOG_BULB) {
            continue;
        }
        const float x = light->pos.x;
        const float y = light->pos.y;
        const float z = light->pos.z;
        for (int32_t r = 0; r < 3; r++) {
            lights[light_count][r] =
                v[0][r] * x + v[1][r] * y + v[2][r] * z + v[3][r];
        }
        light_count++;
    }
    // The passing ones (a muzzle flash, a flare), as the room's lighting has
    // them this frame.
    const DC_UNIFORM_LIGHTS *const passing = m_Frame.lights;
    for (int32_t i = 0; passing != nullptr && i < passing->num_lights
         && light_count < M_BUMP_MAX_LIGHTS;
         i++) {
        const float *const p = passing->lights[i].pos;
        for (int32_t r = 0; r < 3; r++) {
            lights[light_count][r] =
                v[0][r] * p[0] + v[1][r] * p[1] + v[2][r] * p[2] + v[3][r];
        }
        light_count++;
    }

    const M_XVERT *const xv = batcher->xverts;
    const OUTPUT_MESH_INDEX *const indices =
        Vector_GetData(bind->mesh->opaque_vertex_indices);
    const int32_t count = bind->mesh->opaque_vertex_indices->count;
    int32_t page = INT32_MIN;
    m_Frame.bump_pass = true;
    int32_t i = 0;
    while (i + 2 < count) {
        const OUTPUT_MESH_INDEX *const tri = &indices[i];
        const bool quad = i + 5 < count && M_IsQuad(tri)
            && bind->page[tri[3]] == bind->page[tri[0]];
        const int32_t step = quad ? 6 : 3;
        const int32_t tri_page = bind->page[tri[0]];
        const float z_min =
            MIN(xv[tri[0]].z, MIN(xv[tri[1]].z, xv[tri[2]].z));
        const float fade =
            M_Clamp((M_BUMP_RANGE - z_min) * (1.0f / M_BUMP_FADE), 0.0f, 1.0f);
        if (tri_page == M_NO_PAGE || fade <= 0.0f
            || (xv[tri[0]].clip & xv[tri[1]].clip & xv[tri[2]].clip) != 0) {
            i += step;
            continue;
        }
        // The room's light nearest the face, or one high above it.
        float light[3] = {};
        const M_XVERT *const a = &xv[tri[0]];
        float best = -1.0f;
        for (int32_t l = 0; l < light_count; l++) {
            const float d[3] = {
                lights[l][0] - a->x, lights[l][1] - a->y, lights[l][2] - a->z,
            };
            const float dist_sq = M_Dot3(d, d);
            if (best < 0.0f || dist_sq < best) {
                best = dist_sq;
                memcpy(light, lights[l], sizeof(light));
            }
        }
        if (best < 0.0f) {
            // TR's y axis points down.
            for (int32_t r = 0; r < 3; r++) {
                light[r] = (&a->x)[r] - v[1][r] * (8.0f * WALL_L);
            }
        }
        m_Frame.bump_k = M_BumpLight(bind, xv, tri, light, M_BUMP_DEPTH * fade);
        if (m_Frame.bump_k == 0) {
            i += step;
            continue;
        }
        if (tri_page != page) {
            page = tri_page;
            DC_PVR_SetState(DC_BLEND_BUMP, page, true);
        }
        if (!quad || !M_TryEmitQuad(batcher, bind, tri, tri + 3)) {
            M_EmitTriangle(batcher, bind, tri[0], tri[1], tri[2]);
            if (quad) {
                M_EmitTriangle(batcher, bind, tri[3], tri[4], tri[5]);
            }
        }
        m_BumpFaces++;
        i += step;
    }
    m_Frame.bump_pass = false;
}

static void M_DrawInstance(
    MESH_BATCHER *const batcher, const MESH_INSTANCE *const inst,
    const SCENE_PASS pass, const int16_t caster)
{
    M_BINDING *const bind = M_GetBinding(batcher, inst->mesh);
    if (bind == nullptr || bind->vertex_count == 0) {
        return;
    }
    // Every room is staged for the added faces too; most have none.
    const bool blended =
        pass == SCENE_PASS_BLEND_ADD || pass == SCENE_PASS_BLEND_SUB;
    if (blended && inst->mesh->blend_add_vertex_indices->count == 0) {
        return;
    }
#if defined(TRX_DC_WATCHDOG)
    const uint64_t xform_start = timer_us_gettime64();
    const bool transformed = M_TransformInstance(batcher, inst, bind);
    g_DC_BatcherXformUs += (uint32_t)(timer_us_gettime64() - xform_start);
    if (!transformed) {
        return;
    }
#else
    if (!M_TransformInstance(batcher, inst, bind)) {
        return;
    }
#endif
    const bool is_caster = caster >= 0;
    // Boxes make a rough shadow; on the caster itself it would only look
    // wrong.
    DC_PVR_SetShadowReceiver(
        !is_caster && M_ShadowType() == SHADOW_TYPE_VOLUME);
    if (m_Soft.active && pass == SCENE_PASS_OPAQUE) {
        if (is_caster) {
            M_EmitSelfShadow(batcher, bind, caster);
        } else {
            M_EmitSoftShadows(batcher, bind);
        }
    }

    const OUTPUT_MESH *const mesh = inst->mesh;
    if (pass == SCENE_PASS_BLEND_ADD || pass == SCENE_PASS_BLEND_SUB) {
        M_EmitIndices(
            batcher, bind, Vector_GetData(mesh->blend_add_vertex_indices),
            mesh->blend_add_vertex_indices->count,
            pass == SCENE_PASS_BLEND_ADD ? DC_BLEND_ADD : DC_BLEND_SUB);
        return;
    }

    // An instance faded out as a whole draws every face blended.
    const bool faded = inst->tint.a < 1.0f;
    const OUTPUT_MESH_INDEX *const opaque = Vector_GetData(mesh->opaque_vertex_indices);
    const int32_t clean = mesh->opaque_nodiscard_count;
    M_EmitIndices(
        batcher, bind, opaque, clean, faded ? DC_BLEND_ALPHA : DC_BLEND_OPAQUE);
    M_EmitIndices(
        batcher, bind, opaque + clean,
        mesh->opaque_vertex_indices->count - clean,
        faded ? DC_BLEND_ALPHA : DC_BLEND_PUNCH);
    if (batcher->cel && !faded) {
        // The shaded side, over the faces just drawn.
        m_Frame.cel_pass = true;
        M_EmitIndices(
            batcher, bind, opaque, mesh->opaque_vertex_indices->count,
            DC_BLEND_PUNCH_SHADE);
        m_Frame.cel_pass = false;
    }
    m_FrameRooms |= bind->lit != nullptr;
    if (m_Frame.bump && m_Frame.lighting && !faded && bind->lit != nullptr) {
        M_EmitBump(batcher, bind, inst);
    }
    if (batcher->env && !faded) {
        // The reflections, added over the faces just drawn.
        m_Frame.env_pass = true;
        M_EmitIndices(batcher, bind, bind->env, bind->env_count, DC_BLEND_ADD);
        m_Frame.env_pass = false;
    }
    for (int32_t i = 0; i < mesh->transparent_faces->count; i++) {
        const OUTPUT_MESH_FACE *const face =
            Vector_Get(mesh->transparent_faces, i);
        M_EmitIndices(
            batcher, bind, (const OUTPUT_MESH_INDEX *)face->vertex_indices,
            face->vertex_count, DC_BLEND_ALPHA);
    }
}

static void M_PrepareFrame(void)
{
    m_Frame.screen_scale = DC_PVR_GetScreenScale();
    const DC_UNIFORM_MATRICES *const matrices =
        DC_GL_GetUniformBlock(DC_GL_BINDING_MATRICES, nullptr);
    const DC_UNIFORM_GENERAL *const general =
        DC_GL_GetUniformBlock(DC_GL_BINDING_GENERAL, nullptr);
    if (matrices == nullptr || general == nullptr) {
        return;
    }
    memcpy(m_Frame.view, matrices->mat_view, sizeof(m_Frame.view));

    const VIEWPORT_RECT rect = Viewport_GetRect(VIEWPORT_GAME);
    const float half_w = rect.width * 0.5f;
    const float half_h = rect.height * 0.5f;
    m_Frame.center_x = rect.x + half_w;
    m_Frame.center_y = rect.y + half_h;
    m_Frame.proj_x = matrices->mat_proj[0][0] * half_w;
    m_Frame.proj_y = -matrices->mat_proj[1][1] * half_h;
    m_Frame.near_z = Output_GetNearZ() / (float)(1 << W2V_SHIFT);
    if (m_Frame.near_z < 1.0f) {
        m_Frame.near_z = 1.0f;
    }
    m_Frame.half_w = half_w;
    m_Frame.half_h = half_h;
    m_Frame.ortho = matrices->mat_proj[3][3] == 1.0f;
    if (m_Frame.ortho) {
        for (int32_t i = 0; i < 3; i++) {
            m_Frame.ortho_scale[i] = matrices->mat_proj[i][i];
            m_Frame.ortho_offset[i] = matrices->mat_proj[3][i];
        }
        // Nothing to clip: every vertex is in front.
        m_Frame.near_z = -1.0e30f;
    }

    m_Frame.lighting = general->lighting_enabled != 0;
    m_Frame.cel = M_CelOn();
    m_Frame.bump = DC_Mode_Get()->bump_mapping && m_BumpOn && !m_Frame.ortho;
    m_Frame.min_shade = general->min_shade;
    m_Frame.time_in_game = general->time_in_game;
    m_Frame.sunset_duration = general->sunset_duration;
    const float fog_start = general->fog_distance[0];
    const float fog_end = general->fog_distance[1];
    m_Frame.fog = m_Frame.lighting && fog_end > fog_start;
    m_Frame.fog_start = fog_start;
    m_Frame.fog_end = fog_end;
    m_Frame.fog_scale = m_Frame.fog ? 1.0f / (fog_end - fog_start) : 0.0f;
    m_Frame.fog_color[0] = general->fog_color[0];
    m_Frame.fog_color[1] = general->fog_color[1];
    m_Frame.fog_color[2] = general->fog_color[2];
    m_Frame.lights = DC_GL_GetUniformBlock(DC_GL_BINDING_LIGHTS, nullptr);
}

static void M_RenderBegin(const SCENE_SOURCE *const source)
{
    MESH_BATCHER *const batcher = source->priv;
    for (int32_t pass = 0; pass < SCENE_PASS_COUNT; pass++) {
        Vector_Clear(batcher->staged[pass]);
        Vector_Clear(batcher->casters[pass]);
    }
}

static void M_Transform34(
    const float m[3][4], const float in[3], float out[3])
{
    for (int32_t r = 0; r < 3; r++) {
        out[r] = m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2] + m[r][3];
    }
}

// The middle of the casters' bounding boxes, in world space.
static void M_CastersCenter(
    const M_CASTER *const casters, const int32_t count, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    for (int32_t i = 0; i < count; i++) {
        float local[3];
        for (int32_t k = 0; k < 3; k++) {
            local[k] = (casters[i].bounds[0][k] + casters[i].bounds[1][k]) * 0.5f;
        }
        float world[3];
        M_Transform34(casters[i].world, local, world);
        for (int32_t k = 0; k < 3; k++) {
            out[k] += world[k] / count;
        }
    }
}

// Takes the shadow map's lights into this frame's eye space, about where the
// casters are now: the map was drawn from the last frame's casters, and
// follows them this way as they move.
static void M_PrepareSoft(void)
{
    m_Soft.active = M_ShadowType() == SHADOW_TYPE_SOFT
        && !m_Frame.ortho && m_CasterCount > 0 && m_Soft.map_count > 0;
    if (!m_Soft.active) {
        return;
    }
    const float(*const v)[4] = m_Frame.view;
    float center[3];
    M_CastersCenter(m_Casters, m_CasterCount, center);
    for (int32_t r = 0; r < 3; r++) {
        m_Soft.center_eye[r] = v[0][r] * center[0] + v[1][r] * center[1]
            + v[2][r] * center[2] + v[3][r];
    }
    for (int32_t l = 0; l < m_Soft.map_count; l++) {
        const M_SOFT_LIGHT *const light = &m_Soft.lights[l];
        const float *const axes[3] = { light->dir, light->right, light->up };
        for (int32_t a = 0; a < 3; a++) {
            float *const out = m_Soft.axes_eye[l][a];
            for (int32_t r = 0; r < 3; r++) {
                out[r] = v[0][r] * axes[a][0] + v[1][r] * axes[a][1]
                    + v[2][r] * axes[a][2];
            }
            const float len_sq = M_Dot3(out, out);
            for (int32_t r = 0; r < 3; r++) {
                out[r] /= len_sq;
            }
        }
    }
}

static void M_RenderPass(const SCENE_SOURCE *const source, const SCENE_PASS pass)
{
    MESH_BATCHER *const batcher = source->priv;
    if (pass != SCENE_PASS_OPAQUE && pass != SCENE_PASS_BLEND_ADD
        && pass != SCENE_PASS_BLEND_SUB) {
        return;
    }
    M_PrepareFrame();
    if (pass == SCENE_PASS_OPAQUE) {
        M_PrepareSoft();
    } else {
        m_Soft.active = false;
    }
    m_Frame.cull = !m_Frame.ortho;
    m_Frame.inset_uv =
        g_Config.rendering.texture_filter == TEXTURE_FILTER_BILINEAR
        && DC_Mode_Get()->bilinear;
    DC_PVR_SetFilter(m_Frame.inset_uv);
    VECTOR *const staged = batcher->staged[pass];
    const int16_t *const casters = Vector_GetData(batcher->casters[pass]);
    for (int32_t i = 0; i < staged->count; i++) {
        M_DrawInstance(batcher, Vector_Get(staged, i), pass, casters[i]);
    }
    DC_PVR_SetShadowReceiver(false);
}

static bool M_IsDirty(const SCENE_SOURCE *const source, const SCENE_PASS pass)
{
    const MESH_BATCHER *const batcher = source->priv;
    return batcher->staged[pass]->count > 0;
}

static void M_AnimateTextures(const SCENE_SOURCE *const source)
{
    MESH_BATCHER *const batcher = source->priv;
    for (int32_t i = 0; i < batcher->bindings->count; i++) {
        M_BINDING *const bind = *(M_BINDING **)Vector_Get(batcher->bindings, i);
        const VECTOR *const ranges = bind->mesh->animated_vertices;
        for (int32_t j = 0; j < ranges->count; j++) {
            const OUTPUT_VERTEX_RANGE *const range = Vector_Get(ranges, j);
            for (int32_t k = range->vertex_start;
                 k < range->vertex_start + range->vertex_count; k++) {
                M_FillTexture(bind, k);
            }
        }
    }
}

#define M_RESERVED_BINDINGS 256
// Venice stages more than 64 in a pass; growing there mid-level ran out of
// memory.
#define M_RESERVED_INSTANCES 128
#define M_RESERVED_VERTICES 1024

MESH_BATCHER *MeshBatcher_Create(void)
{
    // Made big enough for a level as the game starts: grown as a level is
    // played, these would stay in the middle of the memory the next level
    // loads into, and split it.
    MESH_BATCHER *const batcher = Memory_Alloc(sizeof(MESH_BATCHER));
    batcher->bindings =
        Vector_CreateAtCapacity(sizeof(M_BINDING *), M_RESERVED_BINDINGS);
    for (int32_t pass = 0; pass < SCENE_PASS_COUNT; pass++) {
        const int32_t capacity = pass == SCENE_PASS_OPAQUE
                || pass == SCENE_PASS_TRANSPARENT
                || pass == SCENE_PASS_BLEND_ADD
            ? M_RESERVED_INSTANCES
            : M_RESERVED_INSTANCES / 4;
        batcher->staged[pass] =
            Vector_CreateAtCapacity(sizeof(MESH_INSTANCE), capacity);
        batcher->casters[pass] =
            Vector_CreateAtCapacity(sizeof(int16_t), capacity);
    }
    batcher->xverts_capacity = M_RESERVED_VERTICES;
    batcher->xverts = memalign(32, M_RESERVED_VERTICES * sizeof(M_XVERT));
    batcher->soft = Memory_Alloc(M_RESERVED_VERTICES * sizeof(*batcher->soft));
    batcher->source = (SCENE_SOURCE) {
        .render_begin = M_RenderBegin,
        .render_pass = M_RenderPass,
        .is_dirty = M_IsDirty,
        .animate_textures = M_AnimateTextures,
        .priv = batcher,
    };
    return batcher;
}

void MeshBatcher_Destroy(MESH_BATCHER *const batcher)
{
    if (batcher == nullptr) {
        return;
    }
    while (batcher->bindings->count > 0) {
        M_BINDING *const bind = *(M_BINDING **)Vector_Get(batcher->bindings, 0);
        MeshBatcher_RemoveMesh(batcher, bind->mesh);
    }
    Vector_Free(batcher->bindings);
    for (int32_t pass = 0; pass < SCENE_PASS_COUNT; pass++) {
        Vector_Free(batcher->staged[pass]);
        Vector_Free(batcher->casters[pass]);
    }
    Memory_Free(batcher->xverts);
    Memory_Free(batcher->soft);
    Memory_Free(batcher);
}

static bool M_IsReflective(
    const OUTPUT_MESH_VERTEX *const vertices, const OUTPUT_MESH_INDEX *const tri)
{
    return (vertices[tri[0]].flags & vertices[tri[1]].flags
            & vertices[tri[2]].flags & VERT_REFLECTIVE)
        != 0;
}

// Gathers the opaque triangles of a mesh that reflect, in order, so that the
// halves of a quad stay paired.
static void M_FindReflections(M_BINDING *const bind)
{
    const OUTPUT_MESH *const mesh = bind->mesh;
    const OUTPUT_MESH_VERTEX *const vertices = Vector_GetData(mesh->vertices);
    const OUTPUT_MESH_INDEX *const indices = Vector_GetData(mesh->opaque_vertex_indices);
    const int32_t count = mesh->opaque_vertex_indices->count;
    int32_t found = 0;
    for (int32_t i = 0; i + 2 < count; i += 3) {
        found += M_IsReflective(vertices, &indices[i]) ? 3 : 0;
    }
    if (found == 0) {
        return;
    }
    bind->env = Memory_Alloc(found * sizeof(*bind->env));
    for (int32_t i = 0; i + 2 < count; i += 3) {
        if (M_IsReflective(vertices, &indices[i])) {
            memcpy(&bind->env[bind->env_count], &indices[i], 3 * sizeof(*bind->env));
            bind->env_count += 3;
        }
    }
}

void MeshBatcher_AddMesh(MESH_BATCHER *const batcher, OUTPUT_MESH *const mesh)
{
    ASSERT(mesh->sealed == 1);
    // The mesh grew by doubling as it was built; it is kept for the level's
    // life, so the room it does not use goes back.
    Vector_ShrinkToFit(mesh->vertices);
    Vector_ShrinkToFit(mesh->opaque_vertex_indices);
    Vector_ShrinkToFit(mesh->blend_add_vertex_indices);
    Vector_ShrinkToFit(mesh->transparent_faces);
    Vector_ShrinkToFit(mesh->animated_vertices);
    // Each run of indices is drawn on its own (see M_DrawInstance).
    OUTPUT_MESH_INDEX *const opaque = Vector_GetData(mesh->opaque_vertex_indices);
    const int32_t clean = mesh->opaque_nodiscard_count;
    M_PairQuads(opaque, clean);
    M_PairQuads(opaque + clean, mesh->opaque_vertex_indices->count - clean);
    M_PairQuads(
        Vector_GetData(mesh->blend_add_vertex_indices),
        mesh->blend_add_vertex_indices->count);
    for (int32_t i = 0; i < mesh->transparent_faces->count; i++) {
        OUTPUT_MESH_FACE *const face = Vector_Get(mesh->transparent_faces, i);
        M_PairQuads((OUTPUT_MESH_INDEX *)face->vertex_indices, face->vertex_count);
    }
    M_BINDING *const bind = Memory_Alloc(sizeof(M_BINDING));
    bind->mesh = mesh;
    bind->vertex_count = mesh->vertices->count;
    bind->uv = Memory_Alloc(sizeof(uint16_t) * 2 * (bind->vertex_count + 1));
    bind->page = Memory_Alloc(bind->vertex_count + 1);
    const OUTPUT_MESH_VERTEX *const vertices = Vector_GetData(mesh->vertices);
    bind->plain_object = bind->vertex_count > 0;
    for (int32_t i = 0; i < bind->vertex_count; i++) {
        M_FillTexture(bind, i);
        const float pos[3] = {
            vertices[i].pos.x, vertices[i].pos.y, vertices[i].pos.z,
        };
        for (int32_t k = 0; k < 3; k++) {
            if (i == 0 || pos[k] < bind->bounds[0][k]) {
                bind->bounds[0][k] = pos[k];
            }
            if (i == 0 || pos[k] > bind->bounds[1][k]) {
                bind->bounds[1][k] = pos[k];
            }
        }
        const uint16_t flags = vertices[i].flags;
        bind->needs_room_lights |= (flags & VERT_USE_DYNAMIC_LIGHT) != 0;
        bind->needs_object_light |= (flags & VERT_USE_OBJECT_LIGHT) != 0;
        bind->needs_own_light |= (flags & VERT_USE_OWN_LIGHT) != 0;
        bind->billboard |= (flags & (VERT_BILLBOARD | VERT_ABS_SPRITE)) != 0;
        bind->plain_object &= (flags
                                  & (VERT_USE_OBJECT_LIGHT | VERT_NO_LIGHTING
                                     | VERT_USE_OWN_LIGHT
                                     | VERT_USE_DYNAMIC_LIGHT | VERT_BILLBOARD
                                     | VERT_ABS_SPRITE))
            == VERT_USE_OBJECT_LIGHT;
    }
    if (bind->needs_room_lights) {
        bind->lit = Memory_Alloc(sizeof(uint32_t) * bind->vertex_count);
    }
    M_FindReflections(bind);
    mesh->sealed = 2;
    g_DC_BatcherMeshCount++;
    g_DC_BatcherVertexCount += bind->vertex_count;
    Vector_Add(batcher->bindings, &bind);
    HASH_ADD_PTR(batcher->map, mesh, bind);
}

void MeshBatcher_RemoveMesh(MESH_BATCHER *const batcher, OUTPUT_MESH *const mesh)
{
    M_BINDING *const bind = M_GetBinding(batcher, mesh);
    if (bind == nullptr) {
        return;
    }
    // The kept casters may not outlive their meshes.
    for (int32_t i = 0; i < m_Soft.caster_count; i++) {
        if (m_Soft.casters[i].bind == bind) {
            m_Soft.caster_count = 0;
        }
    }
    g_DC_BatcherMeshCount--;
    g_DC_BatcherVertexCount -= bind->vertex_count;
    Memory_Free(bind->uv);
    Memory_Free(bind->page);
    Memory_Free(bind->lit);
    Memory_Free(bind->env);
    Vector_Remove(batcher->bindings, &bind);
    HASH_DEL(batcher->map, bind);
    Memory_Free(bind);
}

void MeshBatcher_Seal(MESH_BATCHER *const batcher)
{
    int32_t vertices = 0;
    for (int32_t i = 0; i < batcher->bindings->count; i++) {
        const M_BINDING *const bind =
            *(M_BINDING **)Vector_Get(batcher->bindings, i);
        vertices += bind->vertex_count;
    }
    LOG_INFO(
        "%d meshes, %d vertices batched", batcher->bindings->count, vertices);
}

void MeshBatcher_UpdateMeshGeometry(
    const MESH_BATCHER *const batcher, const OUTPUT_MESH *const mesh)
{
    // The vertices are read from the mesh as each frame is drawn.
}

void MeshBatcher_Stage(
    MESH_BATCHER *const batcher, const MESH_INSTANCE *const inst,
    const SCENE_PASS pass)
{
    if (inst->mesh == nullptr) {
        return;
    }
    // Only where M_RenderPass will draw it: an instance is some 400 bytes,
    // copied once a pass. The opaque pass draws the transparent faces too,
    // and the blended ones only the added faces.
    const bool blended =
        pass == SCENE_PASS_BLEND_ADD || pass == SCENE_PASS_BLEND_SUB;
    if ((pass != SCENE_PASS_OPAQUE && !blended)
        || (blended && inst->mesh->blend_add_vertex_indices->count == 0)) {
        return;
    }
    int16_t caster_index = -1;
    if (m_Casting && pass == SCENE_PASS_OPAQUE
        && m_CasterCount < M_MAX_CASTERS) {
        const M_BINDING *const bind = M_GetBinding(batcher, inst->mesh);
        if (bind != nullptr && bind->vertex_count > 0) {
            M_CASTER *const caster = &m_Casters[m_CasterCount++];
            caster->bind = bind;
            memcpy(caster->bounds, bind->bounds, sizeof(caster->bounds));
            M_WorldMatrix(&inst->wmatrix, caster->world);
            caster->room = inst->room;
            caster_index = (int16_t)(m_CasterCount - 1);
        }
    }
    Vector_Add(batcher->staged[pass], inst);
    Vector_Add(batcher->casters[pass], &caster_index);
}

void DC_Shadow_SetCasting(const bool casting)
{
    m_Casting = casting;
}

bool DC_Shadow_IsCasting(void)
{
    return m_Casting;
}

// A light the casters' shadows fall away from.
typedef struct {
    // From the light towards the casters' middle.
    float dir[3];
    float dist_sq;
    // 1 at the light, 0 at the end of its reach.
    float weight;
} M_SHADOW_LIGHT;

// The cel shading's shade by the nearest light's weight (M_SHADOW_LIGHT).
static void M_SetCelShade(const float weight)
{
    m_CelShade = M_CEL_SHADE_FAR + (M_CEL_SHADE_NEAR - M_CEL_SHADE_FAR) * weight;
}

// Adds a light to the list, kept nearest first and at most max long, if it
// reaches the casters' middle and lies within M_SHADOW_LIGHT_RANGE of it.
static void M_ConsiderLight(
    const float center[3], const float x, const float y, const float z,
    float reach, M_SHADOW_LIGHT *const list, int32_t *const count,
    const int32_t max)
{
    if (reach > M_SHADOW_LIGHT_RANGE) {
        reach = M_SHADOW_LIGHT_RANGE;
    }
    const float d[3] = { center[0] - x, center[1] - y, center[2] - z };
    const float dist_sq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (dist_sq < 1.0f || dist_sq > reach * reach) {
        return;
    }
    int32_t i = *count;
    if (i == max) {
        if (list[max - 1].dist_sq <= dist_sq) {
            return;
        }
        i = max - 1;
    } else {
        (*count)++;
    }
    for (; i > 0 && list[i - 1].dist_sq > dist_sq; i--) {
        list[i] = list[i - 1];
    }
    const float dist = sqrtf(dist_sq);
    list[i] = (M_SHADOW_LIGHT) {
        .dir = { d[0] / dist, d[1] / dist, d[2] / dist },
        .dist_sq = dist_sq,
        .weight = 1.0f - dist / reach,
    };
}

// Gathers the up to max lights nearest the casters' middle within reach, the
// room's own and passing ones (a muzzle flash, a flare), each tilted no
// further off the vertical than M_SHADOW_MIN_DROP allows. Returns how many.
static int32_t M_GatherShadowLights(
    const M_CASTER *const casters, const float center[3],
    M_SHADOW_LIGHT *const out, const int32_t max)
{
    int32_t count = 0;
    const ROOM *const room = casters[0].room;
    if (room != nullptr) {
        for (int32_t i = 0; i < room->num_lights; i++) {
            const LIGHT *const light = &room->lights[i];
            if (light->type == LIGHT_TYPE_SUN
                || light->type == LIGHT_TYPE_FOG_BULB) {
                continue;
            }
            const float reach = light->layout == LIGHT_LAYOUT_TR4
                ? (float)light->u.tr4.outer_radius
                : (float)light->u.legacy.falloff.value_1;
            M_ConsiderLight(
                center, light->pos.x, light->pos.y, light->pos.z, reach, out,
                &count, max);
        }
    }

    // The passing lights, as the shading has them this frame.
    Output_Uniforms_UploadRoomLights(Output_GetUniforms(), room);
    const DC_UNIFORM_LIGHTS *const lights =
        DC_GL_GetUniformBlock(DC_GL_BINDING_LIGHTS, nullptr);
    m_Frame.lights = lights;
    for (int32_t i = 0; lights != nullptr && i < lights->num_lights; i++) {
        const DC_UNIFORM_LIGHT *const light = &lights->lights[i];
        const float reach = light->kind == 0.0f
            ? exp2f(light->falloff)
            : light->falloff / (float)(1 << (OUTPUT_DYNAMIC_FALLOFF_SHIFT
                                             - OUTPUT_DYNAMIC_RADIUS_SHIFT));
        M_ConsiderLight(
            center, light->pos[0], light->pos[1], light->pos[2], reach, out,
            &count, max);
    }
    for (int32_t i = 0; i < count; i++) {
        // TR's y axis points down.
        float *const dir = out[i].dir;
        if (dir[1] < M_SHADOW_MIN_DROP) {
            dir[1] = M_SHADOW_MIN_DROP;
            const float inv = 1.0f / sqrtf(M_Dot3(dir, dir));
            dir[0] *= inv;
            dir[1] *= inv;
            dir[2] *= inv;
        }
    }
    return count;
}

// Projects a world point; false when it lies before the near plane.
static bool M_ProjectWorld(const float p[3], float out[3])
{
    const float(*const v)[4] = m_Frame.view;
    const float x = v[0][0] * p[0] + v[1][0] * p[1] + v[2][0] * p[2] + v[3][0];
    const float y = v[0][1] * p[0] + v[1][1] * p[1] + v[2][1] * p[2] + v[3][1];
    const float z = v[0][2] * p[0] + v[1][2] * p[1] + v[2][2] * p[2] + v[3][2];
    if (z < m_Frame.near_z) {
        return false;
    }
    const float inv_z = 1.0f / z;
    out[0] = m_Frame.center_x + m_Frame.proj_x * x * inv_z;
    out[1] = m_Frame.center_y + m_Frame.proj_y * y * inv_z;
    out[2] = DC_PVR_Depth3D(m_Frame.near_z * inv_z);
    return true;
}

// A box's faces by corner (bit 0: x, bit 1: y, bit 2: z; set for the maximum).
static const int8_t m_BoxFaces[6][4] = {
    { 0, 2, 6, 4 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 },
    { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 5, 7, 6 },
};

// The volume a box throws: the faces towards the light where they are, the
// ones away from it pushed along the shadow, and the box's outline joining
// the two, which closes it.
static int32_t M_BuildVolume(
    const M_CASTER *const caster, const float dir[3],
    DC_PVR_TRIANGLE *const out)
{
    float near[8][3];
    float far[8][3];
    float corners[8][3];
    float center[3] = {};
    for (int32_t i = 0; i < 8; i++) {
        const float local[3] = {
            caster->bounds[(i >> 0) & 1][0],
            caster->bounds[(i >> 1) & 1][1],
            caster->bounds[(i >> 2) & 1][2],
        };
        M_Transform34(caster->world, local, corners[i]);
        for (int32_t k = 0; k < 3; k++) {
            center[k] += corners[i][k] / 8.0f;
        }
    }

    // A shadow falling towards the camera is cut short of the near plane: a
    // volume crossing it would not be closed.
    const float(*const v)[4] = m_Frame.view;
    const float dir_z = v[0][2] * dir[0] + v[1][2] * dir[1] + v[2][2] * dir[2];
    float length = M_SHADOW_LENGTH;
    for (int32_t i = 0; i < 8; i++) {
        const float *const p = corners[i];
        const float z =
            v[0][2] * p[0] + v[1][2] * p[1] + v[2][2] * p[2] + v[3][2];
        const float margin = m_Frame.near_z * 1.5f;
        if (z < margin) {
            return 0;
        }
        if (dir_z < 0.0f && z + dir_z * length < margin) {
            length = (margin - z) / dir_z;
        }
    }
    if (length < 32.0f) {
        return 0;
    }
    for (int32_t i = 0; i < 8; i++) {
        float pushed[3];
        for (int32_t k = 0; k < 3; k++) {
            pushed[k] = corners[i][k] + dir[k] * length;
        }
        if (!M_ProjectWorld(corners[i], near[i])
            || !M_ProjectWorld(pushed, far[i])) {
            return 0;
        }
    }

    bool lit[6];
    for (int32_t f = 0; f < 6; f++) {
        // The face's outward direction, from the box's middle to the face's.
        const int8_t *const q = m_BoxFaces[f];
        float world[4][3];
        float face_center[3] = {};
        for (int32_t j = 0; j < 4; j++) {
            const float local[3] = {
                caster->bounds[(q[j] >> 0) & 1][0],
                caster->bounds[(q[j] >> 1) & 1][1],
                caster->bounds[(q[j] >> 2) & 1][2],
            };
            M_Transform34(caster->world, local, world[j]);
            for (int32_t k = 0; k < 3; k++) {
                face_center[k] += world[j][k] / 4.0f;
            }
        }
        const float n_dot_d = (face_center[0] - center[0]) * dir[0]
            + (face_center[1] - center[1]) * dir[1]
            + (face_center[2] - center[2]) * dir[2];
        lit[f] = n_dot_d < 0.0f;
    }

    int32_t count = 0;
    for (int32_t f = 0; f < 6; f++) {
        const int8_t *const q = m_BoxFaces[f];
        float(*const cap)[3] = lit[f] ? near : far;
        const int8_t tris[2][3] = { { q[0], q[1], q[2] }, { q[0], q[2], q[3] } };
        for (int32_t t = 0; t < 2; t++) {
            for (int32_t j = 0; j < 3; j++) {
                memcpy(out[count].v[j], cap[tris[t][j]], sizeof(float) * 3);
            }
            count++;
        }
    }
    // The outline: edges between a face towards the light and one away.
    for (int32_t f = 0; f < 6; f++) {
        for (int32_t g = f + 1; g < 6; g++) {
            if (lit[f] == lit[g]) {
                continue;
            }
            int8_t shared[2];
            int32_t num_shared = 0;
            for (int32_t i = 0; i < 4; i++) {
                for (int32_t j = 0; j < 4; j++) {
                    if (m_BoxFaces[f][i] == m_BoxFaces[g][j]
                        && num_shared < 2) {
                        shared[num_shared++] = m_BoxFaces[f][i];
                    }
                }
            }
            if (num_shared != 2) {
                continue;
            }
            const int8_t a = shared[0];
            const int8_t b = shared[1];
            memcpy(out[count].v[0], near[a], sizeof(float) * 3);
            memcpy(out[count].v[1], near[b], sizeof(float) * 3);
            memcpy(out[count].v[2], far[b], sizeof(float) * 3);
            count++;
            memcpy(out[count].v[0], near[a], sizeof(float) * 3);
            memcpy(out[count].v[1], far[b], sizeof(float) * 3);
            memcpy(out[count].v[2], far[a], sizeof(float) * 3);
            count++;
        }
    }
    return count;
}

// Keeps this frame's casters and picks the lights for the next frame's
// shadow map.
static void M_KeepSoftCasters(const float center[3])
{
    memcpy(m_Soft.casters, m_Casters, sizeof(M_CASTER) * m_CasterCount);
    m_Soft.caster_count = m_CasterCount;
    memcpy(m_Soft.center, center, sizeof(m_Soft.center));

    M_SHADOW_LIGHT lights[M_SOFT_MAX_LIGHTS];
    // As many lights as the mode allows.
    const int32_t max = MIN(DC_Mode_Get()->shadow_lights, M_SOFT_MAX_LIGHTS);
    int32_t count = M_GatherShadowLights(m_Casters, center, lights, max);
    if (count == 0) {
        // No light near: a faint shadow straight down.
        lights[0] = (M_SHADOW_LIGHT) {
            .dir = { 0.0f, 1.0f, 0.0f },
            .weight = M_SOFT_NO_LIGHT_WEIGHT,
        };
        count = 1;
    }
    m_Soft.light_count = count;
    M_SetCelShade(lights[0].weight);
    for (int32_t l = 0; l < count; l++) {
        M_SOFT_LIGHT *const out = &m_Soft.lights[l];
        const float *const d = lights[l].dir;
        memcpy(out->dir, d, sizeof(out->dir));
        // Any direction across the light will do, as long as the map and the
        // projection agree.
        const float helper[3] = {
            fabsf(d[1]) < 0.9f ? 0.0f : 1.0f, fabsf(d[1]) < 0.9f ? 1.0f : 0.0f,
            0.0f,
        };
        float right[3] = {
            d[1] * helper[2] - d[2] * helper[1],
            d[2] * helper[0] - d[0] * helper[2],
            d[0] * helper[1] - d[1] * helper[0],
        };
        const float inv = 1.0f / sqrtf(M_Dot3(right, right));
        for (int32_t k = 0; k < 3; k++) {
            out->right[k] = right[k] * inv;
        }
        const float *const r = out->right;
        out->up[0] = d[1] * r[2] - d[2] * r[1];
        out->up[1] = d[2] * r[0] - d[0] * r[2];
        out->up[2] = d[0] * r[1] - d[1] * r[0];
        out->strength = M_SOFT_STRENGTH_MIN
            + (M_SOFT_STRENGTH_MAX - M_SOFT_STRENGTH_MIN) * lights[l].weight;
    }

    // The slices for self-shadowing, by the order of the casters' middles
    // along the nearest light rather than by equal distances: lit from above,
    // a third of Lara's height holds her head, shoulders and torso, which
    // would then never shadow one another. The nearest part alone (the head,
    // or a hand reaching above it) comes first, so that the torso takes its
    // shadow; the next two fifths (the torso, the arms) after.
    float along[M_MAX_CASTERS];
    for (int32_t i = 0; i < m_CasterCount; i++) {
        float middle[3];
        M_CastersCenter(&m_Casters[i], 1, middle);
        const float d[3] = {
            middle[0] - center[0], middle[1] - center[1], middle[2] - center[2],
        };
        along[i] = M_Dot3(d, m_Soft.lights[0].dir);
    }
    for (int32_t i = 0; i < m_CasterCount; i++) {
        int32_t rank = 0;
        for (int32_t j = 0; j < m_CasterCount; j++) {
            rank += along[j] < along[i] || (along[j] == along[i] && j < i);
        }
        m_Soft.slice[i] = rank == 0 ? 0 : rank * 5 < 2 * m_CasterCount ? 1 : 2;
    }
#if defined(TRX_DC_WATCHDOG)
    static int32_t frame = 0;
    if (++frame % 120 == 0) {
        LOG_DEBUG(
            "soft shadow: %d casters, %d lights, %d faces, first dir %.2f "
            "%.2f %.2f strength %.2f, relief %d",
            m_CasterCount, count, (int)m_SoftFaces, m_Soft.lights[0].dir[0],
            m_Soft.lights[0].dir[1], m_Soft.lights[0].dir[2],
            m_Soft.lights[0].strength, (int)(m_BumpFaces / 120));
    }
    if (frame % 120 == 0) {
        m_BumpFaces = 0;
    }
    m_SoftFaces = 0;
#endif
}

// Draws light l's silhouettes into the shadow map, over the white it starts
// as: every caster in the light's own cell and, for the nearest light, in
// the cells of the slices past its own.
static void M_DrawSilhouettes(const int32_t l)
{
    const M_SOFT_LIGHT *const light = &m_Soft.lights[l];
    const bool self = M_SelfShadowOn();
    const float size = DC_PVR_SHADOW_MAP_SIZE;
    const float *const c = m_Soft.center;
    // In front of the white (the PowerVR keeps the nearest, not the last).
    DC_PVR_SetState(DC_BLEND_OPAQUE, DC_PVR_NO_TEXTURE, true);
    const float z_near = DC_PVR_Depth2D(0.9f);
    for (int32_t i = 0; i < m_Soft.caster_count; i++) {
        const M_CASTER *const caster = &m_Soft.casters[i];
        const M_BINDING *const bind = caster->bind;
        if (bind->vertex_count > M_SOFT_MAX_VERTICES) {
            continue;
        }
        // The cells this caster shows in, by their left edge.
        float lefts[M_SOFT_SLICES];
        int32_t cells = 0;
        lefts[cells++] = M_SOFT_LIGHT_SLOT(l) * size;
        for (int32_t s = m_Soft.slice[i] + 1;
             self && l == 0 && s < M_SOFT_SLICES; s++) {
            lefts[cells++] = M_SOFT_SELF_SLOT(s) * size;
        }

        // From the caster's own space straight to the cell's x and y, as
        // fractions of the cell: (world - c) . across / span + 1/2.
        const float(*const w)[4] = caster->world;
        const float *const across[2] = { light->right, light->up };
        float to_cell[2][4];
        for (int32_t a = 0; a < 2; a++) {
            const float *const v = across[a];
            for (int32_t k = 0; k < 3; k++) {
                to_cell[a][k] =
                    (w[0][k] * v[0] + w[1][k] * v[1] + w[2][k] * v[2])
                    * (1.0f / M_SOFT_SPAN);
            }
            to_cell[a][3] = 0.5f
                + ((w[0][3] - c[0]) * v[0] + (w[1][3] - c[1]) * v[1]
                   + (w[2][3] - c[2]) * v[2])
                    * (1.0f / M_SOFT_SPAN);
        }
        const OUTPUT_MESH_VERTEX *const vertices =
            Vector_GetData(bind->mesh->vertices);
        for (int32_t j = 0; j < bind->vertex_count; j++) {
            const float px = vertices[j].pos.x;
            const float py = vertices[j].pos.y;
            const float pz = vertices[j].pos.z;
            const float x = to_cell[0][0] * px + to_cell[0][1] * py
                + to_cell[0][2] * pz + to_cell[0][3];
            const float y = to_cell[1][0] * px + to_cell[1][1] * py
                + to_cell[1][2] * pz + to_cell[1][3];
            // Kept inside the cell, so as not to spill into the next.
            m_SoftXY[j][0] = M_Clamp(x, 0.0f, 1.0f) * size;
            m_SoftXY[j][1] = M_Clamp(y, 0.0f, 1.0f) * size;
        }
        const OUTPUT_MESH *const mesh = bind->mesh;
        const OUTPUT_MESH_INDEX *const indices =
            Vector_GetData(mesh->opaque_vertex_indices);
        for (int32_t j = 0; j + 2 < mesh->opaque_vertex_indices->count;
             j += 3) {
            const float *const a = m_SoftXY[indices[j]];
            const float *const b = m_SoftXY[indices[j + 1]];
            const float *const e = m_SoftXY[indices[j + 2]];
            // A closed mesh's faces turned one way cover its whole
            // silhouette, at half the cost of all of them.
            // shortcut: holes show where a mesh is open; draw both ways
            // round for such meshes if it shows.
            if ((b[0] - a[0]) * (e[1] - a[1]) - (e[0] - a[0]) * (b[1] - a[1])
                >= 0.0f) {
                continue;
            }
            for (int32_t k = 0; k < cells; k++) {
                const DC_PVR_VERTEX tri[3] = {
                    { .x = lefts[k] + a[0], .y = a[1], .z = z_near,
                      .argb = 0xFF000000 },
                    { .x = lefts[k] + b[0], .y = b[1], .z = z_near,
                      .argb = 0xFF000000 },
                    { .x = lefts[k] + e[0], .y = e[1], .z = z_near,
                      .argb = 0xFF000000 },
                };
                DC_PVR_Triangle(&tri[0], &tri[1], &tri[2]);
            }
        }
    }
}

// Turns the relief off when the frame rate falls short, and back on (see
// M_BUMP_SLOW_FPS). Once a frame, before it starts.
static void M_UpdateBumpLoad(void)
{
    if (!m_FrameRooms) {
        m_FrameVertices = 0;
        return;
    }
    m_FrameRooms = false;
    const int32_t fps = Output_GetMeasuredFPS();
    const int32_t target = DC_Mode_Get()->fps;
    // A relief page being made (as a level starts, or new walls come in)
    // stalls its frame: the next two seconds' rate says nothing of the load.
    static uint32_t made = 0;
    static int32_t quiet = 0;
    if (DC_PVR_GetBumpPagesMade() != made) {
        made = DC_PVR_GetBumpPagesMade();
        quiet = 2 * target;
    }
    if (quiet > 0) {
        quiet--;
    } else if (m_BumpOn && fps > 0 && fps < target - M_BUMP_SLOW_FPS) {
        m_BumpOn = false;
        m_BumpOffVertices = m_FrameVertices;
        LOG_INFO(
            "relief off: %d fps, %u vertices", (int)fps,
            (unsigned)m_BumpOffVertices);
    } else if (
        !m_BumpOn && fps >= target
        && m_FrameVertices < m_BumpOffVertices * M_BUMP_LIGHTER) {
        m_BumpOn = true;
        LOG_INFO("relief on: %u vertices", (unsigned)m_FrameVertices);
    }
    m_FrameVertices = 0;
}

void DC_Shadow_RenderMaps(void)
{
    M_UpdateBumpLoad();
    const int32_t lights = m_Soft.caster_count > 0 ? m_Soft.light_count : 0;
    m_Soft.map_count = 0;
    m_Soft.self = false;
    if (lights == 0 || !DC_PVR_BeginShadowMapFrame()) {
        m_Soft.caster_count = 0;
        return;
    }
    // Lit everywhere but under the silhouettes.
    DC_PVR_SetState(DC_BLEND_OPAQUE, DC_PVR_NO_TEXTURE, true);
    const float w = DC_PVR_SHADOW_MAP_SIZE * DC_PVR_SHADOW_MAP_SLOTS;
    const float h = DC_PVR_SHADOW_MAP_SIZE;
    const float z = DC_PVR_Depth2D(0.1f);
    const DC_PVR_VERTEX lit[4] = {
        { .x = 0.0f, .y = 0.0f, .z = z, .argb = 0xFFFFFFFF },
        { .x = w, .y = 0.0f, .z = z, .argb = 0xFFFFFFFF },
        { .x = 0.0f, .y = h, .z = z, .argb = 0xFFFFFFFF },
        { .x = w, .y = h, .z = z, .argb = 0xFFFFFFFF },
    };
    DC_PVR_Quad(&lit[0], &lit[1], &lit[2], &lit[3]);
    for (int32_t l = 0; l < lights; l++) {
        M_DrawSilhouettes(l);
    }
    DC_PVR_EndFrame();
    m_Soft.map_count = lights;
    m_Soft.self = M_SelfShadowOn();
    m_Soft.self_count = m_Soft.caster_count;
    m_Soft.caster_count = 0;
}

void DC_Shadow_DrawVolumes(void)
{
    if (m_CasterCount == 0) {
        return;
    }
    float center[3];
    M_CastersCenter(m_Casters, m_CasterCount, center);
    if (M_ShadowType() == SHADOW_TYPE_SOFT) {
        M_KeepSoftCasters(center);
        m_CasterCount = 0;
        return;
    }
    M_SHADOW_LIGHT lights[M_SOFT_MAX_LIGHTS];
    const int32_t max = MIN(DC_Mode_Get()->shadow_lights, M_SOFT_MAX_LIGHTS);
    int32_t light_count = M_GatherShadowLights(m_Casters, center, lights, max);
    if (light_count == 0) {
        lights[0] = (M_SHADOW_LIGHT) {
            .dir = { 0.0f, 1.0f, 0.0f },
            .weight = M_SOFT_NO_LIGHT_WEIGHT,
        };
        light_count = 1;
    }
    M_SetCelShade(lights[0].weight);
    // The PowerVR darkens every volume as much: by the nearest light.
    DC_PVR_SetShadowStrength(
        M_VOLUME_STRENGTH_MIN
        + (M_VOLUME_STRENGTH_MAX - M_VOLUME_STRENGTH_MIN) * lights[0].weight);
    const float *const dir = lights[0].dir;

    // 12 cap triangles and at most 12 outline ones.
    DC_PVR_TRIANGLE tris[24];
    int32_t volumes = 0;
    for (int32_t l = 0; l < light_count; l++) {
        for (int32_t i = 0; i < m_CasterCount; i++) {
            const int32_t count =
                M_BuildVolume(&m_Casters[i], lights[l].dir, tris);
            DC_PVR_ShadowVolume(tris, count);
            volumes += count > 0;
        }
    }
#if defined(TRX_DC_WATCHDOG)
    static int32_t frame = 0;
    static int32_t lit_frames = 0;
    lit_frames += dir[1] < 0.999f;
    const DC_UNIFORM_LIGHTS *const passing = m_Frame.lights;
    if (++frame % 120 == 0) {
        LOG_DEBUG(
            "shadow: %d casters, %d volumes, lit %d/120, room lights %d, "
            "passing %d, dir %.2f %.2f %.2f",
            m_CasterCount, volumes, lit_frames,
            m_Casters[0].room != nullptr ? m_Casters[0].room->num_lights : -1,
            passing != nullptr ? (int)passing->num_lights : -1, dir[0], dir[1],
            dir[2]);
        lit_frames = 0;
    }
#endif
    m_CasterCount = 0;
}

const SCENE_SOURCE *MeshBatcher_AsSource(const MESH_BATCHER *const batcher)
{
    return &batcher->source;
}
