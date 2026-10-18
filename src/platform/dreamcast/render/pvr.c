// The PowerVR side of the Dreamcast renderer (see pvr.h).
//
// Polygons go through vertex DMA: each list has a buffer in main memory that
// the frame's polygons are written into in whatever order the engine's passes
// produce them, and KOS hands the buffers to the tile accelerator when the
// scene is finished. That frees the passes from the order the hardware wants
// the lists in.

#include "pvr.h"

#include <trx/core/log.h>
#include <trx/game/output/const.h>
#include <trx/game/level/texture_pages_16.h>

#include <dc/pvr.h>

#include <malloc.h>
#include <string.h>

// Double-buffered by KOS, so each holds two frames.
// Sized by what TR2 levels use: room faces with see-through texels go to the
// punch-through list, which takes about as much as the opaque one.
#define M_VERTBUF_OP (2 * 192 * 1024)
#define M_VERTBUF_TR (2 * 32 * 1024)
#define M_VERTBUF_PT (2 * 160 * 1024)
// Shadow volumes: a few hundred triangles of 64 bytes.
#define M_VERTBUF_MOD (2 * 48 * 1024)
#define M_LIST_COUNT 4
// How much of the colour a shadowed pixel keeps.
#define M_SHADOW_SCALE 0.5f
#define M_MAX_TEXTURE_PAGES 64
// The screen capture: the 640x480 frame, as a strided texture (power of two
// sizes around it, rows 640 pixels apart).
#define M_CAPTURE_WIDTH 640
#define M_CAPTURE_HEIGHT 480
#define M_CAPTURE_TEXTURE_WIDTH 1024
#define M_CAPTURE_TEXTURE_HEIGHT 512
#define M_CAPTURE_BYTES (M_CAPTURE_WIDTH * M_CAPTURE_HEIGHT * 2)
// Depth layers. The first holds the world at plain 1/w, in (0, 1]. A later
// layer starting at depth F holds perspective vertices at F * (1/w) / 0.01,
// so that it stays proportional to 1/w (things nearer than 100 times the near
// distance), and flat ones from F to F + 1. Depths are kept small: Flycast
// scales them by the largest one in the frame.
#define M_LAYER_INV_W_MIN 0.01f
#define M_LAYER_GAP 0.01f
// The background and the sky draw at 1/w times this, behind the world (whose
// 1/w stays above it out to a million times the near distance), and the
// PowerVR's background plane, which polygons must be in front of, further
// back still.
#define M_BEHIND_ALL_SCALE 1.0e-6f
#define M_BACKGROUND_PLANE_DEPTH 1.0e-12f

typedef struct {
    pvr_list_t list;
    void *buffer;
    size_t frame_capacity;
    size_t frame_written;
    bool has_state;
    DC_BLEND blend;
    int32_t page;
    bool depth_test;
    bool filter;
    bool shadow;
} M_LIST;

static bool m_Initialised = false;
static M_LIST m_Lists[M_LIST_COUNT] = {
    { .list = PVR_LIST_OP_POLY },
    { .list = PVR_LIST_TR_POLY },
    { .list = PVR_LIST_PT_POLY },
    { .list = PVR_LIST_OP_MOD },
};
static M_LIST *const m_ModList = &m_Lists[3];
static bool m_ShadowReceiver = false;
static M_LIST *m_Current = nullptr;
static DC_BLEND m_Blend = DC_BLEND_OPAQUE;
static int32_t m_Page = DC_PVR_NO_TEXTURE;
static bool m_DepthTest = true;
static bool m_Bilinear = false;
static bool m_InFrame = false;
static struct {
    // Where the current layer starts; 0 for the first.
    float floor;
    // The largest depth handed out so far this frame.
    float top;
    // Whether the current layer holds anything, so that a new one is needed.
    bool used;
    // What draws behind the whole world: the background pass, and the sky,
    // which the desktop clears depth after.
    bool background;
    bool sky;
} m_Depth = {};
static int32_t m_Submitted = 0;
static int32_t m_Dropped = 0;
static struct {
    pvr_ptr_t texture;
    bool valid;
} m_Capture = {};

static void M_ResetFrame(void);

// The level's texture pages, in one block of video memory. They are written
// there linear as the level loads, and twiddled in place once it is complete
// (DC_PVR_CommitTexturePages), so that no copy of them is held in main RAM.
static struct {
    int32_t count;
    pvr_ptr_t block;
    bool committed;
    pvr_ptr_t ptr[M_MAX_TEXTURE_PAGES];
    uint8_t format[M_MAX_TEXTURE_PAGES];
} m_Pages = {};

static M_LIST *M_GetList(const DC_BLEND blend)
{
    switch (blend) {
    case DC_BLEND_OPAQUE:
        return &m_Lists[0];
    case DC_BLEND_PUNCH:
        return &m_Lists[2];
    default:
        return &m_Lists[1];
    }
}

static void *M_AllocVertbuf(const size_t size)
{
    void *const buffer = memalign(32, size);
    if (buffer == nullptr) {
        LOG_ERROR("Cannot allocate a %u byte vertex buffer", (unsigned)size);
    }
    return buffer;
}

void DC_PVR_Init(void)
{
    if (m_Initialised) {
        return;
    }
    pvr_init_params_t params = {
        .opb_sizes = {
            PVR_BINSIZE_16, // opaque polygons
            PVR_BINSIZE_16, // opaque modifiers (shadow volumes)
            PVR_BINSIZE_16, // translucent polygons
            PVR_BINSIZE_0, // translucent modifiers
            PVR_BINSIZE_16, // punch-through polygons
        },
        .vertex_buf_size = 512 * 1024,
        .dma_enabled = 1,
        .fsaa_enabled = 0,
        .autosort_disabled = 0,
        .opb_overflow_count = 3,
        .vbuf_doublebuf_disabled = 0,
    };
    pvr_init(&params);

    const size_t sizes[M_LIST_COUNT] = {
        M_VERTBUF_OP, M_VERTBUF_TR, M_VERTBUF_PT, M_VERTBUF_MOD,
    };
    for (int32_t i = 0; i < M_LIST_COUNT; i++) {
        m_Lists[i].buffer = M_AllocVertbuf(sizes[i]);
        // KOS splits the buffer in two, wants every write to stay strictly
        // below the half's end and closes each list with up to 64 bytes of
        // its own (a blank header and a zero marker).
        m_Lists[i].frame_capacity = sizes[i] / 2 - 128;
        pvr_set_vertbuf(m_Lists[i].list, m_Lists[i].buffer, sizes[i]);
    }
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    pvr_set_zclip(M_BACKGROUND_PLANE_DEPTH);
    // Polygons inside a shadow volume keep this much of their colour.
    pvr_set_shadow_scale(true, M_SHADOW_SCALE);
    m_Initialised = true;
    LOG_INFO(
        "PowerVR ready, %u KiB of video memory free",
        (unsigned)(pvr_mem_available() / 1024));
}

void DC_PVR_Shutdown(void)
{
    DC_PVR_FreeTexturePages();
}

void DC_PVR_FreeTexturePages(void)
{
    if (m_Pages.block != nullptr) {
        pvr_mem_free(m_Pages.block);
    }
    m_Pages = (typeof(m_Pages)) {};
    // Freed with the level's pages, so that the next level's find video
    // memory in one piece.
    if (m_Capture.texture != nullptr) {
        pvr_mem_free(m_Capture.texture);
        m_Capture.texture = nullptr;
        m_Capture.valid = false;
    }
}

bool DC_PVR_BeginCaptureFrame(void)
{
    if (m_InFrame) {
        return false;
    }
    if (m_Capture.texture == nullptr) {
        m_Capture.texture = pvr_mem_malloc(M_CAPTURE_BYTES);
        if (m_Capture.texture == nullptr) {
            LOG_WARNING("No video memory for a screen capture");
            return false;
        }
    }
    pvr_wait_ready();
    if (pvr_scene_begin_rtt(
            m_Capture.texture, M_CAPTURE_WIDTH, M_CAPTURE_HEIGHT,
            M_CAPTURE_WIDTH)
        != 0) {
        return false;
    }
    M_ResetFrame();
    pvr_txr_set_stride(M_CAPTURE_WIDTH);
    m_Capture.valid = true;
    return true;
}

void DC_PVR_DrawScreenCapture(const float brightness)
{
    if (!m_Capture.valid) {
        return;
    }
    const float b = brightness < 0.0f ? 0.0f : (brightness > 1.0f ? 1.0f : brightness);
    const uint32_t c = (uint32_t)(b * 255.0f);
    const uint32_t argb = 0xFF000000 | (c << 16) | (c << 8) | c;
    const float u1 = (float)M_CAPTURE_WIDTH / M_CAPTURE_TEXTURE_WIDTH;
    const float v1 = (float)M_CAPTURE_HEIGHT / M_CAPTURE_TEXTURE_HEIGHT;

    const bool was_background = m_Depth.background;
    DC_PVR_SetBackgroundMode(true);
    DC_PVR_SetState(DC_BLEND_OPAQUE, DC_PVR_SCREEN_CAPTURE, true);
    const float z = DC_PVR_Depth2D(0.0f);
    const DC_PVR_VERTEX v[4] = {
        { .x = 0, .y = 0, .z = z, .u = 0, .v = 0, .argb = argb },
        { .x = 640, .y = 0, .z = z, .u = u1, .v = 0, .argb = argb },
        { .x = 0, .y = 480, .z = z, .u = 0, .v = v1, .argb = argb },
        { .x = 640, .y = 480, .z = z, .u = u1, .v = v1, .argb = argb },
    };
    DC_PVR_Quad(&v[0], &v[1], &v[2], &v[3]);
    DC_PVR_SetBackgroundMode(was_background);
}

uint16_t *DC_PVR_ReserveTexturePages(const int32_t count)
{
    DC_PVR_FreeTexturePages();
    if (count <= 0) {
        return nullptr;
    }
    if (count > M_MAX_TEXTURE_PAGES) {
        LOG_ERROR(
            "%d texture pages, more than the %d the renderer holds", count,
            M_MAX_TEXTURE_PAGES);
        return nullptr;
    }
    const size_t page_bytes = TEXTURE_PAGE_SIZE * sizeof(uint16_t);
    m_Pages.block = pvr_mem_malloc(page_bytes * count);
    if (m_Pages.block == nullptr) {
        LOG_ERROR(
            "No video memory for %d texture pages (%u KiB free)", count,
            (unsigned)(pvr_mem_available() / 1024));
        return nullptr;
    }
    m_Pages.count = count;
    for (int32_t i = 0; i < count; i++) {
        m_Pages.ptr[i] = (char *)m_Pages.block + i * page_bytes;
    }
    return (uint16_t *)m_Pages.block;
}

void DC_PVR_WriteTexels(
    uint16_t *const dst, const uint16_t *const src, const size_t count)
{
    // Video memory takes no byte writes, which memcpy may make; whole words
    // it does take.
    if ((((uintptr_t)dst | (uintptr_t)src) & 3) == 0) {
        uint32_t *const dst32 = (uint32_t *)dst;
        const uint32_t *const src32 = (const uint32_t *)src;
        for (size_t i = 0; i < count / 2; i++) {
            dst32[i] = src32[i];
        }
        if ((count & 1) != 0) {
            dst[count - 1] = src[count - 1];
        }
        return;
    }
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

void DC_PVR_CommitTexturePages(const int32_t count, const uint8_t *const formats)
{
    printf("[pvr] commit %d pages, block %p, committed %d\n", (int)count, m_Pages.block, (int)m_Pages.committed);
    if (m_Pages.block == nullptr || m_Pages.committed) {
        return;
    }
    // Twiddled textures sample faster; each page is turned in place, by way
    // of one page's worth of main memory.
    const size_t page_bytes = TEXTURE_PAGE_SIZE * sizeof(uint16_t);
    uint16_t *const linear = memalign(32, page_bytes);
    for (int32_t i = 0; i < m_Pages.count && i < count; i++) {
        m_Pages.format[i] = formats != nullptr ? formats[i] : 0;
        if (linear != nullptr) {
            const uint32_t *const src = (const uint32_t *)m_Pages.ptr[i];
            uint32_t *const dst = (uint32_t *)linear;
            for (size_t j = 0; j < page_bytes / 4; j++) {
                dst[j] = src[j];
            }
            pvr_txr_load_ex(
                linear, m_Pages.ptr[i], TEXTURE_PAGE_WIDTH,
                TEXTURE_PAGE_HEIGHT, PVR_TXRLOAD_16BPP);
        }
    }
    m_Pages.committed = linear != nullptr;
    free(linear);
    char formats_text[M_MAX_TEXTURE_PAGES + 1] = {};
    for (int32_t i = 0; i < m_Pages.count; i++) {
        formats_text[i] =
            m_Pages.format[i] == TEXTURE_PAGE_ARGB4444 ? '4' : '1';
    }
    LOG_INFO(
        "%d texture pages ready (%s), %u KiB of video memory free",
        m_Pages.count, formats_text, (unsigned)(pvr_mem_available() / 1024));
}

int32_t DC_PVR_GetTexturePageCount(void)
{
    return m_Pages.count;
}

void DC_PVR_SetBackgroundColor(const float r, const float g, const float b)
{
    pvr_set_bg_color(r, g, b);
}

static void M_ResetFrame(void)
{
    for (int32_t i = 0; i < M_LIST_COUNT; i++) {
        m_Lists[i].frame_written = 0;
        m_Lists[i].has_state = false;
    }
    m_Depth.floor = 0.0f;
    m_Depth.top = 0.0f;
    m_Depth.used = false;
    m_Depth.background = false;
    m_Depth.sky = false;
    m_Submitted = 0;
    m_Dropped = 0;
    m_InFrame = true;
}

void DC_PVR_BeginFrame(void)
{
    if (m_InFrame) {
        return;
    }
    pvr_wait_ready();
    pvr_scene_begin();
    M_ResetFrame();
}

void DC_PVR_EndFrame(void)
{
    if (!m_InFrame) {
        return;
    }
    pvr_scene_finish();
    m_InFrame = false;
#if defined(TRX_DC_WATCHDOG)
    static int32_t frames = 0;
    if (++frames % 120 == 0) {
        LOG_DEBUG(
            "%d polygons (OP %uK TR %uK PT %uK)", m_Submitted,
            (unsigned)(m_Lists[0].frame_written / 1024),
            (unsigned)(m_Lists[1].frame_written / 1024),
            (unsigned)(m_Lists[2].frame_written / 1024));
    }
#endif
    if (m_Dropped > 0) {
        LOG_WARNING(
            "%d polygons did not fit the vertex buffers this frame "
            "(OP %uK TR %uK PT %uK used)",
            m_Dropped, (unsigned)(m_Lists[0].frame_written / 1024),
            (unsigned)(m_Lists[1].frame_written / 1024),
            (unsigned)(m_Lists[2].frame_written / 1024));
    }
}

void DC_PVR_NextDepthLayer(void)
{
    if (!m_Depth.used) {
        return;
    }
    m_Depth.floor = (m_Depth.top > 1.0f ? m_Depth.top : 1.0f) + M_LAYER_GAP;
    m_Depth.used = false;
}

void DC_PVR_SetBackgroundMode(const bool enabled)
{
    m_Depth.background = enabled;
}

void DC_PVR_SetSkyMode(const bool enabled)
{
    m_Depth.sky = enabled;
}

static bool M_IsBehindAll(void)
{
    return m_Depth.background || m_Depth.sky;
}

static float M_UseDepth(const float z)
{
    m_Depth.used = true;
    if (z > m_Depth.top) {
        m_Depth.top = z;
    }
    return z;
}

float DC_PVR_Depth3D(float inv_w)
{
    if (inv_w > 1.0f) {
        inv_w = 1.0f;
    }
    if (M_IsBehindAll()) {
        // Still proportional to 1/w, and far below anything the world uses;
        // it starts no layer of its own.
        return (inv_w > 0.0f ? inv_w : 0.0f) * M_BEHIND_ALL_SCALE;
    }
    if (m_Depth.floor == 0.0f) {
        return M_UseDepth(inv_w > 0.0f ? inv_w : 0.0f);
    }
    if (inv_w < M_LAYER_INV_W_MIN) {
        inv_w = M_LAYER_INV_W_MIN;
    }
    return M_UseDepth(m_Depth.floor * inv_w / M_LAYER_INV_W_MIN);
}

float DC_PVR_Depth2D(float v)
{
    if (v < 0.0f) {
        v = 0.0f;
    } else if (v > 1.0f) {
        v = 1.0f;
    }
    if (M_IsBehindAll()) {
        return (0.5f + 0.5f * v) * M_BEHIND_ALL_SCALE;
    }
    // In the first layer, in front of the world's 1/w.
    const float base = m_Depth.floor == 0.0f ? 1.0f + M_LAYER_GAP : m_Depth.floor;
    return M_UseDepth(base + v);
}

void DC_PVR_SetFilter(const bool bilinear)
{
    m_Bilinear = bilinear;
}

void DC_PVR_SetState(
    const DC_BLEND blend, const int32_t page, const bool depth_test)
{
    m_Blend = blend;
    if (page == DC_PVR_SCREEN_CAPTURE) {
        m_Page = m_Capture.valid ? page : DC_PVR_NO_TEXTURE;
    } else {
        m_Page = page >= 0 && page < m_Pages.count
                && m_Pages.ptr[page] != nullptr
            ? page
            : DC_PVR_NO_TEXTURE;
    }
    m_DepthTest = depth_test;
    m_Current = M_GetList(blend);
}

static bool M_IsShadowed(void)
{
    return m_ShadowReceiver && m_Blend <= DC_BLEND_PUNCH && m_DepthTest;
}

void DC_PVR_SetShadowReceiver(const bool receive)
{
    m_ShadowReceiver = receive;
}

static void M_CompileHeader(pvr_poly_hdr_t *const hdr)
{
    const pvr_list_t list = m_Current->list;
    pvr_poly_cxt_t cxt;
    if (m_Page == DC_PVR_SCREEN_CAPTURE) {
        pvr_poly_cxt_txr(
            &cxt, list,
            PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE,
            M_CAPTURE_TEXTURE_WIDTH, M_CAPTURE_TEXTURE_HEIGHT,
            m_Capture.texture, PVR_FILTER_BILINEAR);
        cxt.txr.env = PVR_TXRENV_MODULATE;
    } else if (m_Page != DC_PVR_NO_TEXTURE) {
        const int32_t format =
            (m_Pages.format[m_Page] == TEXTURE_PAGE_ARGB4444
                 ? PVR_TXRFMT_ARGB4444
                 : PVR_TXRFMT_ARGB1555)
            | (m_Pages.committed ? PVR_TXRFMT_TWIDDLED
                                 : PVR_TXRFMT_NONTWIDDLED);
        pvr_poly_cxt_txr(
            &cxt, list, format, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT,
            m_Pages.ptr[m_Page],
            m_Bilinear ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = PVR_UVCLAMP_NONE;
        // The texture's own alpha is what cuts the colour key out and fades
        // soft edges; the opaque list has no use for it. (The field is true
        // to disable.)
        cxt.txr.alpha = m_Blend == DC_BLEND_OPAQUE;
    } else {
        pvr_poly_cxt_col(&cxt, list);
    }
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.specular = true;
    cxt.gen.alpha = m_Blend >= DC_BLEND_ALPHA;
    cxt.depth.comparison =
        m_DepthTest ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
    // The field goes straight into the hardware's "depth write disable" bit,
    // whatever its documentation says (PVR_DEPTHWRITE_ENABLE is 0).
    cxt.depth.write = m_Blend <= DC_BLEND_PUNCH && m_DepthTest
        ? PVR_DEPTHWRITE_ENABLE
        : PVR_DEPTHWRITE_DISABLE;
    // The shadow volumes go to the opaque modifier list, which darkens the
    // opaque and punch-through polygons that take modifiers ("cheap shadow":
    // one colour, scaled inside a volume).
    if (M_IsShadowed()) {
        cxt.fmt.modifier = true;
        cxt.gen.modifier_mode = false;
    }

    switch (m_Blend) {
    case DC_BLEND_OPAQUE:
    case DC_BLEND_PUNCH:
        cxt.blend.src = PVR_BLEND_ONE;
        cxt.blend.dst = PVR_BLEND_ZERO;
        break;
    case DC_BLEND_ALPHA:
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        break;
    case DC_BLEND_ADD:
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_ONE;
        break;
    case DC_BLEND_SUB:
        // As a destination factor, "dest colour" names the other colour,
        // here the source's.
        cxt.blend.src = PVR_BLEND_ZERO;
        cxt.blend.dst = PVR_BLEND_INVDESTCOLOR;
        break;
    default:
        break;
    }
    pvr_poly_compile(hdr, &cxt);
}

// Makes room for size bytes in the current list, putting a header first if
// the list's state changed. Returns nullptr when the buffer is full.
static void *M_Reserve(const size_t size)
{
    if (!m_InFrame || m_Current == nullptr) {
        return nullptr;
    }
    M_LIST *const list = m_Current;
    const bool same_state = list->has_state && list->blend == m_Blend
        && list->page == m_Page && list->depth_test == m_DepthTest
        && list->filter == m_Bilinear && list->shadow == M_IsShadowed();
    const size_t needed = size + (same_state ? 0 : sizeof(pvr_poly_hdr_t));
    if (list->frame_written + needed > list->frame_capacity) {
        m_Dropped++;
        return nullptr;
    }
    if (!same_state) {
        pvr_poly_hdr_t *const hdr = pvr_vertbuf_tail(list->list);
        M_CompileHeader(hdr);
        pvr_vertbuf_written(list->list, sizeof(pvr_poly_hdr_t));
        list->frame_written += sizeof(pvr_poly_hdr_t);
        list->has_state = true;
        list->blend = m_Blend;
        list->page = m_Page;
        list->depth_test = m_DepthTest;
        list->filter = m_Bilinear;
        list->shadow = M_IsShadowed();
    }
    return pvr_vertbuf_tail(list->list);
}

void DC_PVR_ShadowVolume(const DC_PVR_TRIANGLE *const tris, const int32_t count)
{
    if (!m_InFrame || count <= 0) {
        return;
    }
    M_LIST *const list = m_ModList;
    const size_t needed =
        2 * sizeof(pvr_mod_hdr_t) + count * sizeof(pvr_modifier_vol_t);
    if (list->frame_written + needed > list->frame_capacity) {
        m_Dropped += count;
        return;
    }
    // Every triangle but the last goes under one header; the last one, under
    // a header of its own, closes the volume. What lies inside is shadowed.
    for (int32_t i = 0; i < count; i++) {
        if (i == 0 || i == count - 1) {
            pvr_mod_hdr_t *const hdr = pvr_vertbuf_tail(list->list);
            pvr_mod_compile(
                hdr, list->list,
                i == count - 1 ? PVR_MODIFIER_INCLUDE_LAST_POLY
                               : PVR_MODIFIER_OTHER_POLY,
                PVR_CULLING_NONE);
            pvr_vertbuf_written(list->list, sizeof(pvr_mod_hdr_t));
            list->frame_written += sizeof(pvr_mod_hdr_t);
        }
        pvr_modifier_vol_t *const vol = pvr_vertbuf_tail(list->list);
        const DC_PVR_TRIANGLE *const t = &tris[i];
        vol->flags = PVR_CMD_VERTEX_EOL;
        vol->ax = t->v[0][0];
        vol->ay = t->v[0][1];
        vol->az = t->v[0][2];
        vol->bx = t->v[1][0];
        vol->by = t->v[1][1];
        vol->bz = t->v[1][2];
        vol->cx = t->v[2][0];
        vol->cy = t->v[2][1];
        vol->cz = t->v[2][2];
        vol->d1 = vol->d2 = vol->d3 = vol->d4 = vol->d5 = vol->d6 = 0;
        pvr_vertbuf_written(list->list, sizeof(pvr_modifier_vol_t));
        list->frame_written += sizeof(pvr_modifier_vol_t);
    }
}

static void M_Commit(const size_t size)
{
    pvr_vertbuf_written(m_Current->list, size);
    m_Current->frame_written += size;
    m_Submitted++;
}

static inline void M_Fill(
    pvr_vertex_t *const out, const DC_PVR_VERTEX *const in,
    const uint32_t flags)
{
    out->flags = flags;
    out->x = in->x;
    out->y = in->y;
    out->z = in->z;
    out->u = in->u;
    out->v = in->v;
    out->argb = in->argb;
    out->oargb = in->oargb;
}

void DC_PVR_Triangle(
    const DC_PVR_VERTEX *const a, const DC_PVR_VERTEX *const b,
    const DC_PVR_VERTEX *const c)
{
    pvr_vertex_t *const out = M_Reserve(3 * sizeof(pvr_vertex_t));
    if (out == nullptr) {
        return;
    }
    M_Fill(&out[0], a, PVR_CMD_VERTEX);
    M_Fill(&out[1], b, PVR_CMD_VERTEX);
    M_Fill(&out[2], c, PVR_CMD_VERTEX_EOL);
    M_Commit(3 * sizeof(pvr_vertex_t));
}

void DC_PVR_Quad(
    const DC_PVR_VERTEX *const a, const DC_PVR_VERTEX *const b,
    const DC_PVR_VERTEX *const c, const DC_PVR_VERTEX *const d)
{
    pvr_vertex_t *const out = M_Reserve(4 * sizeof(pvr_vertex_t));
    if (out == nullptr) {
        return;
    }
    M_Fill(&out[0], a, PVR_CMD_VERTEX);
    M_Fill(&out[1], b, PVR_CMD_VERTEX);
    M_Fill(&out[2], c, PVR_CMD_VERTEX);
    M_Fill(&out[3], d, PVR_CMD_VERTEX_EOL);
    M_Commit(4 * sizeof(pvr_vertex_t));
}

int32_t DC_PVR_GetSubmittedCount(void)
{
    return m_Submitted;
}
