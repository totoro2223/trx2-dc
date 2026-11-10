// Dreamcast scene compositor: trx/game/output/scene_compositor.c without
// framebuffers, samplers or blend state. The passes run in the same order and
// the sources see the same calls; what the desktop does with GL state, the
// sources here say per polygon (see pvr.c). A depth clear between flushes
// becomes a new depth layer.

#include "pvr.h"
#include "shadow.h"

#include <platform/dreamcast/dc_world.h>
#include <platform/dreamcast/gl_facade.h>
#include <trx/game/matrix.h>

#include <trx/core/vector.h>
#include <trx/debug.h>
#include <trx/game/output.h>
#include <trx/game/output/lights/priv.h>
#include <trx/game/output/scene_compositor.h>
#include <trx/game/output/uniforms.h>
#include <trx/game/shell.h>

#include <platform/dreamcast/dc_probe.h> // TEMP
#include <trx/core/log.h> // TEMP
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    VECTOR *sources;
    int32_t capture_depth;
} M_PRIV;

static M_PRIV m_Priv = {};

#define M_PROCESS_SOURCES(p, func, ...)                                        \
    do {                                                                       \
        for (int32_t i = 0; i < p->sources->count; i++) {                      \
            const SCENE_SOURCE *const source =                                 \
                *(SCENE_SOURCE **)Vector_Get(p->sources, i);                   \
            if (source->func != nullptr) {                                     \
                source->func(source, ##__VA_ARGS__);                           \
            }                                                                  \
        }                                                                      \
    } while (0)

static bool M_IsSourceDirty(const M_PRIV *const p, const SCENE_PASS pass)
{
    for (int32_t i = 0; i < p->sources->count; i++) {
        const SCENE_SOURCE *const source =
            *(SCENE_SOURCE **)Vector_Get(p->sources, i);
        if (source->is_dirty != nullptr && source->is_dirty(source, pass)) {
            return true;
        }
    }
    return false;
}

static void M_RenderSourcePass(const M_PRIV *const p, const SCENE_PASS pass)
{
    // TEMP: see dc_probe.h; which source each index is, once.
    static bool logged = false;
    if (!logged) {
        logged = true;
        for (int32_t i = 0; i < p->sources->count; i++) {
            const SCENE_SOURCE *const source =
                *(SCENE_SOURCE **)Vector_Get(p->sources, i);
            LOG_INFO(
                "[probe] src%d render_pass=%08x", (int)i,
                (unsigned)(uintptr_t)source->render_pass);
        }
    }
    for (int32_t i = 0; i < p->sources->count; i++) {
        const SCENE_SOURCE *const source =
            *(SCENE_SOURCE **)Vector_Get(p->sources, i);
        if (source->is_dirty != nullptr && source->is_dirty(source, pass)) {
            ASSERT(source->render_pass != nullptr);
            if (i < 10) {
                DC_PROBE(DC_PROBE_SRC0 + i, source->render_pass(source, pass));
            } else {
                source->render_pass(source, pass);
            }
        }
    }
}

#if defined(TRX_DC_WATCHDOG) || defined(TRX_DC_PROBES)
#include <arch/timer.h>

// Time spent in the passes this frame, for the frame statistics.
uint32_t g_DC_PassUs = 0;
#endif

static void M_RenderScenePassesTimed(const M_PRIV *p);

static void M_RenderScenePasses(const M_PRIV *const p)
{
#if defined(TRX_DC_WATCHDOG) || defined(TRX_DC_PROBES)
    const uint64_t start = timer_us_gettime64();
    M_RenderScenePassesTimed(p);
    g_DC_PassUs += (uint32_t)(timer_us_gettime64() - start);
#else
    M_RenderScenePassesTimed(p);
#endif
}

static void M_RenderScenePassesTimed(const M_PRIV *const p)
{
    DC_PROBE_START(probe_lprep); // TEMP
    Output_Uniforms_UploadViewMatrix(Output_GetUniforms(), &g_ViewMatrix);
    Output_Lights_PrepareScene();
    DC_PROBE_STOP(DC_PROBE_P_LPREP, probe_lprep); // TEMP

    // The background lies behind everything the world draws: it leaves the
    // depth buffer alone.
    if (M_IsSourceDirty(p, SCENE_PASS_BACKGROUND)) {
        DC_PVR_SetBackgroundMode(true);
        M_RenderSourcePass(p, SCENE_PASS_BACKGROUND);
        DC_PVR_SetBackgroundMode(false);
    }
    for (SCENE_PASS pass = SCENE_PASS_OPAQUE; pass <= SCENE_PASS_BLEND_ADD;
         pass++) {
        if (M_IsSourceDirty(p, pass)) {
            M_RenderSourcePass(p, pass);
        }
    }
    DC_PROBE(DC_PROBE_P_VOL, DC_Shadow_DrawVolumes()); // TEMP
    DC_Debug_DrawRoomBoxes(); // TEMP
    if (M_IsSourceDirty(p, SCENE_PASS_OVERLAY)) {
        DC_PVR_NextDepthLayer();
        M_RenderSourcePass(p, SCENE_PASS_OVERLAY);
    }
    if (M_IsSourceDirty(p, SCENE_PASS_UI)) {
        DC_PVR_NextDepthLayer();
        Output_Uniforms_UploadOrthoMatrix(Output_GetUniforms());
        M_RenderSourcePass(p, SCENE_PASS_UI);
    }
}

static bool M_IsActive(void)
{
    return !Output_IsHeadless() || m_Priv.capture_depth > 0;
}

void SceneCompositor_Init(void)
{
    m_Priv.sources = Vector_Create(sizeof(SCENE_SOURCE *));
}

void SceneCompositor_Shutdown(void)
{
    if (m_Priv.sources != nullptr) {
        Vector_Free(m_Priv.sources);
        m_Priv.sources = nullptr;
    }
}

void SceneCompositor_AddSource(const SCENE_SOURCE *const source)
{
    Vector_Add(m_Priv.sources, &source);
}

// ---------------------------------------------------------------------------
// The frame between two ticks (dc_world.h, DC_Replay): Game_Draw's flushes
// recorded on the first frame after a tick (the batcher's staging, the sky
// mode, the uniforms as they stood), drawn blended halfway back to the tick
// before; then played back as they were by the frames after it, with no
// staging. A flush with another source in it (the overlay, a lightning) is
// not kept: those frames stage afresh.
// ---------------------------------------------------------------------------

bool DC_Batcher_IsSource(const SCENE_SOURCE *source);
bool DC_Batcher_SaveSegment(const SCENE_SOURCE *source, int32_t seg);
void DC_Batcher_LoadSegment(const SCENE_SOURCE *source, int32_t seg);
void DC_Batcher_LerpSegment(const SCENE_SOURCE *source, int32_t seg, bool apply);
bool DC_Batcher_LerpView(const MATRIX *before, const MATRIX *now, MATRIX *out);
void DC_Batcher_BeginLerp(void);
void DC_Batcher_CommitLerp(void);
void DC_Batcher_ForgetLerp(void);
void DC_Batcher_SetLerping(bool lerping);
void DC_Batcher_TakeLerpStats(uint32_t out[3]);

#define M_BLOCKS (DC_GL_BINDING_FOG_BULBS + 1)

static struct {
    DC_REPLAY_MODE mode;
    bool recording;
    // The last recording may be played back.
    bool valid;
    int32_t segs;
    bool sky[DC_REPLAY_SEGMENTS];
    void *block[DC_REPLAY_SEGMENTS][M_BLOCKS];
    size_t block_size[DC_REPLAY_SEGMENTS][M_BLOCKS];
    size_t block_cap[DC_REPLAY_SEGMENTS][M_BLOCKS];
    // The tick's view, the tick before's, and halfway between them.
    MATRIX view;
    MATRIX view_before;
    bool have_before;
    MATRIX view_lerp;
    bool lerp_view;
    // For the log: frames recorded, played back, and staged afresh because
    // the recording held another source / too many flushes / no memory;
    // frames whose camera was not blended (a cut).
    uint32_t recorded;
    uint32_t played;
    uint32_t refused[3];
    uint32_t cuts;
} m_Replay = {};

static const SCENE_SOURCE *M_BatcherSource(const M_PRIV *const p)
{
    for (int32_t i = 0; i < p->sources->count; i++) {
        const SCENE_SOURCE *const source =
            *(SCENE_SOURCE **)Vector_Get(p->sources, i);
        if (DC_Batcher_IsSource(source)) {
            return source;
        }
    }
    return nullptr;
}

static void M_SaveBlocks(const int32_t seg)
{
    for (int32_t b = 0; b < M_BLOCKS; b++) {
        size_t size = 0;
        const void *const data = DC_GL_GetUniformBlock(b, &size);
        m_Replay.block_size[seg][b] = 0;
        if (data == nullptr || size == 0) {
            continue;
        }
        if (size > m_Replay.block_cap[seg][b]) {
            void *const bigger = realloc(m_Replay.block[seg][b], size);
            if (bigger == nullptr) {
                continue;
            }
            m_Replay.block[seg][b] = bigger;
            m_Replay.block_cap[seg][b] = size;
        }
        memcpy(m_Replay.block[seg][b], data, size);
        m_Replay.block_size[seg][b] = size;
    }
}

static void M_LoadBlocks(const int32_t seg)
{
    for (int32_t b = 0; b < M_BLOCKS; b++) {
        size_t size = 0;
        void *const data = (void *)DC_GL_GetUniformBlock(b, &size);
        if (data != nullptr && size == m_Replay.block_size[seg][b]) {
            memcpy(data, m_Replay.block[seg][b], size);
        }
    }
}

// A flush of the recording frame: kept, and its instances blended halfway
// back. Whether the view is to be blended too, for its passes.
static bool M_RecordSegment(const M_PRIV *const p)
{
    if (m_Replay.segs == 0) {
        m_Replay.view = g_ViewMatrix;
        m_Replay.lerp_view = m_Replay.have_before
            && DC_Batcher_LerpView(
                &m_Replay.view_before, &g_ViewMatrix, &m_Replay.view_lerp);
        if (m_Replay.have_before && !m_Replay.lerp_view) {
            m_Replay.cuts++;
        }
    }
    const int32_t seg = m_Replay.segs++;
    const SCENE_SOURCE *const batcher = M_BatcherSource(p);
    if (seg >= DC_REPLAY_SEGMENTS || batcher == nullptr) {
        if (m_Replay.valid) {
            m_Replay.refused[1]++;
        }
        m_Replay.valid = false;
        return false;
    }
    for (int32_t i = 0; i < p->sources->count && m_Replay.valid; i++) {
        const SCENE_SOURCE *const source =
            *(SCENE_SOURCE **)Vector_Get(p->sources, i);
        if (source == batcher || source->is_dirty == nullptr) {
            continue;
        }
        for (SCENE_PASS pass = 0; pass < SCENE_PASS_COUNT; pass++) {
            if (source->is_dirty(source, pass)) {
                m_Replay.refused[0]++;
                m_Replay.valid = false;
                break;
            }
        }
    }
    M_SaveBlocks(seg);
    m_Replay.sky[seg] = DC_PVR_GetSkyMode();
    if (!DC_Batcher_SaveSegment(batcher, seg) && m_Replay.valid) {
        m_Replay.refused[2]++;
        m_Replay.valid = false;
    }
    DC_Batcher_LerpSegment(batcher, seg, m_Replay.lerp_view);
    return m_Replay.lerp_view;
}

void DC_Replay_SetMode(const DC_REPLAY_MODE mode)
{
    m_Replay.mode = mode;
}

bool DC_Replay_GameDraw(void)
{
    M_PRIV *const p = &m_Priv;
    if (!M_IsActive()) {
        return false;
    }
    if (m_Replay.mode == DC_REPLAY_RECORD) {
        m_Replay.recording = true;
        m_Replay.valid = true;
        m_Replay.segs = 0;
        DC_Batcher_BeginLerp();
        return false;
    }
    if (m_Replay.mode != DC_REPLAY_PLAY || !m_Replay.valid) {
        return false;
    }
    const SCENE_SOURCE *const batcher = M_BatcherSource(p);
    for (int32_t seg = 0; seg < m_Replay.segs; seg++) {
        M_LoadBlocks(seg);
        DC_Batcher_LoadSegment(batcher, seg);
        g_ViewMatrix = m_Replay.view;
        DC_PVR_SetSkyMode(m_Replay.sky[seg]);
        SceneCompositor_Flush();
        DC_PVR_SetSkyMode(false);
    }
    m_Replay.played++;
    return true;
}

void DC_Replay_GameDrawDone(void)
{
    if (!m_Replay.recording) {
        return;
    }
    m_Replay.recording = false;
    m_Replay.valid &= m_Replay.segs > 0;
    m_Replay.view_before = m_Replay.view;
    m_Replay.have_before = m_Replay.segs > 0;
    DC_Batcher_CommitLerp();
    if (++m_Replay.recorded == 300) {
        uint32_t lerp[3];
        DC_Batcher_TakeLerpStats(lerp);
        LOG_INFO(
            "[replay] 300 recorded, %u played back; staged afresh: %u other "
            "source, %u flushes, %u memory; %u cuts; instances %u blended, "
            "%u new, %u too far",
            (unsigned)m_Replay.played, (unsigned)m_Replay.refused[0],
            (unsigned)m_Replay.refused[1], (unsigned)m_Replay.refused[2],
            (unsigned)m_Replay.cuts, (unsigned)lerp[0], (unsigned)lerp[1],
            (unsigned)lerp[2]);
        m_Replay.recorded = 0;
        m_Replay.played = 0;
        m_Replay.cuts = 0;
        memset(m_Replay.refused, 0, sizeof(m_Replay.refused));
    }
}

void DC_Replay_Forget(void)
{
    m_Replay.valid = false;
    m_Replay.have_before = false;
    m_Replay.recording = false;
    DC_Batcher_ForgetLerp();
}

void SceneCompositor_BeginScene(void)
{
    M_PRIV *const p = &m_Priv;
    if (!M_IsActive()) {
        return;
    }
    Output_Uniforms_UploadGeneral(Output_GetUniforms());
    Output_Uniforms_UploadRoomLights(Output_GetUniforms(), nullptr);
    Output_SetCurrentRoom(nullptr);
    M_PROCESS_SOURCES(p, render_begin);
}

void SceneCompositor_Flush(void)
{
    M_PRIV *const p = &m_Priv;
    if (!M_IsActive()) {
        M_PROCESS_SOURCES(p, render_begin);
        return;
    }
    // The frame after a tick: kept for the frames after it, and drawn
    // halfway back to the tick before.
    const bool lerp = m_Replay.recording && M_RecordSegment(p);
    const MATRIX view = g_ViewMatrix;
    if (lerp) {
        g_ViewMatrix = m_Replay.view_lerp;
        DC_Batcher_SetLerping(true);
    }
    M_RenderScenePasses(p);
    if (lerp) {
        g_ViewMatrix = view;
        DC_Batcher_SetLerping(false);
    }
    M_PROCESS_SOURCES(p, render_end);
    M_PROCESS_SOURCES(p, render_begin);
    DC_PVR_NextDepthLayer();
}

void SceneCompositor_EndScene(void)
{
    M_PRIV *const p = &m_Priv;
    if (!M_IsActive()) {
        M_PROCESS_SOURCES(p, render_begin);
        return;
    }
    M_RenderScenePasses(p);
    M_PROCESS_SOURCES(p, render_end);
}

void SceneCompositor_BeginCapture(void)
{
    m_Priv.capture_depth++;
}

void SceneCompositor_EndCapture(void)
{
    ASSERT(m_Priv.capture_depth > 0);
    m_Priv.capture_depth--;
}

void SceneCompositor_AnimateTextures(void)
{
    M_PRIV *const p = &m_Priv;
    M_PROCESS_SOURCES(p, animate_textures);
}

void SceneCompositor_SetSamplerFilter(const TEXTURE_FILTER filter)
{
}

void SceneCompositor_BindTextures(void)
{
}

void SceneCompositor_SetSubtractiveBlend(void)
{
}
