#pragma once

#include <stdbool.h>
#include <stddef.h>

// Saved games live on the RAM disk, where the engine reads and writes them
// as files like on any other platform; the first memory card found keeps a
// copy of each, so that they outlast the console being switched off.

// Copies every saved game on the memory card into the saves directory.
void DC_VMU_RestoreSaves(const char *saves_dir);

// Shows the save icon's "TR" on the screen of every VMU plugged in, at boot
// and on each visit to the main menu.
void DC_VMU_DrawLogo(void);

// Copies a saved game the engine has just written to the memory card;
// title describes it in the Dreamcast's file manager. The card's screen says
// it is saving, then how it went, and the card beeps when it is done. False if
// there is no card or no room left on it.
bool DC_VMU_StoreSave(const char *path, const char *title);

// The same for a small file with no icon, which takes a single block of the
// card (the render options).
bool DC_VMU_StoreSmall(const char *path, const char *title);

// How the last DC_VMU_StoreSave went: 0 nothing yet, 1 written, 2 the card
// refused it (no room, or it failed), 3 no card in the console.
int DC_VMU_LastResult(void);

// Removes a saved game's copy from the memory card.
void DC_VMU_DeleteSave(const char *path);

// The loading percentage on the VMU's screen (loading.c); only a change is
// sent. -1 forgets the last one, for the next load.
void DC_VMU_ShowLoading(int percent);

// The VMU's screen while a level plays (vmu_hud.c): call once per logic
// frame. L + R + B goes from the logo to the level's statistics to the secret
// finder (an arrow and beeps) and back.
void DC_VMU_Hud_Update(void);

// Leaves what a save wrote on the screen for a few seconds.
void DC_VMU_Hud_Hold(void);

// Whether L + R + B was pressed since last asked (sdl_compat.c).
bool DC_Input_TakeVMUMode(void);

// Writes a text file to the memory card (the hang report of dc_strict.c).
// False if there is no card or no room left on it.
bool DC_VMU_WriteLog(const char *card_name, const char *text);
