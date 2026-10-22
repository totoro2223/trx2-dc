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
// The disc's "DC build N" (make_disc.sh's VERSION.TXT).
const char *DC_Loading_GetVersion(void);
