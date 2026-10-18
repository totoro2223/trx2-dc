#pragma once

// Texture pages as the Dreamcast holds them: 16 bits a texel, in the formats
// its PowerVR reads directly, instead of the 8-bit and 32-bit copies kept on
// the desktop. TR2 and TR3 already store their pages as ARGB1555; pages with
// soft edges (such as the injected fonts) keep them as ARGB4444.

#if defined(__DREAMCAST__)

    #include <trx/core/colors.h>
    #include <trx/game/output/const.h>

    #include <stdint.h>

typedef enum {
    TEXTURE_PAGE_ARGB1555 = 0,
    TEXTURE_PAGE_ARGB4444 = 1,
} TEXTURE_PAGE_FORMAT;

static inline uint16_t TexturePage16_ToARGB1555(const RGBA_8888 c)
{
    return (c.a >= 128 ? 0x8000 : 0) | ((c.r >> 3) << 10) | ((c.g >> 3) << 5)
        | (c.b >> 3);
}

static inline uint16_t TexturePage16_ToARGB4444(const RGBA_8888 c)
{
    return ((c.a >> 4) << 12) | ((c.r >> 4) << 8) | ((c.g >> 4) << 4)
        | (c.b >> 4);
}

static inline uint16_t TexturePage16_Encode(
    const TEXTURE_PAGE_FORMAT format, const RGBA_8888 c)
{
    return format == TEXTURE_PAGE_ARGB4444 ? TexturePage16_ToARGB4444(c)
                                           : TexturePage16_ToARGB1555(c);
}

// One bit of alpha does for pages whose texels are either solid or empty.
static inline TEXTURE_PAGE_FORMAT TexturePage16_ChooseFormat(
    const RGBA_8888 *const texels, const int32_t count)
{
    for (int32_t i = 0; i < count; i++) {
        if (texels[i].a != 0 && texels[i].a != 255) {
            return TEXTURE_PAGE_ARGB4444;
        }
    }
    return TEXTURE_PAGE_ARGB1555;
}

#endif
