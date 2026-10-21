#pragma once

#include <stdint.h>

// Lets the player pick where the game starts, among the levels of the game
// on the disc (engine 2 or 3). Returns the level number to start in, or -1
// for the title.
int32_t DC_LevelSelect_Run(int32_t engine);
