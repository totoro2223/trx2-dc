#pragma once

#include <stdbool.h>

// Saved games live on the RAM disk, where the engine reads and writes them
// as files like on any other platform; the first memory card found keeps a
// copy of each, so that they outlast the console being switched off.

// Copies every saved game on the memory card into the saves directory.
void DC_VMU_RestoreSaves(const char *saves_dir);

// Copies a saved game the engine has just written to the memory card;
// title describes it in the Dreamcast's file manager. False if there is no
// card or no room left on it.
bool DC_VMU_StoreSave(const char *path, const char *title);

// Removes a saved game's copy from the memory card.
void DC_VMU_DeleteSave(const char *path);
