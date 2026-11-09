// The render options on the memory card (see dc_settings.c).

#pragma once

#include <stdbool.h>

// Applies the options the memory card kept (restored into the saves directory
// with the saved games), once the engine has read its settings.
void DC_Settings_Load(const char *saves_dir);

// Writes them to the card if they changed since they were loaded or last
// written. Called as the main menu ends.
void DC_Settings_Save(void);

// The 16:9 choice kept on the card; it only takes effect on the next boot.
extern bool g_DC_WidePending;
