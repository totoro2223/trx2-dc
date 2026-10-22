#include <trx/game/game_buf.h>

#include <trx/core/enum_map.h>
#include <trx/core/memory.h>
#include <trx/core/subsystem.h>

#include <stdio.h>

#if defined(__DREAMCAST__)
    #include <platform/dreamcast/dc_level_heap.h>
#endif

static MEMORY_ARENA_ALLOCATOR m_Allocator[GBUF_NUM_MALLOC_TYPES] = {};

static void M_Init(void)
{
    for (int32_t i = 0; i < GBUF_NUM_MALLOC_TYPES; i++) {
        m_Allocator[i].default_chunk_size = 2048;
    }
}

static void M_Shutdown(void)
{
    for (int32_t i = 0; i < GBUF_NUM_MALLOC_TYPES; i++) {
        Memory_ArenaFree(&m_Allocator[i]);
    }
}

void GameBuf_Reset(void)
{
    for (int32_t i = 0; i < GBUF_NUM_MALLOC_TYPES; i++) {
#if defined(__DREAMCAST__)
        // Kept chunks are not reused well by the next level and would hold the
        // old level's ~5 MiB while it loads; 16 MiB cannot afford that.
    #if defined(TRX_DC_WATCHDOG)
        int32_t chunks = 0;
        for (const MEMORY_ARENA_CHUNK *c = m_Allocator[i].first_chunk;
             c != nullptr; c = c->next) {
            chunks++;
        }
        if (chunks > 0) {
            printf("[gamebuf] %d: %d chunks freed\n", (int)i, (int)chunks);
        }
    #endif
        Memory_ArenaFree(&m_Allocator[i]);
#else
        Memory_ArenaReset(&m_Allocator[i]);
#endif
    }
}

void GameBuf_ResetSingle(const GAME_BUFFER buffer)
{
    Memory_ArenaReset(&m_Allocator[buffer]);
}

void *GameBuf_Alloc(const size_t alloc_size, const GAME_BUFFER buffer)
{
    const size_t aligned_size = Memory_Align(alloc_size);
#if defined(__DREAMCAST__)
    // The level's, even when an item is made in play.
    DC_LevelHeap_Enter();
    void *const result = Memory_ArenaAlloc(&m_Allocator[buffer], aligned_size);
    DC_LevelHeap_Leave();
    return result;
#else
    return Memory_ArenaAlloc(&m_Allocator[buffer], aligned_size);
#endif
}

REGISTER_BASE_SUBSYSTEM(.init = M_Init, .shutdown = M_Shutdown)
