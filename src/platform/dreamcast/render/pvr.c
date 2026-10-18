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
#define M_VERTBUF_OP (2 * 160 * 1024)
#define M_VERTBUF_TR (2 * 64 * 1024)
#define M_VERTBUF_PT (2 * 160 * 1024)
#define M_MAX_TEXTURE_PAGES 64
#define M_DEPTH_LAYER_MAX 1.0f
#define M_DEPTH_MIN 1.0e-4f
#define M_DEPTH_RATIO 2.0e4f
#define M_DEPTH_LAYERS 8
// Keeps the first layer clear of the background plane, which the PowerVR
// keeps at a depth of 0.0001.
#define M_DEPTH_FIRST_SCALE 10.0f

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
} M_LIST;

static bool m_Initialised = false;
static M_LIST m_Lists[3] = {
    { .list = PVR_LIST_OP_POLY },
    { .list = PVR_LIST_TR_POLY },
    { .list = PVR_LIST_PT_POLY },
};
static M_LIST *m_Current = nullptr;
static DC_BLEND m_Blend = DC_BLEND_OPAQUE;
static int32_t m_Page = DC_PVR_NO_TEXTURE;
static bool m_DepthTest = true;
static bool m_Bilinear = false;
static bool m_InFrame = false;
static int32_t m_DepthLayer = 0;
static float m_DepthScale = M_DEPTH_FIRST_SCALE;
static int32_t m_Submitted = 0;
static int32_t m_Dropped = 0;

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
            PVR_BINSIZE_0, // opaque modifiers
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

    const size_t sizes[3] = { M_VERTBUF_OP, M_VERTBUF_TR, M_VERTBUF_PT };
    for (int32_t i = 0; i < 3; i++) {
        m_Lists[i].buffer = M_AllocVertbuf(sizes[i]);
        // KOS splits the buffer in two, wants every write to stay strictly
        // below the half's end and closes each list with up to 64 bytes of
        // its own (a blank header and a zero marker).
        m_Lists[i].frame_capacity = sizes[i] / 2 - 128;
        pvr_set_vertbuf(m_Lists[i].list, m_Lists[i].buffer, sizes[i]);
    }
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
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

void DC_PVR_BeginFrame(void)
{
    if (m_InFrame) {
        return;
    }
    pvr_wait_ready();
    pvr_scene_begin();
    for (int32_t i = 0; i < 3; i++) {
        m_Lists[i].frame_written = 0;
        m_Lists[i].has_state = false;
    }
    m_DepthLayer = 0;
    m_DepthScale = M_DEPTH_FIRST_SCALE;
    m_Submitted = 0;
    m_Dropped = 0;
    m_InFrame = true;
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
    if (m_DepthLayer < M_DEPTH_LAYERS - 1) {
        m_DepthLayer++;
        m_DepthScale *= M_DEPTH_RATIO;
    }
}

float DC_PVR_Depth(float inv_w)
{
    // The PowerVR corrects textures for perspective with the same value it
    // depth-tests, so that value must stay proportional to 1/w: each layer
    // scales it into a band of its own instead of adding an offset. A layer
    // spans 1/w from M_DEPTH_MIN (geometry 1/M_DEPTH_MIN times the near
    // distance away) to 1, so M_DEPTH_RATIO times each band clears the last.
    if (inv_w < M_DEPTH_MIN) {
        inv_w = M_DEPTH_MIN;
    } else if (inv_w > M_DEPTH_LAYER_MAX) {
        inv_w = M_DEPTH_LAYER_MAX;
    }
    return inv_w * m_DepthScale;
}

void DC_PVR_SetFilter(const bool bilinear)
{
    m_Bilinear = bilinear;
}

void DC_PVR_SetState(
    const DC_BLEND blend, const int32_t page, const bool depth_test)
{
    m_Blend = blend;
    m_Page = page >= 0 && page < m_Pages.count && m_Pages.ptr[page] != nullptr
        ? page
        : DC_PVR_NO_TEXTURE;
    m_DepthTest = depth_test;
    m_Current = M_GetList(blend);
}

static void M_CompileHeader(pvr_poly_hdr_t *const hdr)
{
    const pvr_list_t list = m_Current->list;
    pvr_poly_cxt_t cxt;
    if (m_Page != DC_PVR_NO_TEXTURE) {
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
    cxt.depth.write = m_Blend <= DC_BLEND_PUNCH ? PVR_DEPTHWRITE_ENABLE
                                                : PVR_DEPTHWRITE_DISABLE;

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
        && list->filter == m_Bilinear;
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
    }
    return pvr_vertbuf_tail(list->list);
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
