#pragma once

#include <stdint.h>

// Lets the player pick where the game starts, among the levels of the game
// on the disc (engine 2 or 3). Returns the level number to start in, or -1
// for the title. mod names an expansion on the disc (tr1-ub, tr2-gm, tr3-la),
// whose levels are listed instead of the base game's; empty for none.
// It returns DC_LEVEL_SELECT_RESTART where the player chose another game or
// the expansion: the caller starts the disc over (dc_games.h holds the choice).
#define DC_LEVEL_SELECT_RESTART (-2)

int32_t DC_LevelSelect_Run(int32_t engine, const char *mod);
