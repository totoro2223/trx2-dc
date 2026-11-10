#include <trx/config.h>
#include <trx/core/log.h>
#include <trx/core/math/geom.h>
#include <trx/core/subsystem.h>
#include <trx/core/utils.h>
#include <trx/core/vector.h>
#include <trx/game/camera.h>
#include <trx/game/cutseq.h>
#include <trx/game/effects.h>
#include <trx/game/fx.h>
#include <trx/game/lara.h>
#include <trx/game/lara/common.h>
#include <trx/game/lara/mesh.h>
#include <trx/game/matrix.h>
#include <trx/game/output.h>
#include <trx/core/memory.h>
#include <trx/game/output/bind.h>
#include <trx/game/output/sources/rooms.h>
#include <trx/game/output/water.h>
#include <trx/game/pathing/lot.h>
#include <trx/game/rooms.h>
#include <trx/game/rope.h>
#include <trx/game/sparks.h>
#include <trx/game/viewport.h>

#include <math.h>
#include <string.h>

// TEMP: see platform/dreamcast/dc_probe.h
#if defined(__DREAMCAST__)
    #include <platform/dreamcast/dc_mode.h>
    #include <platform/dreamcast/dc_probe.h>
    #include <platform/dreamcast/render/shadow.h>
#else
    #define DC_PROBE(i, stmt) stmt
    #define DC_PROBE_START(t)
    #define DC_PROBE_STOP(i, t)
#endif

#define M_MAX_BOUND_ROOMS 128

typedef struct {
    int32_t xv;
    int32_t yv;
    int32_t zv;
} M_PORTAL_VBUF;

static VECTOR *m_RoomsToDraw = nullptr;
static ROOM_DRAWSET m_DrawnStatics = {};

static int32_t m_Outside;
static int32_t m_OutsideRight;
static int32_t m_OutsideLeft;
static int32_t m_OutsideTop;
static int32_t m_OutsideBottom;

static int32_t m_BoundStart;
static int32_t m_BoundEnd;
static int32_t m_BoundRooms[M_MAX_BOUND_ROOMS] = {};

// Reports whether the draw leaves out Lara, the creatures and the effects,
// which is how the level view a loading screen shows draws.
static bool M_IsSceneryOnly(void)
{
    return g_Camera.type == CAM_LOADING_SCREEN;
}

static inline void M_DrawSet_Init(ROOM_DRAWSET *const s)
{
    s->count = 0;
    memset(s->bits, 0, sizeof(s->bits));
}

static inline bool M_DrawSet_Has(
    const ROOM_DRAWSET *const s, const int16_t item_num)
{
    const uint32_t w = item_num >> 6;
    const uint32_t b = item_num & 63;
    return (s->bits[w] >> b) & 1ULL;
}

static inline bool M_DrawSet_Add(ROOM_DRAWSET *const s, const int16_t item_num)
{
    const uint32_t w = item_num >> 6;
    const uint32_t b = item_num & 63;
    const uint64_t mask = 1ULL << b;
    if (s->bits[w] & mask) {
        return false;
    }
    s->bits[w] |= mask;
    s->count++;
    return true;
}

static inline bool M_DrawSet_Remove(
    ROOM_DRAWSET *const s, const int16_t item_num)
{
    const uint32_t w = item_num >> 6;
    const uint32_t b = item_num & 63;
    const uint64_t mask = 1ULL << b;
    if (!(s->bits[w] & mask)) {
        return false;
    }
    s->bits[w] &= ~mask;
    s->count--;
    return true;
}

static inline void M_DrawSet_ForEach(
    const ROOM_DRAWSET *const s, void (*const fn)(int16_t item, void *ud),
    void *ud)
{
    for (uint32_t w = 0; w < ROOM_DRAWSET_WORDS; w++) {
        uint64_t x = s->bits[w];
        while (x != 0ULL) {
            const uint32_t b = __builtin_ctzll(x);
            fn((int16_t)((w << 6) + b), ud);
            x &= x - 1; // clear lowest set bit
        }
    }
}

static void M_EnsureRoomsToDraw(void)
{
    if (m_RoomsToDraw != nullptr) {
        return;
    }
    m_RoomsToDraw = Vector_CreateAtCapacity(sizeof(int16_t), 100);
}

static void M_SetBounds(
    const PORTAL *const portal, int32_t room_num, const ROOM *parent);

static inline bool M_PortalFacesCameraC(
    const ROOM *const room, const PORTAL *const portal)
{
    // clang-format off
    const XYZ_32 offset = {
        .x = portal->normal.x * (room->pos.x + portal->vertex[0].x - g_ViewPos.x),
        .y = portal->normal.y * (room->pos.y + portal->vertex[0].y - g_ViewPos.y),
        .z = portal->normal.z * (room->pos.z + portal->vertex[0].z - g_ViewPos.z),
    };
    // clang-format on
    return offset.x + offset.y + offset.z < 0;
}

#if defined(__DREAMCAST__)
// In assembly (visibility_sh4.S); a build with DC_EMIT_VERIFY runs the C too
// and compares. TRX_DC_ASMOFF bit 262144 turns it off.
extern int dc_portal_faces(
    const int32_t *room_pos, const PORTAL *portal, const int32_t *view_pos);
extern uint32_t g_DC_AsmOff;

static inline bool M_PortalFacesCamera(
    const ROOM *const room, const PORTAL *const portal)
{
    if ((g_DC_AsmOff & 262144) != 0) {
        return M_PortalFacesCameraC(room, portal);
    }
    const bool faces = dc_portal_faces(
                           (const int32_t *)&room->pos, portal,
                           (const int32_t *)&g_ViewPos)
        != 0;
#if defined(DC_EMIT_VERIFY)
    static uint32_t m_Runs;
    static uint32_t m_Bad;
    m_Runs++;
    if (faces != M_PortalFacesCameraC(room, portal)) {
        m_Bad++;
        LOG_WARNING("[portal-verify] MISMATCH %d", (int)faces);
    }
    if (m_Runs % 20000 == 0) {
        LOG_WARNING(
            "[portal-verify] %u runs, %u bad", (unsigned)m_Runs,
            (unsigned)m_Bad);
    }
#endif
    return faces;
}
#else
static inline bool M_PortalFacesCamera(
    const ROOM *const room, const PORTAL *const portal)
{
    return M_PortalFacesCameraC(room, portal);
}
#endif

#if defined(__DREAMCAST__)
static int16_t m_OcclusionQueue[MAX_ROOMS];

// What the walls leave of a portal, seen from the camera: lines of sight to a
// grid of points across it (M_SIGHT_GRID steps a side).
#define M_SIGHT_GRID 4

// The eyes the walls are asked about: where the camera is, the middle of its
// sector of the camera room's grid, and four points about the middle. What
// the walls hide of a portal or of a static is hidden from every one of them,
// and kept for the sector (and a step of the eye's height) in m_SeeCache, so
// that it neither flickers as the camera moves in the sector nor costs rays
// every frame.
#define M_EYES 6
#define M_EYE_SPREAD 384
#define M_SEE_SLOTS 1024
#define M_SEE_Y_SHIFT 9

typedef struct {
    const void *id; // the portal, or the static mesh
    const ROOM *camera;
    int16_t sector;
    int16_t bucket;
    bool used;
    bool hidden;
} M_SEE_ENTRY;

static M_SEE_ENTRY m_SeeCache[M_SEE_SLOTS];
static const ROOM *m_SeeLevel = nullptr;
static int32_t m_SeeRooms = -1;
static XYZ_32 m_Eyes[M_EYES];
static int32_t m_EyeCount = 1;
static int32_t m_EyeSector = -1;
static int16_t m_EyeBucket = 0;
static const ROOM *m_SightCamera = nullptr;

// Once a frame: the eyes of the camera's sector, and the cache emptied if the
// level has changed.
static void M_PrepareEyes(const ROOM *const camera)
{
    if (m_SeeLevel != Room_Get(0) || m_SeeRooms != Room_GetCount()) {
        memset(m_SeeCache, 0, sizeof(m_SeeCache));
        m_SeeLevel = Room_Get(0);
        m_SeeRooms = Room_GetCount();
    }
    m_Eyes[0] = g_ViewPos;
    m_EyeCount = 1;
    m_EyeSector = -1;
    m_EyeBucket = (int16_t)(g_ViewPos.y >> M_SEE_Y_SHIFT);
    if (camera->size.x <= 0 || camera->size.z <= 0) {
        return;
    }
    const int32_t sx = (g_ViewPos.x - camera->pos.x) >> 10;
    const int32_t sz = (g_ViewPos.z - camera->pos.z) >> 10;
    if (g_ViewPos.x < camera->pos.x || g_ViewPos.z < camera->pos.z
        || sx >= camera->size.x || sz >= camera->size.z) {
        return;
    }
    m_EyeSector = (int16_t)(sx * camera->size.z + sz);
    const int32_t mx = camera->pos.x + sx * WALL_L + WALL_L / 2;
    const int32_t mz = camera->pos.z + sz * WALL_L + WALL_L / 2;
    static const int8_t spread[5][2] = {
        { 0, 0 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 },
    };
    for (int32_t i = 0; i < 5; i++) {
        m_Eyes[m_EyeCount++] = (XYZ_32) {
            .x = mx + spread[i][0] * M_EYE_SPREAD,
            .y = g_ViewPos.y,
            .z = mz + spread[i][1] * M_EYE_SPREAD,
        };
    }
}

// The slot of an id for the camera's sector: the entry that has it, or the one
// to make. Null when the camera is not in a sector of its room.
static M_SEE_ENTRY *M_SeeSlot(const void *const id, bool *const found)
{
    *found = false;
    if (m_EyeSector < 0) {
        return nullptr;
    }
    const uint32_t hash = ((uintptr_t)id >> 3) * 2654435761u
        + (uint32_t)m_EyeSector * 40503u + (uint32_t)(m_EyeBucket & 0xFFFF);
    M_SEE_ENTRY *empty = nullptr;
    for (int32_t n = 0; n < 8; n++) {
        M_SEE_ENTRY *const e = &m_SeeCache[(hash + n) % M_SEE_SLOTS];
        if (e->used && e->id == id && e->camera == m_SightCamera
            && e->sector == m_EyeSector && e->bucket == m_EyeBucket) {
            *found = true;
            return e;
        }
        if (!e->used && empty == nullptr) {
            empty = e;
        }
    }
    M_SEE_ENTRY *const e = empty != nullptr ? empty : &m_SeeCache[hash % M_SEE_SLOTS];
    e->id = id;
    e->camera = m_SightCamera;
    e->sector = (int16_t)m_EyeSector;
    e->bucket = m_EyeBucket;
    e->used = true;
    return e;
}

// Whether a wall of the room the point is in or of the camera's lies between
// an eye and the point.
static bool M_PointHiddenFrom(
    const ROOM *const room, const XYZ_32 *const eye, const XYZ_32 *const point)
{
    return OutputSource_Rooms_RayBlocked(room, eye, point)
        || (room != m_SightCamera
            && OutputSource_Rooms_RayBlocked(m_SightCamera, eye, point));
}

// From the camera alone (what moves: the items, the effects).
static bool M_PointHidden(const ROOM *const room, const XYZ_32 *const point)
{
    return M_PointHiddenFrom(room, &g_ViewPos, point);
}

// From every eye: one that sees the point shows it.
static bool M_PointHiddenAll(const ROOM *const room, const XYZ_32 *const point)
{
    for (int32_t i = 0; i < m_EyeCount; i++) {
        if (!M_PointHiddenFrom(room, &m_Eyes[i], point)) {
            return false;
        }
    }
    return true;
}

// The point of the portal at (u, v) steps of the grid, a corner of it for
// 0 or M_SIGHT_GRID, in the portal's own space.
static void M_PortalPoint(
    const PORTAL *const portal, const float u, const float v, float out[3])
{
    const float fu = u / M_SIGHT_GRID;
    const float fv = v / M_SIGHT_GRID;
    const XYZ_16 *const p = portal->vertex;
    const float x[4] = { p[0].x, p[1].x, p[2].x, p[3].x };
    const float y[4] = { p[0].y, p[1].y, p[2].y, p[3].y };
    const float z[4] = { p[0].z, p[1].z, p[2].z, p[3].z };
    const float k[4] = { (1.0f - fu) * (1.0f - fv), fu * (1.0f - fv), fu * fv,
                         (1.0f - fu) * fv };
    out[0] = k[0] * x[0] + k[1] * x[1] + k[2] * x[2] + k[3] * x[3];
    out[1] = k[0] * y[0] + k[1] * y[1] + k[2] * y[2] + k[3] * y[3];
    out[2] = k[0] * z[0] + k[1] * z[1] + k[2] * z[2] + k[3] * z[3];
}

static bool M_GridPointHidden(
    const ROOM *const room, const PORTAL *const portal, const int32_t iu,
    const int32_t iv)
{
    float local[3];
    M_PortalPoint(portal, (float)iu, (float)iv, local);
    const XYZ_32 world = {
        .x = room->pos.x + (int32_t)local[0],
        .y = room->pos.y + (int32_t)local[1],
        .z = room->pos.z + (int32_t)local[2],
    };
    return M_PointHiddenAll(room, &world);
}

// Whether the portal is hidden: its grid of points, each behind a wall of the
// room the portal is in or of the camera's, from every eye. Only all of them
// hidden hides it. Worked out once for each sector the camera is in.
static bool M_PortalHidden(const ROOM *const room, const PORTAL *const portal)
{
    bool found;
    M_SEE_ENTRY *const entry = M_SeeSlot(portal, &found);
    if (found) {
        return entry->hidden;
    }
    g_DC_ProbeUs[DC_PROBE_C_PTEST]++; // TEMP

    // Every point of the grid, the first one seen showing it: a wide portal
    // (the hills of an open room) can lie open between a few that are hidden.
    bool all_hidden = true;
    for (int32_t iu = 0; iu <= M_SIGHT_GRID && all_hidden; iu++) {
        for (int32_t iv = 0; iv <= M_SIGHT_GRID && all_hidden; iv++) {
            all_hidden = M_GridPointHidden(room, portal, iu, iv);
        }
    }
    if (all_hidden) {
        g_DC_ProbeUs[DC_PROBE_C_PHIDE]++; // TEMP
    }
    if (entry != nullptr) {
        entry->hidden = all_hidden;
    }
    return all_hidden;
}

// Marks the rooms of the list that no portal leads to unless a wall hides it
// from the camera (each portal facing the camera, from a room that shows):
// the list stays as it is, for what else reads it, but their geometry is not
// drawn.
static void M_FindOccluded(const int16_t camera_room)
{
    const int32_t count = Room_DrawGetCount();
    for (int32_t i = 0; i < count; i++) {
        Output_Bind_GetRoom(Room_Get(Room_DrawGetRoom(i)))->occluded = true;
    }
    Output_Bind_GetRoom(Room_Get(camera_room))->occluded = false;
    int32_t head = 0;
    int32_t tail = 0;
    m_OcclusionQueue[tail++] = camera_room;
    while (head < tail) {
        const int16_t room_num = m_OcclusionQueue[head++];
        const ROOM *const room = Room_Get(room_num);
        if (room->portals == nullptr) {
            continue;
        }
        for (int32_t i = 0; i < room->portals->count; i++) {
            const PORTAL *const portal = &room->portals->portal[i];
            if (!M_PortalFacesCamera(room, portal)) {
                continue;
            }
            OUTPUT_ROOM_BIND *const to =
                Output_Bind_GetRoom(Room_Get(portal->room_num));
            if (!to->occluded || !to->drawn || tail >= MAX_ROOMS
                || M_PortalHidden(room, portal)) {
                continue;
            }
            to->occluded = false;
            m_OcclusionQueue[tail++] = portal->room_num;
        }
    }
    for (int32_t i = 0; i < count; i++) {
        if (Output_Bind_GetRoom(Room_Get(Room_DrawGetRoom(i)))->occluded) {
            g_DC_ProbeUs[DC_PROBE_C_ROOMS]++; // TEMP
        }
    }
}

// Whether the walls hide an object whose box is a column of the given radius
// about (x, z) from y_top to y_bottom (y grows down): five rays from the eye,
// to the middle, the top, the bottom and the two sides across the view; only
// all five stopped hides it. Not what lies within two blocks of the eye.
static bool M_ObjectHidden(
    const ROOM *const room, const XYZ_32 *const at, const int32_t radius,
    const int32_t y_top, const int32_t y_bottom, const void *const still)
{
    const float dx = (float)(at->x - g_ViewPos.x);
    const float dz = (float)(at->z - g_ViewPos.z);
    const float dy = (float)((y_top + y_bottom) / 2 - g_ViewPos.y);
    const float len_xz = sqrtf(dx * dx + dz * dz);
    if (len_xz < 2.0f * WALL_L || dx * dx + dy * dy + dz * dz < 4.0f * WALL_L * WALL_L) {
        return false;
    }
    DC_PROBE_START(probe);
    g_DC_ProbeUs[DC_PROBE_O_TESTED]++; // TEMP
    const int32_t mid = (y_top + y_bottom) / 2;
    bool hidden = true;
    if (still != nullptr) {
        // A static, which stays: seen from every eye of the camera's sector,
        // from all round it, and kept for the sector.
        bool found;
        M_SEE_ENTRY *const entry = M_SeeSlot(still, &found);
        if (found) {
            hidden = entry->hidden;
        } else {
            const XYZ_32 points[7] = {
                { at->x, mid, at->z },
                { at->x, y_top, at->z },
                { at->x, y_bottom, at->z },
                { at->x + radius, mid, at->z },
                { at->x - radius, mid, at->z },
                { at->x, mid, at->z + radius },
                { at->x, mid, at->z - radius },
            };
            for (int32_t i = 0; i < 7 && hidden; i++) {
                hidden = M_PointHiddenAll(room, &points[i]);
            }
            if (entry != nullptr) {
                entry->hidden = hidden;
            }
        }
    } else {
        const int32_t sx = (int32_t)(-dz / len_xz * radius);
        const int32_t sz = (int32_t)(dx / len_xz * radius);
        const XYZ_32 points[5] = {
            { at->x, mid, at->z },
            { at->x, y_top, at->z },
            { at->x, y_bottom, at->z },
            { at->x + sx, mid, at->z + sz },
            { at->x - sx, mid, at->z - sz },
        };
        for (int32_t i = 0; i < 5 && hidden; i++) {
            hidden = M_PointHidden(room, &points[i]);
        }
    }
    if (hidden) {
        g_DC_ProbeUs[DC_PROBE_O_HIDDEN]++; // TEMP
    }
    DC_PROBE_STOP(DC_PROBE_O_US, probe);
    return hidden;
}
#endif

[[maybe_unused]] static void M_GetBoundsC(void)
{
    while (m_BoundStart != m_BoundEnd) {
        const int16_t room_num = m_BoundRooms[m_BoundStart % M_MAX_BOUND_ROOMS];
        m_BoundStart++;
        const ROOM *const room = Room_Get(room_num);
        OUTPUT_ROOM_BIND *const bind = Output_Bind_GetRoom(room);
        bind->active = false;

        CLAMPG(bind->bound_left, bind->test_left);
        CLAMPG(bind->bound_top, bind->test_top);
        CLAMPL(bind->bound_right, bind->test_right);
        CLAMPL(bind->bound_bottom, bind->test_bottom);

        if (!bind->drawn) {
            Room_MarkToBeDrawn(room_num);
            bind->drawn = true;
            if (room->flags.outside) {
                m_Outside = 1;
            }
        }

        if (!room->flags.inside || room->flags.outside) {
            CLAMPG(m_OutsideLeft, bind->bound_left);
            CLAMPG(m_OutsideTop, bind->bound_top);
            CLAMPL(m_OutsideRight, bind->bound_right);
            CLAMPL(m_OutsideBottom, bind->bound_bottom);
        }

        if (room->portals == nullptr) {
            continue;
        }

        Matrix_Push();
        Matrix_TranslateAbs32(room->pos);
        for (int32_t i = 0; i < room->portals->count; i++) {
            PORTAL *const portal = &room->portals->portal[i];
            if (M_PortalFacesCamera(room, portal)) {
                M_SetBounds(portal, portal->room_num, room);
            }
        }
        Matrix_Pop();
    }
}

static void M_SetBounds(
    const PORTAL *const portal, const int32_t room_num,
    const ROOM *const parent)
{
    const ROOM *const room = Room_Get(room_num);
    const OUTPUT_ROOM_BIND *const parent_bind = Output_Bind_GetRoom(parent);
    OUTPUT_ROOM_BIND *const bind = Output_Bind_GetRoom(room);

    if (bind->bound_left <= parent_bind->test_left
        && bind->bound_top <= parent_bind->test_top
        && bind->bound_right >= parent_bind->test_right
        && bind->bound_bottom >= parent_bind->test_bottom) {
        return;
    }

    const MATRIX *const m = g_MatrixPtr;
    int32_t left = parent_bind->test_right;
    int32_t right = parent_bind->test_left;
    int32_t bottom = parent_bind->test_top;
    int32_t top = parent_bind->test_bottom;

    M_PORTAL_VBUF portal_vbuf[4];
    int32_t too_near = 0;
#if defined(__DREAMCAST__)
    // Corners past the end of the fog, where it is wholly opaque.
    const int32_t far_z = Output_GetFarZ();
    int32_t too_far = 0;
#endif

    for (int32_t i = 0; i < 4; i++) {
        M_PORTAL_VBUF *const dvbuf = &portal_vbuf[i];
        const XYZ_16 *const dvtx = &portal->vertex[i];
        const int32_t xv =
            dvtx->x * m->_00 + dvtx->y * m->_01 + dvtx->z * m->_02 + m->_03;
        const int32_t yv =
            dvtx->x * m->_10 + dvtx->y * m->_11 + dvtx->z * m->_12 + m->_13;
        const int32_t zv =
            dvtx->x * m->_20 + dvtx->y * m->_21 + dvtx->z * m->_22 + m->_23;
        dvbuf->xv = xv;
        dvbuf->yv = yv;
        dvbuf->zv = zv;
#if defined(__DREAMCAST__)
        too_far += zv >= far_z;
#endif

        if (zv <= 0) {
            too_near++;
            continue;
        }

        int32_t xs;
        int32_t ys;
        const int32_t zp = zv / g_PhdPersp;
        if (zp != 0) {
            xs = Viewport_GetCenterX(VIEWPORT_GAME) + xv / zp;
            ys = Viewport_GetCenterY(VIEWPORT_GAME) + yv / zp;
        } else {
            xs = xv < 0 ? g_PhdLeft : g_PhdRight;
            ys = yv < 0 ? g_PhdTop : g_PhdBottom;
        }

        if (xs - 1 < left) {
            left = xs - 1;
        }
        if (xs + 1 > right) {
            right = xs + 1;
        }
        if (ys - 1 < top) {
            top = ys - 1;
        }
        if (ys + 1 > bottom) {
            bottom = ys + 1;
        }
    }

    if (too_near == 4) {
        return;
    }
#if defined(__DREAMCAST__)
    // Everything seen through a portal lies beyond it: a room behind one that
    // the fog hides is not visited, nor are its items posed (fog end in
    // blocks: visuals.fog_end, which the graphics mode may lower).
    if (too_far == 4) {
        return;
    }
#endif

    if (too_near > 0) {
        const M_PORTAL_VBUF *dest = &portal_vbuf[0];
        const M_PORTAL_VBUF *last = &portal_vbuf[3];

        for (int32_t i = 0; i < 4; i++, last = dest, dest++) {
            if ((dest->zv <= 0) == (last->zv <= 0)) {
                continue;
            }

            if (dest->xv < 0 && last->xv < 0) {
                left = Viewport_GetMinX(VIEWPORT_GAME);
            } else if (dest->xv > 0 && last->xv > 0) {
                right = Viewport_GetMaxX(VIEWPORT_GAME);
            } else {
                left = Viewport_GetMinX(VIEWPORT_GAME);
                right = Viewport_GetMaxX(VIEWPORT_GAME);
            }

            if (dest->yv < 0 && last->yv < 0) {
                top = Viewport_GetMinY(VIEWPORT_GAME);
            } else if (dest->yv > 0 && last->yv > 0) {
                bottom = Viewport_GetMaxY(VIEWPORT_GAME);
            } else {
                top = Viewport_GetMinY(VIEWPORT_GAME);
                bottom = Viewport_GetMaxY(VIEWPORT_GAME);
            }
        }
    }

    if (left < parent_bind->test_left) {
        left = parent_bind->test_left;
    }
    if (right > parent_bind->test_right) {
        right = parent_bind->test_right;
    }
    if (top < parent_bind->test_top) {
        top = parent_bind->test_top;
    }
    if (bottom > parent_bind->test_bottom) {
        bottom = parent_bind->test_bottom;
    }

    if (left >= right || top >= bottom) {
        return;
    }

    if (bind->active) {
        CLAMPG(bind->test_left, left);
        CLAMPG(bind->test_top, top);
        CLAMPL(bind->test_right, right);
        CLAMPL(bind->test_bottom, bottom);
    } else {
        m_BoundRooms[m_BoundEnd % M_MAX_BOUND_ROOMS] = room_num;
        m_BoundEnd++;
        bind->active = true;
        bind->test_left = left;
        bind->test_top = top;
        bind->test_right = right;
        bind->test_bottom = bottom;
    }
}

#if defined(__DREAMCAST__)
// The walk over the queue of rooms is in assembly (room_sh4.S); the C above is
// its reference (DC_EMIT_VERIFY runs both and compares) and the other
// platforms' code.
typedef struct {
    ROOM *rooms;
    int32_t room_size;
    OUTPUT_ROOM_BIND *binds;
    int32_t bind_size;
    int32_t *queue;
    int32_t start;
    int32_t end;
    int32_t outside;
    int32_t out_left;
    int32_t out_top;
    int32_t out_right;
    int32_t out_bottom;
    const MATRIX *matrix;
    const MATRIX *view;
    const XYZ_32 *eye;
    int32_t far_z;
    int32_t persp;
    int32_t center_x;
    int32_t center_y;
    int32_t phd_left;
    int32_t phd_right;
    int32_t phd_top;
    int32_t phd_bottom;
    int32_t min_x;
    int32_t max_x;
    int32_t min_y;
    int32_t max_y;
    void (*mark)(int32_t);
    int32_t off_pos;
    int32_t off_portals;
    int32_t off_outside;
    int32_t off_inside;
} M_BOUNDRUN;
_Static_assert(offsetof(M_BOUNDRUN, mark) == 108, "room_sh4.S layout");
_Static_assert(offsetof(M_BOUNDRUN, off_inside) == 124, "room_sh4.S layout");
_Static_assert(sizeof(PORTAL) == 56, "room_sh4.S: sizeof(PORTAL)");
_Static_assert(offsetof(PORTAL, normal) == 2, "room_sh4.S layout");
_Static_assert(offsetof(PORTAL, vertex) == 8, "room_sh4.S layout");
_Static_assert(offsetof(PORTALS, portal) == 2, "room_sh4.S layout");
_Static_assert(sizeof(OUTPUT_ROOM_BIND) == 20, "room_sh4.S layout");
_Static_assert(offsetof(OUTPUT_ROOM_BIND, bound_left) == 4, "room_sh4.S layout");
_Static_assert(offsetof(OUTPUT_ROOM_BIND, test_left) == 12, "room_sh4.S layout");
_Static_assert(offsetof(OUTPUT_ROOM_BIND, test_bottom) == 18, "room_sh4.S layout");
_Static_assert(offsetof(MATRIX, _20) == 64, "room_sh4.S layout");

extern void dc_get_bounds(M_BOUNDRUN *run);

static void M_MarkV(const int32_t room_num)
{
    Room_MarkToBeDrawn((int16_t)room_num);
}

    #if defined(DC_EMIT_VERIFY)
static int32_t m_MarkLog[MAX_ROOMS];
static int32_t m_MarkCount;

static void M_MarkLog(const int32_t room_num)
{
    if (m_MarkCount < MAX_ROOMS) {
        m_MarkLog[m_MarkCount++] = room_num;
    }
}
    #endif

static void M_GetBounds(void)
{
    if (g_MatrixWorldOnly || Room_GetCount() <= 0
        || (g_DC_AsmOff & 1) != 0) {
        M_GetBoundsC();
        return;
    }
    ROOM *const first = Room_Get(0);
    M_BOUNDRUN run = {
        .rooms = first,
        .room_size = sizeof(ROOM),
        .binds = Output_Bind_GetRoom(first),
        .bind_size = sizeof(OUTPUT_ROOM_BIND),
        .queue = m_BoundRooms,
        .start = m_BoundStart,
        .end = m_BoundEnd,
        .outside = m_Outside,
        .out_left = m_OutsideLeft,
        .out_top = m_OutsideTop,
        .out_right = m_OutsideRight,
        .out_bottom = m_OutsideBottom,
        .matrix = g_MatrixPtr,
        .view = &g_ViewMatrix,
        .eye = &g_ViewPos,
        .far_z = Output_GetFarZ(),
        .persp = g_PhdPersp,
        .center_x = Viewport_GetCenterX(VIEWPORT_GAME),
        .center_y = Viewport_GetCenterY(VIEWPORT_GAME),
        .phd_left = g_PhdLeft,
        .phd_right = g_PhdRight,
        .phd_top = g_PhdTop,
        .phd_bottom = g_PhdBottom,
        .min_x = Viewport_GetMinX(VIEWPORT_GAME),
        .max_x = Viewport_GetMaxX(VIEWPORT_GAME),
        .min_y = Viewport_GetMinY(VIEWPORT_GAME),
        .max_y = Viewport_GetMaxY(VIEWPORT_GAME),
        .mark = M_MarkV,
        .off_pos = offsetof(ROOM, pos),
        .off_portals = offsetof(ROOM, portals),
        .off_outside = offsetof(ROOM, flags.outside),
        .off_inside = offsetof(ROOM, flags.inside),
    };
    #if defined(DC_EMIT_VERIFY)
    {
        const int32_t room_count = Room_GetCount();
        static OUTPUT_ROOM_BIND *before = nullptr;
        static OUTPUT_ROOM_BIND *after_asm = nullptr;
        static int32_t cap = 0;
        if (room_count > cap) {
            // Kept small: the verify build has little heap to spare.
            Memory_Free(before);
            Memory_Free(after_asm);
            cap = room_count;
            before = Memory_Alloc(sizeof(OUTPUT_ROOM_BIND) * cap);
            after_asm = Memory_Alloc(sizeof(OUTPUT_ROOM_BIND) * cap);
        }
        static int32_t queue_before[M_MAX_BOUND_ROOMS];
        static int32_t queue_asm[M_MAX_BOUND_ROOMS];
        static int32_t marks_asm[MAX_ROOMS];
        static uint32_t m_Runs;
        static uint32_t m_Bad;
        memcpy(before, run.binds, sizeof(before[0]) * room_count);
        memcpy(queue_before, m_BoundRooms, sizeof(queue_before));
        const int32_t start0 = m_BoundStart;
        const int32_t end0 = m_BoundEnd;
        const int32_t outside0 = m_Outside;
        const int32_t window0[4] = { m_OutsideLeft, m_OutsideTop,
                                     m_OutsideRight, m_OutsideBottom };
        run.mark = M_MarkLog;
        m_MarkCount = 0;
        dc_get_bounds(&run);
        const int32_t marks_n = m_MarkCount;
        memcpy(marks_asm, m_MarkLog, sizeof(marks_asm));
        memcpy(after_asm, run.binds, sizeof(after_asm[0]) * room_count);
        memcpy(queue_asm, m_BoundRooms, sizeof(queue_asm));
        // The C code now, from the same start.
        memcpy(run.binds, before, sizeof(before[0]) * room_count);
        memcpy(m_BoundRooms, queue_before, sizeof(queue_before));
        m_BoundStart = start0;
        m_BoundEnd = end0;
        m_Outside = outside0;
        m_OutsideLeft = window0[0];
        m_OutsideTop = window0[1];
        m_OutsideRight = window0[2];
        m_OutsideBottom = window0[3];
        const int32_t drawn_before = Room_DrawGetCount();
        M_GetBoundsC();
        bool ok = memcmp(after_asm, run.binds, sizeof(after_asm[0]) * room_count)
                == 0
            && memcmp(queue_asm, m_BoundRooms, sizeof(queue_asm)) == 0
            && run.start == m_BoundStart && run.end == m_BoundEnd
            && run.outside == m_Outside && run.out_left == m_OutsideLeft
            && run.out_top == m_OutsideTop && run.out_right == m_OutsideRight
            && run.out_bottom == m_OutsideBottom
            && marks_n == Room_DrawGetCount() - drawn_before;
        for (int32_t i = 0; ok && i < marks_n; i++) {
            ok = marks_asm[i] == Room_DrawGetRoom(drawn_before + i);
        }
        m_Runs++;
        if (!ok && ++m_Bad <= 6) {
            LOG_WARNING(
                "[bounds-verify] MISMATCH marks asm %d C %d, end %d/%d",
                (int)marks_n, (int)(Room_DrawGetCount() - drawn_before),
                (int)run.end, (int)m_BoundEnd);
            int32_t shown = 0;
            for (int32_t i = 0; i < room_count && shown < 4; i++) {
                const OUTPUT_ROOM_BIND *const a = &after_asm[i];
                const OUTPUT_ROOM_BIND *const c = &run.binds[i];
                if (memcmp(a, c, sizeof(*a)) == 0) {
                    continue;
                }
                shown++;
                LOG_WARNING(
                    "[bounds-verify]  room %d asm act %d drw %d b %d %d %d %d "
                    "t %d %d %d %d | C act %d drw %d b %d %d %d %d t %d %d %d "
                    "%d",
                    (int)i, a->active, a->drawn, a->bound_left, a->bound_right,
                    a->bound_top, a->bound_bottom, a->test_left, a->test_right,
                    a->test_top, a->test_bottom, c->active, c->drawn,
                    c->bound_left, c->bound_right, c->bound_top,
                    c->bound_bottom, c->test_left, c->test_right, c->test_top,
                    c->test_bottom);
            }
            LOG_WARNING(
                "[bounds-verify]  start0 %d end0 %d queue0 %d far %d persp %d",
                (int)start0, (int)end0, (int)queue_before[start0 & 127],
                (int)run.far_z, (int)run.persp);
        }
        if (m_Runs % 300 == 0) {
            LOG_WARNING(
                "[bounds-verify] %u runs, %u bad", (unsigned)m_Runs,
                (unsigned)m_Bad);
        }
    }
    #else
    dc_get_bounds(&run);
    m_BoundStart = run.start;
    m_BoundEnd = run.end;
    m_Outside = run.outside;
    m_OutsideLeft = run.out_left;
    m_OutsideTop = run.out_top;
    m_OutsideRight = run.out_right;
    m_OutsideBottom = run.out_bottom;
    #endif
}
#else
static void M_GetBounds(void)
{
    M_GetBoundsC();
}
#endif

static void M_DrawSkybox(void)
{
    if (!Output_IsSkyboxEnabled()) {
        return;
    }

    g_PhdLeft = m_OutsideLeft;
    g_PhdTop = m_OutsideTop;
    g_PhdRight = m_OutsideRight;
    g_PhdBottom = m_OutsideBottom;

    if (!Output_Sky_Draw()) {
        m_Outside = -1;
    }
}

static void M_DrawRoomItem(const int16_t item_num, void *const ud)
{
    ITEM *const item = Item_Get(item_num);
    const OBJECT *const obj = Object_Get(item->object_id);
    OUTPUT_ITEM_BIND *const bind = Output_Bind_GetItem(item);
    if (bind->drawn || !item->is_visible || obj->draw_func == nullptr
        || (M_IsSceneryOnly() && obj->intelligent)) {
        return;
    }

    int32_t left = Viewport_GetMaxX(VIEWPORT_GAME);
    int32_t top = Viewport_GetMaxY(VIEWPORT_GAME);
    int32_t right = Viewport_GetMinX(VIEWPORT_GAME);
    int32_t bottom = Viewport_GetMinY(VIEWPORT_GAME);
    bool overlap = false;
    for (int32_t i = 0; i < Room_DrawGetCount(); i++) {
        const int16_t room_num = Room_DrawGetRoom(i);
        const ROOM *const room = Room_Get(room_num);
        if (!M_DrawSet_Has(&room->drawn_items, item_num)) {
            continue;
        }
        const OUTPUT_ROOM_BIND *const room_bind = Output_Bind_GetRoom(room);
        CLAMPG(left, room_bind->bound_left);
        CLAMPG(top, room_bind->bound_top);
        CLAMPL(right, room_bind->bound_right);
        CLAMPL(bottom, room_bind->bound_bottom);
        overlap = overlap || Room_IsOverlapping(room_num);
    }

    const int32_t old_left = g_PhdLeft;
    const int32_t old_top = g_PhdTop;
    const int32_t old_right = g_PhdRight;
    const int32_t old_bottom = g_PhdBottom;
    g_PhdLeft = left;
    g_PhdTop = top;
    g_PhdRight = right;
    g_PhdBottom = bottom;
    if (overlap) {
        Output_SetObjectScissor(&(VIEWPORT_RECT) {
            .x = left,
            .y = bottom,
            .width = right - left,
            .height = bottom - top,
        });
    }

    Output_Water_SetupFromRoom(Room_Get(item->room_num));

    // A fading body scales down the tint already in force rather than
    // replacing it, so it keeps the water color it is lying in.
    const bool is_fading = item->fade > 0;
    if (is_fading) {
        RGBA_F tint = Output_GetTint();
        tint.a *= item->fade / 255.0f;
        Output_PushTintOverride(tint);
    }
#if defined(__DREAMCAST__)
    // The walls between the eye and a creature or an item hide it: it is not
    // drawn (Lara never is left out).
    bool hidden = false;
    if (item != Lara_GetItem()) {
        hidden = M_ObjectHidden(
            Room_Get(item->room_num), &item->pos, WALL_L, item->pos.y - 2 * WALL_L,
            item->pos.y, nullptr);
        if (hidden) {
            Matrix_Push();
            Matrix_TranslateAbs32(item->pos);
            const float lo[3] = { -WALL_L, -2 * WALL_L, -WALL_L };
            const float hi[3] = { WALL_L, 0, WALL_L };
            DC_Debug_Box(g_WMatrixPtr, lo, hi, 0xC0FF8000u);
            Matrix_Pop();
        }
    }
    if (!hidden)
#endif
    {
#if defined(__DREAMCAST__)
        if (obj->intelligent) {
            DC_PROBE_START(probe_en);
            bind->drawn |= obj->draw_func(item);
            DC_PROBE_STOP(DC_PROBE_N_DRAW, probe_en);
            g_DC_ProbeUs[DC_PROBE_N_DRAWN]++; // TEMP
        } else
#endif
        {
            bind->drawn |= obj->draw_func(item);
        }
    }
    if (is_fading) {
        Output_PopTintOverride();
    }

    Output_SetObjectScissor(nullptr);
    g_PhdLeft = old_left;
    g_PhdTop = old_top;
    g_PhdRight = old_right;
    g_PhdBottom = old_bottom;

    if (Output_IsControlFrame()) {
        Item_ControlDraw(item);
    }
}

static void M_DrawSingleRoom(const ROOM *const room)
{
    Output_SetCurrentRoom(room);
    Output_Water_SetupFromRoom(room);

    OUTPUT_ROOM_BIND *const bind = Output_Bind_GetRoom(room);
    g_PhdLeft = bind->bound_left;
    g_PhdTop = bind->bound_top;
    g_PhdRight = bind->bound_right;
    g_PhdBottom = bind->bound_bottom;

    if (g_Config.debug.enable_debug_room_clip) {
        const VIEWPORT_RECT game = Viewport_GetRect(VIEWPORT_GAME);
        const VIEWPORT_RECT ui = Viewport_GetRect(VIEWPORT_UI);
        const float scale = ui.width / (float)game.width;
        Output_DrawScreenFrame(
            ui.x + (g_PhdLeft - game.x) * scale,
            ui.y + (g_PhdTop - game.y) * scale,
            (g_PhdRight - g_PhdLeft) * scale, (g_PhdBottom - g_PhdTop) * scale,
            (RGBA_8888) { 0, 255, 0, 128 }, (RGBA_8888) { 0, 255, 0, 128 },
            scale);
    }

    Matrix_TranslateAbs32(room->pos);
#if defined(__DREAMCAST__)
    if (!bind->occluded)
#endif
    {
        DC_PROBE(DC_PROBE_S_ROOMGEO, Output_DrawRoom(room, false));
    }
#if defined(__DREAMCAST__)
    DC_Debug_RoomLabel(room, g_WMatrixPtr);
    if (bind->occluded) {
        DC_Debug_RoomBox(room, g_WMatrixPtr, 0xC0FF00FFu);
    }
#endif
#if defined(__DREAMCAST__)
    g_DC_ProbeUs[DC_PROBE_S_ROOMS]++;
#endif

    Output_Water_SetupFromRoom(room);

    Matrix_Push();
    Matrix_TranslateAbs32(room->pos);

    g_PhdLeft = bind->bound_left;
    g_PhdTop = bind->bound_top;
    g_PhdRight = bind->bound_right;
    g_PhdBottom = bind->bound_bottom;

    DC_PROBE_START(probe_statics);
#if defined(__DREAMCAST__)
    const int32_t num_statics = bind->occluded ? 0 : room->num_static_meshes;
#else
    const int32_t num_statics = room->num_static_meshes;
#endif
    for (int32_t i = 0; i < num_statics; i++) {
        const STATIC_MESH *const mesh = &room->static_meshes[i];
        if (M_DrawSet_Has(&m_DrawnStatics, mesh->draw_num)) {
            continue;
        }
#if defined(__DREAMCAST__)
        if (OutputSource_Rooms_IsStaticMerged(mesh->draw_num)) {
            continue; // drawn with the room's own faces
        }
#endif
        const STATIC_OBJECT_3D *const obj =
            Object_Get3DStatic(mesh->static_num);
        if (!obj->visible) {
            continue;
        }

#if defined(__DREAMCAST__)
        {
            const BOUNDS_16 *const b = &obj->draw_bounds;
            const int32_t r =
                MAX(MAX(ABS(b->min.x), ABS(b->max.x)),
                    MAX(ABS(b->min.z), ABS(b->max.z)))
                    * 3 / 2
                + 256;
            if (M_ObjectHidden(
                    Room_Get(mesh->room_num), &mesh->pos, r,
                    mesh->pos.y + b->min.y - 128, mesh->pos.y + b->max.y + 128,
                    mesh)) {
                Matrix_Push();
                Matrix_TranslateAbs32(mesh->pos);
                Matrix_RotY(mesh->rot.y);
                const float lo[3] = { b->min.x, b->min.y, b->min.z };
                const float hi[3] = { b->max.x, b->max.y, b->max.z };
                DC_Debug_Box(g_WMatrixPtr, lo, hi, 0xC0FF8000u);
                Matrix_Pop();
                continue;
            }
        }
#endif
        Matrix_Push();
        Matrix_TranslateAbs32(mesh->pos);
        Matrix_RotY(mesh->rot.y);
#if defined(__DREAMCAST__)
        {
            // A static wholly behind the near plane: its bounding sphere's
            // far edge is not past it. What the corner test would find out in
            // eight transforms; the sphere never rejects a visible one.
            const BOUNDS_16 *const b = &obj->draw_bounds;
            const float bx = MAX(ABS(b->min.x), ABS(b->max.x));
            const float by = MAX(ABS(b->min.y), ABS(b->max.y));
            const float bz = MAX(ABS(b->min.z), ABS(b->max.z));
            const float r = sqrtf(bx * bx + by * by + bz * bz);
            const float m0 = (float)g_MatrixPtr->_20;
            const float m1 = (float)g_MatrixPtr->_21;
            const float m2 = (float)g_MatrixPtr->_22;
            const float row = sqrtf(m0 * m0 + m1 * m1 + m2 * m2);
            if ((float)g_MatrixPtr->_23 + row * r <= (float)Output_GetNearZ()) {
                Matrix_Pop();
                continue;
            }
        }
#endif
        const CLIP clip = Output_CheckBoundsClip(&obj->draw_bounds);
        if (clip != CLIP_NOT_VISIBLE) {
            const ROOM *const owner = Room_Get(mesh->room_num);
            M_DrawSet_Add(&m_DrawnStatics, mesh->draw_num);
            Output_Water_SetupFromRoom(owner);
            Output_CalculateStaticMeshLight(mesh->pos, mesh->shade, owner);
            Object_DrawMesh(obj->mesh_idx, clip, false);
            Output_Water_SetupFromRoom(room);
            if (g_Config.debug.enable_debug_bounding_boxes) {
                const BOUNDS_32 bounds = BOUNDS_32_From16(&obj->draw_bounds);
                Output_DrawCuboid(&bounds);
            }
        }
        Matrix_Pop();
    }

    DC_PROBE_STOP(DC_PROBE_S_STATICS, probe_statics);

    DC_PROBE_START(probe_items);
    M_DrawSet_ForEach(&room->drawn_items, M_DrawRoomItem, nullptr);
    Output_Water_SetupFromRoom(room);
    DC_PROBE_STOP(DC_PROBE_S_ITEMS, probe_items);

    g_PhdLeft = Viewport_GetMinX(VIEWPORT_GAME);
    g_PhdTop = Viewport_GetMinY(VIEWPORT_GAME);
    g_PhdRight = Viewport_GetMaxX(VIEWPORT_GAME);
    g_PhdBottom = Viewport_GetMaxY(VIEWPORT_GAME);

    DC_PROBE_START(probe_effects);
    int16_t effect_num = room->effect_num;
    while (effect_num != NO_EFFECT) {
        const EFFECT *const effect = Effect_Get(effect_num);
#if defined(__DREAMCAST__)
        // Smoke, splashes and blood behind a wall are not drawn.
        if (!M_ObjectHidden(
                Room_Get(effect->room_num), &effect->pos, 256,
                effect->pos.y - 256, effect->pos.y + 256, nullptr)) {
            Effect_Draw(effect_num);
        }
#else
        Effect_Draw(effect_num);
#endif
        effect_num = effect->next_free;
    }
    DC_PROBE_STOP(DC_PROBE_S_EFR, probe_effects);

    Matrix_Pop();
}

static void M_Shutdown(void)
{
    Vector_Free(m_RoomsToDraw);
    m_RoomsToDraw = nullptr;
}

void Room_DrawReset(void)
{
    M_EnsureRoomsToDraw();
    M_DrawSet_Init(&m_DrawnStatics);
    Vector_Clear(m_RoomsToDraw);
}

void Room_MarkToBeDrawn(const int16_t room_num)
{
    if (Vector_Contains(m_RoomsToDraw, &room_num)) {
        return;
    }
    // The originals held 128 visible rooms at most (checked, not overrun).
    if (LOT_IsOriginalSim() && m_RoomsToDraw->count >= 128) {
        return;
    }
    Vector_Add(m_RoomsToDraw, &room_num);
}

int32_t Room_DrawGetCount(void)
{
    if (m_RoomsToDraw == nullptr) {
        return 0;
    }
    return m_RoomsToDraw->count;
}

int16_t Room_DrawGetRoom(const int16_t idx)
{
    return *(int16_t *)Vector_Get(m_RoomsToDraw, idx);
}

void Room_DrawAllRooms(const int16_t current_room, const int16_t target_room)
{
    const ROOM *const room = Room_Get(current_room);
    // The camera may name a room this level does not have, which the bindings
    // below would index out of bounds. There is nothing to draw from there.
    if (room == nullptr) {
        return;
    }
    Output_Bind_ResetRooms();
    OUTPUT_ROOM_BIND *const bind = Output_Bind_GetRoom(room);
    bind->test_left = Viewport_GetMinX(VIEWPORT_GAME);
    bind->test_top = Viewport_GetMinY(VIEWPORT_GAME);
    bind->test_right = Viewport_GetMaxX(VIEWPORT_GAME);
    bind->test_bottom = Viewport_GetMaxY(VIEWPORT_GAME);
    bind->active = true;

    g_PhdLeft = bind->test_left;
    g_PhdTop = bind->test_top;
    g_PhdRight = bind->test_right;
    g_PhdBottom = bind->test_bottom;

    m_BoundRooms[0] = current_room;
    m_BoundStart = 0;
    m_BoundEnd = 1;

    Room_DrawReset();
    m_Outside = room->flags.outside;

    if (m_Outside) {
        m_OutsideLeft = Viewport_GetMinX(VIEWPORT_GAME);
        m_OutsideTop = Viewport_GetMinY(VIEWPORT_GAME);
        m_OutsideRight = Viewport_GetMaxX(VIEWPORT_GAME);
        m_OutsideBottom = Viewport_GetMaxY(VIEWPORT_GAME);
    } else {
        m_OutsideLeft = Viewport_GetMaxX(VIEWPORT_GAME);
        m_OutsideTop = Viewport_GetMaxY(VIEWPORT_GAME);
        m_OutsideBottom = Viewport_GetMinY(VIEWPORT_GAME);
        m_OutsideRight = Viewport_GetMinX(VIEWPORT_GAME);
    }

#if defined(__DREAMCAST__)
    m_SightCamera = room;
    M_PrepareEyes(room);
#endif
    DC_PROBE(DC_PROBE_S_BOUNDS, M_GetBounds());
#if defined(__DREAMCAST__)
    M_FindOccluded(current_room);
#endif

    if (m_Outside) {
        DC_PROBE(DC_PROBE_S_SKY, M_DrawSkybox());
    }

    Output_Bind_ResetItems();

    for (int32_t i = 0; i < Room_DrawGetCount(); i++) {
        const int16_t draw_room_num = Room_DrawGetRoom(i);
        M_DrawSingleRoom(Room_Get(draw_room_num));
    }

    const ITEM *const lara_item = Lara_GetItem();
    if (!M_IsSceneryOnly() && lara_item != nullptr && Object_Get(O_LARA)->loaded
        && Lara_Mesh_IsReady()) {
        const ROOM *const lara_room = Room_Get(lara_item->room_num);
        Output_Water_SetupFromRoom(lara_room);
        Output_SetCurrentRoom(lara_room);
        DC_PROBE(DC_PROBE_S_LARA, Lara_Draw(lara_item));
    }

    CutSeq_DrawActors();

    DC_PROBE_START(probe_fx);
    Output_Water_SetupAboveWater(false);
    if (!M_IsSceneryOnly()) {
        DC_PROBE(DC_PROBE_S_FXM, FX_Draw());
#if defined(__DREAMCAST__)
        // TEST (TRX_DC_ASMOFF2 bit 134217728): no particles.
        if ((g_DC_AsmOff2 & 134217728) == 0)
#endif
        DC_PROBE(DC_PROBE_S_SPK, Sparks_Draw());
    }
    DC_PROBE(DC_PROBE_S_ROPE, Rope_DrawAll());
    DC_PROBE(DC_PROBE_S_FLARE, Output_LensFlares_Draw());
    DC_PROBE_STOP(DC_PROBE_S_FX, probe_fx);
}

bool Room_IsSkyVisible(void)
{
    return m_Outside != 0;
}

void Room_AddDrawnItem(const int16_t room_num, const int16_t item_num)
{
    if (room_num != NO_ROOM) {
        ROOM *const room = Room_Get(room_num);
        M_DrawSet_Add(&room->drawn_items, item_num);
    }
}

void Room_RemoveDrawnItem(const int16_t room_num, const int16_t item_num)
{
    if (room_num != NO_ROOM) {
        ROOM *const room = Room_Get(room_num);
        M_DrawSet_Remove(&room->drawn_items, item_num);
    }
}

// Made as the session starts, before any level: made as a level is played, it
// would stay in the middle of the memory the next level loads into.
REGISTER_SUBSYSTEM(.init = M_EnsureRoomsToDraw, .shutdown = M_Shutdown)
