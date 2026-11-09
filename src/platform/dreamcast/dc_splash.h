#pragma once

#include <stdint.h>

// The credits screen, once per start of the disc: later calls (the player
// switching games) do nothing. engine is the game's number (1..3).
void DC_Splash_Show(int32_t engine);
