// A bump arena for a savegame's JSON tree (json/base.c).
//
// The tree is tens of thousands of nodes of a few bytes each. In the level's
// region every one costs two tags and its own 8-byte rounding, and TR2's
// Barkhang had no room left for the save once the tree was built: 49672
// blocks, 6245K of 6591K. Here the nodes are put one after the other, and all
// of them are let go at once, when the BSON has been written. The save is built
// a section at a time, so the chunks are kept and refilled from one section to
// the next.
//
// DC_SaveArena_Alloc returns nullptr while no arena is open: the caller then
// allocates as usual.

#ifndef TRX_DC_SAVE_ARENA_H
#define TRX_DC_SAVE_ARENA_H

#include <stdbool.h>
#include <stddef.h>

void DC_SaveArena_Begin(void);
// Lets the section just built go, keeping the chunks for the next one.
void DC_SaveArena_Reset(void);
void DC_SaveArena_End(void);

// True between Begin and End: the savegame's tree is being built or read.
bool DC_SaveArena_IsOpen(void);

void *DC_SaveArena_Alloc(size_t size);

// True when the pointer came from the open arena: a no-op free.
bool DC_SaveArena_Free(const void *ptr);

#endif
