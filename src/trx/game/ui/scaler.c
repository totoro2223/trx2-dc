#include <trx/game/ui/scaler.h>

#include <trx/config.h>
#include <trx/core/utils.h>
#include <trx/debug.h>
#include <trx/game/viewport.h>

#if defined(__DREAMCAST__)
    #include <platform/dreamcast/renderer.h>

    // At 320x240 the text takes this much more room: at its own size it has
    // too few lines to read.
    #define M_LOW_RES_TEXT_SCALE 1.25f
#endif

#define M_MAX_SCALE_DEPTH 8

static float m_TextScaleStack[M_MAX_SCALE_DEPTH] = { 1.0f };
static size_t m_TextScaleDepth = 0;

static float M_GetPlayerTextScale(void)
{
#if defined(__DREAMCAST__)
    if (DC_Renderer_IsLowRes()) {
        return g_Config.ui.text_scale * M_LOW_RES_TEXT_SCALE;
    }
#endif
    return g_Config.ui.text_scale;
}

static float M_DoCalc(
    const float unit, const float base_width, const float base_height,
    const double factor)
{
    const float vp_width = Viewport_GetWidth(VIEWPORT_UI);
    const float vp_height = Viewport_GetHeight(VIEWPORT_UI);
    const float sign = unit < 0 ? -1 : 1;
    const float sx =
        ((double)vp_width * ABS(unit) * factor) / MAX(1, base_width);
    const float sy =
        ((double)vp_height * ABS(unit) * factor) / MAX(1, base_height);
    return MIN(sx, sy) * sign;
}

double UI_Scaler_GetScale(const UI_SCALER_TARGET target)
{
    switch (target) {
    case UI_SCALER_TARGET_BAR:
        return g_Config.ui.bar_scale;
    case UI_SCALER_TARGET_TEXT:
        return UI_Scaler_GetTextScale();
    default:
        return 1.0;
    }
}

float UI_Scaler_Calc(const float unit, const UI_SCALER_TARGET target)
{
    return M_DoCalc(unit, 640, 480, UI_Scaler_GetScale(target));
}

float UI_Scaler_CalcInverse(const float unit, const UI_SCALER_TARGET target)
{
    return unit * 0x10000 / MAX(1, UI_Scaler_Calc(0x10000, target));
}

float UI_Scaler_GetTextScale(void)
{
    return M_GetPlayerTextScale() * m_TextScaleStack[m_TextScaleDepth];
}

float UI_Scaler_GetBaseTextScale(void)
{
    return M_GetPlayerTextScale();
}

float UI_Scaler_GetContentScale(void)
{
    return m_TextScaleStack[m_TextScaleDepth];
}

void UI_Scaler_PushTextScale(const float factor)
{
    ASSERT(m_TextScaleDepth + 1 < M_MAX_SCALE_DEPTH);
    const float current = m_TextScaleStack[m_TextScaleDepth];
    m_TextScaleDepth++;
    m_TextScaleStack[m_TextScaleDepth] = current * factor;
}

void UI_Scaler_PopTextScale(void)
{
    ASSERT(m_TextScaleDepth > 0);
    m_TextScaleDepth--;
}
