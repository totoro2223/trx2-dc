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

// The rest of the game is built for size; the loops here run per vertex and
// per face, and -O2 keeps their invariants in registers (measured: pass
// -1.1 ms in Venice).
#pragma GCC optimize("O2", "omit-frame-pointer")

#include "cel.h"
#include "pvr.h"
#include "shadow.h"
#include "uniform_layouts.h"

#include <platform/dreamcast/dc_mode.h>
#include <platform/dreamcast/dc_probe.h> // TEMP
#include <platform/dreamcast/gl_facade.h>
#include <trx/config.h>
#include <trx/core/memory.h>
#include <trx/core/vector.h>
#include <trx/debug.h>
#include <trx/game/const.h>
#include <trx/game/lara/common.h>
#include <trx/game/output.h>
#include <trx/game/output/lights/priv.h>
#include <trx/game/output/mesh_batcher/batcher.h>
#include <trx/game/output/shaders/mesh.h>
#include <trx/game/output/sources/objects.h> // g_DC_FrameCount
#include <trx/game/output/textures.h>
#include <trx/game/output/utils.h>
#include <trx/game/output/vertex_range.h>
#include <trx/game/viewport.h>
#include <trx/version.h>

#include <arch/timer.h>
#include <dc/fmath.h>
#include <dc/pvr.h>
#include <dc/matrix.h>
#if defined(TRX_DC_SQ_VERTICES)
    #include <dc/sq.h>
#endif
#include <float.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <uthash.h>

// The helpers run per vertex and per face must be inlined, whatever GCC
// makes of them.
#define M_HOT static inline __attribute__((always_inline))
#define M_SHADE_NEUTRAL 0x1000
#define M_SHADE_MAX 0x1FFF
#define M_NO_PAGE (-1)

// A room's faces are kept in chunks by where they lie (M_BuildChunks), so
// that the parts of a big room off the screen, past the fog or behind the
// camera are neither transformed nor drawn. Cells of this size, doubled until
// a room has no more chunks than this; rooms with fewer vertices stay whole.
#define M_CHUNK_CELL 2048
#define M_MAX_CHUNKS 128
#define M_CHUNK_MIN_VERTICES 256

typedef struct {
    // Bounding sphere, in object space.
    float center[3];
    float radius;
    // Its vertices: a range of the mesh's, which M_BuildChunks lays out by
    // chunk.
    int32_t vert_start;
    int32_t vert_count;
    // Its faces in each opaque run (0: the opaque ones, 1: the punch-through
    // ones), as ranges of opaque_vertex_indices.
    int32_t start[2];
    int32_t count[2];
} M_CHUNK;

// A range of opaque_vertex_indices that goes out for the instance just
// transformed, in its run, with the nearest its faces come along z.
typedef struct {
    int32_t start;
    int32_t count;
    int32_t run;
    float z_near;
} M_RANGE;

typedef struct M_BINDING {
    OUTPUT_MESH *mesh;
    int32_t vertex_count;
    // Per vertex: the texture coordinates and page the vertex samples, looked
    // up once instead of every frame.
    // (As fractions of the page in 16 bits, 0 to M_UV_ONE.)
    uint16_t *uv;
    int8_t *page;
    // A room's corners share its vertices (M_ShareVertices): per corner, the
    // vertex it reads and its texture index. Null for other meshes, whose
    // vertices are their corners.
    uint16_t *vref;
    int16_t *uvw;
    // An object's corners: per corner, the first one just like it (itself
    // if none), which it copies once transformed (M_FindTwins). Null when
    // there are none, or once the mesh has been reshaped (deforms).
    uint16_t *twin;
    bool deforms;
    bool needs_room_lights;
    bool needs_object_light;
    bool needs_own_light;
    // Every vertex lit by the object's light alone, as Lara, enemies and
    // pickups are (M_TransformInstance's object loop).
    bool plain_object;
    // Has sprites, whose corners stand off the box.
    bool billboard;
    // The flags every vertex has, and the ones any has: a mesh whose
    // vertices all agree is transformed without reading them.
    uint16_t flags_and;
    uint16_t flags_or;
    // Every vertex white and opaque, as TR1 and TR2's rooms and most objects
    // are: its colour then comes from m_WhiteARGB.
    bool white;
    // The texture coordinates are to be looked up again before the mesh is
    // next drawn (M_RefillTextures).
    bool tex_dirty;
    // Object space bounding box: minimum then maximum corner.
    float bounds[2][3];
    // The longest of its normals, for the cel shading (0 until it is told).
    float normal_len;
    // The opaque triangles that reflect (VERT_REFLECTIVE on every corner),
    // drawn again with the environment map over them; quads' halves stay
    // side by side as M_PairQuads left them.
    OUTPUT_MESH_INDEX *env;
    int32_t env_count;
    // A room's chunks (none for other meshes), and the range of its vertices
    // that belong to the faces outside them (the added and transparent
    // ones), which are always transformed.
    M_CHUNK *chunks;
    int32_t chunk_count;
    int32_t always_start;
    int32_t always_count;
    // The transparent faces' indices end to end, as M_PairQuads leaves them:
    // one run to emit instead of a call per face.
    OUTPUT_MESH_INDEX *transp;
    int32_t transp_count;
    UT_hash_handle hh;
} M_BINDING;

// The vertex a corner of the binding reads.
static inline const OUTPUT_MESH_VERTEX *M_Corner(
    const M_BINDING *const bind, const OUTPUT_MESH_VERTEX *const vertices,
    const int32_t i)
{
    return &vertices[bind->vref != nullptr ? bind->vref[i] : i];
}

// A mesh instance that casts a shadow this frame.
typedef struct {
    const struct M_BINDING *bind;
    float bounds[2][3];
    float world[3][4];
    const ROOM *room;
    // The object it belongs to (its casters lie together), and whether that
    // is Lara.
    int16_t group;
    bool lara;
} M_CASTER;

// Lara's meshes and those of the few objects nearest her that cast volumes
// (DC_Shadow_TakeObjectCaster, with rendering.dc_object_shadows).
#define M_MAX_CASTERS 128
#define M_MAX_OBJECT_CASTERS 6
// How near Lara an object must be to cast a volume.
#define M_OBJECT_CAST_RANGE (8.0f * WALL_L)
// How far down a shadow volume reaches past its caster, in world units: its
// length grows as the light tilts, so that her head's still meets the floor.
#define M_SHADOW_LENGTH 1024.0f
// How far a light may tilt a shadow off the vertical: a light level with or
// below the caster would throw it sideways or up.
#define M_SHADOW_MIN_DROP 0.6f
// How far a light may be from the casters and still turn their shadow.
#define M_SHADOW_LIGHT_RANGE (3.0f * WALL_L)
// Within this of a light its shadow is as dark as it gets.
#define M_SHADOW_NEAR (1.0f * WALL_L)

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
#define M_SOFT_REACH 3072.0f
// How much of a face's colour the shadow takes away, from a light at the end
// of its reach to one up close: the nearer the light, the darker.
#define M_SOFT_STRENGTH_MIN 0.6f
#define M_SOFT_STRENGTH_MAX 0.9f
// How strong a shadow is without a light near, as a light's weight.
#define M_SOFT_NO_LIGHT_WEIGHT 0.3f
// The fade length of a shadow without a light of its own: about Lara's
// height, so that the far end of it, her head's, is barely seen.
#define M_FIXED_FADE 1152.0f
// How much of the way to this frame's soft shadow lights the last frame's go:
// a light lost or found, or the room's own taking over, slides in over about
// half a second instead of jumping.
#define M_SOFT_EASE 0.15f
// The same for the shadow volumes (the hard shadows), which the PowerVR
// darkens all as much: they fade with the light's distance by this, and keep
// their length (see M_BuildVolume).
#define M_VOLUME_STRENGTH_MIN 0.2f
#define M_VOLUME_STRENGTH_MAX 0.55f
// With no light near (the room's own taken from its vertices, or straight
// down): faint, and as dark all over as any volume.
#define M_VOLUME_STRENGTH_NO_LIGHT 0.3f
// How much of Lara's colour her own shadow takes away.
#define M_SOFT_SELF_STRENGTH 0.7f
// A world face takes no shadow when the cosine between it and the light is
// under the first, all of it from the second on.
#define M_SOFT_GRAZE_MIN 0.25f
#define M_SOFT_GRAZE_FULL 0.6f
// The high soft shadows and the volumes spread from the light as from a
// point: a shadow grows with its distance from the light. This is the least
// distance the light is taken to be at, so that one close up does not blow the
// shadow up.
#define M_PERSP_MIN_DIST WALL_L
// The least a shadow is let reach before it fades out.
#define M_PERSP_MIN_FADE 256.0f
// The distance of a light that is taken to shine in parallel.
#define M_PARALLEL_DIST 1.0e6f
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
// TR1's, whose Lara and levels take the shade to 45 % dark at most.
#define M_CEL_SHADE_NEAR_TR1 0.55f
// Where the edge lies: the cosine to the light past which a vertex is in
// shade (0 at right angles to it; above 0, the shade narrows).
#define M_CEL_EDGE 0.0f
// A face whose corners all lie under half the alpha is lit all over: the
// alpha runs straight across it. (A colour under this has its alpha so.)
#define M_CEL_CUT 0x80000000u
// The rooms' cel shading (a test, rendering.dc_room_cel): their vertices carry
// no normal, so the cut is in their light as it comes: a vertex darker than
// the edge (its brightness, 0 to 255) is in the shade, drawn as dark as Lara's
// (m_CelShade), and a face that crosses the edge has it cut hard there by the
// punch-through test, as hers. A face wholly in the shade goes out dark at
// once; only those that cross take the second pass (M_RoomCel).
#define M_ROOM_CEL_EDGE 100
// Only within this of the camera, the shade lightening to nothing over the
// last stretch.
#define M_ROOM_CEL_RANGE (15.0f * WALL_L)
#define M_ROOM_CEL_FADE (3.0f * WALL_L)
// A room's light has no direction, so a flare lights a pillar's four sides
// alike: the faces turned from the dynamic lights that reach them (up to
// this many) go out dark all over, as Lara's side away from her light.
#define M_ROOM_CEL_LIGHTS 4

// Relief (bump mapping): the room faces nearer the camera than this take
// their page's relief (DC_BLEND_BUMP), lit from the room's light nearest
// them, fading out over the last stretch.
#define M_BUMP_RANGE (4.0f * WALL_L)
#define M_BUMP_FADE (3.0f * WALL_L)
// The relief is looked for in rooms and faces up to this many times the
// range from the camera, where a flash or other passing light may reach.
#define M_BUMP_LIGHT_REACH 3.0f
// How far a room's light is taken to reach, and how much stronger a passing
// one counts than it where both do.
#define M_BUMP_ROOM_RADIUS (8.0f * WALL_L)
#define M_BUMP_PASSING_POWER 8.0f
// A passing light moves the relief this much farther than it lights.
#define M_BUMP_PASSING_REACH 1.5f
// How far a passing light (a shot, a flare) turns the direction the relief is
// lit from, of the way to itself: the rest stays with the room light. The
// relief is lit by it where it reaches, but it does not slide about as it
// moves, which a full turn made the texture do.
#define M_BUMP_PASSING_TILT 0.2f
// How far off the sun is taken to lie, for the direction its relief is lit from.
#define M_BUMP_SUN_DISTANCE (64.0f * WALL_L)
// How deep the relief's shade runs, of the whole colour.
#define M_BUMP_DEPTH 0.765f
// How much of the colour a flat texel of the relief loses, of the whole.
#define M_BUMP_DIM 0.0f
// The least of the colour a slope turned from the light keeps (M_BumpAt).
#define M_BUMP_FLOOR 0.45f
// How bright the slopes turned to the light shine, of the whole.
#define M_BUMP_GLOW 0.5f
// The lowest a light stands over a face, in radians: one level with it or
// behind it would shade every slope.
#define M_BUMP_MIN_ELEVATION 0.35f
#define M_BUMP_MAX_LIGHTS 16
// The relief goes off while the frame rate falls under this, in any mode, and
// comes back once it is this much over it again in a scene this much lighter
// (in vertices) than the one it went off in: back in the same scene, it would
// only go off again.
#define M_BUMP_MIN_FPS 15
#define M_BUMP_SLOW_FPS 2
// A rate under this is a stall (a level loading, a save), not a load too heavy
// for the relief; and it tries again after this many seconds off.
#define M_BUMP_STALL_FPS 6
#define M_BUMP_RETRY_SECONDS 5
#define M_BUMP_LIGHTER 0.85f
// The overlays (the relief, its shine, the soft shadows) lie on the face they
// darken or light, at the depth it has, and win and lose to it pixel by pixel
// where the rounding differs. Each is a hair nearer than the one under it,
// which also keeps the order they are drawn in: the shine adds light, and the
// shadow darkens it, so they do not agree in either order.
#define M_DEPTH_BIAS_BUMP 1.002f
#define M_DEPTH_BIAS_GLOW 1.003f
#define M_DEPTH_BIAS_SHADOW 1.004f

// Reflections: the faces marked reflective take the environment map
// (DC_PVR_ENV_MAP) added over them, where their normal mirrors the view to,
// in their lit colour times this.
#define M_ENV_STRENGTH 0.4f

// A vertex once transformed and lit: eye space, where it lands on screen,
// and its colour. One cache line, which is what every face reads. The
// colours only some instances need (the cel shading's shaded side, the
// reflections', a coloured fog's) lie in arrays of the batcher alongside, by
// the same index.
typedef struct {
    float x;
    float y;
    float z;
    // Where it lands on screen, when in front of the near plane: which way
    // a face turns is told from its corners.
    float sx;
    float sy;
    float inv_z;
    uint32_t argb;
    // M_CLIP_*: what the vertex lies past; a face whose corners all lie past
    // one same thing is not drawn.
    uint8_t clip;
    uint8_t pad[3];
} M_XVERT;
_Static_assert(sizeof(M_XVERT) == 32, "an M_XVERT is one cache line");

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
// Some fog on the vertex: its offset colour is opaque black, the fog's
// (a coloured fog's lies in the batcher's oargb array instead). Not a side
// a face is culled past.
#define M_CLIP_FOGGED 0x40
// The ones that cull a face whose corners all carry them.
#define M_CLIP_CULL 0x3F
// Room left around a window: the game's bounds are worked out in whole
// pixels from its own projection.
#define M_CLIP_MARGIN 4.0f

// A run of the cel shading's polygons kept for one texture page; it ends
// where the next starts.
typedef struct {
    int32_t page;
    int32_t start;
} M_CEL_RUN;

typedef struct MESH_BATCHER {
    SCENE_SOURCE source;
    VECTOR *bindings; // M_BINDING *
    M_BINDING *map;
    VECTOR *staged[SCENE_PASS_COUNT];
    // Per staged instance: its index among the casters, or -1 (int16_t).
    VECTOR *casters[SCENE_PASS_COUNT];
    M_XVERT *xverts;
    // Alongside xverts, by the same index: the offset colour where the fog
    // is not black (oargb_used says the instance has one), the cel shading's
    // shaded colour (in its alpha how far the vertex turns from the light,
    // half at the edge), and where the vertex samples the environment map
    // and the colour it adds it in.
    uint32_t *oargb;
    bool oargb_used;
    uint32_t *sargb;
    float (*env_uv)[2];
    uint32_t *eargb;
    // Per vertex of the instance just transformed, for the soft shadows:
    // where it falls on the silhouette's cell and along the light, and
    // which of the cell's edges and reaches it lies past (M_SOFT_OFF_*).
    float (*soft)[3];
    uint8_t *soft_off;
    int32_t xverts_capacity;
    // Per shared room vertex (M_ShareVertices): the corner it was
    // transformed at this instance, as share_gen << 16 | corner; the other
    // corners of it copy that one.
    uint32_t *share_slot;
    int32_t share_capacity;
    uint32_t share_gen;
    // The model-view matrix of the instance just transformed.
    matrix_t mv;
    // Whether the instance just transformed is cel shaded, and whether the
    // faces going out now keep their shaded side (M_KeepCel).
    bool cel;
    bool cel_emit;
    // Whether the cel shading is a room's (M_RoomCel): its corners in the
    // shade went out dark already, so a face with all of them there keeps
    // no shaded side.
    bool cel_dark;
    // The dynamic lights that reach a room with the cel shading, in view
    // space with their radius squared: its faces turned from them all go out
    // dark (M_FaceTurnedAway), face by face, so not through the assembly.
    float face_lights[M_ROOM_CEL_LIGHTS][4];
    int32_t face_light_count;
    // Whether the instance just transformed reflects.
    bool env;
    // The page the faces going out sample (M_EmitIndices).
    int32_t emit_page;
    // The cel shading's polygons of the instance being drawn, by page.
    pvr_vertex_t *cel_buf;
    int32_t cel_count;
    int32_t cel_capacity;
    VECTOR *cel_runs; // M_CEL_RUN
    int32_t cel_page; // the last run's
    // The opaque faces of the instance just transformed that go out: its
    // runs whole, or the chunks of them that show. The nearest the instance
    // comes along z.
    M_RANGE ranges[2 * M_MAX_CHUNKS + 2];
    int32_t range_count;
    float z_near;
    // The ranges of vertices of the instance just transformed that were
    // transformed (a room's chunks that show, and its faces outside them),
    // or -1 ranges for all of them.
    int32_t vranges[M_MAX_CHUNKS + 1][2];
    int32_t vrange_count;
    // Per vertex range, the chunk it is (-1 for the faces outside them): a
    // room's chunks are each drawn as soon as they are transformed, while
    // their vertices are still in the cache (M_EmitChunk), and then the
    // opaque faces are done with (emitted) before M_DrawInstance's own pass.
    int16_t vrange_chunk[M_MAX_CHUNKS + 1];
    bool emitted;
    uint32_t chunk_emit_us;
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
    // What pvr.c scales screen positions by (DC_PVR_GetScreenScale).
    float screen_scale;
    // How pvr.c lays depth out this pass (DC_PVR_GetDepth3D).
    DC_PVR_DEPTH3D depth;
    bool fog;
    // Faces turned away are not drawn, as the desktop has OpenGL cull them
    // (double sided faces come twice, once each way round).
    bool cull;
    // What the faces going out stand for (M_EMIT_*), and the offset colour
    // that lights the relief's.
    int32_t emit;
    // The relief offset colour of the corners of the face going out (by the
    // index of the vertex), and the one for any other.
    uint32_t bump_k;
    uint32_t bump_vk[6];
    OUTPUT_MESH_INDEX bump_idx[6];
    int32_t bump_n;
    // The mode draws the relief.
    bool bump;
    // The cel shading is on: the player's setting, where the mode allows it.
    bool cel;
    // The rooms take it too (rendering.dc_room_cel, M_RoomCel).
    bool room_cel;
    // The instance being drawn is one of Lara's meshes, the only ones the
    // cel shading takes (M_DrawInstance).
    bool lara_instance;
    const DC_UNIFORM_LIGHTS *lights;
    // The light mode of the room whose lights are uploaded, or -1: the passing
    // lights are the same for every room, and only that mode differs.
    int32_t lights_mode;
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

// The cel shading: the player's setting, where what is drawn allows it.
static bool M_CelOn(void)
{
    return m_CelAllowed && g_Config.rendering.dc_cel_shading;
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
// Lara's meshes are being staged (DC_Shadow_SetLara): only she takes the cel
// shading. Her meshes that cast no shadow are staged with this caster index.
static bool m_StagingLara = false;
#define M_LARA_NOT_CASTER (-2)
static int32_t m_CasterCount = 0;
static M_CASTER m_Casters[M_MAX_CASTERS];
static int16_t m_CasterGroup = 0;
static int32_t m_ObjectCasters = 0;

typedef struct {
    // From the light towards the casters, and two directions across it (the
    // silhouette's x and y), in world space.
    float dir[3];
    float right[3];
    float up[3];
    float strength;
    // How far the light is from the casters' middle (M_PARALLEL_DIST when its
    // shadow does not grow), and how far past it the shadow fades out.
    float dist;
    float fade;
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
// where the mode asks (a room under water takes none while Lara is above it:
// the soft map would cast on the surface and the pool floor seen through it).
#define M_IS_SOFT(type)                                                        \
    ((type) == SHADOW_TYPE_SOFT || (type) == SHADOW_TYPE_SOFT_HIGH)

// The shadow the player chose (a mode sets it as it starts, see
// Shell_HandleConfigChange).
static SHADOW_TYPE M_ShadowType(void)
{
    // The soft shadows are drawn under water too (they were swapped for
    // volumes there once); what is not done is to cast one on a room under
    // water while Lara is above it, the water's own surface (see is_room).
    return g_Config.visuals.shadow_type;
}
// Faces drawn with a soft shadow, for the debug console.
static int32_t m_SoftFaces = 0;
#if defined(TRX_DC_WATCHDOG)
static int32_t m_ShadowReceivers = 0;
static int32_t m_ShadowRooms = 0;
#endif

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

static void M_FillTexture(
    M_BINDING *const bind, const OUTPUT_MESH_VERTEX *const vertex,
    const int32_t uvw_idx, const int32_t idx)
{
    if (uvw_idx < 0 || (vertex->flags & VERT_FLAT_SHADED) != 0) {
        bind->uv[idx * 2 + 0] = 0;
        bind->uv[idx * 2 + 1] = 0;
        bind->page[idx] = M_NO_PAGE;
        return;
    }
    const OUTPUT_UVW uvw = Output_Textures_GetUVW(uvw_idx);
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

// A TR3 dynamic light (a flare, a gun's flash): it adds its colour, fading
// to nothing at its radius (lights_tr3.glsl's lightDynamicTR3).
typedef struct {
    float pos[3];
    float radius;
    float inv_radius;
    float color[3];
} M_RGB_LIGHT;

typedef struct {
    float l[3]; // object space light direction, for object lighting
    float adder;
    bool has_direction;
    // TR3's object light: an ambient colour and three directional colours
    // (sun, brightest bulb, brightest dynamic), the directions in object
    // space over the normals' 1 << 14 (lights_tr3.glsl's lightObjectsTR3).
    // tr3_double is the 128-neutral doubling of the lighting curve.
    bool tr3;
    float tr3_double;
    float tr3_ambient[3];
    int32_t tr3_count;
    float tr3_dir[3][3];
    float tr3_color[3][3];
    int32_t num_rgb;
    M_RGB_LIGHT rgb[M_MAX_POINT_LIGHTS];
    // For vertices lit by the room's lights: the instance's world matrix in
    // world units, the lights, and the flicker the room's light mode adds
    // (times each vertex's light table offset).
    bool has_room_lights;
    float world[3][4];
    int32_t num_lights;
    M_POINT_LIGHT lights[M_MAX_POINT_LIGHTS];
    float flicker;
    // TR1 and TR2 under water: each vertex's shade swings (see M_Caustic).
    bool caustics;
    // TR3 rooms with a water scheme (water_effect - 2, or -1): the amplitudes
    // of the choppy, shimmer and absolute terms, and the time base.
    int32_t water_scheme;
    float water_amp[3];
    float water_time;
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

// TR3's object light on a normal, per channel (0 to 1).
M_HOT void M_TR3ObjectRGB(
    const M_INSTANCE_LIGHT *const il, const float nx, const float ny,
    const float nz, float rgb[3])
{
    rgb[0] = il->tr3_ambient[0];
    rgb[1] = il->tr3_ambient[1];
    rgb[2] = il->tr3_ambient[2];
    for (int32_t i = 0; i < il->tr3_count; i++) {
        const float *const d = il->tr3_dir[i];
        const float n_dot_l = fipr(d[0], d[1], d[2], 0.0f, nx, ny, nz, 0.0f);
        if (n_dot_l > 0.0f) {
            rgb[0] += il->tr3_color[i][0] * n_dot_l;
            rgb[1] += il->tr3_color[i][1] * n_dot_l;
            rgb[2] += il->tr3_color[i][2] * n_dot_l;
        }
    }
    for (int32_t c = 0; c < 3; c++) {
        rgb[c] = M_Clamp(rgb[c], 0.0f, 1.0f);
    }
}

// TR3's dynamic lights at a point of the world: the colour they add.
M_HOT void M_TR3DynamicRGB(
    const M_INSTANCE_LIGHT *const il, const float world[3], float add[3])
{
    add[0] = add[1] = add[2] = 0.0f;
    for (int32_t i = 0; i < il->num_rgb; i++) {
        const M_RGB_LIGHT *const light = &il->rgb[i];
        const float dx = light->pos[0] - world[0];
        const float dy = light->pos[1] - world[1];
        const float dz = light->pos[2] - world[2];
        const float dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq >= light->radius * light->radius) {
            continue;
        }
        const float dist = dist_sq > 0.0f ? dist_sq * M_InvSqrt(dist_sq) : 0.0f;
        const float f = (light->radius - dist) * light->inv_radius;
        add[0] += f * light->color[0];
        add[1] += f * light->color[1];
        add[2] += f * light->color[2];
    }
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

// 2^e: the lights carry whole exponents, which need no libm (exp2f takes a
// few hundred cycles, three times a light for every room instance).
M_HOT float M_Exp2(const float e)
{
    const int32_t i = (int32_t)e;
    if ((float)i == e && i >= -30 && i <= 30) {
        return i >= 0 ? (float)(1u << i) : 1.0f / (float)(1u << -i);
    }
    return exp2f(e);
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
        if (light->kind == OUTPUT_DYNAMIC_LIGHT_RGB) {
            // Encoded as a radius << OUTPUT_DYNAMIC_FALLOFF_SHIFT, halved
            // as the desktop shader does.
            const float radius = light->falloff * 0.5f;
            const float dx = light->pos[0] - center[0];
            const float dy = light->pos[1] - center[1];
            const float dz = light->pos[2] - center[2];
            const float reach = radius + inst_radius;
            if (radius < 1.0f || out->num_rgb >= M_MAX_POINT_LIGHTS
                || dx * dx + dy * dy + dz * dz > reach * reach) {
                continue;
            }
            M_RGB_LIGHT *const rl = &out->rgb[out->num_rgb++];
            for (int32_t k = 0; k < 3; k++) {
                rl->pos[k] = light->pos[k];
                rl->color[k] = light->color[k];
            }
            rl->radius = radius;
            rl->inv_radius = 1.0f / radius;
            continue;
        }
        if (light->kind != OUTPUT_DYNAMIC_LIGHT_LUM) {
            continue;
        }
        const float radius = M_Exp2(light->falloff);
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
        pl->max_shade = M_Exp2(light->shade);
        pl->inv_div = 1.0f / M_Exp2(2.0f * light->falloff - light->shade);
    }
    const int32_t index =
        M_FlickerIndex(lights->room_light_mode, m_Frame.time_in_game);
    out->flicker = index * 512.0f / 31.0f;
}

// The object's or own light of TR1-3 from the instance's light info, as
// Output_Lights_UploadCPULight and Output_Lights_UploadOwnLight would put it in
// the uniform block and M_PrepareInstanceLight read it back, without the trip:
// no compare, copy or upload per instance, and the TR3 directions normalised
// with the SH-4's own reciprocal root.
static void M_ReadInstanceLight(
    const OUTPUT_LIGHT_INFO *const info, const M_BINDING *const bind,
    const float mv[4][4], M_INSTANCE_LIGHT *const out)
{
    if (out->tr3) {
        out->tr3_ambient[0] = info->tr3_ambient.r;
        out->tr3_ambient[1] = info->tr3_ambient.g;
        out->tr3_ambient[2] = info->tr3_ambient.b;
        if (!bind->needs_object_light) {
            return; // its own light takes the ambient alone
        }
        // The brightest light also leads the cel shading.
        float best = 0.0f;
        for (int32_t i = 0; i < 3; i++) {
            const RGB_F col = info->tr3_light_color[i];
            const float sum = col.r + col.g + col.b;
            if (sum <= 0.0f) {
                continue;
            }
            float v[3] = {
                (float)info->tr3_light_dir_view[i].x,
                (float)info->tr3_light_dir_view[i].y,
                (float)info->tr3_light_dir_view[i].z,
            };
            const float len_sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            if (len_sq > 0.0f) {
                const float inv_len = M_InvSqrt(len_sq);
                v[0] *= inv_len;
                v[1] *= inv_len;
                v[2] *= inv_len;
            }
            float *const d = out->tr3_dir[out->tr3_count];
            for (int32_t c = 0; c < 3; c++) {
                d[c] = (mv[c][0] * v[0] + mv[c][1] * v[1] + mv[c][2] * v[2])
                    * (1.0f / (float)(1 << 14));
            }
            out->tr3_color[out->tr3_count][0] = col.r;
            out->tr3_color[out->tr3_count][1] = col.g;
            out->tr3_color[out->tr3_count][2] = col.b;
            if (sum > best) {
                best = sum;
                // As the TR1-2 vector, which points the way the shade grows
                // (away from the light); cel_scale divides its length out.
                for (int32_t c = 0; c < 3; c++) {
                    out->l[c] = -d[c] * (float)(1 << 14);
                }
                out->has_direction = true;
            }
            out->tr3_count++;
        }
        return;
    }
    out->adder = (float)info->ls_adder;
    if (bind->needs_object_light && info->ls_divider != 0) {
        // The view-space light vector, taken into object space by the
        // transpose of the model-view rotation.
        const float inv_div =
            (float)(1 << W2V_SHIFT) / (float)info->ls_divider;
        const float v[3] = {
            (float)info->ls_vector_view.x,
            (float)info->ls_vector_view.y,
            (float)info->ls_vector_view.z,
        };
        for (int32_t c = 0; c < 3; c++) {
            out->l[c] =
                (mv[c][0] * v[0] + mv[c][1] * v[1] + mv[c][2] * v[2]) * inv_div;
        }
        out->has_direction = true;
    }
}

// TR3's water in a room (the desktop's effectChoppy, effectShimmer and
// effectAbs, lights_common.glsl): vertices flagged MOVE and GLOW get a light
// that swings with the game time and a phase of the vertex's place, by the
// level's water scheme. The amplitudes are the desktop's tables; the phase's
// lane offsets and the sine are tables too, so a vertex costs a few adds.
static const float m_WaterChoppy[22] = {
    16.0f, 0.0f,   0.0f,   0.0f,   0.0f,   16.0f, 16.0f, 16.0f,
    16.0f, 53.0f,  53.0f,  53.0f,  53.0f,  90.0f, 90.0f, 90.0f,
    90.0f, 127.0f, 127.0f, 127.0f, 127.0f, 0.0f,
};
static const float m_WaterShimmer[22] = {
    7.875f,   4.0f,     8.0f,     12.0f,    15.875f,  -3.875f,
    -7.875f,  -11.875f, -15.875f, -3.875f,  -7.875f,  -11.875f,
    -15.875f, -3.875f,  -7.875f,  -11.875f, -15.875f, -3.875f,
    -7.875f,  -11.875f, -15.875f, 0.0f,
};
static const float m_WaterAbs[22] = {
    0.0f,  253.0f, 0.0f, 4.0f,  8.0f,  4.0f, 8.0f, 12.0f,
    16.0f, 4.0f,   8.0f, 12.0f, 16.0f, 4.0f, 8.0f, 12.0f,
    16.0f, 4.0f,   8.0f, 12.0f, 16.0f, 0.0f,
};
#define M_WATER_SINE 64
static float m_WaterSine[M_WATER_SINE];
static float m_WaterLane[22][16];
static bool m_WaterReady = false;

static void M_WaterScheme(const MESH_INSTANCE *const inst, M_INSTANCE_LIGHT *const out)
{
    if (!m_WaterReady) {
        for (int32_t k = 0; k < M_WATER_SINE; k++) {
            m_WaterSine[k] = sinf(6.2831853f * (float)k / M_WATER_SINE);
        }
        for (int32_t sc = 0; sc < 22; sc++) {
            for (int32_t lane = 0; lane < 16; lane++) {
                // The desktop's stand-in for the original's random byte.
                const double v =
                    sin((double)lane * 12.9898 + (double)sc * 19.19)
                    * 43758.5453;
                const double n = v - floor(v);
                m_WaterLane[sc][lane] = (float)(floor(n * 16.0) / 16.0);
            }
        }
        m_WaterReady = true;
    }
    int32_t sc = inst->water_effect - 2;
    sc = sc < 0 ? 0 : sc > 21 ? 21 : sc;
    out->water_scheme = sc;
    out->water_amp[0] = m_WaterChoppy[sc];
    out->water_amp[1] = m_WaterShimmer[sc];
    out->water_amp[2] = m_WaterAbs[sc];
    const float t = m_Frame.time_in_game / 64.0f;
    out->water_time = t - floorf(t);
    // The vertices are placed in the world to find their lane.
    M_WorldMatrix(&inst->wmatrix, out->world);
}

// The colour a vertex gets added, in bytes (the desktop's effect / 256 of a
// colour that runs 0 to 1).
M_HOT float M_WaterAdd(
    const M_INSTANCE_LIGHT *const il, const OUTPUT_MESH_VERTEX *const vertex,
    const uint16_t flags)
{
    const float(*const w)[4] = il->world;
    const float px = vertex->pos.x;
    const float py = vertex->pos.y;
    const float pz = vertex->pos.z;
    const float x = w[0][0] * px + w[0][1] * py + w[0][2] * pz + w[0][3];
    const float y = w[1][0] * px + w[1][1] * py + w[1][2] * pz + w[1][3];
    const float z = w[2][0] * px + w[2][1] * py + w[2][2] * pz + w[2][3];
    const float sum = floorf(x / 64.0f) + floorf(y / 64.0f) + floorf(z / 128.0f);
    const int32_t lane = (int32_t)floorf(sum / 4.0f) & 15;
    float turns = il->water_time + m_WaterLane[il->water_scheme][lane];
    turns -= floorf(turns);
    const float sine = m_WaterSine[(int32_t)(turns * M_WATER_SINE) & (M_WATER_SINE - 1)];
    float add = 0.0f;
    if ((flags & VERT_MOVE) != 0) {
        add += -sine * il->water_amp[0] * 0.5f;
    }
    if ((flags & VERT_GLOW) != 0) {
        add += sine * il->water_amp[1] * 8.0f + il->water_amp[2] * 8.0f;
    }
    return add;
}

static void M_PrepareInstanceLight(
    const MESH_INSTANCE *const inst, const M_BINDING *const bind,
    const float mv[4][4], M_INSTANCE_LIGHT *const out)
{
    out->has_direction = false;
    out->has_room_lights = false;
    out->adder = 0.0f;
    out->num_rgb = 0;
    out->tr3 = g_TRVersion == 3;
    out->caustics = inst->water_effect == 1 && g_TRVersion <= 2;
    out->water_scheme = -1;
    if (g_TRVersion == 3 && inst->water_effect >= 2 && bind != nullptr
        && (bind->flags_or & (VERT_MOVE | VERT_GLOW)) != 0) {
        M_WaterScheme(inst, out);
    }
    out->tr3_count = 0;
    out->tr3_double =
        g_Config.rendering.lighting_curve == LIGHTING_CURVE_FLAT ? 1.0f : 2.0f;
    if (bind->needs_room_lights) {
        const int32_t mode =
            inst->room == nullptr ? RLM_SUNSET : inst->room->light_mode;
        if (mode != m_Frame.lights_mode) {
            Output_Uniforms_UploadRoomLights(Output_GetUniforms(), inst->room);
            m_Frame.lights =
                DC_GL_GetUniformBlock(DC_GL_BINDING_LIGHTS, nullptr);
            m_Frame.lights_mode = mode;
        }
        M_PrepareRoomLights(inst, bind, out);
    }
    if (g_TRVersion <= 3 && (bind->needs_object_light || bind->needs_own_light)) {
        M_ReadInstanceLight(&inst->light_info, bind, mv, out);
        return;
    }
    if (bind->needs_object_light) {
        Output_Lights_UploadCPULight(&inst->light_info);
    } else if (bind->needs_own_light) {
        Output_Lights_UploadOwnLight(&inst->light_info);
    } else {
        return;
    }
    if (out->tr3) {
        const OUTPUT_UNIFORM_LS_TR3 *const ls3 =
            DC_GL_GetUniformBlock(DC_GL_BINDING_LIGHT_SOURCE, nullptr);
        if (ls3 == nullptr) {
            return;
        }
        for (int32_t c = 0; c < 3; c++) {
            out->tr3_ambient[c] = ls3->ambient[c];
        }
        if (!bind->needs_object_light) {
            return; // its own light takes the ambient alone
        }
        // The brightest light also leads the cel shading.
        float best = 0.0f;
        for (int32_t i = 0; i < 3; i++) {
            const float *const col = ls3->color[i];
            const float sum = col[0] + col[1] + col[2];
            if (sum <= 0.0f) {
                continue;
            }
            const float *const v = ls3->dir_view[i];
            float *const d = out->tr3_dir[out->tr3_count];
            for (int32_t c = 0; c < 3; c++) {
                d[c] = (mv[c][0] * v[0] + mv[c][1] * v[1] + mv[c][2] * v[2])
                    * (1.0f / (float)(1 << 14));
                out->tr3_color[out->tr3_count][c] = col[c];
            }
            if (sum > best) {
                best = sum;
                // As the TR1-2 vector, which points the way the shade grows
                // (away from the light); cel_scale divides its length out.
                for (int32_t c = 0; c < 3; c++) {
                    out->l[c] = -d[c] * (float)(1 << 14);
                }
                out->has_direction = true;
            }
            out->tr3_count++;
        }
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

// The caustics of TR1 and TR2 under water (lights_tr12.glsl's
// lightWaterCaustics): a vertex's shade moves by sin(phase) * 0x300, the phase
// a turn over 32 game ticks plus an offset the vertex has of its own. The
// desktop makes the offset from the vertex's world position with a float hash;
// here an integer hash of its position, in 32 steps, and a table of the sines.
#define M_CAUSTIC_SIZE 32
#define M_CAUSTIC_AMPLITUDE 768.0f
static float m_CausticSine[M_CAUSTIC_SIZE];
static bool m_CausticReady = false;

M_HOT float M_Caustic(const OUTPUT_MESH_VERTEX *const vertex)
{
    if (!m_CausticReady) {
        for (int32_t k = 0; k < M_CAUSTIC_SIZE; k++) {
            m_CausticSine[k] = sinf(6.2831853f * (float)k / M_CAUSTIC_SIZE)
                * M_CAUSTIC_AMPLITUDE;
        }
        m_CausticReady = true;
    }
    const uint32_t h = ((uint32_t)(int32_t)vertex->pos.x * 73856093u)
        ^ ((uint32_t)(int32_t)vertex->pos.y * 19349663u)
        ^ ((uint32_t)(int32_t)vertex->pos.z * 83492791u);
    const int32_t at = ((int32_t)m_Frame.time_in_game + (int32_t)(h >> 11))
        & (M_CAUSTIC_SIZE - 1);
    return m_CausticSine[at];
}

M_HOT void M_LightVertex(
    const OUTPUT_MESH_VERTEX *const vertex, const M_INSTANCE_LIGHT *const il,
    const MATRIX *const wmatrix, const RGBA_F tint, float out_rgba[4],
    float *const out_shade_mul)
{
    const uint16_t flags = vertex->flags;
    if (il->tr3) {
        // meshes.glsl's TR3 branch: (colour + dynamic) times the object's
        // or its own light, doubled by the 128-neutral curve.
        float mul_rgb[3] = { 1.0f, 1.0f, 1.0f };
        float add[3] = { 0.0f, 0.0f, 0.0f };
        float dbl = 1.0f;
        if (m_Frame.lighting) {
            dbl = il->tr3_double;
        }
        if (m_Frame.lighting && (flags & VERT_NO_LIGHTING) == 0) {
            if ((flags & VERT_USE_DYNAMIC_LIGHT) != 0 && il->num_rgb > 0
                && il->has_room_lights) {
                const float x = vertex->pos.x;
                const float y = vertex->pos.y;
                const float z = vertex->pos.z;
                const float(*const w)[4] = il->world;
                const float world[3] = {
                    w[0][0] * x + w[0][1] * y + w[0][2] * z + w[0][3],
                    w[1][0] * x + w[1][1] * y + w[1][2] * z + w[1][3],
                    w[2][0] * x + w[2][1] * y + w[2][2] * z + w[2][3],
                };
                M_TR3DynamicRGB(il, world, add);
            }
            if ((flags & VERT_USE_OBJECT_LIGHT) != 0) {
                M_TR3ObjectRGB(
                    il, vertex->normal.x, vertex->normal.y, vertex->normal.z,
                    mul_rgb);
            } else if ((flags & VERT_USE_OWN_LIGHT) != 0) {
                float shade8 = floorf((M_SHADE_MAX - vertex->shade) / 32.0f);
                shade8 = shade8 <= 0.0f ? 255.0f : shade8;
                for (int32_t c = 0; c < 3; c++) {
                    mul_rgb[c] =
                        M_Clamp(il->tr3_ambient[c] * shade8 / 255.0f, 0.0f, 1.0f);
                }
            }
        }
        *out_shade_mul = dbl;
        const float tints[3] = { tint.r, tint.g, tint.b };
        const uint8_t cols[3] = {
            vertex->color.r, vertex->color.g, vertex->color.b,
        };
        for (int32_t c = 0; c < 3; c++) {
            out_rgba[c] = (cols[c] * (1.0f / 255.0f) + add[c]) * mul_rgb[c]
                * dbl * tints[c];
        }
        out_rgba[3] = vertex->color.a * (1.0f / 255.0f) * tint.a;
        return;
    }
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
    if (il->caustics && m_Frame.lighting && (flags & VERT_NO_LIGHTING) == 0) {
        shade = M_Clamp(shade + M_Caustic(vertex), 0.0f, M_SHADE_MAX);
    }
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

// What holds for every vertex of the instance being transformed: the tests
// the whole instance passed (against the screen's sides, the near plane and
// the start of the fog) are not made again per vertex.
typedef struct {
    float left;
    float right;
    float top;
    float bottom;
    // The sides as planes through the eye, once per instance: per side the
    // factors of x (left, right) or y (top, bottom), and of z; positive
    // outside.
    float planes[4][2];
    bool ortho;
    bool sides; // some vertex may lie past a side
    bool near; // some vertex may lie before the near plane
    bool fog; // some vertex may lie in the fog
    float proj_x;
    float proj_y;
    float near_z;
    float fog_start;
    float fog_start_sq;
    float fog_scale;
    // The nearest vertex placed, as 1/z.
    float max_inv_z;
} M_XFORM;

// Eye space to the screen, and the clipping: returns the vertex's M_CLIP_*
// sides (never M_CLIP_FOG or M_CLIP_FOGGED, which the colour decides).
M_HOT uint8_t M_Place(
    M_XVERT *const out, const float x, const float y, const float z,
    M_XFORM *const c)
{
    out->x = x;
    out->y = y;
    out->z = z;
    if (c->ortho) {
        return 0;
    }
    if (c->near && z < c->near_z) {
        return M_CLIP_NEAR;
    }
    // z is past the near plane, so positive: an fsrra in place of a division.
    const float inv_z = M_InvSqrt(z * z);
    const float sx = c->proj_x * x * inv_z;
    const float sy = c->proj_y * y * inv_z;
    out->sx = sx;
    out->sy = sy;
    out->inv_z = inv_z;
    if (inv_z > c->max_inv_z) {
        c->max_inv_z = inv_z;
    }
    if (!c->sides) {
        return 0;
    }
    return (sx < c->left ? M_CLIP_LEFT : 0) | (sx > c->right ? M_CLIP_RIGHT : 0)
        | (sy < c->top ? M_CLIP_TOP : 0) | (sy > c->bottom ? M_CLIP_BOTTOM : 0);
}

// How much fog lies on a vertex at the eye space position, 0 to 1. Nearer
// than the fog starts, as most is, it needs no root.
M_HOT float M_Fog(
    const float x, const float y, const float z, const M_XFORM *const c)
{
    const float dist_sq = fipr_magnitude_sqr(x, y, z, 0.0f);
    if (dist_sq <= c->fog_start_sq) {
        return 0.0f;
    }
    return M_Clamp(
        (dist_sq * M_InvSqrt(dist_sq) - c->fog_start) * c->fog_scale, 0.0f,
        1.0f);
}

// Marks a vertex's fog in its clip, and returns how much of its colour the
// fog leaves it, in 8.8.
M_HOT uint32_t M_FogKeep(const float fog, uint8_t *const clip)
{
    if (fog <= 0.0f) {
        return 256;
    }
    *clip |= fog >= 1.0f ? M_CLIP_FOG | M_CLIP_FOGGED : M_CLIP_FOGGED;
    return (uint32_t)((1.0f - fog) * 256.0f);
}

// How much of its colour a vertex of shade s keeps, in 8.8: M_LightVertex's
// mul = 2 - s / M_SHADE_NEUTRAL, in integers. The shade is clamped as there,
// then held to the frame's minimum: lo is that minimum, at least 0
// (M_ShadeLow).
M_HOT uint32_t M_ShadeK(int32_t s, const int32_t lo)
{
    if (s > M_SHADE_MAX) {
        s = M_SHADE_MAX;
    }
    if (s < lo) {
        s = lo;
    }
    return 512u - ((uint32_t)s >> 4);
}

M_HOT int32_t M_ShadeLow(void)
{
    return MAX((int32_t)m_Frame.min_shade, 0);
}

// A channel product in 8.8 brought back to 8 bits, saturated without a
// branch: x is under 1024, so x >> 8 is 0 or sets every bit when negated.
M_HOT uint32_t M_Sat8(const uint32_t x)
{
    return (x | (0u - (x >> 8))) & 0xFFu;
}

// A vertex colour times a factor per channel (8.8), as ARGB.
M_HOT uint32_t M_Colour(
    const RGBA_8888 c, const uint32_t ka, const uint32_t kr, const uint32_t kg,
    const uint32_t kb)
{
    return (M_Sat8(c.a * ka >> 8) << 24) | (M_Sat8(c.r * kr >> 8) << 16)
        | (M_Sat8(c.g * kg >> 8) << 8) | M_Sat8(c.b * kb >> 8);
}

// The colour of a packed ARGB scaled by k in 8.8 (at most 256), its alpha
// kept.
M_HOT uint32_t M_ScaleRGB8(const uint32_t c, const uint32_t k)
{
    return (c & 0xFF000000u) | ((((c >> 16) & 0xFF) * k >> 8) << 16)
        | ((((c >> 8) & 0xFF) * k >> 8) << 8) | (((c & 0xFF) * k) >> 8);
}

// A tint channel (0 to 1) as an 8.8 factor.
M_HOT uint32_t M_TintK(const float t)
{
    return (uint32_t)(M_Clamp(t, 0.0f, 2.0f) * 256.0f);
}

// M_Colour of a white vertex in an untinted instance, for every k the shade
// and the fog can give (0 to 512): one read in place of four products.
#define M_WHITE_K_MAX 512
static uint32_t m_WhiteARGB[M_WHITE_K_MAX + 1];

static void M_InitWhite(void)
{
    const RGBA_8888 white = { .r = 255, .g = 255, .b = 255, .a = 255 };
    for (uint32_t k = 0; k <= M_WHITE_K_MAX; k++) {
        m_WhiteARGB[k] = M_Colour(white, 256, k, k, k);
    }
}

// A room's vertices: lit by their own shade, the lights passing and the
// room's flicker, in the room's tint, faded into black fog. All in integers:
// no float colour, no clamps, no float to int conversions per channel. Only
// the vertices of the list, when there is one (the chunks that show).
// A reflective vertex's place on the sphere map and its colour there, from
// its eye space position, the fog it keeps and its lit colour (0 to 1).
M_HOT void M_EnvVertex(
    MESH_BATCHER *const batcher, const int32_t i,
    const OUTPUT_MESH_VERTEX *const vertex, const float mv[4][4],
    const float x, const float y, const float z, const float keep,
    const float lit[3])
{
    // The normal in eye space (the camera at the origin, looking down z),
    // turned to face the camera, mirrors the way to the vertex; the sphere
    // map holds what lies that way.
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
        const float inv_d = M_InvSqrt(d_len_sq);
        const float d[3] = { x * inv_d, y * inv_d, z * inv_d };
        const float inv_n = M_InvSqrt(n_len_sq);
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
        // Straight back at the camera is the map's middle, away from it its
        // rim; right and down on the screen, in eye space, by the
        // projection's signs.
        const float m2 =
            r[0] * r[0] + r[1] * r[1] + (r[2] - 1.0f) * (r[2] - 1.0f);
        if (m2 > 0.0f) {
            const float inv_m = 0.5f * M_InvSqrt(m2);
            const float env_x = m_Frame.proj_x < 0.0f ? -1.0f : 1.0f;
            const float env_y = m_Frame.proj_y < 0.0f ? -1.0f : 1.0f;
            eu = 0.5f + env_x * r[0] * inv_m;
            ev = 0.5f + env_y * r[1] * inv_m;
        }
    }
    batcher->env_uv[i][0] = eu;
    batcher->env_uv[i][1] = ev;
    // Half its own, half the light on it: metal shows its surroundings even
    // in the shade, if less.
    float e[3];
    for (int32_t k = 0; k < 3; k++) {
        e[k] = keep * M_ENV_STRENGTH * (0.5f + 0.5f * lit[k]);
    }
    batcher->eargb[i] = M_PackARGB(1.0f, e[0], e[1], e[2]);
}

#if defined(DC_EMIT_VERIFY)
static uint32_t m_XVerifyN = 0;
static uint32_t m_XVerifyBad = 0;

static bool M_XNear(const float a, const float b)
{
    return fabsf(a - b) <= 1.0e-4f * MAX(fabsf(a), fabsf(b)) + 1.0e-6f;
}

// A colour within three levels a channel: the assembly adds and multiplies
// the floats apart where the compiler fuses them (-O2 does), so TR3's
// per-channel light, or the fog's share of a vertex at its edge, can land a
// level or two off at a truncation.
static bool M_ArgbNear(const uint32_t a, const uint32_t b)
{
    for (int32_t s = 0; s < 32; s += 8) {
        const int32_t d = (int32_t)((a >> s) & 0xFF) - (int32_t)((b >> s) & 0xFF);
        if (d > 3 || d < -3) {
            return false;
        }
    }
    return true;
}

// The C loop's vertices against the assembly's, for the ranges.
static void M_VerifyXform(
    const M_XVERT *const c_side, const M_XVERT *const asm_side,
    const int32_t (*const ranges)[2], const int32_t nranges)
{
    for (int32_t r = 0; r < nranges; r++) {
        for (int32_t i = ranges[r][0]; i < ranges[r][0] + ranges[r][1]; i++) {
            const M_XVERT *const a = &asm_side[i];
            const M_XVERT *const b = &c_side[i];
            const bool near = (b->clip & M_CLIP_NEAR) != 0;
            const bool ok = a->clip == b->clip && M_ArgbNear(a->argb, b->argb)
                && M_XNear(a->x, b->x) && M_XNear(a->y, b->y)
                && M_XNear(a->z, b->z)
                && (near
                    || (M_XNear(a->sx, b->sx) && M_XNear(a->sy, b->sy)
                        && M_XNear(a->inv_z, b->inv_z)));
            m_XVerifyN++;
            if (!ok) {
                m_XVerifyBad++;
                if (m_XVerifyBad <= 12) {
                    LOG_WARNING(
                        "[xform-verify] MISMATCH vertex %d: asm clip %02x "
                        "argb %08x x %f sx %f | C clip %02x argb %08x x %f "
                        "sx %f",
                        (int)i, a->clip, (unsigned)a->argb, a->x, a->sx,
                        b->clip, (unsigned)b->argb, b->x, b->sx);
                }
            }
        }
    }
    if (m_XVerifyN >= 400000) {
        LOG_WARNING(
            "[xform-verify] %u vertices, %u bad", (unsigned)m_XVerifyN,
            (unsigned)m_XVerifyBad);
        m_XVerifyN = 0;
    }
}
#endif

// What xform_sh4.S takes: the plain room (see dc_xform_room).
typedef struct {
    M_XFORM *c;
    M_XVERT *xverts;
    const OUTPUT_MESH_VERTEX *vertices;
    const uint16_t *vref;
    uint32_t *slot;
    uint32_t gen;
    const int32_t (*ranges)[2];
    int32_t nranges;
    int32_t min_shade;
    int32_t flicker;
    const uint32_t *white; // nullptr: not white, the tints below apply
    uint32_t hits;
    uint32_t tint[4]; // a, r, g, b in 8.8
    uint32_t tr3_k; // TR3's factor in place of the shade; 0 for TR1/TR2
} M_XROOM;
_Static_assert(offsetof(M_XROOM, tr3_k) == 64, "xform_sh4.S reads these");
_Static_assert(offsetof(M_XROOM, white) == 40, "xform_sh4.S reads these");
_Static_assert(offsetof(M_XROOM, hits) == 44, "xform_sh4.S reads these");
_Static_assert(offsetof(M_XROOM, tint) == 48, "xform_sh4.S reads these");
_Static_assert(sizeof(OUTPUT_MESH_VERTEX) == 28, "xform_sh4.S reads these");
_Static_assert(offsetof(M_XFORM, ortho) == 48, "xform_sh4.S reads these");
_Static_assert(offsetof(M_XFORM, proj_x) == 52, "xform_sh4.S reads these");
_Static_assert(offsetof(M_XFORM, max_inv_z) == 76, "xform_sh4.S reads these");
extern void dc_xform_room(M_XROOM *run);

static void M_XformRoom(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const MESH_INSTANCE *const inst, const M_INSTANCE_LIGHT *const il,
    M_XFORM *const c, const int32_t (*const vranges)[2], const int32_t nvr)
{
    const OUTPUT_MESH_VERTEX *const vertices =
        Vector_GetData(bind->mesh->vertices);
    M_XVERT *const xverts = batcher->xverts;
    const bool lighting = m_Frame.lighting;
    const int32_t min_shade = M_ShadeLow();
    const bool dyn = il->num_lights > 0;
    const bool tr3 = il->tr3;
    const bool rgb_dyn = tr3 && lighting && il->num_rgb > 0;
    const uint32_t tr3_k = lighting ? (uint32_t)(256.0f * il->tr3_double) : 256;
    const int32_t flicker = (int32_t)il->flicker;
    const bool fog_on = c->fog;
    // Most rooms' vertices all carry the same flags: then they are not read.
    const bool water = tr3 && lighting && il->water_scheme >= 0;
    const bool vary = (bind->flags_or & (VERT_NO_FOG | VERT_NO_LIGHTING)) != 0
        || (bind->flags_and & VERT_USE_DYNAMIC_LIGHT) == 0 || water;
    const uint32_t ta = M_TintK(inst->tint.a);
    const uint32_t tr = M_TintK(inst->tint.r);
    const uint32_t tg = M_TintK(inst->tint.g);
    const uint32_t tb = M_TintK(inst->tint.b);
    const bool white =
        bind->white && ta == 256 && tr == 256 && tg == 256 && tb == 256;
    const float(*const w)[4] = il->world;
    // With faces turned from a dynamic light to find (M_FaceTurnedAway), each
    // vertex's colour without those lights too, in the reflections' colours
    // (eargb), which no room has.
    uint32_t *const nodyn =
        batcher->face_light_count > 0 ? batcher->eargb : nullptr;
    // The whole mesh, or the chunks that show: each a run of vertices.
    const int32_t whole[1][2] = { { 0, bind->vertex_count } };
    const int32_t(*const ranges)[2] = nvr < 0 ? whole : vranges;
    const int32_t nranges = nvr < 0 ? 1 : nvr;
    // Shared vertices are lit once per instance, not once per corner.
    const uint16_t *const vref =
        bind->vertex_count <= 0xFFFF ? bind->vref : nullptr;
    uint32_t *slot = nullptr;
    uint32_t gen = 0;
    if (vref != nullptr) {
        const int32_t kept = bind->mesh->vertices->count;
        if (kept > batcher->share_capacity) {
            batcher->share_capacity = kept + 256;
            Memory_Free(batcher->share_slot);
            batcher->share_slot =
                Memory_Alloc(sizeof(uint32_t) * batcher->share_capacity);
            batcher->share_gen = 0xFFFF; // cleared just below

        }
        if (++batcher->share_gen > 0xFFFF) {
            memset(
                batcher->share_slot, 0,
                sizeof(uint32_t) * batcher->share_capacity);
            batcher->share_gen = 1;
        }
        slot = batcher->share_slot;
        gen = batcher->share_gen << 16;
    }
    {
        int32_t total = 0;
        for (int32_t r = 0; r < nranges; r++) {
            total += ranges[r][1];
        }
        g_DC_ProbeUs[DC_PROBE_R_ALL] += total; // TEMP
        g_DC_ProbeUs[dyn ? DC_PROBE_R_DYN : flicker != 0 ? DC_PROBE_R_FLICK
                : vary                                    ? DC_PROBE_R_VARY
                : !white                                  ? DC_PROBE_R_NOTWHITE
                                                          : DC_PROBE_R_PLAIN] +=
            total; // TEMP
        g_DC_ProbeUs[DC_PROBE_R_FOG] += fog_on ? total : 0; // TEMP
    }
    const bool fast =
        vref != nullptr && lighting && (tr3 ? !rgb_dyn : !dyn) && !vary
        && !c->ortho && !il->caustics && !water;
    M_XROOM run = {
        .c = c,
        .xverts = xverts,
        .vertices = vertices,
        .vref = vref,
        .slot = slot,
        .gen = gen,
        .ranges = ranges,
        .nranges = nranges,
        .min_shade = min_shade,
        .flicker = flicker,
        .white = white ? m_WhiteARGB : nullptr,
        .tint = { ta, tr, tg, tb },
        .tr3_k = tr3 ? tr3_k : 0,
    };
#if !defined(DC_EMIT_VERIFY)
    if (fast) {
        dc_xform_room(&run);
        g_DC_ProbeUs[DC_PROBE_R_HIT] += run.hits; // TEMP
        return;
    }
#endif
    for (int32_t r = 0; r < nranges; r++) {
        const int32_t end = ranges[r][0] + ranges[r][1];
        for (int32_t i = ranges[r][0]; i < end; i++) {
            if (vref != nullptr) {
                uint32_t *const s = &slot[vref[i]];
                if ((*s & 0xFFFF0000u) == gen) {
                    g_DC_ProbeUs[DC_PROBE_R_HIT]++; // TEMP
                    xverts[i] = xverts[*s & 0xFFFFu];
                    if (nodyn != nullptr) {
                        nodyn[i] = nodyn[*s & 0xFFFFu];
                    }
                    continue;
                }
                *s = gen | (uint32_t)i;
            }
            const OUTPUT_MESH_VERTEX *const vertex = M_Corner(bind, vertices, i);
            // The SH-4 does not wait for a line it was told to fetch ahead.
            __builtin_prefetch(vertex + 2);
            const uint16_t flags =
                vary ? vertex->flags : VERT_USE_DYNAMIC_LIGHT;
            float x = vertex->pos.x;
            float y = vertex->pos.y;
            float z = vertex->pos.z;
            float w1 = 1.0f;
            mat_trans_nodiv(x, y, z, w1);
            M_XVERT *const out = &xverts[i];
            uint8_t clip = M_Place(out, x, y, z, c);

            uint32_t k = 256;
            RGBA_8888 color = vertex->color;
            // What the dynamic lights take off the shade (TR1, TR2) or add to
            // the colour (TR3), and the shade's factor without them.
            int32_t dyn_drop = 0;
            uint8_t dyn_rgb[3] = { 0, 0, 0 };
            int32_t k_nodyn = -1;
            if (tr3) {
                // Prelit colours, doubled by the curve, plus the colour
                // of the dynamic lights passing.
                k = tr3_k;
                if (rgb_dyn && (flags & VERT_NO_LIGHTING) == 0
                    && (flags & VERT_USE_DYNAMIC_LIGHT) != 0) {
                    const float px = vertex->pos.x;
                    const float py = vertex->pos.y;
                    const float pz = vertex->pos.z;
                    const float world[3] = {
                        w[0][0] * px + w[0][1] * py + w[0][2] * pz + w[0][3],
                        w[1][0] * px + w[1][1] * py + w[1][2] * pz + w[1][3],
                        w[2][0] * px + w[2][1] * py + w[2][2] * pz + w[2][3],
                    };
                    float add[3];
                    M_TR3DynamicRGB(il, world, add);
                    const RGBA_8888 prelit = color;
                    color.r = (uint8_t)MIN(255.0f, color.r + add[0] * 255.0f);
                    color.g = (uint8_t)MIN(255.0f, color.g + add[1] * 255.0f);
                    color.b = (uint8_t)MIN(255.0f, color.b + add[2] * 255.0f);
                    dyn_rgb[0] = color.r - prelit.r;
                    dyn_rgb[1] = color.g - prelit.g;
                    dyn_rgb[2] = color.b - prelit.b;
                }
                if (water && (flags & (VERT_MOVE | VERT_GLOW)) != 0
                    && (flags & VERT_NO_LIGHTING) == 0) {
                    const float add = M_WaterAdd(il, vertex, flags);
                    color.r = (uint8_t)M_Clamp(color.r + add, 0.0f, 255.0f);
                    color.g = (uint8_t)M_Clamp(color.g + add, 0.0f, 255.0f);
                    color.b = (uint8_t)M_Clamp(color.b + add, 0.0f, 255.0f);
                }
            } else if ((flags & VERT_NO_LIGHTING) == 0) {
                int32_t s = M_SHADE_NEUTRAL;
                if (lighting) {
                    s = vertex->shade;
                    if ((flags & VERT_USE_DYNAMIC_LIGHT) != 0) {
                        if (dyn) {
                            const float px = vertex->pos.x;
                            const float py = vertex->pos.y;
                            const float pz = vertex->pos.z;
                            const float world[3] = {
                                w[0][0] * px + w[0][1] * py + w[0][2] * pz
                                    + w[0][3],
                                w[1][0] * px + w[1][1] * py + w[1][2] * pz
                                    + w[1][3],
                                w[2][0] * px + w[2][1] * py + w[2][2] * pz
                                    + w[2][3],
                            };
                            const int32_t lit =
                                (int32_t)M_DynamicLum((float)s, world, il);
                            dyn_drop = s - lit;
                            s = lit;
                        }
                        if (flicker != 0) {
                            s += ((vertex->light_table_idx & 31) - 16)
                                * flicker;
                        }
                    }
                }
                if (il->caustics && lighting) {
                    s += (int32_t)M_Caustic(vertex);
                }
                k = M_ShadeK(s, min_shade);
                if (dyn_drop != 0) {
                    k_nodyn = (int32_t)M_ShadeK(s + dyn_drop, min_shade);
                }
            }
            if (fog_on && (flags & (VERT_NO_FOG | VERT_NO_LIGHTING)) == 0) {
                const uint32_t keep = M_FogKeep(M_Fog(x, y, z, c), &clip);
                k = k * keep >> 8;
                if (k_nodyn >= 0) {
                    k_nodyn = (int32_t)((uint32_t)k_nodyn * keep >> 8);
                }
            }
            out->argb = white && !rgb_dyn ? m_WhiteARGB[k]
                                          : M_Colour(
                                                color, ta, k * tr >> 8,
                                                k * tg >> 8, k * tb >> 8);
            out->clip = clip;
            if (nodyn != nullptr) {
                const uint32_t kn = k_nodyn >= 0 ? (uint32_t)k_nodyn : k;
                const RGBA_8888 prelit = {
                    .r = color.r - dyn_rgb[0],
                    .g = color.g - dyn_rgb[1],
                    .b = color.b - dyn_rgb[2],
                    .a = color.a,
                };
                nodyn[i] = k_nodyn < 0 && !rgb_dyn ? out->argb
                    : white && !rgb_dyn            ? m_WhiteARGB[kn]
                                                   : M_Colour(
                                                         prelit, ta, kn * tr >> 8,
                                                         kn * tg >> 8,
                                                         kn * tb >> 8);
            }
        }
    }
#if defined(DC_EMIT_VERIFY)
    if (fast) {
        // The assembly places them all again (a newer generation, so no
        // vertex counts as placed) and the two are compared.
        static M_XVERT *saved = nullptr;
        static int32_t saved_cap = 0;
        if (bind->vertex_count > saved_cap) {
            Memory_Free(saved);
            saved_cap = bind->vertex_count;
            saved = Memory_Alloc(sizeof(M_XVERT) * saved_cap);
        }
        memcpy(saved, xverts, sizeof(M_XVERT) * bind->vertex_count);
        if (++batcher->share_gen > 0xFFFF) {
            memset(
                batcher->share_slot, 0,
                sizeof(uint32_t) * batcher->share_capacity);
            batcher->share_gen = 1;
        }
        run.gen = batcher->share_gen << 16;
        dc_xform_room(&run);
        M_VerifyXform(saved, xverts, ranges, nranges);
    }
#endif
}

// What xform_obj_sh4.S takes: the plain object (see dc_xform_object).
typedef struct {
    M_XFORM *c;
    M_XVERT *xverts;
    const OUTPUT_MESH_VERTEX *vertices;
    const uint16_t *vref;
    const uint16_t *twin;
    int32_t count;
    int32_t min_shade;
    const uint32_t *white;
    uint32_t *sargb; // nullptr: no cel colour
    float adder;
    float l[3]; // zero when the object has no direction
    float cel_scale;
    uint32_t dark;
    uint32_t tint[4]; // a, r, g, b in 8.8
    const M_INSTANCE_LIGHT *il; // TR3's object light; nullptr for TR1/TR2
} M_XOBJ;
_Static_assert(offsetof(M_XOBJ, il) == 76, "xform_obj_sh4.S reads these");
_Static_assert(offsetof(M_INSTANCE_LIGHT, tr3_double) == 20, "xform_obj_sh4.S");
_Static_assert(offsetof(M_INSTANCE_LIGHT, tr3_ambient) == 24, "xform_obj_sh4.S");
_Static_assert(offsetof(M_INSTANCE_LIGHT, tr3_count) == 36, "xform_obj_sh4.S");
_Static_assert(offsetof(M_INSTANCE_LIGHT, tr3_dir) == 40, "xform_obj_sh4.S");
_Static_assert(offsetof(M_INSTANCE_LIGHT, tr3_color) == 76, "xform_obj_sh4.S");
_Static_assert(offsetof(M_XOBJ, sargb) == 32, "xform_obj_sh4.S reads these");
_Static_assert(offsetof(M_XOBJ, dark) == 56, "xform_obj_sh4.S reads these");
_Static_assert(offsetof(M_XOBJ, tint) == 60, "xform_obj_sh4.S reads these");
extern void dc_xform_object(M_XOBJ *run);

// Lara, enemies, pickups: the object's light on each vertex, with n.l found
// once for the light and the cel shading both. Colours in integers as the
// rooms'.
static void M_XformObject(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const MESH_INSTANCE *const inst, const M_INSTANCE_LIGHT *const il,
    M_XFORM *const c, const float mv[4][4], const float cel_scale)
{
    const OUTPUT_MESH_VERTEX *const vertices =
        Vector_GetData(bind->mesh->vertices);
    M_XVERT *const xverts = batcher->xverts;
    const int32_t count = bind->vertex_count;
    const int32_t min_shade = M_ShadeLow();
    const bool env = batcher->env;
    const bool fog_on = c->fog;
    const bool vary = (bind->flags_or & VERT_NO_FOG) != 0;
    const bool dir = il->has_direction;
    const float l0 = il->l[0];
    const float l1 = il->l[1];
    const float l2 = il->l[2];
    const float adder = il->adder;
    const bool tr3 = il->tr3;
    const bool cel = batcher->cel;
    const uint32_t dark = (uint32_t)(m_CelShade * 256.0f);
    uint32_t *const sargb = batcher->sargb;
    const uint32_t ta = M_TintK(inst->tint.a);
    const uint32_t tr = M_TintK(inst->tint.r);
    const uint32_t tg = M_TintK(inst->tint.g);
    const uint32_t tb = M_TintK(inst->tint.b);
    const bool white =
        bind->white && ta == 256 && tr == 256 && tg == 256 && tb == 256;
    const uint16_t *const twin = bind->twin;
    const bool fast = !env && !vary && !c->ortho
        && (bind->vref == nullptr || count <= 0xFFFF);
    M_XOBJ run = {
        .c = c,
        .xverts = xverts,
        .vertices = vertices,
        .vref = bind->vref,
        .twin = twin,
        .count = count,
        .min_shade = min_shade,
        .white = white ? m_WhiteARGB : nullptr,
        .sargb = cel ? sargb : nullptr,
        .adder = adder,
        .l = { dir ? l0 : 0.0f, dir ? l1 : 0.0f, dir ? l2 : 0.0f },
        .cel_scale = cel_scale,
        .dark = dark,
        .tint = { ta, tr, tg, tb },
        .il = tr3 ? il : nullptr,
    };
#if !defined(DC_EMIT_VERIFY)
    if (fast) {
        dc_xform_object(&run);
        return;
    }
#endif
    for (int32_t i = 0; i < count; i++) {
        if (twin != nullptr && twin[i] != i) {
            const int32_t j = twin[i];
            xverts[i] = xverts[j];
            sargb[i] = sargb[j];
            if (env) {
                batcher->env_uv[i][0] = batcher->env_uv[j][0];
                batcher->env_uv[i][1] = batcher->env_uv[j][1];
                batcher->eargb[i] = batcher->eargb[j];
            }
            continue;
        }
        const OUTPUT_MESH_VERTEX *const vertex = M_Corner(bind, vertices, i);
        __builtin_prefetch(vertex + 2);
        float x = vertex->pos.x;
        float y = vertex->pos.y;
        float z = vertex->pos.z;
        float w1 = 1.0f;
        mat_trans_nodiv(x, y, z, w1);
        M_XVERT *const out = &xverts[i];
        uint8_t clip = M_Place(out, x, y, z, c);

        // As M_LightVertex lights a vertex of the object's light.
        float shade = adder;
        float n_dot_l = 0.0f;
        if (dir) {
            n_dot_l = fipr(
                l0, l1, l2, 0.0f, vertex->normal.x, vertex->normal.y,
                vertex->normal.z, 0.0f);
            shade += n_dot_l * (1.0f / (float)(1 << 16));
        }
        uint32_t fk = 256;
        uint32_t k = tr3 ? 256 : M_ShadeK((int32_t)shade, min_shade);
        if (fog_on && (!vary || (vertex->flags & VERT_NO_FOG) == 0)) {
            fk = M_FogKeep(M_Fog(x, y, z, c), &clip);
            k = k * fk >> 8;
        }
        uint32_t argb;
        if (tr3) {
            // Coloured: a factor per channel.
            float rgb[3];
            M_TR3ObjectRGB(
                il, vertex->normal.x, vertex->normal.y, vertex->normal.z, rgb);
            const float kk = (float)k * il->tr3_double;
            argb = M_Colour(
                vertex->color, ta, (uint32_t)(rgb[0] * kk) * tr >> 8,
                (uint32_t)(rgb[1] * kk) * tg >> 8,
                (uint32_t)(rgb[2] * kk) * tb >> 8);
        } else {
            argb = white ? m_WhiteARGB[k]
                         : M_Colour(
                               vertex->color, ta, k * tr >> 8, k * tg >> 8,
                               k * tb >> 8);
        }
        out->argb = argb;
        out->clip = clip;
        if (cel) {
            // Half at the edge, more into the shade; the PowerVR runs it
            // across each face, and the alpha test cuts it there.
            const float edge = 0.5f + 0.5f * (n_dot_l * cel_scale - M_CEL_EDGE);
            const uint32_t a = (uint32_t)(M_Clamp(edge, 0.0f, 1.0f) * 255.0f);
            // White: one grey, scaled once for the three channels.
            const uint32_t rgb = white && !tr3
                ? ((argb & 0xFFu) * dark >> 8) * 0x010101u
                : M_ScaleRGB8(argb, dark) & 0x00FFFFFFu;
            sargb[i] = (a << 24) | rgb;
        }
        if (env && (vertex->flags & VERT_REFLECTIVE) != 0) {
            // shortcut: the lit colour is read back from the fogged one, so
            // a light brighter than white loses some of its reflection in
            // fog; keep the unfogged colour if that ever shows.
            const float keep = (float)fk * (1.0f / 256.0f);
            const float inv = keep > 0.0f ? 1.0f / (255.0f * keep) : 0.0f;
            const float lit[3] = {
                MIN(1.0f, (float)((argb >> 16) & 0xFFu) * inv),
                MIN(1.0f, (float)((argb >> 8) & 0xFFu) * inv),
                MIN(1.0f, (float)(argb & 0xFFu) * inv),
            };
            M_EnvVertex(batcher, i, vertex, mv, x, y, z, keep, lit);
        }
    }
#if defined(DC_EMIT_VERIFY)
    if (fast) {
        // The assembly places them all again and the two are compared; the
        // cel alpha may differ by one (the edge is rounded another way).
        static M_XVERT *saved = nullptr;
        static uint32_t *saved_s = nullptr;
        static int32_t saved_cap = 0;
        if (count > saved_cap) {
            Memory_Free(saved);
            Memory_Free(saved_s);
            saved_cap = count;
            saved = Memory_Alloc(sizeof(M_XVERT) * saved_cap);
            saved_s = Memory_Alloc(sizeof(uint32_t) * saved_cap);
        }
        memcpy(saved, xverts, sizeof(M_XVERT) * count);
        memcpy(saved_s, sargb, sizeof(uint32_t) * count);
        dc_xform_object(&run);
        const int32_t whole[1][2] = { { 0, count } };
        M_VerifyXform(saved, xverts, whole, 1);
        for (int32_t i = 0; cel && i < count; i++) {
            const uint32_t a = sargb[i];
            const uint32_t b = saved_s[i];
            if (!M_ArgbNear(a, b)) {
                m_XVerifyBad++;
                if (m_XVerifyBad <= 12) {
                    LOG_WARNING(
                        "[xform-verify] MISMATCH cel %d: asm %08x | C %08x",
                        (int)i, (unsigned)a, (unsigned)b);
                }
            }
        }
    }
#endif
}

// What xform_own_sh4.S takes: statics and sprites whose vertices share their
// flags (see dc_xform_own).
typedef struct {
    M_XFORM *c;
    M_XVERT *xverts;
    const OUTPUT_MESH_VERTEX *vertices;
    const uint16_t *vref;
    int32_t count;
    int32_t min_shade;
    const uint32_t *white; // nullptr: tinted
    float adder;
    uint32_t k0;
    int32_t mode; // 0: k0; 1: the vertex's shade; 2: adder + the shade
    int32_t fog;
    int32_t coloured; // TR3's ambient colour dimmed by the shade
    float tr3_double;
    float ambient[3];
    uint32_t tint[4]; // a, r, g, b in 8.8
    uint32_t *sargb; // the cel colours; nullptr: none
    uint32_t dark;
    // TR3's own light with no fog: each channel's whole factor (blue, green,
    // red, 0) by the shade's step.
    const uint16_t (*light)[4];
} M_XOWN;
_Static_assert(offsetof(M_XOWN, sargb) == 80, "xform_own_sh4.S reads these");
_Static_assert(offsetof(M_XOWN, light) == 88, "xform_own_sh4.S reads these");

// TR3's own light on a vertex depends on its shade (as a step of 32, 256 of
// them) and on what holds for the instance: the ambient colour, the curve's
// doubling and the tint. Tables of those are kept for the last few of them,
// which most statics of a room share frame after frame.
typedef struct {
    uint32_t key[7];
    bool valid;
    uint16_t factor[256][4];
} M_OWN_LIGHT;

static const uint16_t (*M_OwnLight(
    const float ambient[3], const float dbl, const uint32_t tr,
    const uint32_t tg, const uint32_t tb))[4]
{
    static M_OWN_LIGHT m_Tables[4];
    static int32_t m_Next = 0;
    uint32_t key[7];
    memcpy(key, ambient, 3 * sizeof(uint32_t));
    memcpy(&key[3], &dbl, sizeof(uint32_t));
    key[4] = tr;
    key[5] = tg;
    key[6] = tb;
    for (int32_t i = 0; i < 4; i++) {
        if (m_Tables[i].valid && memcmp(m_Tables[i].key, key, sizeof(key)) == 0) {
            return m_Tables[i].factor;
        }
    }
    M_OWN_LIGHT *const table = &m_Tables[m_Next++ & 3];
    memcpy(table->key, key, sizeof(key));
    table->valid = true;
    // The fog's share is 256 (none): kk is the curve's doubling alone.
    const float kk = 256.0f * dbl;
    const uint32_t tint[3] = { tr, tg, tb };
    for (int32_t step = 0; step < 256; step++) {
        const float f = (step <= 0 ? 255 : step) * (1.0f / 255.0f);
        for (int32_t ch = 0; ch < 3; ch++) {
            const float mul = M_Clamp(ambient[ch] * f, 0.0f, 1.0f);
            table->factor[step][2 - ch] =
                (uint16_t)((uint32_t)(mul * kk) * tint[ch] >> 8);
        }
        table->factor[step][3] = 0;
    }
    return table->factor;
}
_Static_assert(offsetof(M_XOWN, white) == 24, "xform_own_sh4.S reads these");
_Static_assert(offsetof(M_XOWN, mode) == 36, "xform_own_sh4.S reads these");
_Static_assert(offsetof(M_XOWN, tr3_double) == 48, "xform_own_sh4.S reads these");
_Static_assert(offsetof(M_XOWN, tint) == 64, "xform_own_sh4.S reads these");
extern void dc_xform_own(M_XOWN *run);

// Statics and sprites: meshes lit by their own light or not at all, with no
// room light, object light, reflection or coloured fog to add. M_XformGeneric's
// answer in integers, as the rooms' and objects'.
static void M_XformOwn(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const MESH_INSTANCE *const inst, const M_INSTANCE_LIGHT *const il,
    M_XFORM *const c, const float scale)
{
    const OUTPUT_MESH_VERTEX *const vertices =
        Vector_GetData(bind->mesh->vertices);
    M_XVERT *const xverts = batcher->xverts;
    const int32_t count = bind->vertex_count;
    const bool lighting = m_Frame.lighting;
    const int32_t min_shade = M_ShadeLow();
    const bool fog_on = c->fog;
    const bool tr3 = il->tr3;
    const float adder = il->adder;
    // TR3 doubles the lighting curve; the others take their factor from the
    // shade.
    const uint32_t dbl = tr3 && lighting ? (uint32_t)(256.0f * il->tr3_double)
                                         : 256;
    const uint32_t ta = M_TintK(inst->tint.a);
    const uint32_t tr = M_TintK(inst->tint.r);
    const uint32_t tg = M_TintK(inst->tint.g);
    const uint32_t tb = M_TintK(inst->tint.b);
    const bool white = bind->white && !bind->deforms && ta == 256 && tr == 256
        && tg == 256 && tb == 256;
    // Vertices that share their flags, and none a billboard or a sprite,
    // are done in assembly: the flags decide once what each vertex then
    // computes.
    // Only the bits that change what a vertex computes have to agree.
    const uint16_t decisive = VERT_NO_LIGHTING | VERT_NO_FOG
        | VERT_USE_OWN_LIGHT | VERT_BILLBOARD | VERT_ABS_SPRITE;
    const uint16_t shared = bind->flags_or;
    const bool fast = ((bind->flags_or ^ bind->flags_and) & decisive) == 0
        && !c->ortho && !il->caustics
        && (shared & (VERT_BILLBOARD | VERT_ABS_SPRITE)) == 0
        && (bind->vref == nullptr || count <= 0xFFFF);
    const bool own_coloured = tr3 && lighting
        && (shared & (VERT_NO_LIGHTING | VERT_USE_OWN_LIGHT))
            == VERT_USE_OWN_LIGHT;
    M_XOWN run = {
        .c = c,
        .xverts = xverts,
        .vertices = vertices,
        .vref = bind->vref,
        .count = count,
        .min_shade = min_shade,
        .white = white && !own_coloured ? m_WhiteARGB : nullptr,
        .adder = adder,
        .k0 = tr3 ? dbl
            : (shared & VERT_NO_LIGHTING) != 0 || !lighting
            ? ((shared & VERT_NO_LIGHTING) != 0
                   ? 256
                   : M_ShadeK(M_SHADE_NEUTRAL, min_shade))
            : 0,
        .mode = tr3 || (shared & VERT_NO_LIGHTING) != 0 || !lighting ? 0
            : (shared & VERT_USE_OWN_LIGHT) != 0                     ? 2
                                                                     : 1,
        .fog = fog_on && (shared & (VERT_NO_FOG | VERT_NO_LIGHTING)) == 0,
        .coloured = own_coloured,
        .tr3_double = il->tr3_double,
        .ambient = { il->tr3_ambient[0], il->tr3_ambient[1],
                     il->tr3_ambient[2] },
        .tint = { ta, tr, tg, tb },
        .sargb = batcher->cel ? batcher->sargb : nullptr,
        .dark = (uint32_t)(m_CelShade * 256.0f),
        .light = fast && own_coloured
            ? M_OwnLight(il->tr3_ambient, il->tr3_double, tr, tg, tb)
            : nullptr,
    };
#if !defined(DC_EMIT_VERIFY)
    if (fast) {
        dc_xform_own(&run);
        return;
    }
#endif
    for (int32_t i = 0; i < count; i++) {
        const OUTPUT_MESH_VERTEX *const vertex = M_Corner(bind, vertices, i);
        __builtin_prefetch(vertex + 2);
        const uint16_t flags = vertex->flags;
        float x = vertex->pos.x;
        float y = vertex->pos.y;
        float z = vertex->pos.z;
        float w1 = 1.0f;
        mat_trans_nodiv(x, y, z, w1);
        if ((flags & (VERT_BILLBOARD | VERT_ABS_SPRITE)) != 0) {
            x += vertex->normal.x * scale;
            y += vertex->normal.y * scale;
        }
        M_XVERT *const out = &xverts[i];
        uint8_t clip = M_Place(out, x, y, z, c);

        uint32_t k = 256;
        bool coloured = false;
        float mul[3];
        if (tr3) {
            k = dbl;
            if (lighting && (flags & (VERT_NO_LIGHTING | VERT_USE_OWN_LIGHT))
                    == VERT_USE_OWN_LIGHT) {
                // The ambient colour, dimmed by the vertex's shade.
                const int32_t step =
                    (int32_t)((M_SHADE_MAX - vertex->shade) * (1.0f / 32.0f));
                const float f = (step <= 0 ? 255 : step) * (1.0f / 255.0f);
                for (int32_t ch = 0; ch < 3; ch++) {
                    mul[ch] = M_Clamp(il->tr3_ambient[ch] * f, 0.0f, 1.0f);
                }
                coloured = true;
            }
        } else if ((flags & VERT_NO_LIGHTING) == 0) {
            int32_t s = M_SHADE_NEUTRAL;
            if (lighting) {
                s = (flags & VERT_USE_OWN_LIGHT) != 0
                    ? (int32_t)(adder + vertex->shade)
                    : (int32_t)vertex->shade;
            }
            if (il->caustics && lighting) {
                s += (int32_t)M_Caustic(vertex);
            }
            k = M_ShadeK(s, min_shade);
        }
        uint32_t fk = 256;
        if (fog_on && (flags & (VERT_NO_FOG | VERT_NO_LIGHTING)) == 0) {
            fk = M_FogKeep(M_Fog(x, y, z, c), &clip);
        }
        if (coloured) {
            const float kk = (float)fk * il->tr3_double;
            out->argb = M_Colour(
                vertex->color, ta, (uint32_t)(mul[0] * kk) * tr >> 8,
                (uint32_t)(mul[1] * kk) * tg >> 8,
                (uint32_t)(mul[2] * kk) * tb >> 8);
        } else {
            k = k * fk >> 8;
            out->argb = white ? m_WhiteARGB[k]
                              : M_Colour(
                                    vertex->color, ta, k * tr >> 8,
                                    k * tg >> 8, k * tb >> 8);
        }
        out->clip = clip;
        if (run.sargb != nullptr) {
            // Darkened as the lit colour is, no light on it: alpha 0.
            run.sargb[i] = M_ScaleRGB8(out->argb, run.dark) & 0x00FFFFFFu;
        }
    }
#if defined(DC_EMIT_VERIFY)
    if (fast) {
        // The assembly places them all again and the two are compared.
        static M_XVERT *saved = nullptr;
        static uint32_t *saved_s = nullptr;
        static int32_t saved_cap = 0;
        if (count > saved_cap) {
            Memory_Free(saved);
            Memory_Free(saved_s);
            saved_cap = count;
            saved = Memory_Alloc(sizeof(M_XVERT) * saved_cap);
            saved_s = Memory_Alloc(sizeof(uint32_t) * saved_cap);
        }
        memcpy(saved, xverts, sizeof(M_XVERT) * count);
        if (run.sargb != nullptr) {
            memcpy(saved_s, run.sargb, sizeof(uint32_t) * count);
        }
        dc_xform_own(&run);
        const int32_t whole[1][2] = { { 0, count } };
        M_VerifyXform(saved, xverts, whole, 1);
        for (int32_t i = 0; run.sargb != nullptr && i < count; i++) {
            if (!M_ArgbNear(run.sargb[i], saved_s[i])) {
                m_XVerifyBad++;
                if (m_XVerifyBad <= 12) {
                    LOG_WARNING(
                        "[xform-verify] MISMATCH own cel %d: asm %08x | C %08x",
                        (int)i, (unsigned)run.sargb[i], (unsigned)saved_s[i]);
                }
            }
        }
    }
#endif
}

// Everything else: sprites, meshes lit by their own light or not at all,
// coloured fog, reflections, the UI's models. The desktop shader's lighting
// in floats, a vertex at a time.
static void M_XformGeneric(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const MESH_INSTANCE *const inst, const M_INSTANCE_LIGHT *const il,
    M_XFORM *const c, const float mv[4][4], const float scale,
    const float cel_scale, const bool black_fog)
{
    const OUTPUT_MESH_VERTEX *const vertices =
        Vector_GetData(bind->mesh->vertices);
    const bool cel = batcher->cel;
    const bool env = batcher->env;
    batcher->oargb_used = !black_fog;
    for (int32_t i = 0; i < bind->vertex_count; i++) {
        const OUTPUT_MESH_VERTEX *const vertex = M_Corner(bind, vertices, i);
        float x = vertex->pos.x;
        float y = vertex->pos.y;
        float z = vertex->pos.z;
        float w1 = 1.0f;
        mat_trans_nodiv(x, y, z, w1);
        if ((vertex->flags & (VERT_BILLBOARD | VERT_ABS_SPRITE)) != 0) {
            x += vertex->normal.x * scale;
            y += vertex->normal.y * scale;
        }
        M_XVERT *const out = &batcher->xverts[i];
        uint8_t clip = M_Place(out, x, y, z, c);

        float fog = 0.0f;
        if (c->fog
            && (vertex->flags & (VERT_NO_FOG | VERT_NO_LIGHTING)) == 0) {
            fog = M_Fog(x, y, z, c);
        }
        if (fog >= 1.0f) {
            clip |= M_CLIP_FOG;
        }

        float rgba[4];
        float mul;
        M_LightVertex(vertex, il, &inst->wmatrix, inst->tint, rgba, &mul);
        const float keep = 1.0f - fog;
        out->argb =
            M_PackARGB(rgba[3], rgba[0] * keep, rgba[1] * keep, rgba[2] * keep);
        if (fog > 0.0f) {
            clip |= M_CLIP_FOGGED;
        }
        if (!black_fog) {
            batcher->oargb[i] = fog > 0.0f
                ? M_PackARGB(
                      1.0f, m_Frame.fog_color[0] * fog * inst->tint.r,
                      m_Frame.fog_color[1] * fog * inst->tint.g,
                      m_Frame.fog_color[2] * fog * inst->tint.b)
                : 0;
        }
        out->clip = clip;
        if (cel) {
            float shade = 0.0f;
            if ((vertex->flags & (VERT_USE_OBJECT_LIGHT | VERT_NO_LIGHTING))
                == VERT_USE_OBJECT_LIGHT) {
                const float cos = fipr(il->l[0], il->l[1], il->l[2], 0.0f,
                                      vertex->normal.x, vertex->normal.y,
                                      vertex->normal.z, 0.0f)
                    * cel_scale;
                shade = 0.5f + 0.5f * (cos - M_CEL_EDGE);
            }
            const float dark = keep * m_CelShade;
            batcher->sargb[i] = M_PackARGB(
                shade, rgba[0] * dark, rgba[1] * dark, rgba[2] * dark);
        }
        if (env && (vertex->flags & VERT_REFLECTIVE) != 0) {
            const float lit[3] = {
                M_Clamp(rgba[0], 0.0f, 1.0f),
                M_Clamp(rgba[1], 0.0f, 1.0f),
                M_Clamp(rgba[2], 0.0f, 1.0f),
            };
            M_EnvVertex(batcher, i, vertex, mv, x, y, z, keep, lit);
        }
    }
}

// Cel shading only this near the camera: further off, an enemy is a few
// pixels, and its shaded side a second set of its faces for nothing.
#define M_CEL_RANGE (6.0f * WALL_L)

static void M_GrowVertexBuffers(MESH_BATCHER *const batcher, const int32_t count)
{
    batcher->xverts_capacity = count + 256;
    const int32_t n = batcher->xverts_capacity;
    // From the heap, as at boot, not the level's region: they outlive the
    // level, and they grow while it loads (the transparency of each object
    // mesh is found by drawing it), at the load's peak, when the region has
    // only holes left; two of them at 4.6 MB cut TR3's next free span from
    // 6.2 to 4.6 MB (DC builds 311-318).
    Memory_Free(batcher->xverts);
    batcher->xverts = memalign(32, n * sizeof(M_XVERT));
    Memory_Free(batcher->soft);
    batcher->soft = calloc(n, sizeof(*batcher->soft));
    Memory_Free(batcher->soft_off);
    batcher->soft_off = calloc(n, sizeof(*batcher->soft_off));
    Memory_Free(batcher->oargb);
    batcher->oargb = calloc(n, sizeof(*batcher->oargb));
    Memory_Free(batcher->sargb);
    batcher->sargb = calloc(n, sizeof(*batcher->sargb));
    Memory_Free(batcher->env_uv);
    batcher->env_uv = calloc(n, sizeof(*batcher->env_uv));
    Memory_Free(batcher->eargb);
    batcher->eargb = calloc(n, sizeof(*batcher->eargb));
}

// Per vertex of the frame being drawn, and up to a few megabytes in the
// fullest levels: a save lets it go so that the saved game's tree fits, and the
// next frame takes back what it needs (M_GrowVertexBuffers).
void MeshBatcher_ReleaseScratch(MESH_BATCHER *const batcher)
{
    if (batcher == nullptr) {
        return;
    }
    Memory_Free(batcher->xverts);
    batcher->xverts = nullptr;
    Memory_Free(batcher->soft);
    batcher->soft = nullptr;
    Memory_Free(batcher->soft_off);
    batcher->soft_off = nullptr;
    Memory_Free(batcher->oargb);
    batcher->oargb = nullptr;
    Memory_Free(batcher->sargb);
    batcher->sargb = nullptr;
    Memory_Free(batcher->env_uv);
    batcher->env_uv = nullptr;
    Memory_Free(batcher->eargb);
    batcher->eargb = nullptr;
    Memory_Free(batcher->share_slot);
    batcher->share_slot = nullptr;
    Memory_Free(batcher->cel_buf);
    batcher->cel_buf = nullptr;
    batcher->xverts_capacity = 0;
    batcher->share_capacity = 0;
    batcher->share_gen = 0;
    Vector_Clear(batcher->cel_runs);
    batcher->cel_count = 0;
    batcher->cel_capacity = 0;
}

// The instance just transformed goes out with its opaque runs whole.
static void M_WholeRanges(
    MESH_BATCHER *const batcher, const OUTPUT_MESH *const mesh)
{
    const int32_t clean = mesh->opaque_nodiscard_count;
    batcher->ranges[0] = (M_RANGE) {
        .start = 0,
        .count = clean,
        .run = 0,
        .z_near = batcher->z_near,
    };
    batcher->ranges[1] = (M_RANGE) {
        .start = clean,
        .count = mesh->opaque_vertex_indices->count - clean,
        .run = 1,
        .z_near = batcher->z_near,
    };
    batcher->range_count = 2;
}

// The depth the PowerVR takes for a perspective vertex of 1/w in the current
// layer (DC_PVR_GetDepth3D), worked out here per vertex.
M_HOT float M_Depth(const float inv_w)
{
    float v = inv_w > 1.0f ? 1.0f : inv_w;
    if (v < m_Frame.depth.lo) {
        v = m_Frame.depth.lo;
    }
    return v * m_Frame.depth.scale;
}

// Whether a sphere about an eye space point can show: not wholly past the
// fog, before the near plane or past a side. Every face within one that
// cannot would be culled corner by corner (M_CLIP_CULL).
static bool M_SphereShows(
    const float eye[3], const float radius, const M_XFORM *const c)
{
    if (m_Frame.fog) {
        const float reach = m_Frame.fog_end + radius;
        if (eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]
            > reach * reach) {
            return false;
        }
    }
    if (eye[2] + radius < c->near_z) {
        return false;
    }
    if (c->sides) {
        for (int32_t p = 0; p < 4; p++) {
            const float a = c->planes[p][0];
            const float b = c->planes[p][1];
            const float d = a * eye[p < 2 ? 0 : 1] + b * eye[2];
            if (d > 0.0f && d * d > radius * radius * (a * a + b * b)) {
                return false;
            }
        }
    }
    return true;
}

// Picks the chunks of a room that show, and gathers their faces into the
// ranges and their vertices (with the ones always transformed) into the
// vertex ranges. Returns how many vertex ranges, or -1 when every chunk shows
// and the room goes out whole.
static int32_t M_CullChunks(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const float mv[4][4], const float scale, const M_XFORM *const c)
{
    bool shows[M_MAX_CHUNKS];
    float z_near[M_MAX_CHUNKS];
    int32_t shown = 0;
    for (int32_t k = 0; k < bind->chunk_count; k++) {
        const M_CHUNK *const chunk = &bind->chunks[k];
        const float *const p = chunk->center;
        float eye[3];
        for (int32_t r = 0; r < 3; r++) {
            eye[r] = mv[0][r] * p[0] + mv[1][r] * p[1] + mv[2][r] * p[2]
                + mv[3][r];
        }
        const float radius = chunk->radius * scale;
        shows[k] = M_SphereShows(eye, radius, c);
        z_near[k] = eye[2] - radius;
        shown += shows[k] ? 1 : 0;
    }

    // Chunk by chunk even when all show: each is drawn as it is transformed.
    int32_t nvr = 0;
    for (int32_t k = 0; k < bind->chunk_count; k++) {
        const M_CHUNK *const chunk = &bind->chunks[k];
        if (shows[k] && chunk->vert_count > 0) {
            batcher->vranges[nvr][0] = chunk->vert_start;
            batcher->vranges[nvr][1] = chunk->vert_count;
            batcher->vrange_chunk[nvr] = (int16_t)k;
            nvr++;
        }
    }
    if (bind->always_count > 0) {
        batcher->vranges[nvr][0] = bind->always_start;
        batcher->vranges[nvr][1] = bind->always_count;
        batcher->vrange_chunk[nvr] = -1;
        nvr++;
    }

    int32_t n = 0;
    for (int32_t run = 0; run < 2; run++) {
        for (int32_t k = 0; k < bind->chunk_count; k++) {
            const M_CHUNK *const chunk = &bind->chunks[k];
            if (shows[k] && chunk->count[run] > 0) {
                batcher->ranges[n++] = (M_RANGE) {
                    .start = chunk->start[run],
                    .count = chunk->count[run],
                    .run = run,
                    .z_near = z_near[k],
                };
            }
        }
    }
    batcher->range_count = n;
    return nvr;
}

static void M_EmitIndices(
    MESH_BATCHER *batcher, const M_BINDING *bind,
    const OUTPUT_MESH_INDEX *indices, int32_t count, DC_BLEND blend,
    int32_t *page);

// The rooms' cel shading of count vertices just transformed from start (see
// M_ROOM_CEL_EDGE): each one's shaded colour and how far into the shade it
// lies as its alpha, and those in the shade going out dark already.
static void M_RoomCel(
    MESH_BATCHER *const batcher, const int32_t start, const int32_t count)
{
    M_XVERT *const xv = batcher->xverts;
    uint32_t *const sargb = batcher->sargb;
    for (int32_t i = start; i < start + count; i++) {
        if (xv[i].z >= M_ROOM_CEL_RANGE) {
            // Lit, in its own colour (a face turned from a light keeps it).
            sargb[i] = xv[i].argb & 0x00FFFFFFu;
            continue;
        }
        const float k =
            M_Clamp((M_ROOM_CEL_RANGE - xv[i].z) / M_ROOM_CEL_FADE, 0.0f, 1.0f);
        const uint32_t dark =
            (uint32_t)(256.0f * (1.0f - (1.0f - m_CelShade) * k));
        const uint32_t argb = xv[i].argb;
        const int32_t luma = (int32_t)((((argb >> 16) & 0xFF) * 77
                                        + ((argb >> 8) & 0xFF) * 151
                                        + (argb & 0xFF) * 28)
                                       >> 8);
        // Half the alpha at the edge, in step with the brightness (and at
        // half its slope, so that it never clips): the PowerVR runs both
        // across a face alike, and the cut lies where its light crosses the
        // edge.
        const uint32_t a = (uint32_t)(128 + (M_ROOM_CEL_EDGE - luma) / 2);
        const uint32_t shade = M_ScaleRGB8(argb, dark) & 0x00FFFFFFu;
        sargb[i] = (a << 24) | shade;
        if (a >= 128) {
            xv[i].argb = (argb & 0xFF000000u) | shade;
            if (batcher->face_light_count > 0) {
                // In the shade without the dynamic lights too.
                const uint32_t n = batcher->eargb[i];
                batcher->eargb[i] = (n & 0xFF000000u) | M_ScaleRGB8(n, dark);
            }
        }
    }
}

// The dynamic lights that reach a room (il, in world space), in view space
// for M_FaceTurnedAway: the room's world matrix undone, its model-view done.
static void M_PrepareFaceLights(
    MESH_BATCHER *const batcher, const M_INSTANCE_LIGHT *const il,
    const float mv[4][4])
{
    batcher->face_light_count = 0;
    const float(*const w)[4] = il->world;
    // The inverse of the matrix's rotation and scale, by its cofactors.
    const float det = w[0][0] * (w[1][1] * w[2][2] - w[1][2] * w[2][1])
        - w[0][1] * (w[1][0] * w[2][2] - w[1][2] * w[2][0])
        + w[0][2] * (w[1][0] * w[2][1] - w[1][1] * w[2][0]);
    if (!il->has_room_lights || fabsf(det) < 1.0e-6f) {
        return;
    }
    const float k = 1.0f / det;
    const float inv[3][3] = {
        { (w[1][1] * w[2][2] - w[1][2] * w[2][1]) * k,
          (w[0][2] * w[2][1] - w[0][1] * w[2][2]) * k,
          (w[0][1] * w[1][2] - w[0][2] * w[1][1]) * k },
        { (w[1][2] * w[2][0] - w[1][0] * w[2][2]) * k,
          (w[0][0] * w[2][2] - w[0][2] * w[2][0]) * k,
          (w[0][2] * w[1][0] - w[0][0] * w[1][2]) * k },
        { (w[1][0] * w[2][1] - w[1][1] * w[2][0]) * k,
          (w[0][1] * w[2][0] - w[0][0] * w[2][1]) * k,
          (w[0][0] * w[1][1] - w[0][1] * w[1][0]) * k },
    };
    const int32_t total = il->num_lights + il->num_rgb;
    for (int32_t i = 0; i < total; i++) {
        if (batcher->face_light_count >= M_ROOM_CEL_LIGHTS) {
            break;
        }
        const bool rgb = i >= il->num_lights;
        const float *const pos =
            rgb ? il->rgb[i - il->num_lights].pos : il->lights[i].pos;
        const float radius_sq = rgb
            ? il->rgb[i - il->num_lights].radius
                * il->rgb[i - il->num_lights].radius
            : il->lights[i].radius_sq;
        const float d[3] = { pos[0] - w[0][3], pos[1] - w[1][3],
                             pos[2] - w[2][3] };
        float local[3];
        for (int32_t r = 0; r < 3; r++) {
            local[r] = inv[r][0] * d[0] + inv[r][1] * d[1] + inv[r][2] * d[2];
        }
        float *const out = batcher->face_lights[batcher->face_light_count++];
        for (int32_t r = 0; r < 3; r++) {
            out[r] = mv[0][r] * local[0] + mv[1][r] * local[1]
                + mv[2][r] * local[2] + mv[3][r];
        }
        out[3] = radius_sq;
    }
}

// A room chunk's opaque runs, as M_DrawInstance emits the ranges of a whole
// instance.
static void M_EmitChunk(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const M_CHUNK *const chunk, const bool faded)
{
    const OUTPUT_MESH_INDEX *const opaque =
        Vector_GetData(bind->mesh->opaque_vertex_indices);
    for (int32_t run = 0; run < 2; run++) {
        if (chunk->count[run] == 0) {
            continue;
        }
        int32_t page = INT32_MIN;
        M_EmitIndices(
            batcher, bind, opaque + chunk->start[run], chunk->count[run],
            faded          ? DC_BLEND_ALPHA
                : run == 0 ? DC_BLEND_OPAQUE
                           : DC_BLEND_PUNCH,
            &page);
    }
}

static bool M_TransformInstance(
    MESH_BATCHER *const batcher, const MESH_INSTANCE *const inst,
    M_BINDING *const bind)
{
    if (bind->vertex_count > batcher->xverts_capacity) {
        M_GrowVertexBuffers(batcher, bind->vertex_count);
    }

    float mv[4][4];
    float scale;
    M_ModelView(&inst->wmatrix, mv, &scale);

    M_XFORM c = {
        .ortho = m_Frame.ortho,
        .sides = true,
        .near = true,
        .fog = m_Frame.fog,
        .proj_x = m_Frame.proj_x,
        .proj_y = m_Frame.proj_y,
        .near_z = m_Frame.near_z,
        .fog_start = m_Frame.fog_start,
        .fog_start_sq = m_Frame.fog_start * m_Frame.fog_start,
        .fog_scale = m_Frame.fog_scale,
    };
    // The sides faces are dropped past, about the screen's middle: the
    // screen's, or a room's own window onto it.
    c.left = -m_Frame.half_w;
    c.right = m_Frame.half_w;
    c.top = -m_Frame.half_h;
    c.bottom = m_Frame.half_h;
    if (bind->needs_room_lights && inst->scissor.width > 0) {
        const VIEWPORT_RECT *const s = &inst->scissor;
        c.left = MAX(c.left, s->x - m_Frame.center_x - M_CLIP_MARGIN);
        c.right =
            MIN(c.right, s->x + s->width - m_Frame.center_x + M_CLIP_MARGIN);
        c.top = MAX(c.top, s->y - s->height - m_Frame.center_y - M_CLIP_MARGIN);
        c.bottom = MIN(c.bottom, s->y - m_Frame.center_y + M_CLIP_MARGIN);
    }
    {
        const float px = m_Frame.proj_x;
        const float py = m_Frame.proj_y;
        const float planes[4][2] = {
            { -px, c.left }, { px, -c.right }, { -py, c.top }, { py, -c.bottom },
        };
        memcpy(c.planes, planes, sizeof(planes));
    }

    float eye_sq = 0.0f;
    batcher->z_near = -FLT_MAX;
    batcher->vrange_count = -1;
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
        eye_sq = eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2];
        batcher->z_near = eye[2] - radius;
        // Wholly past the end of the fog: nothing of it would show.
        const float reach = m_Frame.fog_end + radius;
        if (m_Frame.fog && eye_sq > reach * reach) {
            return false;
        }
        // Wholly before the fog starts: no vertex needs the fog worked out.
        const float clear = m_Frame.fog_start - radius;
        if (clear > 0.0f && eye_sq < clear * clear) {
            c.fog = false;
        }
        // Wholly past the near plane: no vertex can lie before it. (Sprites'
        // corners stand off their middle in x and y only.)
        c.near = eye[2] - radius < m_Frame.near_z;
        // Wholly past one of the sides, or wholly within them all. Each
        // side is a plane through the eye, sx = side for x:
        // proj_x * x - side * z = 0, the same for y.
        if (!bind->billboard) {
            bool inside = true;
            for (int32_t p = 0; p < 4; p++) {
                const float a = c.planes[p][0];
                const float b = c.planes[p][1];
                const float d = a * eye[p < 2 ? 0 : 1] + b * eye[2];
                const float rr = radius * radius * (a * a + b * b);
                if (d > 0.0f && d * d > rr) {
                    return false;
                }
                if (!(d < 0.0f && d * d > rr)) {
                    inside = false;
                }
            }
            c.sides = !inside;
        }
    }

    g_DC_BatcherFrameInstances++;
    M_WholeRanges(batcher, bind->mesh);
    M_INSTANCE_LIGHT il;
    M_PrepareInstanceLight(inst, bind, mv, &il);

    // Cel shaded: for each vertex how far it turns from the light, as a
    // cosine. The light runs across the lit side as before, so that it
    // keeps its roundness; only the shade is cut hard.
    batcher->cel = m_Frame.cel && m_Frame.lighting && !m_Frame.ortho
        && m_Frame.lara_instance && bind->needs_object_light
        && il.has_direction
        && eye_sq < M_CEL_RANGE * M_CEL_RANGE;
    batcher->cel_dark = false;
    batcher->face_light_count = 0;
    float cel_scale = 0.0f;
    if (batcher->cel) {
        if (bind->normal_len == 0.0f) {
            const OUTPUT_MESH_VERTEX *const vs =
                Vector_GetData(bind->mesh->vertices);
            float len_sq = 0.0f;
            for (int32_t i = 0; i < bind->vertex_count; i++) {
                const float x = M_Corner(bind, vs, i)->normal.x;
                const float y = M_Corner(bind, vs, i)->normal.y;
                const float z = M_Corner(bind, vs, i)->normal.z;
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

    const bool black_fog = m_Frame.fog_color[0] * inst->tint.r == 0.0f
        && m_Frame.fog_color[1] * inst->tint.g == 0.0f
        && m_Frame.fog_color[2] * inst->tint.b == 0.0f;
    batcher->env = bind->env_count > 0 && !m_Frame.ortho;
    batcher->oargb_used = false;

    memcpy(batcher->mv, mv, sizeof(batcher->mv));
    mat_load(&batcher->mv);

    int32_t transformed = bind->vertex_count;
    if (bind->needs_room_lights && !bind->needs_object_light
        && !bind->needs_own_light && !bind->billboard && black_fog
        && !m_Frame.ortho && !batcher->env && !batcher->cel) {
        // Not on a room fading out: it keeps no shaded side (M_DrawInstance).
        batcher->cel = m_Frame.cel && m_Frame.room_cel && m_Frame.lighting
            && inst->tint.a >= 1.0f;
        batcher->cel_dark = batcher->cel;
        if (batcher->cel) {
            M_PrepareFaceLights(batcher, &il, mv);
        }
        const int32_t nvr = bind->chunk_count > 0
            ? M_CullChunks(batcher, bind, mv, scale, &c)
            : -1;
        batcher->vrange_count = nvr;
        if (nvr >= 0) {
            // Each chunk drawn as soon as it is transformed: a room's vertices
            // are many times the data cache, and emitted after them all, every
            // corner missed it. (Flycast models no cache: nothing to measure
            // there; for the console.)
            transformed = 0;
            const bool faded = inst->tint.a < 1.0f;
            batcher->cel_emit = batcher->cel && !faded;
            for (int32_t r = 0; r < nvr; r++) {
                transformed += batcher->vranges[r][1];
                DC_PROBE(
                    DC_PROBE_XT_ROOM,
                    M_XformRoom(
                        batcher, bind, inst, &il, &c, &batcher->vranges[r], 1));
                if (batcher->cel_dark) {
                    M_RoomCel(
                        batcher, batcher->vranges[r][0],
                        batcher->vranges[r][1]);
                }
                const int32_t k = batcher->vrange_chunk[r];
                if (k < 0) {
                    continue;
                }
                const uint64_t emit_start = timer_us_gettime64();
                M_EmitChunk(batcher, bind, &bind->chunks[k], faded);
                batcher->chunk_emit_us +=
                    (uint32_t)(timer_us_gettime64() - emit_start);
            }
            batcher->emitted = true;
        } else {
            DC_PROBE(
                DC_PROBE_XT_ROOM,
                M_XformRoom(
                    batcher, bind, inst, &il, &c, batcher->vranges, nvr));
            if (batcher->cel_dark) {
                M_RoomCel(batcher, 0, bind->vertex_count);
            }
        }
        g_DC_ProbeUs[DC_PROBE_XF_LIT] += transformed; // TEMP
    } else if (
        bind->plain_object && m_Frame.lighting && black_fog && !m_Frame.ortho
        && !il.caustics) {
        g_DC_ProbeUs[DC_PROBE_XF_PLAIN] += bind->vertex_count; // TEMP
        DC_PROBE(
            DC_PROBE_XT_PLAIN,
            M_XformObject(batcher, bind, inst, &il, &c, mv, cel_scale));
    } else {
        g_DC_ProbeUs[DC_PROBE_XF_GENERIC] += bind->vertex_count; // TEMP
        g_DC_ProbeUs[bind->needs_own_light ? DC_PROBE_XG_OWN
                     : batcher->env        ? DC_PROBE_XG_ENV
                     : !black_fog          ? DC_PROBE_XG_FOG
                     : bind->billboard     ? DC_PROBE_XG_BILL
                                           : DC_PROBE_XG_OTHER] +=
            bind->vertex_count; // TEMP
        DC_PROBE_START(probe_gen);
        if (!bind->needs_room_lights && !bind->needs_object_light && black_fog
            && !m_Frame.ortho && !batcher->env) {
            M_XformOwn(batcher, bind, inst, &il, &c, scale);
        } else {
            M_XformGeneric(
                batcher, bind, inst, &il, &c, mv, scale, cel_scale, black_fog);
        }
        DC_PROBE_STOP(DC_PROBE_XT_GENERIC, probe_gen);
    }
    // The nearest vertex, once for the instance: the layers drawn over the
    // world start above it.
    if (m_Frame.depth.track && c.max_inv_z > 0.0f) {
        DC_PVR_UseDepth(M_Depth(m_Frame.near_z * c.max_inv_z));
    }
    g_DC_BatcherFrameVertices += transformed;
    m_FrameVertices += transformed;
    return true;
}

// What the faces going out stand for, which decides their colours.
typedef enum {
    M_EMIT_PLAIN, // the faces themselves, in their lit colours
    M_EMIT_ENV, // the reflections: the environment map, added
    M_EMIT_BUMP, // the relief: white, with the offset colour lighting it
} M_EMIT;

// The colours a vertex of the instance goes out in, by the pass.
M_HOT void M_Colours(
    const MESH_BATCHER *const batcher, const int32_t idx, uint32_t *const argb,
    uint32_t *const oargb)
{
    switch (m_Frame.emit) {
    case M_EMIT_ENV:
        *argb = batcher->eargb[idx];
        *oargb = 0;
        return;
    case M_EMIT_BUMP:
        *argb = 0xFFFFFFFFu;
        *oargb = m_Frame.bump_k;
        for (int32_t c = 0; c < m_Frame.bump_n; c++) {
            if (m_Frame.bump_idx[c] == idx) {
                *oargb = m_Frame.bump_vk[c];
                break;
            }
        }
        return;
    default: {
        const M_XVERT *const xv = &batcher->xverts[idx];
        *argb = xv->argb;
        *oargb = batcher->oargb_used ? batcher->oargb[idx]
            : (xv->clip & M_CLIP_FOGGED)  ? 0xFF000000u
                                          : 0;
    }
    }
}

// How much nearer than its own the depth of what is projected is taken to be
// (see M_DEPTH_BIAS_BUMP).
static float m_DepthBias = 1.0f;

// A transformed vertex onto the screen, straight into a list's buffer.
M_HOT void M_Project(
    const M_XVERT *const in, const float u, const float v, const uint32_t argb,
    const uint32_t oargb, const uint32_t flags, pvr_vertex_t *const out)
{
    out->flags = flags;
    if (m_Frame.ortho) {
        const float(*const s) = m_Frame.ortho_scale;
        const float(*const o) = m_Frame.ortho_offset;
        out->x = (m_Frame.center_x + (s[0] * in->x + o[0]) * m_Frame.half_w)
            * m_Frame.screen_scale;
        out->y = (m_Frame.center_y - (s[1] * in->y + o[1]) * m_Frame.half_h)
            * m_Frame.screen_scale;
        // -1 at the near end; the PowerVR wants the nearer bigger.
        out->z = DC_PVR_Depth2D(0.5f - 0.5f * (s[2] * in->z + o[2]));
    } else {
        out->x = (m_Frame.center_x + in->sx) * m_Frame.screen_scale;
        out->y = (m_Frame.center_y + in->sy) * m_Frame.screen_scale;
        out->z = M_Depth(m_Frame.near_z * in->inv_z * m_DepthBias);
    }
    out->u = u;
    out->v = v;
    out->argb = argb;
    out->oargb = oargb;
}

// A vertex of a face being clipped: transformed, with its texture
// coordinates and colours alongside.
typedef struct {
    M_XVERT xv;
    float u;
    float v;
    uint32_t argb;
    uint32_t oargb;
    uint32_t sargb;
} M_CLIP_VERT;

static M_CLIP_VERT M_LoadClipVert(
    const MESH_BATCHER *const batcher, const int32_t idx, const float u,
    const float v)
{
    M_CLIP_VERT out = { .xv = batcher->xverts[idx], .u = u, .v = v };
    M_Colours(batcher, idx, &out.argb, &out.oargb);
    out.sargb = batcher->cel_emit ? batcher->sargb[idx] : 0;
    return out;
}

static M_CLIP_VERT M_ClipEdge(
    const M_CLIP_VERT *const a, const M_CLIP_VERT *const b)
{
    const float t = (m_Frame.near_z - a->xv.z) / (b->xv.z - a->xv.z);
    const float x = a->xv.x + (b->xv.x - a->xv.x) * t;
    const float y = a->xv.y + (b->xv.y - a->xv.y) * t;
    const float inv_z = 1.0f / m_Frame.near_z;
    const int32_t ti = (int32_t)(t * 256.0f + 0.5f);
    return (M_CLIP_VERT) {
        .xv = {
            .x = x,
            .y = y,
            .z = m_Frame.near_z,
            .sx = m_Frame.proj_x * x * inv_z,
            .sy = m_Frame.proj_y * y * inv_z,
            .inv_z = inv_z,
        },
        .u = a->u + (b->u - a->u) * t,
        .v = a->v + (b->v - a->v) * t,
        .argb = M_LerpARGB(a->argb, b->argb, ti),
        .oargb = M_LerpARGB(a->oargb, b->oargb, ti),
        .sargb = M_LerpARGB(a->sargb, b->sargb, ti),
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

// Reads a face's texture coordinates (pulled towards their middle already,
// when filtering bilinearly: M_InsetRun).
M_HOT void M_GetFaceUVs(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const OUTPUT_MESH_INDEX *const idx, const int32_t n, float uv[][2])
{
    if (m_Frame.emit == M_EMIT_ENV) {
        for (int32_t k = 0; k < n; k++) {
            uv[k][0] = batcher->env_uv[idx[k]][0];
            uv[k][1] = batcher->env_uv[idx[k]][1];
        }
        return;
    }
    for (int32_t k = 0; k < n; k++) {
        uv[k][0] = bind->uv[idx[k] * 2] * (1.0f / M_UV_ONE);
        uv[k][1] = bind->uv[idx[k] * 2 + 1] * (1.0f / M_UV_ONE);
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

// The cel shading's shaded side of a face just emitted: the same vertices
// in the shaded colours, kept for the punch-through list until the instance
// is done (M_FlushCel). Nothing is projected again.
static void M_KeepCel(
    MESH_BATCHER *const batcher, const pvr_vertex_t *const src,
    const int32_t n, const uint32_t sargb[])
{
    if (batcher->cel_count + n > batcher->cel_capacity) {
        batcher->cel_capacity = (batcher->cel_count + n) * 2;
        batcher->cel_buf = Memory_Realloc(
            batcher->cel_buf, batcher->cel_capacity * sizeof(pvr_vertex_t));
    }
    if (batcher->cel_runs->count == 0
        || batcher->cel_page != batcher->emit_page) {
        const M_CEL_RUN fresh = {
            .page = batcher->emit_page,
            .start = batcher->cel_count,
        };
        Vector_Add(batcher->cel_runs, &fresh);
        batcher->cel_page = batcher->emit_page;
    }
    pvr_vertex_t *const dst = &batcher->cel_buf[batcher->cel_count];
    for (int32_t k = 0; k < n; k++) {
        dst[k] = src[k];
        dst[k].argb = sargb[k];
        dst[k].oargb = 0;
    }
    batcher->cel_count += n;
}

static void M_FlushCel(MESH_BATCHER *const batcher)
{
    const M_CEL_RUN *const runs = Vector_GetData(batcher->cel_runs);
    const int32_t num_runs = batcher->cel_runs->count;
    for (int32_t i = 0; i < num_runs; i++) {
        const M_CEL_RUN *const run = &runs[i];
        const int32_t end =
            i + 1 < num_runs ? runs[i + 1].start : batcher->cel_count;
        const int32_t count = end - run->start;
        DC_PVR_SetState(
            DC_BLEND_PUNCH_SHADE,
            run->page == M_NO_PAGE ? DC_PVR_NO_TEXTURE : run->page, true);
        pvr_vertex_t *const out = DC_PVR_ReservePoly(count);
        if (out == nullptr) {
            continue;
        }
        memcpy(out, &batcher->cel_buf[run->start], count * sizeof(*out));
        DC_PVR_CommitPoly(count);
    }
    Vector_Clear(batcher->cel_runs);
    batcher->cel_count = 0;
}

// Whether the cel shading has anything to draw on a face: a face whose
// corners all lie under half the alpha is lit all over.
M_HOT bool M_CelShows(const uint32_t sargb_or)
{
    return sargb_or >= M_CEL_CUT;
}

// Whether a face keeps its shaded side (M_KeepCel), from its corners' cel
// colours or-ed and and-ed: one that shows, unless a room's corners are all in
// the shade, which went out dark already (M_RoomCel). As emit_sh4.S's
// CEL_FACE.
M_HOT bool M_CelKeeps(
    const MESH_BATCHER *const batcher, const uint32_t sargb_or,
    const uint32_t sargb_and)
{
    return M_CelShows(sargb_or)
        && !(batcher->cel_dark && sargb_and >= M_CEL_CUT);
}

// How far a room face (corners a, b, c in view space) is turned from the
// dynamic lights that reach it (M_PrepareFaceLights), 0 to 256: by the cosine
// between it and the light, the camera's side of its plane counting as its
// front whichever way it winds. A face square to the light takes it all (0),
// one edge-on half of it, and one turned 30 degrees past that none: it keeps
// its colour without those lights (eargb, M_XformRoom). The light that lights
// it most counts; 0 when none reaches it.
M_HOT uint32_t M_FaceTurnedAway(
    const MESH_BATCHER *const batcher, const M_XVERT *const a,
    const M_XVERT *const b, const M_XVERT *const c)
{
    const float e1[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
    const float e2[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
    const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1],
                         e1[2] * e2[0] - e1[0] * e2[2],
                         e1[0] * e2[1] - e1[1] * e2[0] };
    // The camera stands at the view's origin.
    const float eye = -(n[0] * a->x + n[1] * a->y + n[2] * a->z);
    const float n_sq = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    if (n_sq <= 0.0f) {
        return 0;
    }
    const float inv_n = eye < 0.0f ? -M_InvSqrt(n_sq) : M_InvSqrt(n_sq);
    float away = 2.0f;
    for (int32_t i = 0; i < batcher->face_light_count; i++) {
        const float *const l = batcher->face_lights[i];
        const float d[3] = { l[0] - a->x, l[1] - a->y, l[2] - a->z };
        // Reached by its nearest corner: a wall or a pillar's side is often
        // far longer than the light's radius.
        const float db[3] = { l[0] - b->x, l[1] - b->y, l[2] - b->z };
        const float dc[3] = { l[0] - c->x, l[1] - c->y, l[2] - c->z };
        const float dist_sq = MIN(
            d[0] * d[0] + d[1] * d[1] + d[2] * d[2],
            MIN(db[0] * db[0] + db[1] * db[1] + db[2] * db[2],
                dc[0] * dc[0] + dc[1] * dc[1] + dc[2] * dc[2]));
        if (dist_sq > l[3]) {
            continue;
        }
        const float d_sq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        const float facing = d_sq > 0.0f
            ? (n[0] * d[0] + n[1] * d[1] + n[2] * d[2]) * inv_n
                * M_InvSqrt(d_sq)
            : 1.0f;
        away = MIN(away, M_Clamp(0.5f - facing, 0.0f, 1.0f));
    }
    return away > 1.0f ? 0 : (uint32_t)(away * 256.0f);
}

// A triangle known to show: in front of the near plane and facing the camera.
static void M_EmitFront(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const OUTPUT_MESH_INDEX idx[3])
{
    const M_XVERT *const xv = batcher->xverts;
    const uint32_t *const sargb = batcher->sargb;
    float uv[3][2];
    M_GetFaceUVs(batcher, bind, idx, 3, uv);
    pvr_vertex_t *const out = DC_PVR_ReservePoly(3);
    if (out == nullptr) {
        return;
    }
    const uint32_t away = batcher->face_light_count > 0
            && m_Frame.emit == M_EMIT_PLAIN
        ? M_FaceTurnedAway(batcher, &xv[idx[0]], &xv[idx[1]], &xv[idx[2]])
        : 0;
    const bool dark = away > 0;
    uint32_t cel_argb[3];
    for (int32_t k = 0; k < 3; k++) {
        uint32_t argb;
        uint32_t oargb;
        M_Colours(batcher, idx[k], &argb, &oargb);
        if (dark) {
            argb = M_LerpARGB(argb, batcher->eargb[idx[k]], (int32_t)away);
        }
        M_Project(
            &xv[idx[k]], uv[k][0], uv[k][1], argb, oargb,
            k == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, &out[k]);
        cel_argb[k] = sargb[idx[k]];
    }
    DC_PVR_CommitPoly(3);
    if (batcher->cel_emit && !dark
        && M_CelKeeps(
            batcher, cel_argb[0] | cel_argb[1] | cel_argb[2],
            cel_argb[0] & cel_argb[1] & cel_argb[2])) {
        M_KeepCel(batcher, out, 3, cel_argb);
    }
}

static void M_EmitTriangle(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const uint32_t i0, const uint32_t i1, const uint32_t i2)
{
    const OUTPUT_MESH_INDEX idx[3] = { i0, i1, i2 };
    const M_XVERT *const xv = batcher->xverts;
    if ((xv[i0].clip & xv[i1].clip & xv[i2].clip & M_CLIP_CULL) != 0) {
        g_DC_ProbeUs[DC_PROBE_E_OUT]++; // TEMP
        return;
    }
    if (((xv[i0].clip | xv[i1].clip | xv[i2].clip) & M_CLIP_NEAR) == 0) {
        if (m_Frame.cull && M_IsBackFacing(&xv[i0], &xv[i1], &xv[i2])) {
            g_DC_ProbeUs[DC_PROBE_E_BACK]++; // TEMP
            return;
        }
        g_DC_ProbeUs[DC_PROBE_E_TRI]++; // TEMP
        M_EmitFront(batcher, bind, idx);
        return;
    }

    const uint32_t *const sargb = batcher->sargb;
    const uint32_t away = batcher->face_light_count > 0 && batcher->cel_emit
            && m_Frame.emit == M_EMIT_PLAIN
        ? M_FaceTurnedAway(batcher, &xv[i0], &xv[i1], &xv[i2])
        : 0;
    const bool dark = away > 0;
    const bool cel = batcher->cel_emit && !dark
        && M_CelKeeps(
            batcher, sargb[i0] | sargb[i1] | sargb[i2],
            sargb[i0] & sargb[i1] & sargb[i2]);
    float uv[3][2];
    const float near_z = m_Frame.near_z;
    g_DC_ProbeUs[DC_PROBE_E_CLIP]++; // TEMP

    // Sutherland-Hodgman against the near plane: three corners in, at most
    // four out.
    M_GetFaceUVs(batcher, bind, idx, 3, uv);
    M_CLIP_VERT in[3];
    for (int32_t k = 0; k < 3; k++) {
        in[k] = M_LoadClipVert(batcher, idx[k], uv[k][0], uv[k][1]);
        if (dark) {
            // No cel copy of a darkened face: its field carries the colour
            // without the dynamic lights through the clip instead.
            in[k].sargb = batcher->eargb[idx[k]];
        }
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
    if (count < 3) {
        return;
    }
    pvr_vertex_t *const out = DC_PVR_ReservePoly(count);
    if (out == nullptr) {
        return;
    }
    // A fan 0-1-2-3 as a strip: 1, 0, 2, 3.
    static const int32_t order[2][4] = { { 0, 1, 2, 3 }, { 1, 0, 2, 3 } };
    const int32_t *const ord = order[count == 4];
    uint32_t cel_argb[4];
    for (int32_t k = 0; k < count; k++) {
        const M_CLIP_VERT *const p = &poly[ord[k]];
        M_Project(
            &p->xv, p->u, p->v,
            dark ? M_LerpARGB(p->argb, p->sargb, (int32_t)away) : p->argb,
            p->oargb, k == count - 1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
            &out[k]);
        cel_argb[k] = p->sargb;
    }
    DC_PVR_CommitPoly(count);
    if (cel) {
        M_KeepCel(batcher, out, count, cel_argb);
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
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const OUTPUT_MESH_INDEX *const t0, const OUTPUT_MESH_INDEX *const t1)
{
    if (!M_IsQuad(t0)) {
        return false;
    }
    const OUTPUT_MESH_INDEX strip[4] = { t0[0], t0[1], t0[2], t1[2] };
    const M_XVERT *const xv = batcher->xverts;
    const uint8_t clip_and = xv[strip[0]].clip & xv[strip[1]].clip
        & xv[strip[2]].clip & xv[strip[3]].clip;
    if ((clip_and & M_CLIP_CULL) != 0) {
        g_DC_ProbeUs[DC_PROBE_E_OUT] += 2; // TEMP
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
            g_DC_ProbeUs[DC_PROBE_E_BACK] += 2; // TEMP
            return true;
        }
        if (back0 || back1) {
            // Folded: the half that faces the camera alone, without looking
            // at either again.
            M_EmitFront(batcher, bind, back0 ? t1 : t0);
            return true;
        }
    }
    g_DC_ProbeUs[DC_PROBE_E_QUAD]++; // TEMP
    float uv[4][2];
    M_GetFaceUVs(batcher, bind, strip, 4, uv);
    pvr_vertex_t *const out = DC_PVR_ReservePoly(4);
    if (out == nullptr) {
        return true;
    }
    const uint32_t *const sargb = batcher->sargb;
    const uint32_t away = batcher->face_light_count > 0
            && m_Frame.emit == M_EMIT_PLAIN
        ? M_FaceTurnedAway(batcher, &xv[t0[0]], &xv[t0[1]], &xv[t0[2]])
        : 0;
    const bool dark = away > 0;
    uint32_t cel_argb[4];
    for (int32_t k = 0; k < 4; k++) {
        uint32_t argb;
        uint32_t oargb;
        M_Colours(batcher, strip[k], &argb, &oargb);
        if (dark) {
            argb = M_LerpARGB(argb, batcher->eargb[strip[k]], (int32_t)away);
        }
        M_Project(
            &xv[strip[k]], uv[k][0], uv[k][1], argb, oargb,
            k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, &out[k]);
        cel_argb[k] = sargb[strip[k]];
    }
    DC_PVR_CommitPoly(4);
    if (batcher->cel_emit && !dark
        && M_CelKeeps(
            batcher, cel_argb[0] | cel_argb[1] | cel_argb[2] | cel_argb[3],
            cel_argb[0] & cel_argb[1] & cel_argb[2] & cel_argb[3])) {
        M_KeepCel(batcher, out, 4, cel_argb);
    }
    return true;
}

// What render/emit_sh4.S takes and gives back (the layout is its own: see the
// header of that file).
typedef struct {
    float center_x;
    float center_y;
    float screen_scale;
    float near_z;
    float depth_lo;
    float depth_scale;
    float uv_scale;
    float one;
    const OUTPUT_MESH_INDEX *indices;
    int32_t count;
    const M_XVERT *xverts;
    const uint16_t *uv;
    const int8_t *page;
    const uint32_t *oargb;
    uint8_t *out;
    int32_t space;
    int32_t page_id;
    int32_t cull;
    // Lara's cel shading: the shaded colours, and where the copies of her
    // faces go (nullptr for none).
    const uint32_t *sargb;
    pvr_vertex_t *cel_out;
    // Back from it: indices taken, bytes written, polygons made, and where
    // the cel copies end.
    int32_t used;
    int32_t written;
    int32_t polys;
    pvr_vertex_t *cel_end;
    // A room's cel shading (M_CelKeeps): 1 to keep no shaded side of a face
    // with all its corners in the shade.
    int32_t cel_dark;
    // The store queue address of out less out, with dc_sq_vertices (0: the
    // vertices go through the cache).
    int32_t sq_delta;
} M_EMIT_RUN;
_Static_assert(offsetof(M_EMIT_RUN, cel_dark) == 96, "emit_sh4.S layout");
_Static_assert(offsetof(M_EMIT_RUN, sq_delta) == 100, "emit_sh4.S layout");
_Static_assert(offsetof(M_EMIT_RUN, indices) == 32, "emit_sh4.S layout");
_Static_assert(offsetof(M_EMIT_RUN, page_id) == 64, "emit_sh4.S layout");
_Static_assert(offsetof(M_EMIT_RUN, sargb) == 72, "emit_sh4.S layout");
_Static_assert(offsetof(M_EMIT_RUN, used) == 80, "emit_sh4.S layout");
_Static_assert(sizeof(pvr_vertex_t) == 32, "emit_sh4.S copies 32-byte vertices");
_Static_assert(sizeof(OUTPUT_MESH_INDEX) == 2, "emit_sh4.S reads 16-bit indices");

extern void dc_emit_run(M_EMIT_RUN *run);

#if defined(DC_EMIT_VERIFY)
// Checking the assembly against the C code: the run goes into a scratch buffer
// first, the C code then does the same faces into the list as ever, and the two
// are compared where the faces end. (-Ddc_emit_verify=true)
static struct {
    bool active;
    const OUTPUT_MESH_INDEX *end;
    int32_t faces;
    const uint8_t *at;
    int32_t written;
    // The cel copies: where the C code's start in cel_buf, and how many the
    // assembly made.
    int32_t cel_start;
    int32_t cel_verts;
} m_Verify = {};
static uint8_t m_VerifyScratch[16384] __attribute__((aligned(32)));
static pvr_vertex_t m_VerifyCel[4096] __attribute__((aligned(32)));
static uint32_t m_VerifyRuns = 0;
static uint32_t m_VerifyFaces = 0;
static uint32_t m_VerifyBad = 0;

static bool M_VerifyNear(const float a, const float b, const float rel)
{
    const float m = MAX(fabsf(a), fabsf(b));
    return fabsf(a - b) <= rel * m + 1.0e-9f;
}

static void M_VerifyEnd(
    const MESH_BATCHER *const batcher, const OUTPUT_MESH_INDEX *const reached)
{
    m_Verify.active = false;
    m_VerifyRuns++;
    bool ok = reached == m_Verify.end;
    int32_t bad_at = -1;
    if (ok && m_Verify.cel_verts >= 0) {
        // Lara's copies: as many, and the same bytes.
        ok = batcher->cel_count - m_Verify.cel_start == m_Verify.cel_verts
            && memcmp(
                   m_VerifyCel, &batcher->cel_buf[m_Verify.cel_start],
                   sizeof(pvr_vertex_t) * m_Verify.cel_verts)
                == 0;
        if (!ok) {
            bad_at = -2;
        }
    }
    if (ok) {
        const uint32_t *const a = (const uint32_t *)m_VerifyScratch;
        const uint32_t *const b = (const uint32_t *)m_Verify.at;
        const float *const fa = (const float *)a;
        const float *const fb = (const float *)b;
        for (int32_t v = 0; v < m_Verify.written / 32 && ok; v++) {
            const int32_t w = v * 8;
            ok = a[w] == b[w] && a[w + 6] == b[w + 6] && a[w + 7] == b[w + 7]
                && M_VerifyNear(fa[w + 1], fb[w + 1], 1.0e-4f)
                && M_VerifyNear(fa[w + 2], fb[w + 2], 1.0e-4f)
                && M_VerifyNear(fa[w + 3], fb[w + 3], 1.0e-4f)
                && M_VerifyNear(fa[w + 4], fb[w + 4], 1.0e-6f)
                && M_VerifyNear(fa[w + 5], fb[w + 5], 1.0e-6f);
            bad_at = ok ? -1 : v;
        }
    }
    m_VerifyFaces += (uint32_t)m_Verify.faces;
    if (!ok) {
        m_VerifyBad++;
        if (m_VerifyBad <= 12) {
            const uint32_t *const a = (const uint32_t *)m_VerifyScratch;
            const uint32_t *const b = (const uint32_t *)m_Verify.at;
            const int32_t w = bad_at < 0 ? 0 : bad_at * 8;
            LOG_WARNING(
                "[emit-verify] MISMATCH off by %d indices, vertex %d of %d: "
                "asm %08x %08x %08x %08x %08x %08x %08x %08x | C %08x %08x "
                "%08x %08x %08x %08x %08x %08x",
                (int)(reached - m_Verify.end), (int)bad_at,
                (int)(m_Verify.written / 32), (unsigned)a[w], (unsigned)a[w + 1],
                (unsigned)a[w + 2], (unsigned)a[w + 3], (unsigned)a[w + 4],
                (unsigned)a[w + 5], (unsigned)a[w + 6], (unsigned)a[w + 7],
                (unsigned)b[w], (unsigned)b[w + 1], (unsigned)b[w + 2],
                (unsigned)b[w + 3], (unsigned)b[w + 4], (unsigned)b[w + 5],
                (unsigned)b[w + 6], (unsigned)b[w + 7]);
        }
    }
    if (m_VerifyRuns % 4000 == 0) {
        LOG_WARNING(
            "[emit-verify] %u runs, %u indices, %u bad", (unsigned)m_VerifyRuns,
            (unsigned)m_VerifyFaces, (unsigned)m_VerifyBad);
    }
}
#endif

// Takes the faces of a run that are plain (see emit_sh4.S) off M_EmitIndices's
// hands: returns how many indices it took, none when the first face is not.
static int32_t M_EmitRunFast(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const OUTPUT_MESH_INDEX *const indices, const int32_t count,
    const int32_t page)
{
    size_t space;
    bool header;
    uint8_t *const at = DC_PVR_BeginRun(&space, &header);
    if (at == nullptr) {
        return 0;
    }
    M_EMIT_RUN run = {
        .center_x = m_Frame.center_x,
        .center_y = m_Frame.center_y,
        .screen_scale = m_Frame.screen_scale,
        .near_z = m_Frame.near_z,
        .depth_lo = m_Frame.depth.lo,
        .depth_scale = m_Frame.depth.scale,
        .uv_scale = 1.0f / M_UV_ONE,
        .one = 1.0f,
        .indices = indices,
        .count = count,
        .xverts = batcher->xverts,
        .uv = bind->uv,
        .page = bind->page,
        .oargb = batcher->oargb_used ? batcher->oargb : nullptr,
        .out = at,
        .space = (int32_t)MIN(space, (size_t)INT32_MAX),
        .page_id = page,
        .cull = m_Frame.cull ? 1 : 0,
        .cel_dark = batcher->cel_dark ? 1 : 0,
    };
    // Lara's cel shading keeps a copy of each lit face (M_KeepCel): room for
    // them all, and the run of her page they belong to, made first.
    const bool cel = batcher->cel_emit;
    bool fresh_run = false;
    int32_t old_cel_page = 0;
    if (cel) {
        if (batcher->cel_count + count > batcher->cel_capacity) {
            batcher->cel_capacity = (batcher->cel_count + count) * 2;
            batcher->cel_buf = Memory_Realloc(
                batcher->cel_buf,
                batcher->cel_capacity * sizeof(pvr_vertex_t));
        }
        run.sargb = batcher->sargb;
        run.cel_out = &batcher->cel_buf[batcher->cel_count];
#if !defined(DC_EMIT_VERIFY)
        if (batcher->cel_runs->count == 0
            || batcher->cel_page != batcher->emit_page) {
            const M_CEL_RUN made = {
                .page = batcher->emit_page,
                .start = batcher->cel_count,
            };
            Vector_Add(batcher->cel_runs, &made);
            old_cel_page = batcher->cel_page;
            batcher->cel_page = batcher->emit_page;
            fresh_run = true;
        }
#endif
    }
#if defined(DC_EMIT_VERIFY)
    if (m_Verify.active) {
        return 0;
    }
    run.out = m_VerifyScratch;
    run.space = MIN(run.space, (int32_t)sizeof(m_VerifyScratch));
    if (cel) {
        run.cel_out = m_VerifyCel;
        if (count > (int32_t)(sizeof(m_VerifyCel) / sizeof(m_VerifyCel[0]))) {
            return 0;
        }
    }
    dc_emit_run(&run);
    if (run.used > 0) {
        m_Verify.active = true;
        m_Verify.end = indices + run.used;
        m_Verify.faces = run.used;
        m_Verify.at = at;
        m_Verify.written = run.written;
        m_Verify.cel_start = batcher->cel_count;
        m_Verify.cel_verts = cel ? (int32_t)(run.cel_end - m_VerifyCel) : -1;
    }
    return 0;
#else
#if defined(TRX_DC_SQ_VERTICES)
    // shortcut: the cel copies read back what the run wrote, which a store
    // queue cannot give, so runs with them keep the cache.
    const bool sq = !cel;
    if (sq) {
        run.out = (uint8_t *)sq_lock(at);
        run.sq_delta = (int32_t)((uintptr_t)run.out - (uintptr_t)at);
    }
#endif
    dc_emit_run(&run);
#if defined(TRX_DC_SQ_VERTICES)
    if (sq) {
        sq_wait();
        sq_unlock();
    }
#endif
    DC_PVR_EndRun((size_t)run.written, run.polys, header);
    if (cel) {
        const int32_t made = (int32_t)(run.cel_end - run.cel_out);
        batcher->cel_count += made;
        if (fresh_run && made == 0) {
            // None of her faces here is lit: no run for them.
            Vector_RemoveAt(batcher->cel_runs, batcher->cel_runs->count - 1);
            batcher->cel_page = old_cel_page;
        }
    }
    g_DC_ProbeUs[DC_PROBE_A_RUNS]++; // TEMP
    g_DC_ProbeUs[DC_PROBE_A_FACES] += run.used / 3; // TEMP
    g_DC_ProbeUs[DC_PROBE_A_EMPTY] += run.used == 0 ? 1 : 0; // TEMP
    return run.used;
#endif
}

// Emits a run of faces. *page is the page the list was last set to with
// this blend (INT32_MIN for none), kept across the runs of an instance so
// that consecutive runs on one page share a header.
static void M_EmitIndices(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const OUTPUT_MESH_INDEX *const indices, const int32_t count,
    const DC_BLEND blend, int32_t *const page)
{
    const bool env = m_Frame.emit == M_EMIT_ENV;
    // The plain faces of a room or an object (not the reflections, the relief
    // or Lara's cel shading, nor a model on the canvas) go through assembly.
    // Nor a room's faces that a dynamic light reaches with the cel shading:
    // each is looked at for the way it turns (M_FaceTurnedAway).
    const bool fast = !env && !m_Frame.ortho && m_Frame.emit == M_EMIT_PLAIN
        && batcher->face_light_count == 0;
    int32_t i = 0;
    while (i + 2 < count) {
#if defined(DC_EMIT_VERIFY)
        if (m_Verify.active && &indices[i] >= m_Verify.end) {
            M_VerifyEnd(batcher, &indices[i]);
        }
#endif
        const int32_t tri_page = env ? DC_PVR_ENV_MAP : bind->page[indices[i]];
        if (tri_page != *page) {
            *page = tri_page;
            batcher->emit_page = tri_page;
            DC_PVR_SetState(
                blend, tri_page == M_NO_PAGE ? DC_PVR_NO_TEXTURE : tri_page,
                true);
        }
        if (fast) {
            const int32_t used =
                M_EmitRunFast(batcher, bind, &indices[i], count - i, tri_page);
            if (used > 0) {
                i += used;
                continue;
            }
        }
        g_DC_ProbeUs[DC_PROBE_A_CFACES]++; // TEMP
        if (i + 5 < count && (env || bind->page[indices[i + 3]] == tri_page)
            && M_TryEmitQuad(batcher, bind, &indices[i], &indices[i + 3])) {
            i += 6;
            continue;
        }
        M_EmitTriangle(batcher, bind, indices[i], indices[i + 1], indices[i + 2]);
        i += 3;
    }
#if defined(DC_EMIT_VERIFY)
    if (m_Verify.active && &indices[i] >= m_Verify.end) {
        M_VerifyEnd(batcher, &indices[i]);
    }
    m_Verify.active = false;
#endif
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
// How much nearer the middle of the shadow map a point z past the casters'
// middle lies than in parallel, for a light dist away: the shadow of the
// casters grows with the distance from the light (z in world units).
static float M_PerspScale(const float dist, const float z)
{
    const float depth = dist + z;
    // A soft shadow never grows past three times its caster: the depth is
    // held at a third of the light's distance at the least. (The volumes keep
    // to twice: see M_BuildVolume.)
    const float least = dist * (1.0f / 3.0f);
    const float r = M_InvSqrt(depth > least ? depth : least);
    return dist * r * r;
}

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
    const float inv_fade = 1.0f / m_Soft.lights[l].fade;
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
    const M_SOFT_LIGHT *const light = &m_Soft.lights[l];
    const bool persp = light->dist < M_PARALLEL_DIST;
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
        if (persp) {
            const float f = M_PerspScale(light->dist, z);
            x = 0.5f + (x - 0.5f) * f;
            y = 0.5f + (y - 0.5f) * f;
        }
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

    // Per vertex, what it lies past, as bits: a face whose corners all lie
    // past one same thing is off, told with two ands instead of eighteen
    // comparisons. The near plane's bit comes from the clip.
    enum {
        M_SOFT_OFF_LEFT = 0x01,
        M_SOFT_OFF_RIGHT = 0x02,
        M_SOFT_OFF_TOP = 0x04,
        M_SOFT_OFF_BOTTOM = 0x08,
        M_SOFT_OFF_FAR = 0x10,
        M_SOFT_OFF_BEHIND = 0x20,
    };
    mat_load(&m);
    float(*const soft)[3] = batcher->soft;
    uint8_t *const soft_off = batcher->soft_off;
    // Only the vertices transformed this instance: a room's chunks off the
    // screen were not.
    const int32_t whole[1][2] = { { 0, bind->vertex_count } };
    const int32_t(*const vr)[2] =
        batcher->vrange_count < 0 ? whole : batcher->vranges;
    const int32_t nvr = batcher->vrange_count < 0 ? 1 : batcher->vrange_count;
    for (int32_t r = 0; r < nvr; r++) {
        const int32_t end = vr[r][0] + vr[r][1];
        for (int32_t i = vr[r][0]; i < end; i++) {
            const M_XVERT *const xv = &batcher->xverts[i];
            float x = xv->x;
            float y = xv->y;
            float z = xv->z;
            float w = 1.0f;
            mat_trans_nodiv(x, y, z, w);
            if (persp) {
                const float f = M_PerspScale(light->dist, z);
                x = 0.5f + (x - 0.5f) * f;
                y = 0.5f + (y - 0.5f) * f;
            }
            soft[i][0] = x;
            soft[i][1] = y;
            soft[i][2] = z;
            uint8_t off = (x < 0.0f ? M_SOFT_OFF_LEFT : 0)
                | (x > 1.0f ? M_SOFT_OFF_RIGHT : 0)
                | (y < 0.0f ? M_SOFT_OFF_TOP : 0)
                | (y > 1.0f ? M_SOFT_OFF_BOTTOM : 0);
            if (world) {
                off |= (z > M_SOFT_REACH ? M_SOFT_OFF_FAR : 0)
                    | (z < -M_SOFT_SPAN * 0.25f ? M_SOFT_OFF_BEHIND : 0);
            }
            soft_off[i] = off;
        }
    }

    const OUTPUT_MESH *const mesh = bind->mesh;
    const OUTPUT_MESH_INDEX *const indices = Vector_GetData(mesh->opaque_vertex_indices);
    // TR2's rock and ground mostly carry see-through texels, so those faces
    // take the shadow too.
    // shortcut: the shadow also darkens what shows through their holes, and
    // faces crossing the near plane take none; split them out if it shows.
    // Only the faces that go out: a room's chunks off the screen were not
    // transformed.
    for (int32_t r = 0; r < batcher->range_count; r++) {
        const M_RANGE *const range = &batcher->ranges[r];
        const int32_t end = range->start + range->count;
        for (int32_t i = range->start; i + 2 < end; i += 3) {
            const OUTPUT_MESH_INDEX idx[3] = { indices[i], indices[i + 1], indices[i + 2] };
            // Faces are tested as a whole: a big one can lie under the casters
            // with every corner far off. Off the silhouette's cell, out of
            // reach, between the light and the casters, or culled:
            if ((soft_off[idx[0]] & soft_off[idx[1]] & soft_off[idx[2]]) != 0) {
                continue;
            }
            const M_XVERT *const xv[3] = {
                &batcher->xverts[idx[0]],
                &batcher->xverts[idx[1]],
                &batcher->xverts[idx[2]],
            };
            const uint8_t clip_and = xv[0]->clip & xv[1]->clip & xv[2]->clip;
            const uint8_t clip_or = xv[0]->clip | xv[1]->clip | xv[2]->clip;
            if ((clip_and & M_CLIP_CULL) != 0 || (clip_or & M_CLIP_NEAR) != 0
                || M_IsBackFacing(xv[0], xv[1], xv[2])) {
                continue;
            }
            const float *const s[3] = { soft[idx[0]], soft[idx[1]], soft[idx[2]] };
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
            const float n_dot_l = M_Dot3(n, axes[0]);
            if ((M_Dot3(n, p0) > 0.0f) == (n_dot_l < 0.0f)) {
                continue;
            }
            // A face lying along the light (a wall under a light overhead)
            // would take the silhouette smeared down all of its height: the
            // more it turns that way, the less shadow it takes.
            float face_strength = strength;
            if (world) {
                // The axes carry the view's scale: both lengths go.
                const float len_sq = M_Dot3(n, n) * M_Dot3(axes[0], axes[0]);
                const float cos_l =
                    len_sq > 0.0f ? fabsf(n_dot_l) * M_InvSqrt(len_sq) : 0.0f;
                face_strength *= M_Clamp(
                    (cos_l - M_SOFT_GRAZE_MIN)
                        * (1.0f / (M_SOFT_GRAZE_FULL - M_SOFT_GRAZE_MIN)),
                    0.0f, 1.0f);
                if (face_strength <= 0.0f) {
                    continue;
                }
            }

            DC_PVR_SetState(DC_BLEND_MUL, DC_PVR_SHADOW_MAP, true);
            pvr_vertex_t *const out = DC_PVR_ReservePoly(3);
            if (out == nullptr) {
                return;
            }
            for (int32_t k = 0; k < 3; k++) {
                const float t = face_strength
                    * M_Clamp(1.0f - s[k][2] * inv_fade, 0.0f, 1.0f);
                M_Project(
                    xv[k], (slot + s[k][0]) * (1.0f / DC_PVR_SHADOW_MAP_SLOTS),
                    s[k][1], M_PackARGB(1.0f, t, t, t),
                    M_PackARGB(1.0f, 1.0f - t, 1.0f - t, 1.0f - t),
                    k == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, &out[k]);
            }
            DC_PVR_CommitPoly(3);
            m_SoftFaces++;
        }
    }
}

static void M_EmitSoftShadows(
    const MESH_BATCHER *const batcher, const M_BINDING *const bind)
{
    m_DepthBias = M_DEPTH_BIAS_SHADOW;
    for (int32_t l = 0; l < m_Soft.map_count; l++) {
        M_EmitShadowFaces(batcher, bind, l, M_SOFT_LIGHT_SLOT(l), true);
    }
    m_DepthBias = 1.0f;
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
    m_DepthBias = M_DEPTH_BIAS_SHADOW;
    M_EmitShadowFaces(
        batcher, bind, 0, M_SOFT_SELF_SLOT(m_Soft.slice[caster]), false);
    m_DepthBias = 1.0f;
}

// Faces given their relief this frame, for the debug console.
static int32_t m_BumpFaces = 0;

M_HOT uint32_t M_PackUnit(const float x)
{
    return (uint32_t)(M_Clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// atan2 to within 0.0015 radians (the relief quantises the angle to 256
// steps of 0.025): a cubic in the smaller over the larger, then the octant.
M_HOT float M_Atan2(const float y, const float x)
{
    const float ax = fabsf(x);
    const float ay = fabsf(y);
    const float lo = ax < ay ? ax : ay;
    const float hi = ax < ay ? ay : ax;
    if (hi <= 0.0f) {
        return 0.0f;
    }
    const float a = lo / hi;
    const float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a
        + a;
    if (ay > ax) {
        r = (float)(M_PI / 2.0) - r;
    }
    if (x < 0.0f) {
        r = (float)M_PI - r;
    }
    return y < 0.0f ? -r : r;
}

// The lowest a light stands over a face, as the sine of M_BUMP_MIN_ELEVATION.
#define M_BUMP_MIN_SIN 0.342898f

// A face's relief is lit in this frame: which way u and v run across it, and
// its normal turned towards the camera, with their lengths squared.
typedef struct {
    float t[3];
    float bt[3];
    float n[3];
    float tl;
    float bl;
    float nl;
} M_BUMP_FRAME;

// The frame of the face of the three vertices idx, or false when it has none.
static bool M_BumpFrame(
    const M_BINDING *const bind, const M_XVERT *const xv,
    const OUTPUT_MESH_INDEX *const idx, M_BUMP_FRAME *const out)
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
        return false;
    }
    for (int32_t k = 0; k < 3; k++) {
        out->t[k] = e1[k] * dv2 - e2[k] * dv1;
        out->bt[k] = e2[k] * du1 - e1[k] * du2;
    }
    if (det < 0.0f) {
        for (int32_t k = 0; k < 3; k++) {
            out->t[k] = -out->t[k];
            out->bt[k] = -out->bt[k];
        }
    }
    out->n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    out->n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    out->n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    const float a_pos[3] = { a->x, a->y, a->z };
    if (M_Dot3(out->n, a_pos) > 0.0f) {
        for (int32_t k = 0; k < 3; k++) {
            out->n[k] = -out->n[k];
        }
    }
    out->tl = M_Dot3(out->t, out->t);
    out->bl = M_Dot3(out->bt, out->bt);
    out->nl = M_Dot3(out->n, out->n);
    return out->tl > 0.0f && out->bl > 0.0f && out->nl > 0.0f;
}

// The offset colour that lights a face's relief (DC_BLEND_BUMP) at the point
// pos (eye space) from an eye space point light: K1, K2 and K3 so that a flat
// texel keeps the colour (K1 + K2 whole) and a slope turned from the light
// loses up to depth of it, and Q the way the light lies across the face,
// from u towards v. No trigonometry: K2 is depth times the sine of the
// elevation, which is the light's height over the face as it is, and K3 depth
// times its cosine, the root of what is left. Given at each vertex, the
// PowerVR carries it across the face, so that the light is seen to lie
// differently on the two sides of a face.
static uint32_t M_BumpAt(
    const M_BUMP_FRAME *const f, const float pos[3], const float light[3],
    const float depth, const bool glow)
{
    float l[3] = { light[0] - pos[0], light[1] - pos[1], light[2] - pos[2] };
    // The shine lights the slopes that throw the light at the camera: they
    // face half way between the light and the eye (the camera is the origin
    // of eye space), so it moves as the camera does. The shade takes the
    // light alone: half way to the eye, a muzzle flash (near the camera)
    // turned the shade about with every sway of the gun, and the texture
    // seemed to slide.
    const float l_sq = M_Dot3(l, l);
    const float c_sq = M_Dot3(pos, pos);
    if (glow && l_sq > 0.0f && c_sq > 0.0f) {
        const float li = M_InvSqrt(l_sq);
        const float ci = M_InvSqrt(c_sq);
        float h[3];
        for (int32_t k = 0; k < 3; k++) {
            h[k] = l[k] * li - pos[k] * ci;
        }
        if (M_Dot3(h, h) > 1.0e-4f) {
            memcpy(l, h, sizeof(l));
        }
    }
    const float ll = M_Dot3(l, l);
    if (ll <= 0.0f) {
        return 0;
    }
    const float lx = M_Dot3(l, f->t) * M_InvSqrt(f->tl * ll);
    const float ly = M_Dot3(l, f->bt) * M_InvSqrt(f->bl * ll);
    float lz = M_Dot3(l, f->n) * M_InvSqrt(f->nl * ll);
    // How far the light lies off the face's normal: with it near the normal
    // the direction it lies across the face (Q) turns about at a breath, and
    // the relief would shimmer, so it is let go as that nears.
    const float off = sqrtf(MAX(1.0f - lz * lz, 0.0f));
    const float steady = M_Clamp((off - 0.30f) * (1.0f / 0.45f), 0.0f, 1.0f);
    const float steady_smooth = steady * steady * (3.0f - 2.0f * steady);
    // The shine is let go sooner than the shade: where the light, the eye and
    // the face line up it is the one that shimmers most.
    const float shine_steady = M_Clamp((off - 0.5f) * (1.0f / 0.4f), 0.0f, 1.0f);
    const float shine_smooth =
        shine_steady * shine_steady * (3.0f - 2.0f * shine_steady);
    lz = M_Clamp(lz, M_BUMP_MIN_SIN, 1.0f);
    float q = M_Atan2(ly, lx);
    if (q < 0.0f) {
        q += 2.0f * (float)M_PI;
    }
    const uint32_t q8 =
        (uint32_t)(q * (256.0f / (2.0f * (float)M_PI))) & 0xFF;
    if (glow) {
        return (M_PackUnit(depth * shine_smooth) << 8) | q8;
    }
    // A flat texel keeps this much of the colour, so that the slopes turned
    // to the light can be lighter than it and the rest darker: only then does
    // the relief read as raised and not as dents. It is the whole colour as
    // the face leaves the relief's range.
    const float flat = 1.0f - M_BUMP_DIM * (depth * (1.0f / M_BUMP_DEPTH));
    const float k2 = MIN(depth * lz, flat);
    const float k3 = depth * sqrtf(1.0f - lz * lz) * steady_smooth;
    // The PowerVR's intensity is K1 + K2 sin S + K3 cos S cos(R - Q), linear
    // in the K: squeezed into M_BUMP_FLOOR to 1, a slope turned from the
    // light is shaded and no longer black, and a flat texel stays as it was.
    const float keep = 1.0f - M_BUMP_FLOOR;
    return (M_PackUnit(M_BUMP_FLOOR + keep * (flat - k2)) << 24)
        | (M_PackUnit(keep * k2) << 16) | (M_PackUnit(keep * k3) << 8) | q8;
}

// The relief's lights: a room's own, and the passing ones, in eye space, each
// with how far it reaches and how strong it is.
typedef struct {
    float pos[M_BUMP_MAX_LIGHTS][3];
    float radius[M_BUMP_MAX_LIGHTS];
    float power[M_BUMP_MAX_LIGHTS];
    bool passing[M_BUMP_MAX_LIGHTS];
    int32_t count;
} M_BUMP_LIGHTS;

// The relief's offset colour at one vertex of a face (see M_BumpAt), and how
// much of the relief the vertex takes in *fade. The light is the blend of
// the lights that reach the vertex, by how near it stands to their reach and
// how strong they are, so that the relief turns smoothly as a flash comes and
// goes; with none, one high above it. Near the camera the relief is eased in
// over a long stretch, so that none starts suddenly; and wherever a flash or
// other passing light reaches the vertex, however far from the camera, by how
// near it stands to that light.
static uint32_t M_BumpVertex(
    const M_BUMP_LIGHTS *const lights, const M_BUMP_FRAME *const frame,
    const M_XVERT *const vert, const bool glow, float *const fade,
    float *const by_light)
{
    const float(*const v)[4] = m_Frame.view;
    const float pos[3] = { vert->x, vert->y, vert->z };
    float light[3] = {};
    float room_pos[3] = {};
    float passing_pos[3] = {};
    float room_total = 0.0f;
    float passing = 0.0f;
    for (int32_t l = 0; l < lights->count; l++) {
        const float *const p = lights->pos[l];
        const float d[3] = { p[0] - pos[0], p[1] - pos[1], p[2] - pos[2] };
        const float dist_sq = M_Dot3(d, d);
        const float radius = lights->radius[l];
        if (dist_sq >= radius * radius) {
            continue;
        }
        const float ratio_sq = dist_sq / (radius * radius);
        const float near = 1.0f - sqrtf(ratio_sq);
        const float weight =
            lights->power[l] * near * near / (1.0f + ratio_sq);
        float *const sum = lights->passing[l] ? passing_pos : room_pos;
        for (int32_t r = 0; r < 3; r++) {
            sum[r] += p[r] * weight;
        }
        if (lights->passing[l]) {
            passing += weight;
        } else {
            room_total += weight;
        }
    }
    const float total = room_total + passing;
    float scale = 1.0f;
    if (total > 0.0f) {
        scale = M_Clamp(total * 4.0f, 0.0f, 1.0f);
    }
    // The room lights give the direction; with none, one high above the
    // vertex (TR's y axis points down).
    if (room_total > 0.0f) {
        for (int32_t r = 0; r < 3; r++) {
            light[r] = room_pos[r] / room_total;
        }
    } else {
        for (int32_t r = 0; r < 3; r++) {
            light[r] = pos[r] - v[1][r] * (8.0f * WALL_L);
        }
    }
    if (passing > 0.0f) {
        // The directions are mixed, not the places: a light passing close to
        // a face turns the direction to it by a lot for a little movement,
        // and with it the relief, whatever share of the place it has.
        const float tilt =
            M_Clamp(passing * 4.0f, 0.0f, 1.0f) * M_BUMP_PASSING_TILT;
        float room_dir[3];
        float passing_dir[3];
        for (int32_t r = 0; r < 3; r++) {
            room_dir[r] = light[r] - pos[r];
            passing_dir[r] = passing_pos[r] / passing - pos[r];
        }
        const float room_sq = M_Dot3(room_dir, room_dir);
        const float passing_sq = M_Dot3(passing_dir, passing_dir);
        if (room_sq > 1.0f && passing_sq > 1.0f) {
            const float room_inv = M_InvSqrt(room_sq);
            const float passing_inv = M_InvSqrt(passing_sq);
            float mixed[3];
            for (int32_t r = 0; r < 3; r++) {
                mixed[r] = room_dir[r] * room_inv * (1.0f - tilt)
                    + passing_dir[r] * passing_inv * tilt;
            }
            const float mixed_sq = M_Dot3(mixed, mixed);
            if (mixed_sq > 1.0e-6f) {
                // As far off as the sun is taken to lie: a direction only.
                const float far = (8.0f * WALL_L) * M_InvSqrt(mixed_sq);
                for (int32_t r = 0; r < 3; r++) {
                    light[r] = pos[r] + mixed[r] * far;
                }
            }
        }
    }
    const float near = M_Clamp(
        (M_BUMP_RANGE - vert->z) * (1.0f / M_BUMP_FADE), 0.0f, 1.0f);
    const float lit_by_light = M_Clamp(passing * 4.0f, 0.0f, 1.0f);
    *fade = MAX(near * near * (3.0f - 2.0f * near) * scale, lit_by_light);
    *by_light = lit_by_light;
    float strength = (glow ? M_BUMP_GLOW : M_BUMP_DEPTH) * *fade;
    if (glow) {
        // The shine adds light, so it follows how lit the vertex is: none
        // where it stands in the dark.
        const uint32_t c = vert->argb;
        float lit = (float)MAX((c >> 16) & 0xFF, MAX((c >> 8) & 0xFF, c & 0xFF))
            * (1.0f / 255.0f);
        strength *= lit * lit;
    }
    return M_BumpAt(frame, pos, light, strength, glow);
}

// How much of a face's relief must come from a passing light for the face to
// be cut up so that the light shows across it (see M_EmitBumpGrid), and into
// how many strips each way.
#define M_BUMP_SPLIT_LIGHT 0.05f
// A face is cut up only when it is nearer than this: farther off the pieces
// are too small to show the light across them.
#define M_BUMP_SPLIT_RANGE (2.0f * WALL_L)
// A face whose relief is under this of the whole at its strongest corner is
// left as it is: the shade would hardly show, and the face costs a second
// polygon.
#define M_BUMP_MIN_FADE 0.12f
#define M_BUMP_SPLIT 4
// How far apart the directions at a face's corners may lie (of 256), for each
// to keep its own.
#define M_BUMP_SPLIT_Q 96
#define M_BUMP_GRID_NEAR 1.0f

// Draws a quad's relief in M_BUMP_SPLIT squared pieces, with the offset
// colour found at every corner of every piece: the light's nearness to the
// middle of a big face then shows, which the four corners alone cannot give.
// For a quad wholly in front of the camera; false when it is not one.
static bool M_EmitBumpGrid(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const M_BUMP_LIGHTS *const lights, const M_BUMP_FRAME *const frame,
    const OUTPUT_MESH_INDEX *const tri, const bool glow)
{
    const OUTPUT_MESH_INDEX strip[4] = { tri[0], tri[1], tri[2], tri[5] };
    const M_XVERT *const xv = batcher->xverts;
    for (int32_t c = 0; c < 4; c++) {
        if ((xv[strip[c]].clip & M_CLIP_NEAR) != 0) {
            return false;
        }
    }
    if (m_Frame.cull
        && M_IsBackFacing(&xv[strip[0]], &xv[strip[1]], &xv[strip[2]])) {
        return true;
    }
    float uv[4][2];
    M_GetFaceUVs(batcher, bind, strip, 4, uv);
    enum { M_N = M_BUMP_SPLIT + 1 };
    M_XVERT grid[M_N * M_N];
    float grid_uv[M_N * M_N][2] = {};
    uint32_t grid_k[M_N * M_N];
    for (int32_t row = 0; row < M_N; row++) {
        const float t = (float)row / M_BUMP_SPLIT;
        for (int32_t col = 0; col < M_N; col++) {
            const float s = (float)col / M_BUMP_SPLIT;
            // The strip's corners 0, 1, 2, 3 lie at (0,0), (1,0), (0,1), (1,1).
            // The face under it is drawn as two triangles either side of the
            // 1-2 diagonal, each flat in its coordinates: a point takes its
            // place in its own triangle, and not the blend of all four corners,
            // which on a face that is no parallelogram lay elsewhere on the
            // texture and made the relief slide as a flash came and went. The
            // pieces' own diagonals run along that one, so they meet it.
            float w[4];
            if (s + t <= 1.0f) {
                w[0] = 1.0f - s - t;
                w[1] = s;
                w[2] = t;
                w[3] = 0.0f;
            } else {
                w[0] = 0.0f;
                w[1] = 1.0f - t;
                w[2] = 1.0f - s;
                w[3] = s + t - 1.0f;
            }
            M_XVERT *const g = &grid[row * M_N + col];
            memset(g, 0, sizeof(*g));
            for (int32_t c = 0; c < 4; c++) {
                g->x += xv[strip[c]].x * w[c];
                g->y += xv[strip[c]].y * w[c];
                g->z += xv[strip[c]].z * w[c];
                grid_uv[row * M_N + col][0] += uv[c][0] * w[c];
                grid_uv[row * M_N + col][1] += uv[c][1] * w[c];
            }
            g->inv_z = 1.0f / g->z;
            g->sx = m_Frame.proj_x * g->x * g->inv_z;
            g->sy = m_Frame.proj_y * g->y * g->inv_z;
            // A hair nearer than the face under it: the pieces do not lie at
            // exactly the depth its whole does, and would win and lose to it
            // pixel by pixel, shimmering as the camera moves.
            g->inv_z *= M_BUMP_GRID_NEAR;
            float argb[4] = {};
            for (int32_t c = 0; c < 4; c++) {
                const uint32_t k = xv[strip[c]].argb;
                for (int32_t b = 0; b < 4; b++) {
                    argb[b] += (float)((k >> (b * 8)) & 0xFF) * w[c];
                }
            }
            g->argb = 0;
            for (int32_t b = 0; b < 4; b++) {
                g->argb |= (uint32_t)(argb[b] + 0.5f) << (b * 8);
            }
            float fade;
            float by_light;
            grid_k[row * M_N + col] =
                M_BumpVertex(lights, frame, g, glow, &fade, &by_light);
        }
    }
    for (int32_t row = 0; row < M_BUMP_SPLIT; row++) {
        for (int32_t col = 0; col < M_BUMP_SPLIT; col++) {
            const int32_t at[4] = {
                row * M_N + col, row * M_N + col + 1, (row + 1) * M_N + col,
                (row + 1) * M_N + col + 1,
            };
            uint32_t k[4];
            for (int32_t c = 0; c < 4; c++) {
                k[c] = grid_k[at[c]];
            }
            // Q is one byte that comes round from 255 to 0, and would run
            // through every direction in between: a piece whose corners lie
            // too far apart (right under the light, where the relief is let
            // go in any case) has the direction of its strongest corner.
            uint32_t q_min = 255;
            uint32_t q_max = 0;
            int32_t strongest = 0;
            for (int32_t c = 0; c < 4; c++) {
                q_min = MIN(q_min, k[c] & 0xFF);
                q_max = MAX(q_max, k[c] & 0xFF);
                if (((k[c] >> 8) & 0xFF) > ((k[strongest] >> 8) & 0xFF)) {
                    strongest = c;
                }
            }
            if (q_max - q_min > M_BUMP_SPLIT_Q) {
                const uint32_t q = k[strongest] & 0xFF;
                for (int32_t c = 0; c < 4; c++) {
                    k[c] = (k[c] & ~0xFFu) | q;
                }
            }
            pvr_vertex_t *const out = DC_PVR_ReservePoly(4);
            if (out == nullptr) {
                return true;
            }
            for (int32_t c = 0; c < 4; c++) {
                M_Project(
                    &grid[at[c]], grid_uv[at[c]][0], grid_uv[at[c]][1],
                    0xFFFFFFFFu, k[c],
                    c == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, &out[c]);
            }
            DC_PVR_CommitPoly(4);
        }
    }
    return true;
}

// A face with its relief worked out, waiting to be drawn.
typedef struct {
    int32_t first;
    int32_t page;
    int32_t step;
    uint32_t k[6];
    float max_by_light;
    float z_min;
} M_BUMP_ENTRY;

#define M_BUMP_QUEUE 1024
static M_BUMP_ENTRY m_BumpQueue[M_BUMP_QUEUE];

static int M_BumpEntryOrder(const void *const a, const void *const b)
{
    const M_BUMP_ENTRY *const x = a;
    const M_BUMP_ENTRY *const y = b;
    if (x->page != y->page) {
        return x->page < y->page ? -1 : 1;
    }
    return x->first < y->first ? -1 : x->first > y->first;
}

// Draws the queued faces, a page at a time: every change of page is a new
// polygon header.
static void M_FlushBump(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const M_BUMP_LIGHTS *const lights, const OUTPUT_MESH_INDEX *const indices,
    M_BUMP_ENTRY *const queue, const int32_t count, int32_t *const page,
    const bool glow)
{
    qsort(queue, (size_t)count, sizeof(queue[0]), M_BumpEntryOrder);
    for (int32_t n = 0; n < count; n++) {
        const M_BUMP_ENTRY *const e = &queue[n];
        const OUTPUT_MESH_INDEX *const tri = &indices[e->first];
        const bool quad = e->step == 6;
        m_Frame.bump_n = e->step;
        for (int32_t c = 0; c < e->step; c++) {
            m_Frame.bump_idx[c] = tri[c];
            m_Frame.bump_vk[c] = e->k[c];
        }
        if (e->page != *page) {
            *page = e->page;
            DC_PVR_SetState(
                glow ? DC_BLEND_BUMP_GLOW : DC_BLEND_BUMP, e->page, true);
        }
        // A face a passing light lies near is cut up, so that the light is
        // seen to lie nearer one part of it.
        M_BUMP_FRAME frame;
        if (quad && e->max_by_light > M_BUMP_SPLIT_LIGHT
            && e->z_min < M_BUMP_SPLIT_RANGE
            && M_BumpFrame(bind, batcher->xverts, tri, &frame)
            && M_EmitBumpGrid(batcher, bind, lights, &frame, tri, glow)) {
            m_BumpFaces++;
            continue;
        }
        if (!quad || !M_TryEmitQuad(batcher, bind, tri, tri + 3)) {
            M_EmitTriangle(batcher, bind, tri[0], tri[1], tri[2]);
            if (quad) {
                M_EmitTriangle(batcher, bind, tri[3], tri[4], tri[5]);
            }
        }
        m_BumpFaces++;
    }
}

// Draws the relief of the faces of one range near the camera over them.
static void M_EmitBumpRange(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const M_BUMP_LIGHTS *const lights, const OUTPUT_MESH_INDEX *const indices,
    const int32_t count, int32_t *const page, const bool glow)
{
    const M_XVERT *const xv = batcher->xverts;
    int32_t queued = 0;
    m_DepthBias = glow ? M_DEPTH_BIAS_GLOW : M_DEPTH_BIAS_BUMP;
    int32_t i = 0;
    while (i + 2 < count) {
        const OUTPUT_MESH_INDEX *const tri = &indices[i];
        const bool quad = i + 5 < count && M_IsQuad(tri)
            && bind->page[tri[3]] == bind->page[tri[0]];
        const int32_t step = quad ? 6 : 3;
        const int32_t tri_page = bind->page[tri[0]];
        const float z_min =
            MIN(xv[tri[0]].z, MIN(xv[tri[1]].z, xv[tri[2]].z));
        M_BUMP_FRAME frame;
        if (tri_page == M_NO_PAGE || z_min >= M_BUMP_RANGE * M_BUMP_LIGHT_REACH
            || (xv[tri[0]].clip & xv[tri[1]].clip & xv[tri[2]].clip
                & M_CLIP_CULL)
                != 0
            || !M_BumpFrame(bind, xv, tri, &frame)) {
            i += step;
            continue;
        }
        // The relief at each corner. A quad's second triangle repeats two of
        // the first's corners: they are not worked out twice.
        uint32_t k[6];
        float max_fade = 0.0f;
        uint32_t q_min = 255;
        uint32_t q_max = 0;
        float max_by_light = 0.0f;
        for (int32_t c = 0; c < step; c++) {
            int32_t same = -1;
            for (int32_t d = 0; d < c; d++) {
                if (tri[d] == tri[c]) {
                    same = d;
                    break;
                }
            }
            if (same >= 0) {
                k[c] = k[same];
                continue;
            }
            float fade;
            float by_light;
            k[c] = M_BumpVertex(
                lights, &frame, &xv[tri[c]], glow, &fade, &by_light);
            max_fade = MAX(max_fade, fade);
            max_by_light = MAX(max_by_light, by_light);
            q_min = MIN(q_min, k[c] & 0xFF);
            q_max = MAX(q_max, k[c] & 0xFF);
        }
        if (max_fade < M_BUMP_MIN_FADE) {
            i += step;
            continue;
        }
        // Q is one byte that comes round from 255 to 0: a face whose corners
        // lie either side of that, (as the one right under a light does)
        // would run through every direction in between. It takes the middle's
        // direction for all its corners, and keeps their own strength: the
        // relief at a corner it shares with the next face stays what that
        // face has there, so that the two meet.
        if (q_max - q_min > 128) {
            const int32_t corners = quad ? 4 : 3;
            const OUTPUT_MESH_INDEX at[4] = {
                tri[0], tri[1], tri[2], quad ? tri[5] : tri[2],
            };
            M_XVERT mid = xv[tri[0]];
            mid.x = mid.y = mid.z = 0.0f;
            for (int32_t c = 0; c < corners; c++) {
                mid.x += xv[at[c]].x * (1.0f / corners);
                mid.y += xv[at[c]].y * (1.0f / corners);
                mid.z += xv[at[c]].z * (1.0f / corners);
            }
            float fade;
            float by_light;
            const uint32_t mid_q = M_BumpVertex(
                lights, &frame, &mid, glow, &fade, &by_light)
                & 0xFF;
            for (int32_t c = 0; c < step; c++) {
                k[c] = (k[c] & ~0xFFu) | mid_q;
            }
        }
        M_BUMP_ENTRY *const e = &m_BumpQueue[queued++];
        e->first = i;
        e->page = tri_page;
        e->step = step;
        memcpy(e->k, k, sizeof(k));
        e->max_by_light = max_by_light;
        e->z_min = z_min;
        if (queued == M_BUMP_QUEUE) {
            M_FlushBump(
                batcher, bind, lights, indices, m_BumpQueue, queued, page,
                glow);
            queued = 0;
        }
        i += step;
    }
    M_FlushBump(
        batcher, bind, lights, indices, m_BumpQueue, queued, page, glow);
    m_DepthBias = 1.0f;
}

// A room with no light of its own (TR1's have none): where its vertices are
// brightest says where the light they were made with came from. The relief is
// lit from that place, with no falloff, and as strongly as the contrast of the
// room allows, but never under the least. Worked out once for a room and kept.
#define M_BUMP_ROOM_MIN_VERTICES 12
#define M_BUMP_ROOM_FULL_CONTRAST 0.5f
// The power of that light (a light of 0.25 or more gives the relief whole;
// see M_BumpVertex), at the least contrast and at the fullest.
#define M_BUMP_ROOM_MIN_POWER 0.125f
#define M_BUMP_ROOM_MAX_POWER 0.25f
#define M_BUMP_ROOM_CACHE 6

static float M_VertexBrightness(const ROOM_VERTEX *vertex);
static void M_VertexWorld(
    const ROOM *room, const ROOM_VERTEX *vertex, float out[3]);

static struct {
    const ROOM *room;
    const ROOM_VERTEX *vertices;
    int32_t num_vertices;
    int32_t x;
    int32_t z;
    bool found;
    float at[3];
    float power;
} m_BumpRooms[M_BUMP_ROOM_CACHE] = {};
static int32_t m_BumpRoomNext = 0;

// The place of the brightest part of the room's vertices and the power of
// the light that gives, or false when the room has too few vertices or no
// part of it is brighter than the rest.
static bool M_RoomBrightLight(
    const ROOM *const room, float at[3], float *const power)
{
    for (int32_t n = 0; n < M_BUMP_ROOM_CACHE; n++) {
        const typeof(m_BumpRooms[0]) *const c = &m_BumpRooms[n];
        if (c->room == room && c->vertices == room->mesh.vertices
            && c->num_vertices == room->mesh.num_vertices
            && c->x == room->pos.x && c->z == room->pos.z) {
            memcpy(at, c->at, sizeof(c->at));
            *power = c->power;
            return c->found;
        }
    }
    typeof(m_BumpRooms[0]) *const c = &m_BumpRooms[m_BumpRoomNext];
    m_BumpRoomNext = (m_BumpRoomNext + 1) % M_BUMP_ROOM_CACHE;
    *c = (typeof(*c)) {
        .room = room,
        .vertices = room->mesh.vertices,
        .num_vertices = room->mesh.num_vertices,
        .x = room->pos.x,
        .z = room->pos.z,
    };
    const ROOM_VERTEX *const vertices = room->mesh.vertices;
    const int32_t num = room->mesh.num_vertices;
    if (vertices == nullptr || num < M_BUMP_ROOM_MIN_VERTICES) {
        return false;
    }
    float sum = 0.0f;
    for (int32_t i = 0; i < num; i++) {
        sum += M_VertexBrightness(&vertices[i]);
    }
    const float mean = sum / num;
    float weight = 0.0f;
    float weighted_brightness = 0.0f;
    float mid[3] = {};
    for (int32_t i = 0; i < num; i++) {
        const float b = M_VertexBrightness(&vertices[i]);
        const float w = b - mean;
        if (w <= 0.0f) {
            continue;
        }
        float p[3];
        M_VertexWorld(room, &vertices[i], p);
        weight += w;
        weighted_brightness += w * b;
        for (int32_t k = 0; k < 3; k++) {
            mid[k] += w * p[k];
        }
    }
    if (weight <= 0.0f) {
        return false;
    }
    const float contrast = weighted_brightness / weight - mean;
    const float strength =
        M_Clamp(contrast * (1.0f / M_BUMP_ROOM_FULL_CONTRAST), 0.0f, 1.0f);
    c->found = true;
    for (int32_t k = 0; k < 3; k++) {
        c->at[k] = mid[k] / weight;
    }
    c->power = M_BUMP_ROOM_MIN_POWER
        + (M_BUMP_ROOM_MAX_POWER - M_BUMP_ROOM_MIN_POWER) * strength;
    memcpy(at, c->at, sizeof(c->at));
    *power = c->power;
    return true;
}

// Draws the relief of a room's faces near the camera over them, each lit by
// the light nearest it, the room's or a passing one (or from above, with
// none).
static void M_EmitBump(
    MESH_BATCHER *const batcher, const M_BINDING *const bind,
    const MESH_INSTANCE *const inst)
{
    // Most rooms lie wholly past the relief's reach: their faces are not
    // looked at (a passing light takes it three times as far).
    if (batcher->z_near > M_BUMP_RANGE * M_BUMP_LIGHT_REACH) {
        return;
    }
    const float(*const v)[4] = m_Frame.view;
    M_BUMP_LIGHTS lights = {};
    const ROOM *const room = inst->room;
    for (int32_t i = 0;
         room != nullptr && i < room->num_lights
         && lights.count < M_BUMP_MAX_LIGHTS;
         i++) {
        const LIGHT *const light = &room->lights[i];
        if (light->type == LIGHT_TYPE_FOG_BULB
            || light->type == LIGHT_TYPE_SHADOW) {
            continue;
        }
        if (light->type == LIGHT_TYPE_SUN) {
            // A sun has a direction only: a far point that way, towards the
            // light, whose reach takes in everything.
            if (light->layout != LIGHT_LAYOUT_LEGACY) {
                continue;
            }
            const float d[3] = {
                (float)light->u.legacy.dir.x,
                (float)light->u.legacy.dir.y,
                (float)light->u.legacy.dir.z,
            };
            const float len_sq = M_Dot3(d, d);
            if (len_sq <= 0.0f) {
                continue;
            }
            const float dist = M_BUMP_SUN_DISTANCE * M_InvSqrt(len_sq);
            for (int32_t r = 0; r < 3; r++) {
                lights.pos[lights.count][r] =
                    (v[0][r] * d[0] + v[1][r] * d[1] + v[2][r] * d[2]) * dist;
            }
            lights.radius[lights.count] = M_BUMP_SUN_DISTANCE * 64.0f;
            lights.power[lights.count] = 1.0f;
            lights.passing[lights.count] = false;
            lights.count++;
            continue;
        }
        const float x = light->pos.x;
        const float y = light->pos.y;
        const float z = light->pos.z;
        for (int32_t r = 0; r < 3; r++) {
            lights.pos[lights.count][r] =
                v[0][r] * x + v[1][r] * y + v[2][r] * z + v[3][r];
        }
        lights.radius[lights.count] = M_BUMP_ROOM_RADIUS;
        lights.power[lights.count] = 1.0f;
        lights.passing[lights.count] = false;
        lights.count++;
    }
    // No light in the room: the place its vertices are brightest in.
    float bright_at[3];
    float bright_power;
    if (lights.count == 0 && room != nullptr
        && M_RoomBrightLight(room, bright_at, &bright_power)) {
        for (int32_t r = 0; r < 3; r++) {
            lights.pos[0][r] = v[0][r] * bright_at[0] + v[1][r] * bright_at[1]
                + v[2][r] * bright_at[2] + v[3][r];
        }
        // No falloff: the relief is lit alike all through the room.
        lights.radius[0] = M_BUMP_SUN_DISTANCE * 64.0f;
        lights.power[0] = bright_power;
        lights.passing[0] = false;
        lights.count = 1;
    }
    // The passing ones (a muzzle flash, a flare), as the room's lighting has
    // them this frame, with the reach the lighting gives them.
    const DC_UNIFORM_LIGHTS *const passing = m_Frame.lights;
    bool any_passing = false;
    for (int32_t i = 0; passing != nullptr && i < passing->num_lights
         && lights.count < M_BUMP_MAX_LIGHTS;
         i++) {
        const DC_UNIFORM_LIGHT *const light = &passing->lights[i];
        float radius;
        if (light->kind == OUTPUT_DYNAMIC_LIGHT_RGB) {
            radius = light->falloff * 0.5f * M_BUMP_PASSING_REACH;
        } else if (light->kind == OUTPUT_DYNAMIC_LIGHT_LUM) {
            radius = M_Exp2(light->falloff) * M_BUMP_PASSING_REACH;
        } else {
            continue;
        }
        if (radius < 1.0f) {
            continue;
        }
        const float *const p = light->pos;
        for (int32_t r = 0; r < 3; r++) {
            lights.pos[lights.count][r] =
                v[0][r] * p[0] + v[1][r] * p[1] + v[2][r] * p[2] + v[3][r];
        }
        lights.radius[lights.count] = radius;
        lights.power[lights.count] = M_BUMP_PASSING_POWER;
        lights.passing[lights.count] = true;
        lights.count++;
        any_passing = true;
    }
    const float limit =
        any_passing ? M_BUMP_RANGE * M_BUMP_LIGHT_REACH : M_BUMP_RANGE;
    if (batcher->z_near > limit) {
        return;
    }

    const OUTPUT_MESH_INDEX *const indices =
        Vector_GetData(bind->mesh->opaque_vertex_indices);
    m_Frame.emit = M_EMIT_BUMP;
    // The shade first, then the shine.
    for (int32_t pass = 0; pass < 2; pass++) {
        int32_t page = INT32_MIN;
        for (int32_t i = 0; i < batcher->range_count; i++) {
            const M_RANGE *const range = &batcher->ranges[i];
            if (range->z_near <= limit) {
                M_EmitBumpRange(
                    batcher, bind, &lights, indices + range->start,
                    range->count, &page, pass == 1);
            }
        }
    }
    m_Frame.emit = M_EMIT_PLAIN;
}

// Pulls the texture coordinates of each face of a run towards the face's
// middle by up to half a texel, so that bilinear filtering samples nothing
// of the next texture on the page (the desktop clamps them to the texture in
// its shader). Once, as the coordinates are filled: TR's vertices belong
// each to one face.
static void M_InsetRun(
    const M_BINDING *const bind, const OUTPUT_MESH_INDEX *const indices,
    const int32_t count)
{
    int32_t i = 0;
    while (i + 2 < count) {
        const bool quad = i + 5 < count && M_IsQuad(&indices[i])
            && bind->page[indices[i + 3]] == bind->page[indices[i]];
        const int32_t n = quad ? 4 : 3;
        const OUTPUT_MESH_INDEX corners[4] = {
            indices[i], indices[i + 1], indices[i + 2],
            quad ? indices[i + 5] : indices[i],
        };
        i += quad ? 6 : 3;
        if (bind->page[corners[0]] == M_NO_PAGE) {
            continue;
        }
        float uv[4][2];
        float cu = 0.0f;
        float cv = 0.0f;
        for (int32_t k = 0; k < n; k++) {
            uv[k][0] = bind->uv[corners[k] * 2] * (1.0f / M_UV_ONE);
            uv[k][1] = bind->uv[corners[k] * 2 + 1] * (1.0f / M_UV_ONE);
            cu += uv[k][0];
            cv += uv[k][1];
        }
        const float inv_n = n == 4 ? 0.25f : 1.0f / 3.0f;
        cu *= inv_n;
        cv *= inv_n;
        for (int32_t k = 0; k < n; k++) {
            bind->uv[corners[k] * 2] =
                M_PackUV(uv[k][0] + M_Inset(cu - uv[k][0]));
            bind->uv[corners[k] * 2 + 1] =
                M_PackUV(uv[k][1] + M_Inset(cv - uv[k][1]));
        }
    }
}

// Whether the faces' texture coordinates are pulled in (bilinear filtering
// on, where the mode allows it).
static bool m_InsetUV = false;

// Looks every vertex's texture up again (the animated textures moved on, or
// the filtering changed), and pulls the faces' coordinates in if need be.
static void M_RefillTextures(M_BINDING *const bind)
{
    const OUTPUT_MESH_VERTEX *const vertices =
        Vector_GetData(bind->mesh->vertices);
    for (int32_t i = 0; i < bind->vertex_count; i++) {
        if (bind->vref != nullptr) {
            M_FillTexture(bind, &vertices[bind->vref[i]], bind->uvw[i], i);
        } else {
            M_FillTexture(bind, &vertices[i], vertices[i].uvw_idx, i);
        }
    }
    if (m_InsetUV) {
        const OUTPUT_MESH *const mesh = bind->mesh;
        const OUTPUT_MESH_INDEX *const opaque =
            Vector_GetData(mesh->opaque_vertex_indices);
        const int32_t clean = mesh->opaque_nodiscard_count;
        M_InsetRun(bind, opaque, clean);
        M_InsetRun(
            bind, opaque + clean, mesh->opaque_vertex_indices->count - clean);
        M_InsetRun(
            bind, Vector_GetData(mesh->blend_add_vertex_indices),
            mesh->blend_add_vertex_indices->count);
        for (int32_t i = 0; i < mesh->transparent_faces->count; i++) {
            const OUTPUT_MESH_FACE *const face =
                Vector_Get(mesh->transparent_faces, i);
            M_InsetRun(
                bind, (const OUTPUT_MESH_INDEX *)face->vertex_indices,
                face->vertex_count);
        }
    }
    bind->tex_dirty = false;
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
    if (bind->tex_dirty) {
        M_RefillTextures(bind);
    }
    m_Frame.lara_instance = caster == M_LARA_NOT_CASTER
        || (caster >= 0 && m_Casters[caster].lara);
    // Lara's shadow falls on the rooms and on herself only, not on enemies
    // or other objects. Boxes make a rough shadow; on the caster itself it
    // would only look wrong.
    // Nor on a room under water while she wades above it: seen through the
    // surface it looks cast on the water itself. Swimming at the surface, it
    // falls on the bottom.
    // Told before the transform: a room's chunks go out on the way.
    const LARA_WATER_STATE water = Lara_GetLaraInfo()->water_status;
    const bool is_room = bind->needs_room_lights
        && (inst->room == nullptr || !inst->room->flags.underwater
            || water == LWS_UNDERWATER || water == LWS_SURFACE);
    DC_PVR_SetShadowReceiver(
        is_room && M_ShadowType() == SHADOW_TYPE_VOLUME);
    batcher->emitted = false;
    batcher->chunk_emit_us = 0;
#if defined(TRX_DC_WATCHDOG)
    const uint64_t xform_start = timer_us_gettime64();
    const bool transformed = M_TransformInstance(batcher, inst, bind);
    // The chunks drawn on the way count as emission, not transformation.
    g_DC_BatcherXformUs += (uint32_t)(timer_us_gettime64() - xform_start)
        - batcher->chunk_emit_us;
    g_DC_ProbeUs[DC_PROBE_EMIT] += batcher->chunk_emit_us; // TEMP
    if (!transformed) {
        return;
    }
#else
    if (!M_TransformInstance(batcher, inst, bind)) {
        return;
    }
#endif
    const bool is_caster = caster >= 0;
#if defined(TRX_DC_WATCHDOG)
    m_ShadowReceivers += is_room && M_ShadowType() == SHADOW_TYPE_VOLUME;
    m_ShadowRooms += bind->needs_room_lights;
#endif
    if (m_Soft.active && pass == SCENE_PASS_OPAQUE) {
        if (is_caster) {
            DC_PROBE(DC_PROBE_SOFT, M_EmitSelfShadow(batcher, bind, caster));
        } else if (is_room) {
            DC_PROBE(DC_PROBE_SOFT, M_EmitSoftShadows(batcher, bind));
        }
    }

    const OUTPUT_MESH *const mesh = inst->mesh;
    int32_t page = INT32_MIN;
    if (blended) {
        M_EmitIndices(
            batcher, bind, Vector_GetData(mesh->blend_add_vertex_indices),
            mesh->blend_add_vertex_indices->count,
            pass == SCENE_PASS_BLEND_ADD ? DC_BLEND_ADD : DC_BLEND_SUB, &page);
        return;
    }

    // An instance faded out as a whole draws every face blended.
    const bool faded = inst->tint.a < 1.0f;
    const OUTPUT_MESH_INDEX *const opaque = Vector_GetData(mesh->opaque_vertex_indices);
    // The shaded side of the cel shading is kept face by face as they go
    // out, and drawn over them after.
    batcher->cel_emit = batcher->cel && !faded;
    DC_PROBE_START(probe_emit);
    int32_t last_run = -1;
    for (int32_t i = 0; !batcher->emitted && i < batcher->range_count; i++) {
        const M_RANGE *const range = &batcher->ranges[i];
        // The header carries the blend too: a new run means a new one.
        if (range->run != last_run) {
            last_run = range->run;
            page = INT32_MIN;
        }
        M_EmitIndices(
            batcher, bind, opaque + range->start, range->count,
            faded              ? DC_BLEND_ALPHA
                : range->run == 0 ? DC_BLEND_OPAQUE
                                  : DC_BLEND_PUNCH,
            &page);
    }
    DC_PROBE_STOP(DC_PROBE_EMIT, probe_emit);
    batcher->cel_emit = false;
    if (batcher->cel_count > 0) {
        DC_PROBE(DC_PROBE_CEL, M_FlushCel(batcher));
    }
    m_FrameRooms |= bind->needs_room_lights;
    if (m_Frame.bump && m_Frame.lighting && !faded && bind->needs_room_lights) {
        DC_PROBE(DC_PROBE_BUMP, M_EmitBump(batcher, bind, inst));
    }
    if (batcher->env && !faded) {
        // The reflections, added over the faces just drawn.
        m_Frame.emit = M_EMIT_ENV;
        page = INT32_MIN;
        DC_PROBE(
            DC_PROBE_ENV,
            M_EmitIndices(
                batcher, bind, bind->env, bind->env_count, DC_BLEND_ADD,
                &page));
        m_Frame.emit = M_EMIT_PLAIN;
    }
    page = INT32_MIN;
    DC_PROBE_START(probe_transp); // TEMP
    g_DC_ProbeUs[DC_PROBE_P_NTRANSP] += mesh->transparent_faces->count; // TEMP
    // The PowerVR sorts the translucent list itself: the faces go as one run.
    M_EmitIndices(
        batcher, bind, bind->transp, bind->transp_count, DC_BLEND_ALPHA, &page);
    DC_PROBE_STOP(DC_PROBE_P_TRANSP, probe_transp); // TEMP
}

static void M_PrepareFrame(void)
{
    if (m_WhiteARGB[M_WHITE_K_MAX] == 0) {
        M_InitWhite();
    }
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
    m_Frame.room_cel = g_Config.rendering.dc_room_cel;
    m_Frame.bump = g_Config.rendering.dc_bump_mapping
        && m_BumpOn && !m_Frame.ortho;
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
    m_Frame.lights_mode = -1;
    DC_PVR_GetDepth3D(&m_Frame.depth);
    m_Frame.emit = 0; // M_EMIT_PLAIN
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
    m_Soft.active = M_IS_SOFT(M_ShadowType())
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
        DC_PROBE(DC_PROBE_P_PSOFT, M_PrepareSoft()); // TEMP
    } else {
        m_Soft.active = false;
    }
    m_Frame.cull = !m_Frame.ortho;
    // Bilinear filtering samples half a texel past a face's edge, into the
    // next texture on the page; the faces' coordinates are pulled in by that
    // much instead, as they are filled (M_InsetRun).
    const bool inset =
        g_Config.rendering.texture_filter == TEXTURE_FILTER_BILINEAR;
    if (inset != m_InsetUV) {
        m_InsetUV = inset;
        for (int32_t i = 0; i < batcher->bindings->count; i++) {
            (*(M_BINDING **)Vector_Get(batcher->bindings, i))->tex_dirty = true;
        }
    }
    DC_PVR_SetFilter(inset);
    VECTOR *const staged = batcher->staged[pass];
    const int16_t *const casters = Vector_GetData(batcher->casters[pass]);
    for (int32_t i = 0; i < staged->count; i++) {
        DC_PROBE(
            DC_PROBE_P_DRAWI,
            M_DrawInstance(batcher, Vector_Get(staged, i), pass, casters[i]));
    }
    DC_PVR_SetShadowReceiver(false);
}

static bool M_IsDirty(const SCENE_SOURCE *const source, const SCENE_PASS pass)
{
    const MESH_BATCHER *const batcher = source->priv;
    return batcher->staged[pass]->count > 0;
}

// The animated textures moved on: the meshes that carry any look theirs up
// again as they are next drawn, so the ones out of sight cost nothing.
static void M_AnimateTextures(const SCENE_SOURCE *const source)
{
    MESH_BATCHER *const batcher = source->priv;
    for (int32_t i = 0; i < batcher->bindings->count; i++) {
        M_BINDING *const bind = *(M_BINDING **)Vector_Get(batcher->bindings, i);
        if (bind->mesh->animated_vertices->count > 0) {
            bind->tex_dirty = true;
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
    M_GrowVertexBuffers(batcher, M_RESERVED_VERTICES - 256);
    batcher->cel_runs = Vector_Create(sizeof(M_CEL_RUN));
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
    Memory_Free(batcher->share_slot);
    Memory_Free(batcher->soft);
    Memory_Free(batcher->soft_off);
    Memory_Free(batcher->oargb);
    Memory_Free(batcher->sargb);
    Memory_Free(batcher->env_uv);
    Memory_Free(batcher->eargb);
    Memory_Free(batcher->cel_buf);
    Vector_Free(batcher->cel_runs);
    Memory_Free(batcher);
}

// A face of a run, for sorting the run by texture page: a quad's halves
// (M_PairQuads) stay together.
typedef struct {
    int32_t start;
    int32_t length;
    int32_t page;
} M_PRIM;

static int M_ComparePrims(const void *const a, const void *const b)
{
    const M_PRIM *const pa = a;
    const M_PRIM *const pb = b;
    if (pa->page != pb->page) {
        return pa->page < pb->page ? -1 : 1;
    }
    return pa->start < pb->start ? -1 : 1;
}

// Puts the faces of a run that sample one page together, so that the list
// takes one header for all of them (M_EmitIndices). The order within a run
// does not matter to the PowerVR, which sorts by depth.
static void M_SortByPage(
    const M_BINDING *const bind, OUTPUT_MESH_INDEX *const indices,
    const int32_t count)
{
    if (count < 6) {
        return;
    }
    M_PRIM *const prims = Memory_Alloc(sizeof(M_PRIM) * (count / 3));
    int32_t n = 0;
    for (int32_t i = 0; i + 2 < count;) {
        const bool quad = i + 5 < count && M_IsQuad(&indices[i])
            && bind->page[indices[i + 3]] == bind->page[indices[i]];
        prims[n++] = (M_PRIM) {
            .start = i,
            .length = quad ? 6 : 3,
            .page = bind->page[indices[i]],
        };
        i += quad ? 6 : 3;
    }
    qsort(prims, n, sizeof(M_PRIM), M_ComparePrims);
    OUTPUT_MESH_INDEX *const sorted = Memory_Alloc(sizeof(*indices) * count);
    int32_t at = 0;
    for (int32_t i = 0; i < n; i++) {
        memcpy(
            &sorted[at], &indices[prims[i].start],
            sizeof(*indices) * prims[i].length);
        at += prims[i].length;
    }
    memcpy(indices, sorted, sizeof(*indices) * at);
    Memory_Free(sorted);
    Memory_Free(prims);
}

// A face of a run, for sorting a room's run by chunk then page.
typedef struct {
    int32_t start;
    int32_t length;
    int32_t page;
    int32_t cell;
} M_CHUNK_PRIM;

static int M_CompareChunkPrims(const void *const a, const void *const b)
{
    const M_CHUNK_PRIM *const pa = a;
    const M_CHUNK_PRIM *const pb = b;
    if (pa->cell != pb->cell) {
        return pa->cell < pb->cell ? -1 : 1;
    }
    if (pa->page != pb->page) {
        return pa->page < pb->page ? -1 : 1;
    }
    return pa->start < pb->start ? -1 : 1;
}

// Gives the vertices of a run of faces their places in the new order, after
// the ones placed so far. False when a face of another owner placed one of
// them first: such a vertex would be transformed with the wrong chunk.
static bool M_PlaceVertices(
    const OUTPUT_MESH_INDEX *const indices, const int32_t count,
    int32_t *const remap, int32_t *const owner, const int32_t who,
    int32_t *const next)
{
    bool own = true;
    for (int32_t i = 0; i < count; i++) {
        const OUTPUT_MESH_INDEX v = indices[i];
        if (owner[v] < 0) {
            owner[v] = who;
            remap[v] = (*next)++;
        } else if (owner[v] != who) {
            own = false;
        }
    }
    return own;
}

static void M_RemapIndices(
    OUTPUT_MESH_INDEX *const indices, const int32_t count,
    const int32_t *const remap)
{
    for (int32_t i = 0; i < count; i++) {
        indices[i] = (OUTPUT_MESH_INDEX)remap[indices[i]];
    }
}

// Sorts each opaque run of a room by chunk (the cell of the grid its faces'
// middle lies in) and within a chunk by page, then lays the room's vertices
// out in the same order, so that each chunk's vertices are one range of the
// mesh, and the faces outside the runs (the added and transparent ones) have
// another after them: a chunk that shows is transformed as a run, with no
// list of its vertices. A room it cannot cut in two or more, or whose faces
// share a vertex across chunks, stays whole, sorted by page alone.
static void M_BuildChunks(M_BINDING *const bind)
{
    OUTPUT_MESH *const mesh = bind->mesh;
    OUTPUT_MESH_INDEX *const opaque = Vector_GetData(mesh->opaque_vertex_indices);
    const int32_t total = mesh->opaque_vertex_indices->count;
    const int32_t clean = mesh->opaque_nodiscard_count;
    const OUTPUT_MESH_VERTEX *const vertices = Vector_GetData(mesh->vertices);

    // The grid's cells, grown until the room has few enough.
    int32_t cell = M_CHUNK_CELL;
    int32_t dim[3];
    for (;;) {
        for (int32_t k = 0; k < 3; k++) {
            dim[k] =
                (int32_t)((bind->bounds[1][k] - bind->bounds[0][k]) / cell) + 1;
        }
        if (dim[0] * dim[1] * dim[2] <= M_MAX_CHUNKS) {
            break;
        }
        cell *= 2;
    }
    const int32_t cells = dim[0] * dim[1] * dim[2];
    if (bind->vertex_count < M_CHUNK_MIN_VERTICES || cells < 2
        || !bind->needs_room_lights || bind->billboard || total < 6) {
        M_SortByPage(bind, opaque, clean);
        M_SortByPage(bind, opaque + clean, total - clean);
        return;
    }

    // Each run's faces by cell, then page, as M_SortByPage keeps quads.
    M_CHUNK_PRIM *const prims = Memory_Alloc(sizeof(M_CHUNK_PRIM) * (total / 3));
    OUTPUT_MESH_INDEX *const sorted = Memory_Alloc(sizeof(*opaque) * total);
    int32_t used[M_MAX_CHUNKS];
    for (int32_t i = 0; i < cells; i++) {
        used[i] = -1;
    }
    int32_t chunk_count = 0;
    int32_t run_cells[2][M_MAX_CHUNKS][2]; // per run and cell: start, count
    memset(run_cells, 0, sizeof(run_cells));
    for (int32_t run = 0; run < 2; run++) {
        const int32_t base = run == 0 ? 0 : clean;
        const int32_t count = run == 0 ? clean : total - clean;
        OUTPUT_MESH_INDEX *const indices = opaque + base;
        int32_t n = 0;
        for (int32_t i = 0; i + 2 < count;) {
            const bool quad = i + 5 < count && M_IsQuad(&indices[i])
                && bind->page[indices[i + 3]] == bind->page[indices[i]];
            const int32_t length = quad ? 6 : 3;
            float mid[3] = {};
            for (int32_t j = 0; j < length; j++) {
                const OUTPUT_MESH_VERTEX *const v = &vertices[indices[i + j]];
                mid[0] += v->pos.x;
                mid[1] += v->pos.y;
                mid[2] += v->pos.z;
            }
            int32_t at[3];
            for (int32_t k = 0; k < 3; k++) {
                at[k] = (int32_t)((mid[k] / length - bind->bounds[0][k]) / cell);
                CLAMP(at[k], 0, dim[k] - 1);
            }
            const int32_t c = (at[2] * dim[1] + at[1]) * dim[0] + at[0];
            if (used[c] < 0) {
                used[c] = chunk_count++;
            }
            prims[n++] = (M_CHUNK_PRIM) {
                .start = i,
                .length = length,
                .page = bind->page[indices[i]],
                .cell = used[c],
            };
            i += length;
        }
        qsort(prims, n, sizeof(M_CHUNK_PRIM), M_CompareChunkPrims);
        int32_t at = 0;
        for (int32_t i = 0; i < n; i++) {
            const int32_t c = prims[i].cell;
            if (run_cells[run][c][1] == 0) {
                run_cells[run][c][0] = base + at;
            }
            run_cells[run][c][1] += prims[i].length;
            memcpy(
                &sorted[at], &indices[prims[i].start],
                sizeof(*indices) * prims[i].length);
            at += prims[i].length;
        }
        memcpy(indices, sorted, sizeof(*indices) * at);
    }
    Memory_Free(sorted);
    Memory_Free(prims);
    if (chunk_count < 2) {
        return;
    }

    // Each vertex's place in the new order: the chunks' faces in turn, then
    // the faces outside the runs, then whatever no face names.
    const int32_t nv = bind->vertex_count;
    int32_t *const remap = Memory_Alloc(sizeof(int32_t) * nv);
    int32_t *const owner = Memory_Alloc(sizeof(int32_t) * nv);
    for (int32_t i = 0; i < nv; i++) {
        remap[i] = -1;
        owner[i] = -1;
    }
    M_CHUNK *const chunks = Memory_Alloc(sizeof(M_CHUNK) * chunk_count);
    int32_t next = 0;
    bool own = true;
    for (int32_t c = 0; c < chunk_count; c++) {
        M_CHUNK *const chunk = &chunks[c];
        chunk->vert_start = next;
        for (int32_t run = 0; run < 2; run++) {
            chunk->start[run] = run_cells[run][c][0];
            chunk->count[run] = run_cells[run][c][1];
            own &= M_PlaceVertices(
                opaque + chunk->start[run], chunk->count[run], remap, owner, c,
                &next);
        }
        chunk->vert_count = next - chunk->vert_start;
    }
    const int32_t always_start = next;
    own &= M_PlaceVertices(
        Vector_GetData(mesh->blend_add_vertex_indices),
        mesh->blend_add_vertex_indices->count, remap, owner, chunk_count,
        &next);
    for (int32_t i = 0; i < mesh->transparent_faces->count; i++) {
        const OUTPUT_MESH_FACE *const face = Vector_Get(mesh->transparent_faces, i);
        own &= M_PlaceVertices(
            (const OUTPUT_MESH_INDEX *)face->vertex_indices, face->vertex_count,
            remap, owner, chunk_count, &next);
    }
    const int32_t always_count = next - always_start;
    if (!own) {
        Memory_Free(chunks);
        Memory_Free(owner);
        Memory_Free(remap);
        return;
    }
    for (int32_t i = 0; i < nv; i++) {
        if (remap[i] < 0) {
            remap[i] = next++;
        }
    }

    // The vertices, and what the binding keeps per vertex, in the new order.
    OUTPUT_MESH_VERTEX *const vs = Vector_GetData(mesh->vertices);
    OUTPUT_MESH_VERTEX *const moved = Memory_Alloc(sizeof(*vs) * nv);
    uint16_t *const uv = Memory_Alloc(sizeof(uint16_t) * 2 * (nv + 1));
    int8_t *const page = Memory_Alloc(nv + 1);
    for (int32_t i = 0; i < nv; i++) {
        const int32_t to = remap[i];
        moved[to] = vs[i];
        uv[to * 2] = bind->uv[i * 2];
        uv[to * 2 + 1] = bind->uv[i * 2 + 1];
        page[to] = bind->page[i];
    }
    memcpy(vs, moved, sizeof(*vs) * nv);
    Memory_Free(moved);
    Memory_Free(bind->uv);
    bind->uv = uv;
    Memory_Free(bind->page);
    bind->page = page;

    // Every index into them.
    M_RemapIndices(opaque, total, remap);
    M_RemapIndices(
        Vector_GetData(mesh->blend_add_vertex_indices),
        mesh->blend_add_vertex_indices->count, remap);
    for (int32_t i = 0; i < mesh->transparent_faces->count; i++) {
        OUTPUT_MESH_FACE *const face = Vector_Get(mesh->transparent_faces, i);
        M_RemapIndices(
            (OUTPUT_MESH_INDEX *)face->vertex_indices, face->vertex_count, remap);
    }
    if (mesh->opaque_discard_indices != nullptr) {
        M_RemapIndices(
            Vector_GetData(mesh->opaque_discard_indices),
            mesh->opaque_discard_indices->count, remap);
    }
    // The animated vertices as runs of the new order (the batcher only asks
    // whether a mesh has any).
    VECTOR *const animated = mesh->animated_vertices;
    if (animated->count > 0) {
        bool *const is_animated = Memory_Alloc(nv);
        memset(is_animated, 0, nv);
        for (int32_t i = 0; i < animated->count; i++) {
            const OUTPUT_VERTEX_RANGE *const range = Vector_Get(animated, i);
            for (int32_t v = range->vertex_start;
                 v < range->vertex_start + range->vertex_count && v < nv; v++) {
                is_animated[remap[v]] = true;
            }
        }
        Vector_Clear(animated);
        for (int32_t i = 0; i < nv;) {
            if (!is_animated[i]) {
                i++;
                continue;
            }
            const int32_t start = i;
            while (i < nv && is_animated[i]) {
                i++;
            }
            const OUTPUT_VERTEX_RANGE range = {
                .vertex_start = start,
                .vertex_count = i - start,
            };
            Vector_Add(animated, &range);
        }
        Memory_Free(is_animated);
    }

    // Each chunk's bounds, from its vertices where they lie now.
    for (int32_t c = 0; c < chunk_count; c++) {
        M_CHUNK *const chunk = &chunks[c];
        float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (int32_t i = chunk->vert_start;
             i < chunk->vert_start + chunk->vert_count; i++) {
            const float p[3] = { vs[i].pos.x, vs[i].pos.y, vs[i].pos.z };
            for (int32_t k = 0; k < 3; k++) {
                lo[k] = MIN(lo[k], p[k]);
                hi[k] = MAX(hi[k], p[k]);
            }
        }
        float radius_sq = 0.0f;
        for (int32_t k = 0; k < 3; k++) {
            const float half = (hi[k] - lo[k]) * 0.5f;
            chunk->center[k] = lo[k] + half;
            radius_sq += half * half;
        }
        chunk->radius = sqrtf(radius_sq);
    }
    bind->chunks = chunks;
    bind->chunk_count = chunk_count;
    bind->always_start = always_start;
    bind->always_count = always_count;
    Memory_Free(owner);
    Memory_Free(remap);
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

static bool M_SameVertex(
    const OUTPUT_MESH_VERTEX *const a, const OUTPUT_MESH_VERTEX *const b)
{
    return a->pos.x == b->pos.x && a->pos.y == b->pos.y && a->pos.z == b->pos.z
        && a->pos.w == b->pos.w && a->normal.x == b->normal.x
        && a->normal.y == b->normal.y && a->normal.z == b->normal.z
        && a->flags == b->flags && a->shade == b->shade
        && a->light_table_idx == b->light_table_idx
        && memcmp(&a->color, &b->color, sizeof(a->color)) == 0;
}

static uint32_t M_HashVertex(const OUTPUT_MESH_VERTEX *const v)
{
    uint32_t h = 2166136261u;
    const int32_t parts[] = {
        v->pos.x, v->pos.y, v->pos.z, v->normal.x, v->flags, v->shade,
        (int32_t)(v->color.r | v->color.g << 8 | v->color.b << 16),
    };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        h = (h ^ (uint32_t)parts[i]) * 16777619u;
    }
    return h;
}

// A room's faces each hold their own corners, which repeat a vertex of the
// room but for the texture: about three corners to a vertex. Each distinct
// vertex is kept once, and each corner its index and texture index (some 20
// bytes a corner less). Rooms only: an object's corners are rewritten in
// place as it moves (objects.c, M_ForEachFaceVertex).
static void M_ShareVertices(M_BINDING *const bind)
{
    const int32_t n = bind->vertex_count;
    if (n == 0 || !bind->needs_room_lights || bind->needs_object_light
        || bind->needs_own_light) {
        return;
    }
    OUTPUT_MESH *const mesh = bind->mesh;
    OUTPUT_MESH_VERTEX *const vs = Vector_GetData(mesh->vertices);
    for (int32_t i = 0; i < n; i++) {
        if (vs[i].uvw_idx < INT16_MIN || vs[i].uvw_idx > INT16_MAX) {
            return;
        }
    }
    int32_t size = 1;
    while (size < n * 2) {
        size <<= 1;
    }
    int32_t *const table = Memory_Alloc(sizeof(int32_t) * size);
    memset(table, 0xFF, sizeof(int32_t) * size);
    uint16_t *const vref = Memory_Alloc(sizeof(uint16_t) * n);
    int16_t *const uvw = Memory_Alloc(sizeof(int16_t) * n);
    int32_t kept = 0;
    for (int32_t i = 0; i < n; i++) {
        OUTPUT_MESH_VERTEX v = vs[i];
        uvw[i] = (int16_t)v.uvw_idx;
        v.uvw_idx = 0;
        uint32_t h = M_HashVertex(&v) & (size - 1);
        while (table[h] >= 0 && !M_SameVertex(&vs[table[h]], &v)) {
            h = (h + 1) & (size - 1);
        }
        if (table[h] < 0) {
            // Never past i: the kept vertices fill the front in order.
            vs[kept] = v;
            table[h] = kept++;
        }
        vref[i] = (uint16_t)table[h];
    }
    Memory_Free(table);
    mesh->vertices->count = kept;
    Vector_ShrinkToFit(mesh->vertices);
    bind->vref = vref;
    bind->uvw = uvw;
}

// An object keeps a vertex per corner (objects.c rewrites them as it
// moves), but the corners of a vertex light and project the same: each is
// pointed at the first like it, for the object loop to copy.
static void M_FindTwins(M_BINDING *const bind)
{
    const int32_t n = bind->vertex_count;
    if (n < 8 || n > 0xFFFF || !bind->plain_object || bind->deforms) {
        return;
    }
    const OUTPUT_MESH_VERTEX *const vs = Vector_GetData(bind->mesh->vertices);
    int32_t size = 1;
    while (size < n * 2) {
        size <<= 1;
    }
    int32_t *const table = Memory_Alloc(sizeof(int32_t) * size);
    memset(table, 0xFF, sizeof(int32_t) * size);
    uint16_t *const twin = Memory_Alloc(sizeof(uint16_t) * n);
    int32_t twins = 0;
    for (int32_t i = 0; i < n; i++) {
        uint32_t h = M_HashVertex(&vs[i]) & (size - 1);
        while (table[h] >= 0 && !M_SameVertex(&vs[table[h]], &vs[i])) {
            h = (h + 1) & (size - 1);
        }
        if (table[h] < 0) {
            table[h] = i;
        } else {
            twins++;
        }
        twin[i] = (uint16_t)table[h];
    }
    Memory_Free(table);
    if (twins == 0) {
        Memory_Free(twin);
        return;
    }
    bind->twin = twin;
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
    bind->flags_and = bind->vertex_count > 0 ? 0xFFFF : 0;
    bind->white = bind->vertex_count > 0;
    for (int32_t i = 0; i < bind->vertex_count; i++) {
        bind->flags_and &= vertices[i].flags;
        bind->flags_or |= vertices[i].flags;
        const RGBA_8888 col = vertices[i].color;
        bind->white &= (col.r & col.g & col.b & col.a) == 255;
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
    M_RefillTextures(bind);
    M_BuildChunks(bind);
    M_SortByPage(
        bind, Vector_GetData(mesh->blend_add_vertex_indices),
        mesh->blend_add_vertex_indices->count);
    M_FindReflections(bind);
    M_ShareVertices(bind);
    M_FindTwins(bind);
    // The transparent faces end to end, their indices final by now.
    bind->transp_count = 0;
    for (int32_t i = 0; i < mesh->transparent_faces->count; i++) {
        const OUTPUT_MESH_FACE *const face = Vector_Get(mesh->transparent_faces, i);
        bind->transp_count += face->vertex_count;
    }
    if (bind->transp_count > 0) {
        bind->transp =
            Memory_Alloc(sizeof(OUTPUT_MESH_INDEX) * bind->transp_count);
        int32_t at = 0;
        for (int32_t i = 0; i < mesh->transparent_faces->count; i++) {
            const OUTPUT_MESH_FACE *const face =
                Vector_Get(mesh->transparent_faces, i);
            memcpy(
                &bind->transp[at], face->vertex_indices,
                sizeof(OUTPUT_MESH_INDEX) * face->vertex_count);
            at += face->vertex_count;
        }
    }
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
    Memory_Free(bind->vref);
    Memory_Free(bind->uvw);
    Memory_Free(bind->twin);
    Memory_Free(bind->env);
    Memory_Free(bind->chunks);
    Memory_Free(bind->transp);
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
    // The vertices are read from the mesh as each frame is drawn; corners
    // alike so far may part from now on.
    M_BINDING *const bind = M_GetBinding(batcher, mesh);
    if (bind != nullptr) {
        Memory_Free(bind->twin);
        bind->twin = nullptr;
        bind->deforms = true;
    }
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
    int16_t caster_index = m_StagingLara ? M_LARA_NOT_CASTER : -1;
    if (m_Casting && pass == SCENE_PASS_OPAQUE
        && m_CasterCount < M_MAX_CASTERS) {
        const M_BINDING *const bind = M_GetBinding(batcher, inst->mesh);
        if (bind != nullptr && bind->vertex_count > 0) {
            M_CASTER *const caster = &m_Casters[m_CasterCount++];
            caster->bind = bind;
            memcpy(caster->bounds, bind->bounds, sizeof(caster->bounds));
            M_WorldMatrix(&inst->wmatrix, caster->world);
            caster->room = inst->room;
            caster->group = m_CasterGroup;
            caster->lara = m_StagingLara;
            caster_index = (int16_t)(m_CasterCount - 1);
        }
    }
    Vector_Add(batcher->staged[pass], inst);
    Vector_Add(batcher->casters[pass], &caster_index);
}

void DC_Shadow_SetCasting(const bool casting)
{
    if (casting && !m_Casting) {
        m_CasterGroup++;
    }
    m_Casting = casting;
}

bool DC_Shadow_TakeObjectCaster(const ITEM *const item)
{
    const ITEM *const lara = Lara_GetItem();
    if (M_ShadowType() != SHADOW_TYPE_VOLUME
        || !g_Config.rendering.dc_object_shadows || lara == nullptr
        || m_ObjectCasters >= M_MAX_OBJECT_CASTERS
        || m_CasterCount > M_MAX_CASTERS - 16) {
        return false;
    }
    const float dx = (float)(item->pos.x - lara->pos.x);
    const float dy = (float)(item->pos.y - lara->pos.y);
    const float dz = (float)(item->pos.z - lara->pos.z);
    if (dx * dx + dy * dy + dz * dz
        > M_OBJECT_CAST_RANGE * M_OBJECT_CAST_RANGE) {
        return false;
    }
    m_ObjectCasters++;
    return true;
}

void DC_Shadow_SetLara(const bool lara)
{
    m_StagingLara = lara;
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
    // How far the light is, and how far it reaches (0 for none).
    float dist;
    float reach;
    // A shadow without a light of its own (the room has none near): as strong
    // as a shadow from above, easing off only a little along its reach.
    bool fixed;
} M_SHADOW_LIGHT;

// The cel shading's shade by the nearest light's weight (M_SHADOW_LIGHT).
static void M_SetCelShade(const float weight)
{
    const float darkest =
        g_TRVersion == 1 ? M_CEL_SHADE_NEAR_TR1 : M_CEL_SHADE_NEAR;
    m_CelShade = M_CEL_SHADE_FAR + (darkest - M_CEL_SHADE_FAR) * weight;
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
    // Full within M_SHADOW_NEAR of the light (or half its reach, for a
    // short one), none at the end of its reach.
    const float near = MIN(M_SHADOW_NEAR, reach * 0.5f);
    list[i] = (M_SHADOW_LIGHT) {
        .dir = { d[0] / dist, d[1] / dist, d[2] / dist },
        .dist_sq = dist_sq,
        .weight = M_Clamp((reach - dist) / (reach - near), 0.0f, 1.0f),
        .dist = dist,
        .reach = reach,
    };
}

// A room with no light near: the direction its sun shines from (TR3), or else
// the place its vertices are brightest in (M_RoomBrightLight, the same the
// relief is lit from), as a point light with no falloff and nothing spread
// about. When a room gives none, the last place found stands: the shadows keep
// falling from where they did.
// The least distance to that place that gives a direction.
#define M_BRIGHT_MIN_DIST 256.0f
// The weight of that light (a light's weight is how near it is), from the
// faintest contrast to the fullest, and of a room's sun.
#define M_BRIGHT_WEIGHT_MAX 0.7f
#define M_SUN_WEIGHT 0.5f

static struct {
    bool found;
    float at[3];
    float strength;
} m_LastBright = {};

// How bright a room vertex is, 0 to 1: TR1 and TR2 keep a shade (0 the
// brightest), TR3 a colour.
static float M_VertexBrightness(const ROOM_VERTEX *const vertex)
{
    if (g_TRVersion >= 3) {
        return (vertex->color.r * 0.30f + vertex->color.g * 0.59f
                + vertex->color.b * 0.11f)
            * (1.0f / 255.0f);
    }
    return M_Clamp(
        (M_SHADE_MAX - vertex->light_base) * (1.0f / M_SHADE_MAX), 0.0f, 1.0f);
}

static void M_VertexWorld(
    const ROOM *const room, const ROOM_VERTEX *const vertex, float out[3])
{
    out[0] = (float)(room->pos.x + vertex->pos.x);
    out[1] = (float)(room->pos.y + vertex->pos.y);
    out[2] = (float)(room->pos.z + vertex->pos.z);
}

// Adds the light a room without any is taken to have, if it gives one.
static int32_t M_GatherRoomLight(
    const ROOM *const room, const float center[3], M_SHADOW_LIGHT *const out)
{
    for (int32_t i = 0; i < room->num_lights; i++) {
        const LIGHT *const light = &room->lights[i];
        if (light->type != LIGHT_TYPE_SUN
            || light->layout != LIGHT_LAYOUT_LEGACY) {
            continue;
        }
        const float d[3] = {
            (float)light->u.legacy.dir.x,
            (float)light->u.legacy.dir.y,
            (float)light->u.legacy.dir.z,
        };
        const float len_sq = M_Dot3(d, d);
        if (len_sq <= 0.0f) {
            continue;
        }
        // The vector points towards the sun; the shadow falls the other way.
        const float inv = -M_InvSqrt(len_sq);
        *out = (M_SHADOW_LIGHT) {
            .dir = { d[0] * inv, d[1] * inv, d[2] * inv },
            .weight = M_SUN_WEIGHT,
        };
        return 1;
    }

    float at[3];
    float power;
    if (M_RoomBrightLight(room, at, &power)) {
        memcpy(m_LastBright.at, at, sizeof(at));
        m_LastBright.strength = M_Clamp(
            (power - M_BUMP_ROOM_MIN_POWER)
                / (M_BUMP_ROOM_MAX_POWER - M_BUMP_ROOM_MIN_POWER),
            0.0f, 1.0f);
        m_LastBright.found = true;
    }
    if (!m_LastBright.found) {
        return 0;
    }
    // From the bright place to the casters.
    float d[3] = {
        center[0] - m_LastBright.at[0], center[1] - m_LastBright.at[1],
        center[2] - m_LastBright.at[2],
    };
    const float dist = sqrtf(M_Dot3(d, d));
    if (dist < M_BRIGHT_MIN_DIST) {
        return 0;
    }
    for (int32_t k = 0; k < 3; k++) {
        d[k] /= dist;
    }
    *out = (M_SHADOW_LIGHT) {
        .dir = { d[0], d[1], d[2] },
        .dist_sq = dist * dist,
        .weight = M_SOFT_NO_LIGHT_WEIGHT,
        .dist = M_Clamp(dist, M_PERSP_MIN_DIST, M_SHADOW_LIGHT_RANGE),
        .reach = M_SHADOW_LIGHT_RANGE,
        .fixed = true,
    };
    return 1;
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
    // Another room's mode is in the block now.
    m_Frame.lights_mode = -1;
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
    if (count == 0 && room != nullptr) {
        count += M_GatherRoomLight(room, center, &out[0]);
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
    const M_CASTER *const caster, const float dir[3], const float origin[3],
    const float length_max, DC_PVR_TRIANGLE *const out)
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
    // The shadow spreads from the light: each corner is pushed along the line
    // from the light through it, so that the volume widens away from it.
    float push[8][3];
    float length = length_max;
    for (int32_t i = 0; i < 8; i++) {
        const float *const p = corners[i];
        if (origin != nullptr) {
            for (int32_t k = 0; k < 3; k++) {
                push[i][k] = p[k] - origin[k];
            }
            const float len_sq = M_Dot3(push[i], push[i]);
            const float inv = len_sq > 1.0f ? M_InvSqrt(len_sq) : 1.0f;
            // The volume widens from the light as (d + length) / d. A cap on
            // the length by each corner's distance cut the volumes of the parts
            // nearest a light (a flare in the hand) short of the floor, and
            // left only the feet's shadow.
            for (int32_t k = 0; k < 3; k++) {
                push[i][k] = len_sq > 1.0f ? push[i][k] * inv : dir[k];
            }
            // The parts above a light in the hand (a flare) would throw their
            // shadow up, and only the legs' would reach the floor: every part
            // is tilted no further off the vertical than the other lights.
            if (push[i][1] < M_SHADOW_MIN_DROP) {
                push[i][1] = M_SHADOW_MIN_DROP;
                const float norm = 1.0f / sqrtf(M_Dot3(push[i], push[i]));
                for (int32_t k = 0; k < 3; k++) {
                    push[i][k] *= norm;
                }
            }
        } else {
            memcpy(push[i], dir, sizeof(push[i]));
        }
        const float z =
            v[0][2] * p[0] + v[1][2] * p[1] + v[2][2] * p[2] + v[3][2];
        const float push_z =
            v[0][2] * push[i][0] + v[1][2] * push[i][1] + v[2][2] * push[i][2];
        const float margin = m_Frame.near_z * 1.5f;
        if (z < margin) {
            return 0;
        }
        if (push_z < 0.0f && z + push_z * length < margin) {
            length = (margin - z) / push_z;
        }
    }
    if (length < 32.0f) {
        return 0;
    }
    for (int32_t i = 0; i < 8; i++) {
        float pushed[3];
        for (int32_t k = 0; k < 3; k++) {
            pushed[k] = corners[i][k] + push[i][k] * length;
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
        // The face is towards the light when it lies on that side of the
        // box's middle: the line from the light to the face points inwards.
        const float *const along = origin == nullptr ? nullptr : origin;
        const float ray[3] = {
            along == nullptr ? dir[0] : face_center[0] - along[0],
            along == nullptr ? dir[1] : face_center[1] - along[1],
            along == nullptr ? dir[2] : face_center[2] - along[2],
        };
        const float n_dot_d = (face_center[0] - center[0]) * ray[0]
            + (face_center[1] - center[1]) * ray[1]
            + (face_center[2] - center[2]) * ray[2];
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
    const bool high = M_ShadowType() == SHADOW_TYPE_SOFT_HIGH;
    // As many lights as the mode allows.
    const int32_t max = MIN(DC_Mode_GetShadowLights(), M_SOFT_MAX_LIGHTS);
    int32_t count = M_GatherShadowLights(m_Casters, center, lights, max);
    if (count == 0) {
        // No light near: a faint shadow straight down.
        lights[0] = (M_SHADOW_LIGHT) {
            .dir = { 0.0f, 1.0f, 0.0f },
            .weight = M_SOFT_NO_LIGHT_WEIGHT,
            .fixed = true,
        };
        count = 1;
    }
    const int32_t last_count = m_Soft.light_count;
    m_Soft.light_count = count;
    M_SetCelShade(lights[0].weight);
    for (int32_t l = 0; l < count; l++) {
        M_SOFT_LIGHT *const out = &m_Soft.lights[l];
        float strength = M_SOFT_STRENGTH_MIN
            + (M_SOFT_STRENGTH_MAX - M_SOFT_STRENGTH_MIN) * lights[l].weight;
        float dist = M_PARALLEL_DIST;
        float fade = M_SOFT_REACH;
        if (high && lights[l].reach > 0.0f) {
            dist = MAX(lights[l].dist, M_PERSP_MIN_DIST);
            fade = M_Clamp(
                lights[l].reach - lights[l].dist, M_PERSP_MIN_FADE,
                M_SOFT_REACH);
        }
        if (lights[l].fixed) {
            // Eased off along the reach, the far end most.
            fade = M_FIXED_FADE;
        }
        float d[3] = { lights[l].dir[0], lights[l].dir[1], lights[l].dir[2] };
        if (l < last_count) {
            // From the last frame's light in this place, part of the way.
            for (int32_t k = 0; k < 3; k++) {
                d[k] = out->dir[k] + (d[k] - out->dir[k]) * M_SOFT_EASE;
            }
            const float len_sq = M_Dot3(d, d);
            if (len_sq > 1.0e-4f) {
                const float inv = M_InvSqrt(len_sq);
                for (int32_t k = 0; k < 3; k++) {
                    d[k] *= inv;
                }
            } else {
                memcpy(d, lights[l].dir, sizeof(d));
            }
            strength = out->strength + (strength - out->strength) * M_SOFT_EASE;
            fade = out->fade + (fade - out->fade) * M_SOFT_EASE;
            // Through its inverse: a parallel light is a million away.
            dist = 1.0f
                / (1.0f / out->dist
                   + (1.0f / dist - 1.0f / out->dist) * M_SOFT_EASE);
        } else {
            // A new one comes in faint.
            strength *= M_SOFT_EASE;
        }
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
        out->strength = strength;
        out->dist = dist;
        out->fade = fade;
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
// Whether a caster's face, laid on its cell (m_SoftXY), faces the way that
// draws it.
M_HOT bool M_SilFront(
    const OUTPUT_MESH_INDEX ia, const OUTPUT_MESH_INDEX ib,
    const OUTPUT_MESH_INDEX ie)
{
    const float *const a = m_SoftXY[ia];
    const float *const b = m_SoftXY[ib];
    const float *const e = m_SoftXY[ie];
    return (b[0] - a[0]) * (e[1] - a[1]) - (e[0] - a[0]) * (b[1] - a[1]) < 0.0f;
}

// A corner of a silhouette in the cell whose left edge is at left.
M_HOT DC_PVR_VERTEX M_SilVertex(
    const float left, const OUTPUT_MESH_INDEX i, const float z)
{
    return (DC_PVR_VERTEX) {
        .x = left + m_SoftXY[i][0],
        .y = m_SoftXY[i][1],
        .z = z,
        .argb = 0xFF000000,
    };
}

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
        // And how far past the casters' middle along the light, for a light
        // whose shadow grows with the distance from it.
        const bool persp = light->dist < M_PARALLEL_DIST;
        float to_depth[4];
        for (int32_t k = 0; k < 3; k++) {
            to_depth[k] = w[0][k] * light->dir[0] + w[1][k] * light->dir[1]
                + w[2][k] * light->dir[2];
        }
        to_depth[3] = (w[0][3] - c[0]) * light->dir[0]
            + (w[1][3] - c[1]) * light->dir[1]
            + (w[2][3] - c[2]) * light->dir[2];
        const OUTPUT_MESH_VERTEX *const vertices =
            Vector_GetData(bind->mesh->vertices);
        const uint16_t *const twin = bind->twin;
        for (int32_t j = 0; j < bind->vertex_count; j++) {
            if (twin != nullptr && twin[j] != j) {
                m_SoftXY[j][0] = m_SoftXY[twin[j]][0];
                m_SoftXY[j][1] = m_SoftXY[twin[j]][1];
                continue;
            }
            const float px = M_Corner(bind, vertices, j)->pos.x;
            const float py = M_Corner(bind, vertices, j)->pos.y;
            const float pz = M_Corner(bind, vertices, j)->pos.z;
            float x = to_cell[0][0] * px + to_cell[0][1] * py
                + to_cell[0][2] * pz + to_cell[0][3];
            float y = to_cell[1][0] * px + to_cell[1][1] * py
                + to_cell[1][2] * pz + to_cell[1][3];
            if (persp) {
                const float z = to_depth[0] * px + to_depth[1] * py
                    + to_depth[2] * pz + to_depth[3];
                const float f = M_PerspScale(light->dist, z);
                x = 0.5f + (x - 0.5f) * f;
                y = 0.5f + (y - 0.5f) * f;
            }
            // Kept inside the cell, so as not to spill into the next.
            m_SoftXY[j][0] = M_Clamp(x, 0.0f, 1.0f) * size;
            m_SoftXY[j][1] = M_Clamp(y, 0.0f, 1.0f) * size;
        }
        const OUTPUT_MESH *const mesh = bind->mesh;
        const OUTPUT_MESH_INDEX *const indices =
            Vector_GetData(mesh->opaque_vertex_indices);
        const int32_t count = mesh->opaque_vertex_indices->count;
        int32_t j = 0;
        while (j + 2 < count) {
            const OUTPUT_MESH_INDEX *const t = &indices[j];
            // A quad's halves (M_PairQuads) go as one strip of four when
            // both are drawn, as in M_TryEmitQuad.
            const bool quad = j + 5 < count && M_IsQuad(t);
            // A closed mesh's faces turned one way cover its whole
            // silhouette, at half the cost of all of them.
            // shortcut: holes show where a mesh is open; draw both ways
            // round for such meshes if it shows.
            const bool front0 = M_SilFront(t[0], t[1], t[2]);
            const bool front1 = quad && M_SilFront(t[3], t[4], t[5]);
            for (int32_t k = 0; k < cells; k++) {
                if (front0 && front1) {
                    const DC_PVR_VERTEX v[4] = {
                        M_SilVertex(lefts[k], t[0], z_near),
                        M_SilVertex(lefts[k], t[1], z_near),
                        M_SilVertex(lefts[k], t[2], z_near),
                        M_SilVertex(lefts[k], t[5], z_near),
                    };
                    DC_PVR_Quad(&v[0], &v[1], &v[2], &v[3]);
                    continue;
                }
                for (int32_t h = 0; h < (quad ? 2 : 1); h++) {
                    if (!(h == 0 ? front0 : front1)) {
                        continue;
                    }
                    const OUTPUT_MESH_INDEX *const f = &t[h * 3];
                    const DC_PVR_VERTEX v[3] = {
                        M_SilVertex(lefts[k], f[0], z_near),
                        M_SilVertex(lefts[k], f[1], z_near),
                        M_SilVertex(lefts[k], f[2], z_near),
                    };
                    DC_PVR_Triangle(&v[0], &v[1], &v[2]);
                }
            }
            j += quad ? 6 : 3;
        }
    }
}

// Turns the relief off when the frame rate falls short, and back on (see
// M_BUMP_MIN_FPS). Once a frame, before it starts.
static void M_UpdateBumpLoad(void)
{
    if (!m_FrameRooms) {
        m_FrameVertices = 0;
        return;
    }
    m_FrameRooms = false;
    const int32_t fps = Output_GetMeasuredFPS();
    const int32_t target = DC_Mode_GetFPS();
    // A relief page being made (as a level starts, or new walls come in)
    // stalls its frame: the next two seconds' rate says nothing of the load.
    static uint32_t made = 0;
    static int32_t quiet = 0;
    static int32_t off_frames = 0;
    if (DC_PVR_GetBumpPagesMade() != made) {
        made = DC_PVR_GetBumpPagesMade();
        quiet = 2 * target;
    }
    if (quiet > 0) {
        quiet--;
    } else if (
        m_BumpOn && fps >= M_BUMP_STALL_FPS && fps < M_BUMP_MIN_FPS) {
        m_BumpOn = false;
        m_BumpOffVertices = m_FrameVertices;
        off_frames = 0;
        LOG_INFO(
            "relief off: %d fps, %u vertices", (int)fps,
            (unsigned)m_BumpOffVertices);
    } else if (
        !m_BumpOn && ++off_frames > 0
        && ((fps >= M_BUMP_MIN_FPS + M_BUMP_SLOW_FPS
             && m_FrameVertices < m_BumpOffVertices * M_BUMP_LIGHTER)
            || (off_frames > M_BUMP_RETRY_SECONDS * target
                && fps >= target - M_BUMP_SLOW_FPS))) {
        // Lighter, or after a while and with the frame rate whole again: a
        // load that took the rate down leaves the same scene to try again.
        m_BumpOn = true;
        LOG_INFO("relief on: %u vertices", (unsigned)m_FrameVertices);
    }
    m_FrameVertices = 0;
}

// The map can be drawn every Nth frame and kept in between (1 draws it every
// frame). Every other frame (15 Hz) saved the silhouettes (~1.1 ms in Venice)
// and a scene of its own on the PowerVR, but the shadow looked worse, so it
// is drawn every frame. What it was drawn for is kept with it.
#define M_SOFT_MAP_EVERY 1
static struct {
    bool valid;
    int32_t light_count;
    M_SOFT_LIGHT lights[M_SOFT_MAX_LIGHTS];
    int32_t self_count;
    int8_t slice[M_MAX_CASTERS];
} m_SoftDrawn = {};

void DC_Shadow_RenderMaps(void)
{
    M_UpdateBumpLoad();
    const int32_t lights = m_Soft.caster_count > 0 ? m_Soft.light_count : 0;
    m_Soft.map_count = 0;
    m_Soft.self = false;
    if (lights == 0) {
        m_Soft.caster_count = 0;
        m_SoftDrawn.valid = false;
        return;
    }
    if (M_SOFT_MAP_EVERY > 1 && m_SoftDrawn.valid
        && g_DC_FrameCount % M_SOFT_MAP_EVERY != 0) {
        // The last map, with the lights and slices it was drawn for.
        m_Soft.light_count = m_SoftDrawn.light_count;
        memcpy(m_Soft.lights, m_SoftDrawn.lights, sizeof(m_Soft.lights));
        memcpy(m_Soft.slice, m_SoftDrawn.slice, sizeof(m_Soft.slice));
        m_Soft.map_count = m_SoftDrawn.light_count;
        m_Soft.self = M_SelfShadowOn();
        m_Soft.self_count = m_SoftDrawn.self_count;
        m_Soft.caster_count = 0;
        return;
    }
    if (!DC_PVR_BeginShadowMapFrame()) {
        m_Soft.caster_count = 0;
        m_SoftDrawn.valid = false;
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
    DC_PROBE_START(probe_sil); // TEMP
    for (int32_t l = 0; l < lights; l++) {
        M_DrawSilhouettes(l);
    }
    DC_PROBE_STOP(DC_PROBE_M_SIL, probe_sil); // TEMP
    DC_PROBE(DC_PROBE_M_END, DC_PVR_EndFrame()); // TEMP
    m_Soft.map_count = lights;
    m_Soft.self = M_SelfShadowOn();
    m_Soft.self_count = m_Soft.caster_count;
    m_Soft.caster_count = 0;
    m_SoftDrawn.valid = true;
    m_SoftDrawn.light_count = lights;
    memcpy(m_SoftDrawn.lights, m_Soft.lights, sizeof(m_SoftDrawn.lights));
    memcpy(m_SoftDrawn.slice, m_Soft.slice, sizeof(m_SoftDrawn.slice));
    m_SoftDrawn.self_count = m_Soft.self_count;
}

// The volumes of one object's casters, from the lights near it. Lara's sets
// how dark they all go (the PowerVR darkens every volume as much) and the cel
// shading. Returns the volumes sent; dir_out takes the nearest light's way.
static int32_t M_DrawGroupVolumes(
    const M_CASTER *const casters, const int32_t caster_count,
    float dir_out[3])
{
    float center[3];
    M_CastersCenter(casters, caster_count, center);
    M_SHADOW_LIGHT lights[M_SOFT_MAX_LIGHTS];
    const int32_t max = MIN(DC_Mode_GetShadowLights(), M_SOFT_MAX_LIGHTS);
    int32_t light_count = M_GatherShadowLights(casters, center, lights, max);
    if (light_count == 0) {
        lights[0] = (M_SHADOW_LIGHT) {
            .dir = { 0.0f, 1.0f, 0.0f },
            .weight = M_SOFT_NO_LIGHT_WEIGHT,
            .fixed = true,
        };
        light_count = 1;
    }
    if (casters[0].lara) {
        M_SetCelShade(lights[0].weight);
        DC_PVR_SetShadowStrength(
            lights[0].fixed ? M_VOLUME_STRENGTH_NO_LIGHT
                            : M_VOLUME_STRENGTH_MIN
                    + (M_VOLUME_STRENGTH_MAX - M_VOLUME_STRENGTH_MIN)
                        * lights[0].weight);
    }
    memcpy(dir_out, lights[0].dir, sizeof(float) * 3);

    // 12 cap triangles and two for each of the box's 12 edges at most: a
    // light inside or beside the box can leave every edge on the outline
    // (24 triangles overflowed the stack there).
    DC_PVR_TRIANGLE tris[36];
    int32_t volumes = 0;
    for (int32_t l = 0; l < light_count; l++) {
        for (int32_t i = 0; i < caster_count; i++) {
            // Where the light is, taken along the (tilted) direction it
            // shines in; a light with none shines in parallel. One up close
            // (a muzzle flash at her hands) is taken a block away, as the
            // soft shadows do: else her head and chest lie above it and
            // cast nothing on the floor.
            const float dist = MAX(lights[l].dist, M_PERSP_MIN_DIST);
            float origin[3];
            for (int32_t k = 0; k < 3; k++) {
                origin[k] = center[k] - lights[l].dir[k] * dist;
            }
            const bool point = lights[l].reach > 0.0f;
            const int32_t count = M_BuildVolume(
                &casters[i], lights[l].dir, point ? origin : nullptr,
                M_SHADOW_LENGTH / MAX(lights[l].dir[1], 0.5f),
                tris);
            DC_PVR_ShadowVolume(tris, count);
            volumes += count > 0;
        }
    }
    return volumes;
}

// The room's own walls and pillars, with object shadows on: the solid
// sectors of Lara's room near the passing light nearest her (a flare, a muzzle
// flash) cast volumes from it, as her meshes do. A light that stays put has its
// shadows in the room's own light already. Each solid sector is a column a
// block wide from the room's ceiling to its floor; the steps of the floor and
// ceiling cast nothing.
#define M_ROOM_CAST_RANGE (3.0f * WALL_L)
#define M_ROOM_CAST_MAX 32
// Drawn a little inside its faces, so that the lit side does not shade itself.
#define M_ROOM_CAST_INSET 32.0f

static int32_t M_DrawRoomVolumes(
    const M_CASTER *const lara, const int32_t lara_count)
{
    const ROOM *const room = lara[0].room;
    if (room == nullptr || room->sectors == nullptr) {
        return 0;
    }
    float center[3];
    M_CastersCenter(lara, lara_count, center);
    Output_Uniforms_UploadRoomLights(Output_GetUniforms(), room);
    const DC_UNIFORM_LIGHTS *const lights =
        DC_GL_GetUniformBlock(DC_GL_BINDING_LIGHTS, nullptr);
    float at[3];
    float reach = 0.0f;
    float best = FLT_MAX;
    for (int32_t i = 0; lights != nullptr && i < lights->num_lights; i++) {
        const DC_UNIFORM_LIGHT *const light = &lights->lights[i];
        if (light->kind == 0.0f) {
            continue; // the room's own
        }
        const float r = light->falloff
            / (float)(1 << (OUTPUT_DYNAMIC_FALLOFF_SHIFT
                            - OUTPUT_DYNAMIC_RADIUS_SHIFT));
        const float d[3] = { light->pos[0] - center[0],
                             light->pos[1] - center[1],
                             light->pos[2] - center[2] };
        const float dist_sq = M_Dot3(d, d);
        if (dist_sq < best && dist_sq < r * r) {
            best = dist_sq;
            memcpy(at, light->pos, sizeof(at));
            reach = MIN(r, M_ROOM_CAST_RANGE);
        }
    }
    if (reach <= 0.0f) {
        return 0;
    }

    const int32_t x0 = MAX(0, (int32_t)((at[0] - reach - room->pos.x) / WALL_L));
    const int32_t x1 =
        MIN(room->size.x - 1, (int32_t)((at[0] + reach - room->pos.x) / WALL_L));
    const int32_t z0 = MAX(0, (int32_t)((at[2] - reach - room->pos.z) / WALL_L));
    const int32_t z1 =
        MIN(room->size.z - 1, (int32_t)((at[2] + reach - room->pos.z) / WALL_L));
    DC_PVR_TRIANGLE tris[36];
    int32_t volumes = 0;
    for (int32_t x = x0; x <= x1 && volumes < M_ROOM_CAST_MAX; x++) {
        for (int32_t z = z0; z <= z1 && volumes < M_ROOM_CAST_MAX; z++) {
            const SECTOR *const sector = &room->sectors[z + x * room->size.z];
            if (sector->floor.height != NO_HEIGHT
                || sector->portal_room.wall != NO_ROOM) {
                continue;
            }
            const M_CASTER column = {
                .bounds = {
                    { room->pos.x + x * WALL_L + M_ROOM_CAST_INSET,
                      (float)room->max_ceiling,
                      room->pos.z + z * WALL_L + M_ROOM_CAST_INSET },
                    { room->pos.x + (x + 1) * WALL_L - M_ROOM_CAST_INSET,
                      (float)room->min_floor,
                      room->pos.z + (z + 1) * WALL_L - M_ROOM_CAST_INSET },
                },
                .world = { { 1.0f, 0.0f, 0.0f, 0.0f },
                           { 0.0f, 1.0f, 0.0f, 0.0f },
                           { 0.0f, 0.0f, 1.0f, 0.0f } },
                .room = room,
            };
            float d[3] = {
                (column.bounds[0][0] + column.bounds[1][0]) * 0.5f - at[0],
                (column.bounds[0][1] + column.bounds[1][1]) * 0.5f - at[1],
                (column.bounds[0][2] + column.bounds[1][2]) * 0.5f - at[2],
            };
            const float len_sq = M_Dot3(d, d);
            if (len_sq > (reach + WALL_L) * (reach + WALL_L)) {
                continue;
            }
            const float inv = M_InvSqrt(len_sq);
            for (int32_t k = 0; k < 3; k++) {
                d[k] *= inv;
            }
            const int32_t count = M_BuildVolume(&column, d, at, reach, tris);
            DC_PVR_ShadowVolume(tris, count);
            volumes += count > 0;
        }
    }
    return volumes;
}

void DC_Shadow_DrawVolumes(void)
{
    // What the objects took this frame is spent.
    m_ObjectCasters = 0;
    if (m_CasterCount == 0) {
        return;
    }
    if (M_IS_SOFT(M_ShadowType())) {
        float center[3];
        M_CastersCenter(m_Casters, m_CasterCount, center);
        M_KeepSoftCasters(center);
        m_CasterCount = 0;
        return;
    }
    // Object by object: each one's casters lie together.
    int32_t volumes = 0;
    float dir[3] = { 0.0f, 1.0f, 0.0f };
    for (int32_t first = 0; first < m_CasterCount;) {
        int32_t end = first + 1;
        while (end < m_CasterCount
               && m_Casters[end].group == m_Casters[first].group) {
            end++;
        }
        float group_dir[3];
        volumes +=
            M_DrawGroupVolumes(&m_Casters[first], end - first, group_dir);
        if (m_Casters[first].lara) {
            memcpy(dir, group_dir, sizeof(dir));
            if (g_Config.rendering.dc_object_shadows) {
                volumes += M_DrawRoomVolumes(&m_Casters[first], end - first);
            }
        }
        first = end;
    }
#if defined(TRX_DC_WATCHDOG)
    static int32_t frame = 0;
    static int32_t lit_frames = 0;
    lit_frames += dir[1] < 0.999f;
    const DC_UNIFORM_LIGHTS *const passing = m_Frame.lights;
    if (++frame % 120 == 0) {
        LOG_DEBUG(
            "shadow: %d casters, %d volumes, receivers %d/%d, lit %d/120, "
            "room lights %d, passing %d, dir %.2f %.2f %.2f",
            m_CasterCount, volumes, m_ShadowReceivers, m_ShadowRooms,
            lit_frames,
            m_Casters[0].room != nullptr ? m_Casters[0].room->num_lights : -1,
            passing != nullptr ? (int)passing->num_lights : -1, dir[0], dir[1],
            dir[2]);
        lit_frames = 0;
        m_ShadowReceivers = 0;
        m_ShadowRooms = 0;
    }
#endif
    m_CasterCount = 0;
}

const SCENE_SOURCE *MeshBatcher_AsSource(const MESH_BATCHER *const batcher)
{
    return &batcher->source;
}
