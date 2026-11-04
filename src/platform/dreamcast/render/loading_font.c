#include "loading_font.h"

#include "pvr.h"

#include <platform/dreamcast/dc_games.h>
#include <trx/core/log.h>
#include <trx/core/utils.h>

#include <trx/game/objects/common.h>
#include <trx/game/output/const.h>
#include <trx/game/output/textures.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M_GAMES 3
#define M_FIRST ' '
#define M_COUNT 95 // ' ' .. '~'
#define M_SPACE 8
#define M_GAP 1

typedef struct {
    uint8_t w;
    uint8_t h;
    int8_t y;
    uint16_t *texels;
} M_GLYPH;

static M_GLYPH m_Fonts[M_GAMES][M_COUNT];
static bool m_Have[M_GAMES];

// The font's sprite of each single character (text_autogen.def's).
static int32_t M_SpriteOf(const char c)
{
    static const struct {
        const char *text;
        int32_t sprite;
    } map[] = {
#define X_GLYPH_DEFINE(text_, role_, mesh_idx_) { text_, mesh_idx_ },
#include <trx/game/ui/text_autogen.def>
#undef X_GLYPH_DEFINE
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].text[0] == c && map[i].text[1] == '\0'
            && map[i].sprite >= 0) {
            return map[i].sprite;
        }
    }
    return -1;
}

// lfont.bin: per character from ' ': w, h, y (bytes), then w*h ARGB4444
// little-endian words. Made from the DC_LoadingFont_Capture log lines by
// tools/dreamcast/lfont.py.
void DC_LoadingFont_Load(const int game)
{
    if (game < 1 || game > M_GAMES || m_Have[game - 1]) {
        return;
    }
    char path[64];
    DC_Games_Path(path, sizeof(path), "lfont.bin");
    FILE *const fp = fopen(path, "rb");
    if (fp == nullptr) {
        return;
    }
    M_GLYPH *const font = m_Fonts[game - 1];
    for (int32_t i = 0; i < M_COUNT; i++) {
        uint8_t head[3];
        if (fread(head, 1, 3, fp) != 3) {
            break;
        }
        const size_t n = (size_t)head[0] * head[1];
        if (n > 0) {
            uint16_t *const texels = malloc(n * sizeof(uint16_t));
            if (texels == nullptr || fread(texels, 2, n, fp) != n) {
                free(texels);
                break;
            }
            font[i] = (M_GLYPH) {
                .w = head[0], .h = head[1], .y = (int8_t)head[2],
                .texels = texels,
            };
        }
    }
    fclose(fp);
    m_Have[game - 1] = font['A' - M_FIRST].texels != nullptr;
}

void DC_LoadingFont_Capture(const int game)
{
    if (game < 1 || game > M_GAMES || m_Have[game - 1]) {
        return;
    }
    const OBJECT *const object = Object_Get(O_ALPHABET);
    if (!object->loaded || object->mesh_count >= 0) {
        return;
    }
    M_GLYPH *const font = m_Fonts[game - 1];
    for (int32_t i = 0; i < M_COUNT; i++) {
        const int32_t sprite_idx = M_SpriteOf((char)(M_FIRST + i));
        if (sprite_idx < 0 || sprite_idx >= -object->mesh_count) {
            continue;
        }
        const SPRITE_TEXTURE *const s =
            Output_GetSpriteTexture(object->mesh_idx + sprite_idx);
        if (s == nullptr) {
            continue;
        }
        const int32_t w = s->width / 256;
        const int32_t h = s->height / 256;
        if (w <= 0 || h <= 0 || w > 40 || h > 40) {
            continue;
        }
        uint16_t *const texels = malloc(w * h * sizeof(uint16_t));
        if (texels == nullptr) {
            continue;
        }
        const int32_t x0 = s->offset & 0xFF;
        const int32_t y0 = s->offset >> 8;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                texels[y * w + x] =
                    DC_PVR_GetTexel4444(s->tex_page, x0 + x, y0 + y);
            }
        }
        font[i] = (M_GLYPH) {
            .w = w, .h = h, .y = (int8_t)s->y0, .texels = texels,
        };
    }
    m_Have[game - 1] = font['A' - M_FIRST].texels != nullptr;
    // For lfont.py (only a disc with a log shows it): a header, then a line
    // a row, for the log keeps no long lines.
    for (int32_t i = 0; i < M_COUNT; i++) {
        LOG_INFO(
            "LFONT %d %d %d %d %d", game, i, font[i].w, font[i].h, font[i].y);
        for (int32_t y = 0; y < font[i].h; y++) {
            char row[40 * 4 + 1];
            for (int32_t x = 0; x < font[i].w; x++) {
                snprintf(row + x * 4, 5, "%04x", font[i].texels[y * font[i].w + x]);
            }
            LOG_INFO("LFONTROW %d %d %s", game, i, row);
        }
    }
}

static int32_t M_Advance(const M_GLYPH *const g)
{
    return g != nullptr && g->texels != nullptr ? g->w + M_GAP : M_SPACE;
}

static int32_t M_Length(const M_GLYPH *const font, const char *str)
{
    int32_t length = 0;
    for (; *str != '\0'; str++) {
        const int32_t i = *str - M_FIRST;
        length += M_Advance(i >= 0 && i < M_COUNT ? &font[i] : nullptr);
    }
    return length;
}

int32_t DC_LoadingFont_Scale(
    const int game, const int32_t width, const int32_t max_scale,
    const char *const line1, const char *const line2)
{
    if (game < 1 || game > M_GAMES || !m_Have[game - 1]) {
        return 1;
    }
    const int32_t length = MAX(
        M_Length(m_Fonts[game - 1], line1), M_Length(m_Fonts[game - 1], line2));
    return length * max_scale <= width ? max_scale : 1;
}

int32_t DC_LoadingFont_Draw(
    const int game, uint16_t *const dst, const int32_t stride,
    const int32_t width, const int32_t height, const char *str,
    const int32_t max_scale)
{
    if (game < 1 || game > M_GAMES || !m_Have[game - 1]) {
        return -1;
    }
    const M_GLYPH *const font = m_Fonts[game - 1];
    const int32_t scale = max_scale;
    // Glyphs sit relative to the capital A's top, as the game places them.
    const int32_t top = font['A' - M_FIRST].y;
    int32_t pen = 0;
    for (; *str != '\0'; str++) {
        const int32_t i = *str - M_FIRST;
        const M_GLYPH *const g =
            i >= 0 && i < M_COUNT ? &font[i] : nullptr;
        if (g == nullptr || g->texels == nullptr) {
            pen += M_SPACE * scale;
            continue;
        }
        const int32_t oy = (g->y - top) * scale;
        for (int32_t y = 0; y < g->h * scale; y++) {
            const int32_t dy = oy + y;
            if (dy < 0 || dy >= height) {
                continue;
            }
            for (int32_t x = 0; x < g->w * scale; x++) {
                if (pen + x >= width) {
                    break;
                }
                dst[dy * stride + pen + x] =
                    g->texels[(y / scale) * g->w + x / scale];
            }
        }
        pen += (g->w + M_GAP) * scale;
    }
    return pen;
}
