#pragma once

// Dreamcast loading screen: while a level loads, how much of the files it
// reads has come off the disc, as a percentage, and the file being read.

#include <stddef.h>
#include <stdint.h>

// Starts the screen for a load that reads about total_bytes from the disc,
// over the level's picture (its gameflow's loading screen, or nullptr).
void DC_Loading_Begin(uint64_t total_bytes, const char *picture);
// Shows a step that reads nothing, in place of a file's name.
void DC_Loading_SetStep(const char *step);
void DC_Loading_End(void);
// A notice on the loading screen, held for hold_ms: the quick save's.
void DC_Loading_Notice(const char *title, const char *text, int32_t hold_ms);
// The passport's message box, in the game (nolua_passport.c): the text for
// hold ticks, or until the next one if hold is 0. The text must outlive it.
void DC_QuickSave_Say(const char *text, int32_t hold);
// What the quick save is told of the card, once the save has been written.
void DC_Loading_SaveDone(bool saved);
// The disc's "DC build N" (make_disc.sh's VERSION.TXT).
const char *DC_Loading_GetVersion(void);
