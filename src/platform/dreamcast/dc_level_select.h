#pragma once

#include <stdint.h>

// Lets the player pick where the game starts. Returns the TR2 level number
// to start in, or -1 for the title.
int32_t DC_LevelSelect_Run(void);
