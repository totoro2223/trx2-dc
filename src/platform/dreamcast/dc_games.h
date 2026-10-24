// A disc that holds all three games (make_disc_all.sh): each in a folder of
// its own (g1, g2, g3) under the disc's root, with the expansions that came
// with the remastered data. The game to run is picked at boot and changed
// from the main menu by starting the disc over: the console reboots, and the
// choice is left in a few words at the top of RAM, which the reboot does not
// clear.

#pragma once

#include <stdbool.h>
#include <stddef.h>

// Where the running game's files are: "/cd", or "/cd/g2" on a disc of several.
extern char g_DC_Root[40];

// Reads the disc's list of games and the choice left before the last reboot.
// Runs first at start-up, for everything that follows opens files by the root.
void DC_Games_Init(void);

// Writes "<root>/<name>" into out.
void DC_Games_Path(char *out, size_t size, const char *name);

// The game running (1, 2 or 3) and the games before and after it on the disc
// (0 for none: a disc of one game has neither).
int DC_Games_CurrentId(void);
int DC_Games_PrevId(void);
int DC_Games_NextId(void);

// The expansion the next boot runs ("tr1-ub", "tr2-gm" or "tr3-la"), or "".
const char *DC_Games_Expansion(void);

// Whether the running game's folder holds its expansion.
bool DC_Games_ExpansionAvailable(void);

// Leave a choice for the next boot: the game before (-1) or after (+1) this
// one, or its expansion. The caller ends the game; the console starts over.
bool DC_Games_Choose(int delta);
bool DC_Games_ChooseExpansion(void);

// The game that is running stays the choice across the reboot that follows
// leaving it (the list of levels of the same game comes back).
void DC_Games_Remember(void);

// A crash is about to start the disc over (see dc_main.c): leaves the game
// where it was and counts the restart. False after three in a row.
bool DC_Games_Recover(void);
