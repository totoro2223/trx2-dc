#pragma once

#include <stddef.h>
#include <stdint.h>

// Lua's allocator (lua_Alloc), which keeps Lua's blocks in a region of their
// own, out of the shared heap (see dc_lua_alloc.c).
void *DC_LuaAlloc(void *ud, void *ptr, size_t osize, size_t nsize);

typedef struct {
    // Handed out of the small blocks' part, and in use in the big blocks'.
    int32_t small_kb;
    int32_t big_kb;
    // Blocks that went to the heap for want of room in the region.
    int32_t spilled;
} DC_LUA_ALLOC_STATS;

void DC_LuaAlloc_GetStats(DC_LUA_ALLOC_STATS *out);
