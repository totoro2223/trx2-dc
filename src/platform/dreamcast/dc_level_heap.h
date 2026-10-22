#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The game's memory, the level's data apart as the PlayStation kept it
// (see dc_level_heap.c).

// Between these, Memory_Alloc takes from the region: what a level loads and
// builds. They nest.
void DC_LevelHeap_Enter(void);
void DC_LevelHeap_Leave(void);

// A block from the region, the level's end of it between Enter and Leave;
// nullptr before the first level load or when the region is full.
void *DC_LevelHeap_Alloc(size_t size);
bool DC_LevelHeap_Owns(const void *ptr);
// For a block the region owns; never fails short of all memory gone.
void *DC_LevelHeap_Realloc(void *ptr, size_t size);
void DC_LevelHeap_Free(void *ptr);

typedef struct {
    int32_t size_kb;
    int32_t used_kb;
    // The most in use at once since boot.
    int32_t peak_kb;
    int32_t blocks;
    int32_t largest_free_kb;
    // Blocks that went to the heap for want of room in the region.
    int32_t spilled;
} DC_LEVEL_HEAP_STATS;

void DC_LevelHeap_GetStats(DC_LEVEL_HEAP_STATS *out);
