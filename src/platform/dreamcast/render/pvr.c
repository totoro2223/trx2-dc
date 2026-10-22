// The PowerVR side of the Dreamcast renderer (see pvr.h).
//
// Each list has a buffer in main memory that the frame's polygons are written
// into in whatever order the engine's passes produce them; when the frame
// ends, each buffer goes to the tile accelerator in one go through the store
// queues. That frees the passes from the order the hardware wants the lists
// in, without KOS's vertex DMA: its chain of transfers kept the CPU waiting
// with interrupts off, about a quarter of every frame in Flycast.

#include "pvr.h"

#include <platform/dreamcast/dc_mode.h>

#include <trx/core/log.h>
#include <trx/core/math/const.h>
#include <trx/core/utils.h>
#include <trx/game/output/const.h>
#include <trx/game/level/texture_pages_16.h>

#include <arch/timer.h>
#include <dc/biosfont.h>
#include <dc/pvr.h>
#include <dc/video.h>

#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Sized by what TR2 levels use: room faces with see-through texels go to the
// punch-through list, which takes about as much as the opaque one.
// Measured on the Great Wall with quads sent as strips: the opaque list
// peaks at about 130 KiB a frame, punch-through at about 70, translucent at
// under 10; each keeps only a margin. The translucent one also takes the
// relief of the room faces near the camera (DC_BLEND_BUMP).
#define M_VERTBUF_OP (160 * 1024)
#define M_VERTBUF_TR (64 * 1024)
#define M_VERTBUF_PT (104 * 1024)
// Shadow volumes: 15 casters of at most 24 triangles of 64 bytes, and their
// headers, for each of up to 3 lights.
#define M_VERTBUF_MOD (3 * 24 * 1024)
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
    bool half;
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
// Pages drawn smaller than their texels (the interface's text at 320x240)
// sample a copy at half their size, made the first time it is asked for:
// unfiltered, the whole page would drop whole strokes of the letters. (A
// mipmapped page would do on the console, but Flycast samples mipmaps only
// when filtering.)
static bool m_HalfPages = false;
static struct {
    pvr_ptr_t ptr[M_MAX_TEXTURE_PAGES];
    bool failed[M_MAX_TEXTURE_PAGES];
} m_Half = {};
// The pages' relief (M_GetBumpPage), likewise.
static struct {
    pvr_ptr_t ptr[M_MAX_TEXTURE_PAGES];
    bool failed[M_MAX_TEXTURE_PAGES];
    // Pages made so far: making one stalls the frame it happens in.
    uint32_t made;
} m_Bump = {};
static bool m_InFrame = false;
// The state was set since M_Reserve last matched a list to it.
static bool m_StateDirty = true;
// Compiled polygon headers by state, which holds the textures' addresses:
// any change of a texture starts a new generation and empties it.
// 256 slots: with 64, a scene's few dozen states collided and were compiled
// again every frame (43 a frame in Venice).
#define M_HEADER_CACHE_BITS 8
#define M_HEADER_CACHE (1 << M_HEADER_CACHE_BITS)
static uint32_t m_HeaderGen = 1;
static struct {
    uint32_t gen;
    uint32_t key;
    pvr_poly_hdr_t hdr;
} m_HeaderCache[M_HEADER_CACHE];
// A screen frame is being built whose scene has not started yet.
static bool m_ScreenPending = false;
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
// The soft shadows' silhouettes: cells of SIZE x SIZE texels side by side,
// 16-bit as the PowerVR renders them, all rendered in one frame (each frame
// rendered into a texture costs KOS a wait for the vertical blank). Flycast
// renders nothing into a texture taller than its width, and stops on a
// strided texture narrower than the stride; this one is neither.
#define M_SHADOW_MAP_WIDTH (DC_PVR_SHADOW_MAP_SIZE * DC_PVR_SHADOW_MAP_SLOTS)
#define M_SHADOW_MAP_BYTES (M_SHADOW_MAP_WIDTH * DC_PVR_SHADOW_MAP_SIZE * 2)
static pvr_ptr_t m_ShadowMap = nullptr;
// The environment map (DC_PVR_ENV_MAP), made by M_MakeEnvMap.
#define M_ENV_MAP_SIZE 64
static pvr_ptr_t m_EnvMap = nullptr;
// 320x240: everything drawn to the screen is given in 640x480 and halved.
static bool m_LowRes = false;
static float m_ScreenScale = 1.0f;
static int32_t m_Submitted = 0;
static int32_t m_Dropped = 0;
static uint32_t m_WaitUs = 0;
static uint32_t m_MapWaitUs = 0;
static struct {
    pvr_ptr_t texture;
    bool valid;
} m_Capture = {};

// The loading screen's text, drawn by the BIOS font straight into video
// memory (16-bit writes, which video memory takes), so that it needs no main
// memory while a level loads.
#define M_LOADING_TEXT_WIDTH 512
#define M_LOADING_TEXT_HEIGHT 64
static pvr_ptr_t m_LoadingText = nullptr;
// The picture behind it and the title menu, 512x512 RGB565, stretched over
// the screen.
#define M_PICTURE_SIZE 512
#define M_PICTURE_BYTES                                                \
    (M_PICTURE_SIZE * M_PICTURE_SIZE * sizeof(uint16_t))
static pvr_ptr_t m_Picture = nullptr;

static void M_ResetFrame(void);
static void M_InitHardware(void);
static void M_CloseVQ(void);
static int32_t M_Twiddle(int32_t x, int32_t y);

// The level's texture pages, each in a block of video memory of its own,
// twiddled, or in VQ when the disc has them so (M_LoadVQ); no copy of them is
// held in main RAM.
static struct {
    int32_t count;
    bool committed;
    pvr_ptr_t ptr[M_MAX_TEXTURE_PAGES];
    uint8_t format[M_MAX_TEXTURE_PAGES];
    bool vq[M_MAX_TEXTURE_PAGES];
} m_Pages = {};
// A VQ page: a codebook of 256 2x2 blocks, then a byte per block of the page,
// twiddled (tools/dreamcast/texvq.c makes them).
#define M_VQ_PATH "/cd/textures.vq"
#define M_VQ_BOOK_BYTES (256 * 4 * 2)
#define M_VQ_BYTES (M_VQ_BOOK_BYTES + TEXTURE_PAGE_SIZE / 4)
// The disc's VQ pages while a level loads: the file, its index, and room for
// one page on its way to video memory.
static struct {
    FILE *fp;
    uint32_t count;
    uint32_t *index;
    uint8_t *data;
    // Where the file stands, to seek only when the next page is elsewhere.
    long at;
} m_VQ = {};

static M_LIST *M_GetList(const DC_BLEND blend)
{
    switch (blend) {
    case DC_BLEND_OPAQUE:
        return &m_Lists[0];
    case DC_BLEND_PUNCH:
    case DC_BLEND_PUNCH_SHADE:
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
    M_InitHardware();
    const size_t sizes[M_LIST_COUNT] = {
        M_VERTBUF_OP, M_VERTBUF_TR, M_VERTBUF_PT, M_VERTBUF_MOD,
    };
    for (int32_t i = 0; i < M_LIST_COUNT; i++) {
        m_Lists[i].buffer = M_AllocVertbuf(sizes[i]);
        m_Lists[i].frame_capacity = sizes[i];
    }
    m_Initialised = true;
    LOG_INFO(
        "PowerVR ready, %u KiB of video memory free",
        (unsigned)(pvr_mem_available() / 1024));
}

// Switches between 640x480 and 320x240. The PowerVR is set up again for the
// new screen, which empties video memory: only between levels, once their
// pages are gone.
static void M_SetLowRes(const bool low_res)
{
    if (low_res == m_LowRes || m_InFrame) {
        return;
    }
    pvr_wait_ready();
    pvr_shutdown();
    m_HeaderGen++;
    vid_set_mode(low_res ? DM_320x240 : DM_640x480, PM_RGB565);
    m_LoadingText = nullptr;
    m_Picture = nullptr;
    M_InitHardware();
    m_LowRes = low_res;
    LOG_INFO("Screen now %s", low_res ? "320x240" : "640x480");
}

// A night sky over dark ground as a polished surface shows it: a bright band
// at the horizon, a key light up to one side and a weaker one opposite. Made
// here, as the game's data holds none.
static void M_MakeEnvMap(void)
{
    m_EnvMap = pvr_mem_malloc(M_ENV_MAP_SIZE * M_ENV_MAP_SIZE * 2);
    if (m_EnvMap == nullptr) {
        return;
    }
    // Video memory takes 16-bit writes.
    volatile uint16_t *const texels = m_EnvMap;
    for (int32_t y = 0; y < M_ENV_MAP_SIZE; y++) {
        for (int32_t x = 0; x < M_ENV_MAP_SIZE; x++) {
            // The normal this texel stands for, y up.
            const float nx = (x + 0.5f) * (2.0f / M_ENV_MAP_SIZE) - 1.0f;
            const float ny = 1.0f - (y + 0.5f) * (2.0f / M_ENV_MAP_SIZE);
            const float r2 = nx * nx + ny * ny;
            const float nz = r2 < 1.0f ? sqrtf(1.0f - r2) : 0.0f;
            // Sky above, ground below, the horizon between: dark, so that
            // only the highlights cover the texture under them.
            float v = ny > 0.0f ? 0.04f + 0.12f * ny
                                : 0.02f * (1.0f + ny);
            v += 0.3f * expf(-ny * ny * 40.0f);
            // The lights: the cosine to each, sharpened.
            const float k1 = -0.45f * nx + 0.6f * ny + 0.66f * nz;
            const float k2 = 0.7f * nx + 0.2f * ny + 0.68f * nz;
            if (k1 > 0.0f) {
                v += 0.9f * powf(k1, 24.0f);
            }
            if (k2 > 0.0f) {
                v += 0.35f * powf(k2, 12.0f);
            }
            v = v > 1.0f ? 1.0f : v;
            // Cold: blue a little over red.
            const uint32_t r = (uint32_t)(v * 0.85f * 31.0f + 0.5f);
            const uint32_t g = (uint32_t)(v * 0.92f * 63.0f + 0.5f);
            const uint32_t b = (uint32_t)(v * 31.0f + 0.5f);
            texels[y * M_ENV_MAP_SIZE + x] =
                (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

static void M_InitHardware(void)
{
    pvr_init_params_t params = {
        .opb_sizes = {
            PVR_BINSIZE_16, // opaque polygons
            PVR_BINSIZE_16, // opaque modifiers (shadow volumes)
            PVR_BINSIZE_16, // translucent polygons
            PVR_BINSIZE_0, // translucent modifiers
            PVR_BINSIZE_16, // punch-through polygons
        },
        .vertex_buf_size = 512 * 1024,
        .dma_enabled = 0,
        .fsaa_enabled = 0,
        .autosort_disabled = 0,
        .opb_overflow_count = 3,
        .vbuf_doublebuf_disabled = 0,
    };
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    pvr_set_zclip(M_BACKGROUND_PLANE_DEPTH);
    // Polygons inside a shadow volume keep this much of their colour.
    pvr_set_shadow_scale(true, M_SHADOW_SCALE);
    // Punch-through keeps the texels whose alpha reaches half. TR's colour
    // key is all or nothing either side of it; the cel shading's edge lies
    // on it (DC_BLEND_PUNCH_SHADE).
    PVR_SET(PVR_PT_ALPHA_REF, 0x80);
    // Kept for good, before any level's pages, so that it never splits them.
    m_ShadowMap = pvr_mem_malloc(M_SHADOW_MAP_BYTES);
    M_MakeEnvMap();
}

void DC_PVR_Shutdown(void)
{
    DC_PVR_FreeTexturePages();
}

void DC_PVR_FreeTexturePages(void)
{
    for (int32_t i = 0; i < m_Pages.count; i++) {
        if (m_Pages.ptr[i] != nullptr) {
            pvr_mem_free(m_Pages.ptr[i]);
        }
    }
    m_HeaderGen++;
    m_Pages = (typeof(m_Pages)) {};
    M_CloseVQ();
    for (int32_t i = 0; i < M_MAX_TEXTURE_PAGES; i++) {
        if (m_Half.ptr[i] != nullptr) {
            pvr_mem_free(m_Half.ptr[i]);
        }
    }
    m_Half = (typeof(m_Half)) {};
    for (int32_t i = 0; i < M_MAX_TEXTURE_PAGES; i++) {
        if (m_Bump.ptr[i] != nullptr) {
            pvr_mem_free(m_Bump.ptr[i]);
        }
    }
    m_Bump = (typeof(m_Bump)) {};
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
        m_HeaderGen++;
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

bool DC_PVR_BeginShadowMapFrame(void)
{
    if (m_InFrame || m_ShadowMap == nullptr) {
        return false;
    }
    const uint64_t start = timer_us_gettime64();
    pvr_wait_ready();
    m_MapWaitUs = (uint32_t)(timer_us_gettime64() - start);
    if (pvr_scene_begin_rtt(
            m_ShadowMap, M_SHADOW_MAP_WIDTH, DC_PVR_SHADOW_MAP_SIZE,
            M_SHADOW_MAP_WIDTH)
        != 0) {
        return false;
    }
    M_ResetFrame();
    return true;
}

void DC_PVR_DrawLoadingScreen(
    const char *const line1, const char *const line2, int32_t percent)
{
    if (m_InFrame) {
        return;
    }
    if (m_LoadingText == nullptr) {
        // Kept for good: the level's texture pages come and go around it.
        m_HeaderGen++;
        m_LoadingText = pvr_mem_malloc(
            M_LOADING_TEXT_WIDTH * M_LOADING_TEXT_HEIGHT * sizeof(uint16_t));
        if (m_LoadingText == nullptr) {
            return;
        }
    }
    // The previous frame may still be drawing from the text; let it finish.
    pvr_wait_ready();
    uint32_t *const words = (uint32_t *)m_LoadingText;
    for (int32_t i = 0; i < M_LOADING_TEXT_WIDTH * M_LOADING_TEXT_HEIGHT / 2;
         i++) {
        words[i] = 0;
    }
    // ARGB4444: white on clear.
    uint16_t *const text = (uint16_t *)m_LoadingText;
    bfont_draw_str_ex(
        text, M_LOADING_TEXT_WIDTH, 0xFFFF, 0x0000, 16, true, line1);
    bfont_draw_str_ex(
        text + M_LOADING_TEXT_WIDTH * (M_LOADING_TEXT_HEIGHT / 2),
        M_LOADING_TEXT_WIDTH, 0xFFFF, 0x0000, 16, true, line2);

    DC_PVR_SetBackgroundColor(0.0f, 0.0f, 0.0f);
    DC_PVR_BeginFrame();
    const bool picture = m_Picture != nullptr;
    const float x0 = (640 - M_LOADING_TEXT_WIDTH) / 2.0f + 32.0f;
    // Under the picture, the text and bar sit at its foot on a dark band.
    const float y0 = picture ? 368.0f : 190.0f;
    const float z = DC_PVR_Depth2D(0.5f);
    if (picture) {
        const uint32_t white = 0xFFFFFFFF;
        const float zp = DC_PVR_Depth2D(0.1f);
        DC_PVR_SetState(DC_BLEND_OPAQUE, DC_PVR_PICTURE, false);
        const DC_PVR_VERTEX p[4] = {
            { .x = 0, .y = 0, .z = zp, .u = 0, .v = 0, .argb = white },
            { .x = 640, .y = 0, .z = zp, .u = 1, .v = 0, .argb = white },
            { .x = 0, .y = 480, .z = zp, .u = 0, .v = 1, .argb = white },
            { .x = 640, .y = 480, .z = zp, .u = 1, .v = 1, .argb = white },
        };
        DC_PVR_Quad(&p[0], &p[1], &p[2], &p[3]);
        const uint32_t shade = 0xA0000000;
        const float zs = DC_PVR_Depth2D(0.3f);
        DC_PVR_SetState(DC_BLEND_ALPHA, DC_PVR_NO_TEXTURE, false);
        const DC_PVR_VERTEX s[4] = {
            { .x = 0, .y = y0 - 8, .z = zs, .argb = shade },
            { .x = 640, .y = y0 - 8, .z = zs, .argb = shade },
            { .x = 0, .y = 472, .z = zs, .argb = shade },
            { .x = 640, .y = 472, .z = zs, .argb = shade },
        };
        DC_PVR_Quad(&s[0], &s[1], &s[2], &s[3]);
    }
    DC_PVR_SetState(DC_BLEND_ALPHA, DC_PVR_LOADING_TEXT, false);
    const uint32_t white = 0xFFFFFFFF;
    const DC_PVR_VERTEX t[4] = {
        { .x = x0, .y = y0, .z = z, .u = 0, .v = 0, .argb = white },
        { .x = x0 + M_LOADING_TEXT_WIDTH, .y = y0, .z = z, .u = 1, .v = 0,
          .argb = white },
        { .x = x0, .y = y0 + M_LOADING_TEXT_HEIGHT, .z = z, .u = 0, .v = 1,
          .argb = white },
        { .x = x0 + M_LOADING_TEXT_WIDTH, .y = y0 + M_LOADING_TEXT_HEIGHT,
          .z = z, .u = 1, .v = 1, .argb = white },
    };
    DC_PVR_Quad(&t[0], &t[1], &t[2], &t[3]);

    // The bar: its track, and the part filled.
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    const float bx0 = 120.0f;
    const float bx1 = 520.0f;
    const float by0 = y0 + M_LOADING_TEXT_HEIGHT + 16.0f;
    const float by1 = by0 + 12.0f;
    const float fill = bx0 + (bx1 - bx0) * percent / 100.0f;
    DC_PVR_SetState(DC_BLEND_ALPHA, DC_PVR_NO_TEXTURE, false);
    const uint32_t track = 0xFF303030;
    const uint32_t bar = 0xFFC8A040;
    const DC_PVR_VERTEX b0[4] = {
        { .x = bx0, .y = by0, .z = z, .argb = track },
        { .x = bx1, .y = by0, .z = z, .argb = track },
        { .x = bx0, .y = by1, .z = z, .argb = track },
        { .x = bx1, .y = by1, .z = z, .argb = track },
    };
    DC_PVR_Quad(&b0[0], &b0[1], &b0[2], &b0[3]);
    const float zf = DC_PVR_Depth2D(0.6f);
    const DC_PVR_VERTEX b1[4] = {
        { .x = bx0, .y = by0, .z = zf, .argb = bar },
        { .x = fill, .y = by0, .z = zf, .argb = bar },
        { .x = bx0, .y = by1, .z = zf, .argb = bar },
        { .x = fill, .y = by1, .z = zf, .argb = bar },
    };
    DC_PVR_Quad(&b1[0], &b1[1], &b1[2], &b1[3]);
    DC_PVR_EndFrame();
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

static void M_CloseVQ(void)
{
    if (m_VQ.fp != nullptr) {
        fclose(m_VQ.fp);
    }
    free(m_VQ.index);
    free(m_VQ.data);
    m_VQ = (typeof(m_VQ)) {};
}

// The disc's VQ pages, if it has them, for the level about to load.
static void M_OpenVQ(void)
{
    m_VQ.fp = fopen(M_VQ_PATH, "rb");
    uint32_t head[2];
    if (m_VQ.fp == nullptr || fread(head, 4, 2, m_VQ.fp) != 2
        || memcmp(head, "TVQ1", 4) != 0) {
        M_CloseVQ();
        return;
    }
    m_VQ.count = head[1];
    m_VQ.at = -1;
    m_VQ.index = malloc(m_VQ.count * 8);
    m_VQ.data = memalign(32, M_VQ_BYTES);
    if (m_VQ.index == nullptr || m_VQ.data == nullptr
        || fread(m_VQ.index, 8, m_VQ.count, m_VQ.fp) != m_VQ.count) {
        M_CloseVQ();
    }
}

bool DC_PVR_ReserveTexturePages(const int32_t count)
{
    DC_PVR_FreeTexturePages();
    M_SetLowRes(DC_Mode_Get()->low_res);
    if (count <= 0) {
        return false;
    }
    if (count > M_MAX_TEXTURE_PAGES) {
        LOG_ERROR(
            "%d texture pages, more than the %d the renderer holds", count,
            M_MAX_TEXTURE_PAGES);
        return false;
    }
    m_Pages.count = count;
    M_OpenVQ();
    return true;
}

static void M_WriteTexels(
    uint16_t *const dst, const uint16_t *const src, const size_t count)
{
    // Video memory takes no byte writes, which memcpy may make; whole words
    // it does take.
    uint32_t *const dst32 = (uint32_t *)dst;
    const uint32_t *const src32 = (const uint32_t *)src;
    for (size_t i = 0; i < count / 2; i++) {
        dst32[i] = src32[i];
    }
}

void DC_PVR_SetPicture(const char *const name)
{
    // The gameflow's "india.webp" (or a path ending so) is the disc's
    // "/cd/pix/india.565" (make_disc.sh).
    static char loaded[64] = "";
    char path[64] = "";
    if (name != nullptr) {
        const char *base = strrchr(name, '/');
        base = base != nullptr ? base + 1 : name;
        snprintf(
            path, sizeof(path), "/cd/pix/%.*s.565", (int)strcspn(base, "."),
            base);
    }
    if (m_Picture != nullptr && strcmp(path, loaded) == 0) {
        return;
    }
    if (m_Picture != nullptr) {
        // The last frame may still be drawing from it.
        pvr_wait_ready();
        pvr_mem_free(m_Picture);
        m_Picture = nullptr;
        m_HeaderGen++;
    }
    strcpy(loaded, path);
    if (name == nullptr) {
        return;
    }
    FILE *const fp = fopen(path, "rb");
    if (fp == nullptr) {
        return;
    }
    // Read a piece at a time straight into video memory: a level may be
    // about to need all the main memory there is.
    enum { M_CHUNK = 16384 };
    uint16_t *const chunk = malloc(M_CHUNK);
    m_Picture = chunk != nullptr ? pvr_mem_malloc(M_PICTURE_BYTES) : nullptr;
    size_t done = 0;
    while (m_Picture != nullptr && done < M_PICTURE_BYTES
           && fread(chunk, 1, M_CHUNK, fp) == M_CHUNK) {
        M_WriteTexels((uint16_t *)m_Picture + done / 2, chunk, M_CHUNK / 2);
        done += M_CHUNK;
    }
    if (m_Picture != nullptr && done < M_PICTURE_BYTES) {
        LOG_WARNING("%s: a short picture", path);
        pvr_mem_free(m_Picture);
        m_Picture = nullptr;
    }
    m_HeaderGen++;
    free(chunk);
    fclose(fp);
}

void DC_PVR_DrawPicture(const float brightness)
{
    if (m_Picture == nullptr) {
        return;
    }
    const float b = brightness < 0.0f ? 0.0f : (brightness > 1.0f ? 1.0f : brightness);
    const uint32_t c = (uint32_t)(b * 255.0f);
    const uint32_t argb = 0xFF000000 | (c << 16) | (c << 8) | c;
    const bool was_background = m_Depth.background;
    DC_PVR_SetBackgroundMode(true);
    DC_PVR_SetState(DC_BLEND_OPAQUE, DC_PVR_PICTURE, true);
    const float z = DC_PVR_Depth2D(0.0f);
    const DC_PVR_VERTEX v[4] = {
        { .x = 0, .y = 0, .z = z, .u = 0, .v = 0, .argb = argb },
        { .x = 640, .y = 0, .z = z, .u = 1, .v = 0, .argb = argb },
        { .x = 0, .y = 480, .z = z, .u = 0, .v = 1, .argb = argb },
        { .x = 640, .y = 480, .z = z, .u = 1, .v = 1, .argb = argb },
    };
    DC_PVR_Quad(&v[0], &v[1], &v[2], &v[3]);
    DC_PVR_SetBackgroundMode(was_background);
}

// FNV-1a over the page a 32-bit word at a time, as texvq.c keys them.
static uint32_t M_HashPage(const uint16_t *const texels)
{
    const uint32_t *const words = (const uint32_t *)texels;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < TEXTURE_PAGE_SIZE / 2; i++) {
        h = (h ^ words[i]) * 16777619u;
    }
    return h;
}

// The page in VQ, an eighth of the size, if the disc has it so: its index is
// {hash, position} pairs sorted by hash, ARGB1555 pages only. A level's own
// pages lie one after another in the file, so mostly no seek is needed.
static pvr_ptr_t M_LoadVQ(const uint16_t *const texels)
{
    if (m_VQ.fp == nullptr) {
        return nullptr;
    }
    const uint32_t hash = M_HashPage(texels);
    int32_t lo = 0;
    int32_t hi = (int32_t)m_VQ.count - 1;
    while (lo <= hi) {
        const int32_t mid = (lo + hi) / 2;
        if (m_VQ.index[mid * 2] < hash) {
            lo = mid + 1;
        } else if (m_VQ.index[mid * 2] > hash) {
            hi = mid - 1;
        } else {
            const long at =
                8 + m_VQ.count * 8 + (long)m_VQ.index[mid * 2 + 1] * M_VQ_BYTES;
            pvr_ptr_t ptr = pvr_mem_malloc(M_VQ_BYTES);
            if (ptr != nullptr
                && ((at != m_VQ.at && fseek(m_VQ.fp, at, SEEK_SET) != 0)
                    || fread(m_VQ.data, 1, M_VQ_BYTES, m_VQ.fp)
                        != M_VQ_BYTES)) {
                pvr_mem_free(ptr);
                ptr = nullptr;
            }
            m_VQ.at = ptr != nullptr ? at + M_VQ_BYTES : -1;
            if (ptr != nullptr) {
                M_WriteTexels(ptr, (const uint16_t *)m_VQ.data, M_VQ_BYTES / 2);
            }
            return ptr;
        }
    }
    return nullptr;
}

void DC_PVR_StoreTexturePage(
    const int32_t page, const uint16_t *const texels, const uint8_t format)
{
    if (page < 0 || page >= m_Pages.count) {
        return;
    }
    if (m_Pages.ptr[page] != nullptr) {
        pvr_mem_free(m_Pages.ptr[page]);
    }
    m_HeaderGen++;
    m_Pages.format[page] = format;
    m_Pages.ptr[page] =
        format == TEXTURE_PAGE_ARGB1555 ? M_LoadVQ(texels) : nullptr;
    m_Pages.vq[page] = m_Pages.ptr[page] != nullptr;
    if (m_Pages.vq[page]) {
        return;
    }
    // Twiddled, as textures sample faster.
    m_Pages.ptr[page] = pvr_mem_malloc(TEXTURE_PAGE_SIZE * sizeof(uint16_t));
    if (m_Pages.ptr[page] == nullptr) {
        LOG_ERROR(
            "No video memory for texture page %d (%u KiB free)", page,
            (unsigned)(pvr_mem_available() / 1024));
        return;
    }
    pvr_txr_load_ex(
        (void *)texels, m_Pages.ptr[page], TEXTURE_PAGE_WIDTH,
        TEXTURE_PAGE_HEIGHT, PVR_TXRLOAD_16BPP);
}

void DC_PVR_CommitTexturePages(void)
{
    M_CloseVQ();
    m_Pages.committed = true;
    m_HeaderGen++;
    char formats_text[M_MAX_TEXTURE_PAGES + 1] = {};
    for (int32_t i = 0; i < m_Pages.count; i++) {
        formats_text[i] = m_Pages.vq[i]
            ? 'v'
            : (m_Pages.format[i] == TEXTURE_PAGE_ARGB4444 ? '4' : '1');
    }
    LOG_INFO(
        "%d texture pages ready (%s), %u KiB of video memory free",
        m_Pages.count, formats_text, (unsigned)(pvr_mem_available() / 1024));
}

int32_t DC_PVR_GetTexturePageCount(void)
{
    return m_Pages.count;
}

uint32_t DC_PVR_GetBumpPagesMade(void)
{
    return m_Bump.made;
}

bool DC_PVR_IsLowRes(void)
{
    return m_LowRes;
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
    m_StateDirty = true;
    // Frames rendered into a texture keep their size; the screen's set it.
    m_ScreenScale = 1.0f;
}

void DC_PVR_BeginFrame(void)
{
    if (m_InFrame) {
        return;
    }
    // The scene starts only once its lists are built (DC_PVR_EndFrame): they
    // wait in main memory, so the previous frame goes on drawing meanwhile
    // instead of holding the CPU up here.
    m_ScreenPending = true;
    M_ResetFrame();
    m_ScreenScale = m_LowRes ? 0.5f : 1.0f;
}

void DC_PVR_EndFrame(void)
{
    if (!m_InFrame) {
        return;
    }
    if (m_ScreenPending) {
        m_ScreenPending = false;
        const uint64_t start = timer_us_gettime64();
        pvr_wait_ready();
        m_WaitUs = (uint32_t)(timer_us_gettime64() - start);
        pvr_scene_begin();
    }
    // Each list once, as the tile accelerator wants; KOS sends a blank one
    // for those left empty.
    for (int32_t i = 0; i < M_LIST_COUNT; i++) {
        if (m_Lists[i].frame_written == 0) {
            continue;
        }
        pvr_list_begin(m_Lists[i].list);
        pvr_prim(m_Lists[i].buffer, m_Lists[i].frame_written);
        pvr_list_finish();
    }
    pvr_scene_finish();
    m_InFrame = false;
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

void DC_PVR_GetDepth3D(DC_PVR_DEPTH3D *const out)
{
    if (M_IsBehindAll()) {
        *out = (DC_PVR_DEPTH3D) { .scale = M_BEHIND_ALL_SCALE };
    } else if (m_Depth.floor == 0.0f) {
        *out = (DC_PVR_DEPTH3D) { .scale = 1.0f, .track = true };
    } else {
        *out = (DC_PVR_DEPTH3D) {
            .lo = M_LAYER_INV_W_MIN,
            .scale = m_Depth.floor / M_LAYER_INV_W_MIN,
            .track = true,
        };
    }
}

void DC_PVR_UseDepth(const float z)
{
    M_UseDepth(z);
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
    m_StateDirty |= m_Bilinear != bilinear;
    m_Bilinear = bilinear;
}

void DC_PVR_SetHalfPages(const bool half)
{
    m_StateDirty |= m_HalfPages != half;
    m_HalfPages = half;
}

// One texel of the half page from the 2x2 it covers: the colour of the solid
// ones, and as alpha the most solid. Strokes a texel wide keep all their
// length so (an average would cut the thin ones and the diagonals).
static uint16_t M_HalfTexel(const uint16_t *const t, const bool argb4444)
{
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    uint32_t weight = 0;
    if (argb4444) {
        uint32_t a = 0;
        for (int32_t k = 0; k < 4; k++) {
            const uint32_t ta = t[k] >> 12;
            a = ta > a ? ta : a;
            weight += ta;
            r += ((t[k] >> 8) & 15) * ta;
            g += ((t[k] >> 4) & 15) * ta;
            b += (t[k] & 15) * ta;
        }
        if (weight == 0) {
            return 0;
        }
        return (uint16_t)((a << 12) | ((r / weight) << 8)
                          | ((g / weight) << 4) | (b / weight));
    }
    for (int32_t k = 0; k < 4; k++) {
        if ((t[k] & 0x8000) != 0) {
            weight++;
            r += (t[k] >> 10) & 31;
            g += (t[k] >> 5) & 31;
            b += t[k] & 31;
        }
    }
    if (weight == 0) {
        return 0;
    }
    return (uint16_t)(0x8000 | ((r / weight) << 10) | ((g / weight) << 5)
                      | (b / weight));
}

// The four texels of a committed page's 2x2 block i. The page is twiddled,
// and a square twiddled texture lays out each 2x2 block as four texels in a
// row, in the order the texel it makes lies in the half page; a VQ page
// holds them in its codebook.
static const uint16_t *M_PageBlock(const int32_t page, const int32_t i)
{
    const uint16_t *const texels = (const uint16_t *)m_Pages.ptr[page];
    if (!m_Pages.vq[page]) {
        return &texels[i * 4];
    }
    // The block's byte, read with the word it is in.
    const volatile uint32_t *const words =
        (const volatile uint32_t *)((const uint8_t *)texels + M_VQ_BOOK_BYTES);
    return &texels[((words[i >> 2] >> ((i & 3) * 8)) & 0xFF) * 4];
}

bool DC_PVR_HasTransparency(
    const int32_t page, const int32_t x0, const int32_t y0, const int32_t x1,
    const int32_t y1)
{
    if (page < 0 || page >= m_Pages.count || m_Pages.ptr[page] == nullptr) {
        return true;
    }
    const bool argb4444 = m_Pages.format[page] == TEXTURE_PAGE_ARGB4444;
    // A 2x2 block at a time, its texels in twiddled order: (0,0), (0,1),
    // (1,0), (1,1); the block's place is read once.
    for (int32_t by = y0 >> 1; by <= y1 >> 1; by++) {
        for (int32_t bx = x0 >> 1; bx <= x1 >> 1; bx++) {
            const uint16_t *const t = M_PageBlock(page, M_Twiddle(bx, by));
            for (int32_t k = 0; k < 4; k++) {
                const int32_t x = bx * 2 + (k >> 1);
                const int32_t y = by * 2 + (k & 1);
                if (x < x0 || x > x1 || y < y0 || y > y1) {
                    continue;
                }
                const uint16_t texel = t[k];
                if (argb4444 ? (texel >> 12) != 0xF : (texel & 0x8000) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

uint16_t DC_PVR_GetTexel(const int32_t page, const int32_t x, const int32_t y)
{
    if (page < 0 || page >= m_Pages.count || m_Pages.ptr[page] == nullptr) {
        return 0;
    }
    const int32_t t = M_Twiddle(x, y);
    return M_PageBlock(page, t >> 2)[t & 3];
}

// The page at half its size.
static pvr_ptr_t M_GetHalfPage(const int32_t page)
{
    if (m_Half.ptr[page] != nullptr || m_Half.failed[page]) {
        return m_Half.ptr[page];
    }
    const int32_t texels = TEXTURE_PAGE_SIZE / 4;
    uint16_t *const half = pvr_mem_malloc(texels * sizeof(uint16_t));
    if (half == nullptr) {
        LOG_WARNING("No video memory for page %d at half size", page);
        m_Half.failed[page] = true;
        return nullptr;
    }
    const bool argb4444 = m_Pages.format[page] == TEXTURE_PAGE_ARGB4444;
    for (int32_t i = 0; i < texels; i++) {
        half[i] = M_HalfTexel(M_PageBlock(page, i), argb4444);
    }
    m_Half.ptr[page] = half;
    m_HeaderGen++;
    LOG_INFO("Page %d made at half size", page);
    return half;
}


// The pages' relief, for DC_BLEND_BUMP: at half their size, as the PowerVR's
// bump maps take it, each texel the way its surface faces, made the first
// time a page is asked for so. A texture's brightness stands for its height,
// which is about right for rock, brick and earth; see-through texels lie
// flat.
// How steep a step of the whole brightness range makes the surface.
#define M_BUMP_SLOPE 6.0f
#define M_BUMP_SIZE (TEXTURE_PAGE_WIDTH / 2)

// Where texel (x, y) of a square twiddled texture lies: the bits of y and x
// in turn, y's lowest first.
static int32_t M_Twiddle(const int32_t x, const int32_t y)
{
    // Each 8-bit coordinate spread to every other bit.
    uint32_t sx = x & 0xFF;
    uint32_t sy = y & 0xFF;
    sx = (sx | (sx << 4)) & 0x0F0F;
    sy = (sy | (sy << 4)) & 0x0F0F;
    sx = (sx | (sx << 2)) & 0x3333;
    sy = (sy | (sy << 2)) & 0x3333;
    sx = (sx | (sx << 1)) & 0x5555;
    sy = (sy | (sy << 1)) & 0x5555;
    return (int32_t)(sy | (sx << 1));
}

// A texel's height in a relief being made; one that is see-through or off
// the page stands at the height of the texel asking, h.
static int32_t M_HeightAt(
    const uint8_t *const height, const int32_t x, const int32_t y,
    const int32_t h)
{
    if (x < 0 || x >= M_BUMP_SIZE || y < 0 || y >= M_BUMP_SIZE) {
        return h;
    }
    const int32_t at = height[y * M_BUMP_SIZE + x];
    return at != 0 ? at : h;
}

static pvr_ptr_t M_GetBumpPage(const int32_t page)
{
    if (m_Bump.ptr[page] != nullptr || m_Bump.failed[page]) {
        return m_Bump.ptr[page];
    }
    m_Bump.failed[page] = true;
    const int32_t size = M_BUMP_SIZE;
    uint8_t *const height = malloc(size * size);
    if (height == nullptr) {
        return nullptr;
    }
    uint16_t *const bump = pvr_mem_malloc(size * size * sizeof(uint16_t));
    if (bump == nullptr) {
        LOG_WARNING("No video memory for page %d's relief", page);
        free(height);
        return nullptr;
    }
    // Heights by row, from the page's 2x2 groups (see M_HalfTexel); 0 for
    // see-through, as no texel is wholly black and solid.
    int32_t lo = 255;
    int32_t hi = 0;
    const bool argb4444 = m_Pages.format[page] == TEXTURE_PAGE_ARGB4444;
    for (int32_t y = 0; y < size; y++) {
        for (int32_t x = 0; x < size; x++) {
            const uint16_t t =
                M_HalfTexel(M_PageBlock(page, M_Twiddle(x, y)), argb4444);
            int32_t r, g, b;
            bool solid;
            if (argb4444) {
                solid = (t >> 12) >= 8;
                r = ((t >> 8) & 15) * 17;
                g = ((t >> 4) & 15) * 17;
                b = (t & 15) * 17;
            } else {
                solid = (t & 0x8000) != 0;
                r = ((t >> 10) & 31) * 255 / 31;
                g = ((t >> 5) & 31) * 255 / 31;
                b = (t & 31) * 255 / 31;
            }
            const int32_t luma = (r * 77 + g * 151 + b * 28) >> 8;
            height[y * size + x] = solid ? (uint8_t)MAX(luma, 1) : 0;
            if (solid) {
                lo = MIN(lo, luma);
                hi = MAX(hi, luma);
            }
        }
    }
    // Stretched to the whole range: TR2's rock is dark and low in contrast,
    // and would come out all but flat.
    if (hi > lo) {
        for (int32_t i = 0; i < size * size; i++) {
            if (height[i] != 0) {
                height[i] = (uint8_t)(
                    1 + (MAX(height[i], lo) - lo) * 254 / (hi - lo));
            }
        }
    }
    // The PowerVR's bump texel: the surface's elevation off its plane in
    // the high byte (0 to 90 degrees), the way it leans about the normal in
    // the low one (a whole turn, from u towards v).
    for (int32_t y = 0; y < size; y++) {
        for (int32_t x = 0; x < size; x++) {
            const int32_t h = height[y * size + x];
            uint16_t texel = 0xFF00;
            if (h != 0) {
                const float dx = (M_HeightAt(height, x + 1, y, h)
                                  - M_HeightAt(height, x - 1, y, h))
                    * (0.5f / 255.0f);
                const float dy = (M_HeightAt(height, x, y + 1, h)
                                  - M_HeightAt(height, x, y - 1, h))
                    * (0.5f / 255.0f);
                const float nx = -dx * M_BUMP_SLOPE;
                const float ny = -dy * M_BUMP_SLOPE;
                const float len = sqrtf(nx * nx + ny * ny + 1.0f);
                const float s = asinf(1.0f / len);
                float r = atan2f(ny, nx);
                if (r < 0.0f) {
                    r += 2.0f * (float)M_PI;
                }
                const uint32_t sb =
                    (uint32_t)(s * (255.0f / ((float)M_PI * 0.5f)) + 0.5f);
                const uint32_t rb =
                    (uint32_t)(r * (256.0f / (2.0f * (float)M_PI))) & 0xFF;
                texel = (uint16_t)((MIN(sb, 255u) << 8) | rb);
            }
            ((volatile uint16_t *)bump)[M_Twiddle(x, y)] = texel;
        }
    }
    free(height);
    m_Bump.ptr[page] = bump;
    m_HeaderGen++;
    m_Bump.failed[page] = false;
    m_Bump.made++;
    LOG_INFO("Page %d's relief made", page);
    return bump;
}

// The current page at half its size, when that is asked for.
static pvr_ptr_t M_CurrentHalfPage(void)
{
    if (!m_HalfPages || m_Page < 0 || !m_Pages.committed) {
        return nullptr;
    }
    return M_GetHalfPage(m_Page);
}

void DC_PVR_SetState(
    const DC_BLEND blend, const int32_t page, const bool depth_test)
{
    m_Blend = blend;
    if (page == DC_PVR_SCREEN_CAPTURE) {
        m_Page = m_Capture.valid ? page : DC_PVR_NO_TEXTURE;
    } else if (page == DC_PVR_LOADING_TEXT) {
        m_Page = m_LoadingText != nullptr ? page : DC_PVR_NO_TEXTURE;
    } else if (page == DC_PVR_PICTURE) {
        m_Page = m_Picture != nullptr ? page : DC_PVR_NO_TEXTURE;
    } else if (page == DC_PVR_SHADOW_MAP) {
        m_Page = m_ShadowMap != nullptr ? page : DC_PVR_NO_TEXTURE;
    } else if (page == DC_PVR_ENV_MAP) {
        m_Page = m_EnvMap != nullptr ? page : DC_PVR_NO_TEXTURE;
    } else {
        m_Page = page >= 0 && page < m_Pages.count
                && m_Pages.ptr[page] != nullptr
            ? page
            : DC_PVR_NO_TEXTURE;
    }
    m_DepthTest = depth_test;
    m_Current = M_GetList(blend);
    m_StateDirty = true;
}

static bool M_IsShadowed(void)
{
    return m_ShadowReceiver
        && (m_Blend <= DC_BLEND_PUNCH || m_Blend == DC_BLEND_PUNCH_SHADE)
        && m_DepthTest;
}

void DC_PVR_SetShadowStrength(const float strength)
{
    pvr_set_shadow_scale(true, 1.0f - strength);
}

void DC_PVR_SetShadowReceiver(const bool receive)
{
    m_StateDirty |= m_ShadowReceiver != receive;
    m_ShadowReceiver = receive;
}

static void M_CompileHeader(pvr_poly_hdr_t *const hdr)
{
    const pvr_list_t list = m_Current->list;
    pvr_poly_cxt_t cxt;
    const pvr_ptr_t bump = m_Blend == DC_BLEND_BUMP && m_Page >= 0
        ? M_GetBumpPage(m_Page)
        : nullptr;
    if (m_Blend == DC_BLEND_BUMP) {
        // Without its relief the face is drawn white: what is there stays.
        if (bump != nullptr) {
            pvr_poly_cxt_txr(
                &cxt, list, PVR_TXRFMT_BUMP | PVR_TXRFMT_TWIDDLED,
                M_BUMP_SIZE, M_BUMP_SIZE, bump,
                m_Bilinear ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST);
            // The texture as it is: KOS's decal keeps the vertex alpha.
            cxt.txr.env = PVR_TXRENV_REPLACE;
        } else {
            pvr_poly_cxt_col(&cxt, list);
        }
    } else if (m_Page == DC_PVR_SCREEN_CAPTURE) {
        pvr_poly_cxt_txr(
            &cxt, list,
            PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE,
            M_CAPTURE_TEXTURE_WIDTH, M_CAPTURE_TEXTURE_HEIGHT,
            m_Capture.texture, PVR_FILTER_BILINEAR);
        cxt.txr.env = PVR_TXRENV_MODULATE;
    } else if (m_Page == DC_PVR_LOADING_TEXT) {
        pvr_poly_cxt_txr(
            &cxt, list, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
            M_LOADING_TEXT_WIDTH, M_LOADING_TEXT_HEIGHT, m_LoadingText,
            PVR_FILTER_NEAREST);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    } else if (m_Page == DC_PVR_PICTURE) {
        pvr_poly_cxt_txr(
            &cxt, list, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
            M_PICTURE_SIZE, M_PICTURE_SIZE, m_Picture,
            PVR_FILTER_BILINEAR);
        cxt.txr.env = PVR_TXRENV_MODULATE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    } else if (m_Page == DC_PVR_SHADOW_MAP) {
        pvr_poly_cxt_txr(
            &cxt, list, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
            M_SHADOW_MAP_WIDTH, DC_PVR_SHADOW_MAP_SIZE, m_ShadowMap,
            PVR_FILTER_BILINEAR);
        cxt.txr.env = PVR_TXRENV_MODULATE;
        // Past the map's edges it is lit: its white border stretches out.
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    } else if (m_Page == DC_PVR_ENV_MAP) {
        pvr_poly_cxt_txr(
            &cxt, list, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
            M_ENV_MAP_SIZE, M_ENV_MAP_SIZE, m_EnvMap, PVR_FILTER_BILINEAR);
        cxt.txr.env = PVR_TXRENV_MODULATE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    } else if (m_Page != DC_PVR_NO_TEXTURE) {
        // The same coordinates sample the half page as the whole.
        const pvr_ptr_t half = M_CurrentHalfPage();
        const int32_t format =
            (m_Pages.format[m_Page] == TEXTURE_PAGE_ARGB4444
                 ? PVR_TXRFMT_ARGB4444
                 : PVR_TXRFMT_ARGB1555)
            | PVR_TXRFMT_TWIDDLED
            | (m_Pages.vq[m_Page] && half == nullptr ? PVR_TXRFMT_VQ_ENABLE
                                                     : 0);
        pvr_poly_cxt_txr(
            &cxt, list, format,
            half != nullptr ? TEXTURE_PAGE_WIDTH / 2 : TEXTURE_PAGE_WIDTH,
            half != nullptr ? TEXTURE_PAGE_HEIGHT / 2 : TEXTURE_PAGE_HEIGHT,
            half != nullptr ? half : m_Pages.ptr[m_Page],
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
    // (Without it the vertex alpha reads as whole.)
    cxt.gen.alpha =
        m_Blend >= DC_BLEND_ALPHA || m_Blend == DC_BLEND_PUNCH_SHADE;
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
    case DC_BLEND_PUNCH_SHADE:
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
    case DC_BLEND_BUMP:
        // The relief comes out in the alpha, the colour white.
        cxt.blend.src = PVR_BLEND_ZERO;
        cxt.blend.dst = PVR_BLEND_SRCALPHA;
        break;
    case DC_BLEND_MUL:
        // As a source factor, "dest colour" is the colour already there.
        cxt.blend.src = PVR_BLEND_DESTCOLOR;
        cxt.blend.dst = PVR_BLEND_ZERO;
        break;
    default:
        break;
    }
    pvr_poly_compile(hdr, &cxt);
}

// The header for the current state, compiled once per generation: the same
// few dozen states come back every frame.
static void M_CachedHeader(pvr_poly_hdr_t *const hdr)
{
    const uint32_t key = ((uint32_t)m_Page & 0xFFFF)
        | ((uint32_t)m_Blend << 16) | ((uint32_t)(m_Current - m_Lists) << 22)
        | (m_DepthTest ? 1u << 25 : 0) | (m_Bilinear ? 1u << 26 : 0)
        | (m_HalfPages ? 1u << 27 : 0) | (M_IsShadowed() ? 1u << 28 : 0);
    const uint32_t slot = (key * 2654435761u) >> (32 - M_HEADER_CACHE_BITS);
    if (m_HeaderCache[slot].gen != m_HeaderGen
        || m_HeaderCache[slot].key != key) {
        M_CompileHeader(&m_HeaderCache[slot].hdr);
        m_HeaderCache[slot].gen = m_HeaderGen;
        m_HeaderCache[slot].key = key;
    }
    *hdr = m_HeaderCache[slot].hdr;
}

// Makes room for size bytes in the current list, putting a header first if
// the list's state changed. Returns nullptr when the buffer is full.
static inline void *M_Tail(const M_LIST *const list)
{
    return (uint8_t *)list->buffer + list->frame_written;
}

static void *M_Reserve(const size_t size)
{
    if (!m_InFrame || m_Current == nullptr) {
        return nullptr;
    }
    M_LIST *const list = m_Current;
    // Nothing set since the list last took this state: most polygons.
    const bool same_state = !m_StateDirty
        || (list->has_state && list->blend == m_Blend
        && list->page == m_Page && list->depth_test == m_DepthTest
        && list->filter == m_Bilinear && list->half == m_HalfPages
        && list->shadow == M_IsShadowed());
    const size_t needed = size + (same_state ? 0 : sizeof(pvr_poly_hdr_t));
    if (list->frame_written + needed > list->frame_capacity) {
        m_Dropped++;
        return nullptr;
    }
    if (!same_state) {
        pvr_poly_hdr_t *const hdr = M_Tail(list);
        M_CachedHeader(hdr);
        list->frame_written += sizeof(pvr_poly_hdr_t);
        list->has_state = true;
        list->blend = m_Blend;
        list->page = m_Page;
        list->depth_test = m_DepthTest;
        list->filter = m_Bilinear;
        list->half = m_HalfPages;
        list->shadow = M_IsShadowed();
    }
    m_StateDirty = false;
    return M_Tail(list);
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
            pvr_mod_hdr_t *const hdr = M_Tail(list);
            pvr_mod_compile(
                hdr, list->list,
                i == count - 1 ? PVR_MODIFIER_INCLUDE_LAST_POLY
                               : PVR_MODIFIER_OTHER_POLY,
                PVR_CULLING_NONE);
            list->frame_written += sizeof(pvr_mod_hdr_t);
        }
        pvr_modifier_vol_t *const vol = M_Tail(list);
        const DC_PVR_TRIANGLE *const t = &tris[i];
        vol->flags = PVR_CMD_VERTEX_EOL;
        vol->ax = t->v[0][0] * m_ScreenScale;
        vol->ay = t->v[0][1] * m_ScreenScale;
        vol->az = t->v[0][2];
        vol->bx = t->v[1][0] * m_ScreenScale;
        vol->by = t->v[1][1] * m_ScreenScale;
        vol->bz = t->v[1][2];
        vol->cx = t->v[2][0] * m_ScreenScale;
        vol->cy = t->v[2][1] * m_ScreenScale;
        vol->cz = t->v[2][2];
        vol->d1 = vol->d2 = vol->d3 = vol->d4 = vol->d5 = vol->d6 = 0;
        list->frame_written += sizeof(pvr_modifier_vol_t);
    }
}

static void M_Commit(const size_t size)
{
    m_Current->frame_written += size;
    m_Submitted++;
}

static inline void M_Fill(
    pvr_vertex_t *const out, const DC_PVR_VERTEX *const in,
    const uint32_t flags)
{
    out->flags = flags;
    out->x = in->x * m_ScreenScale;
    out->y = in->y * m_ScreenScale;
    out->z = in->z;
    out->u = in->u;
    out->v = in->v;
    out->argb = in->argb;
    out->oargb = in->oargb;
}

pvr_vertex_t *DC_PVR_ReservePoly(const int32_t count)
{
    return M_Reserve(count * sizeof(pvr_vertex_t));
}

void DC_PVR_CommitPoly(const int32_t count)
{
    M_Commit(count * sizeof(pvr_vertex_t));
}

float DC_PVR_GetScreenScale(void)
{
    return m_ScreenScale;
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

void DC_PVR_GetFrameStats(DC_PVR_FRAME_STATS *const out)
{
    out->polygons = m_Submitted;
    for (int32_t i = 0; i < M_LIST_COUNT; i++) {
        out->list_bytes[i] = (uint32_t)m_Lists[i].frame_written;
    }
    out->wait_us = m_WaitUs;
    out->map_wait_us = m_MapWaitUs;
    m_MapWaitUs = 0;
    pvr_stats_t stats;
    out->render_us =
        pvr_get_stats(&stats) == 0 ? (uint32_t)(stats.rnd_last_time / 1000) : 0;
}
