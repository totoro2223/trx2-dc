#pragma once

#include <stdbool.h>
#include <stdint.h>

// The BIOS font at half its size, for the screens that show it at 320x240
// (the level select, the loading screen, the hang report): at its own size
// 26 letters fill a line, and a 640x480 text halved by the screen drops
// strokes. Each 2x2 block of a letter becomes a pixel, so that every stroke
// lands on whole pixels.
#define DC_BFONT_SMALL_WIDTH 6
#define DC_BFONT_SMALL_HEIGHT 12

// Draws str at dst, a 16 bit buffer stride pixels wide, in the colours of that
// buffer; without opaque, the pixels that are not the letters' stay as they
// are.
void DC_BFontSmall_DrawStr(
    uint16_t *dst, uint32_t stride, uint16_t fg, uint16_t bg, bool opaque,
    const char *str);
