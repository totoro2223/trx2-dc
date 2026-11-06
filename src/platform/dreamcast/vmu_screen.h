#pragma once

#include <dc/maple.h>
#include <dc/vmu_fb.h>

// Prints text (lines split by '\n') in the VMU's 4x6 font, each line centred
// across the screen and the block centred between rows y and y + height.
void DC_VMU_PrintCentered(
    vmufb_t *fb, int y, int height, int line_spacing, const char *text);

// Sends a picture to a card's screen and remembers it, so that a card plugged
// in later gets it too.
void DC_VMU_Present(const vmufb_t *fb, maple_device_t *dev);

// Call every frame: a screen that has just been plugged in gets the last
// picture.
void DC_VMU_Poll(void);
