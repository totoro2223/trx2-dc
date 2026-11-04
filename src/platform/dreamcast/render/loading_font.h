#pragma once

// The loading screen's text in the running game's own font. The font is the
// level's sprites, which are not there while the first level loads, so the
// glyphs are copied out of the texture pages once a level has loaded and the
// next loads use them; until then the BIOS font stands in.

#include <stdbool.h>
#include <stdint.h>

// Reads the font the disc carries (lfont.bin), if it has one. Run before
// the disc reads are watched: it reads a file.
void DC_LoadingFont_Load(int game);
// Copies the font's glyphs from the loaded level for game (1..3).
void DC_LoadingFont_Capture(int game);
// Writes str into dst (ARGB4444, stride in texels), at max_scale times the
// glyphs' size (1 if the line would not fit), clipped to width x height.
// Returns the width written, or -1 if the game has no font yet.
int32_t DC_LoadingFont_Draw(
    int game, uint16_t *dst, int32_t stride, int32_t width, int32_t height,
    const char *str, int32_t max_scale);
// The largest scale, up to max_scale, at which every line fits width (so
// that the lines are drawn alike); 1 if the game has no font.
int32_t DC_LoadingFont_Scale(
    int game, int32_t width, int32_t max_scale, const char *line1,
    const char *line2);
