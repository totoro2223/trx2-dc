// Dreamcast scene compositor: trx/game/output/scene_compositor.c without
// framebuffers, samplers or blend state. The passes run in the same order and
// the sources see the same calls; what the desktop does with GL state, the
// sources here say per polygon (see pvr.c). A depth clear between flushes
// becomes a new depth layer.

#include "pvr.h"
#include "shadow.h"

#include <trx/core/vector.h>
#include <trx/debug.h>
#include <trx/game/output.h>
#include <trx/game/output/lights/priv.h>
#include <trx/game/output/scene_compositor.h>
#include <trx/game/output/uniforms.h>
#include <trx/game/shell.h>

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
    for (int32_t i = 0; i < p->sources->count; i++) {
        const SCENE_SOURCE *const source =
            *(SCENE_SOURCE **)Vector_Get(p->sources, i);
        if (source->is_dirty != nullptr && source->is_dirty(source, pass)) {
            ASSERT(source->render_pass != nullptr);
            source->render_pass(source, pass);
        }
    }
}

#if defined(TRX_DC_WATCHDOG)
#include <arch/timer.h>

// Time spent in the passes this frame, for the frame statistics.
uint32_t g_DC_PassUs = 0;
#endif

static void M_RenderScenePassesTimed(const M_PRIV *p);

static void M_RenderScenePasses(const M_PRIV *const p)
{
#if defined(TRX_DC_WATCHDOG)
    const uint64_t start = timer_us_gettime64();
    M_RenderScenePassesTimed(p);
    g_DC_PassUs += (uint32_t)(timer_us_gettime64() - start);
#else
    M_RenderScenePassesTimed(p);
#endif
}

static void M_RenderScenePassesTimed(const M_PRIV *const p)
{
    Output_Uniforms_UploadViewMatrix(Output_GetUniforms(), &g_ViewMatrix);
    Output_Lights_PrepareScene();

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
    DC_Shadow_DrawVolumes();
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
    M_RenderScenePasses(p);
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
