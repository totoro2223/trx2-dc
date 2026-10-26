#pragma once

#include <stddef.h>
#include <stdint.h>

// Tools for finding what the real console would show and the emulator does
// not: its RAM starts as garbage (Flycast's starts as zeros), and a hang
// leaves no trace without a serial cable.

// The stages the game goes through from the power-on to its first frame. Each
// sets the colour of the screen's border (red when the game starts, on
// through the rainbow, to black once it is running), so that a console that
// hangs can be told by it where it did.
typedef enum {
    DC_CRUMB_BOOT = 1,
    DC_CRUMB_GAMES,
    DC_CRUMB_SELECTOR,
    DC_CRUMB_ENGINE,
    DC_CRUMB_LOADING,
    DC_CRUMB_LOADED,
    DC_CRUMB_FIRST_FRAME,
    DC_CRUMB_RUNNING,
} DC_CRUMB;

// Notes that the game reached a stage: the border colour, the serial line
// "[crumb] ...", and a place in the ring the hang report prints.
void DC_Crumb(DC_CRUMB stage, const char *text);

// The game is alive: called as each frame goes out (also from the loading
// screen's frames). With none for DC_STALL_SECONDS, the screen shows the
// stage the game stopped at and a file called TRXLOG goes to the memory card.
void DC_Crumb_Frame(void);

// Only a sign of life, for the frames that are not the game's own (the loading
// screen's).
void DC_Crumb_Beat(void);

// Starts the thread that watches for that stall.
void DC_Crumb_StartWatch(void);

// Seconds without a frame after which the game is taken to be hung: a level
// loads in well under that, even from a slow disc.
#define DC_STALL_SECONDS 20

// With the strict build (-Ddc_strict=true): the free heap is filled with
// DC_STRICT_FRESH at the start, every block malloc gives with it, and every
// block free takes back with DC_STRICT_FREED, so that what reads memory it
// never wrote, or after freeing it, reads garbage here too.
#define DC_STRICT_FRESH 0xCD
#define DC_STRICT_FREED 0xDD

void DC_Strict_PoisonRAM(void);
void DC_Strict_FillNew(void *ptr, size_t size);
void DC_Strict_FillFreed(void *ptr);
