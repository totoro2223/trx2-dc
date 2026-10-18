#pragma once

// Dreamcast renderer entry points the shared engine code calls in place of
// its OpenGL code (see platform/dreamcast/render).

#include <stddef.h>
#include <stdint.h>

// Texture pages: 256x256 texels each, 16 bits a texel, in the format given
// per page (see trx/game/level/texture_pages_16.h). They are kept in video
// memory only: reserved as a level starts loading, filled through
// DC_Renderer_WriteTexels (video memory takes no byte writes, so plain
// memcpy will not do), and made ready to draw once the level is complete.
// Reserving drops the pages of the previous level.
uint16_t *DC_Renderer_ReserveTexturePages(int32_t count);
void DC_Renderer_WriteTexels(uint16_t *dst, const uint16_t *src, size_t count);
void DC_Renderer_CommitTexturePages(int32_t count, const uint8_t *formats);
void DC_Renderer_FreeTexturePages(void);
int32_t DC_Renderer_GetTexturePageCount(void);
