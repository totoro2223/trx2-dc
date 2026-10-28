#include "bfont_small.h"

#include <dc/biosfont.h>

void DC_BFontSmall_DrawStr(
    uint16_t *dst, const uint32_t stride, const uint16_t fg, const uint16_t bg,
    const bool opaque, const char *str)
{
    // One letter at its own size: 1 where the font has ink.
    static uint16_t big[BFONT_THIN_WIDTH * BFONT_HEIGHT];
    for (; *str != '\0'; str++, dst += DC_BFONT_SMALL_WIDTH) {
        bfont_draw_ex(
            big, BFONT_THIN_WIDTH, 1, 0, 16, true, (uint8_t)*str, false, false);
        for (int32_t y = 0; y < DC_BFONT_SMALL_HEIGHT; y++) {
            for (int32_t x = 0; x < DC_BFONT_SMALL_WIDTH; x++) {
                const uint16_t *const at =
                    &big[2 * y * BFONT_THIN_WIDTH + 2 * x];
                const int32_t ink = (at[0] != 0) + (at[1] != 0)
                    + (at[BFONT_THIN_WIDTH] != 0)
                    + (at[BFONT_THIN_WIDTH + 1] != 0);
                if (ink >= 2) {
                    dst[y * stride + x] = fg;
                } else if (opaque) {
                    dst[y * stride + x] = bg;
                }
            }
        }
    }
}
