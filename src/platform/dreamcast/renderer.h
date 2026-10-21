#pragma once

// Dreamcast renderer entry points the shared engine code calls in place of
// its OpenGL code (see platform/dreamcast/render).

#include <stddef.h>
#include <stdint.h>

// Texture pages: 256x256 texels each, 16 bits a texel, in the format given
// per page (see trx/game/level/texture_pages_16.h). They are kept in video
// memory only: each goes there from main memory once it is whole
// (DC_Renderer_StoreTexturePage, again for a page that changes), in VQ when
// the disc has it so (tools/dreamcast/texvq.c), and DC_Renderer_GetTexel
// reads it back as drawn. Reserving drops the pages of the previous level;
// committing ends the level's loading.
bool DC_Renderer_ReserveTexturePages(int32_t count);
void DC_Renderer_StoreTexturePage(
    int32_t page, const uint16_t *texels, uint8_t format);
uint16_t DC_Renderer_GetTexel(int32_t page, int32_t x, int32_t y);
// Whether any texel from (x0, y0) to (x1, y1) inclusive is not wholly solid.
bool DC_Renderer_HasTransparency(
    int32_t page, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
void DC_Renderer_CommitTexturePages(void);
void DC_Renderer_FreeTexturePages(void);
int32_t DC_Renderer_GetTexturePageCount(void);
// The screen is 320x240 (Experimental 60): the interface is drawn bigger and
// unfiltered, to stay readable at half the lines.
bool DC_Renderer_IsLowRes(void);
