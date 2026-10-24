#include <trx/game/game_buf.h>

#include <trx/core/enum_map.h>
#include <trx/core/memory.h>
#include <trx/core/subsystem.h>

#include <stdio.h>

#if defined(__DREAMCAST__)
    #include <platform/dreamcast/dc_level_heap.h>

    #include <kos/mutex.h>
#endif

static MEMORY_ARENA_ALLOCATOR m_Allocator[GBUF_NUM_MALLOC_TYPES] = {};
#if defined(__DREAMCAST__)
// The injections are read on the thread pool, and two threads taking from one
// arena at once lost chunks from its list: they were never freed (about a
// megabyte a level), and a run of levels ran out of memory.
static mutex_t m_Lock = MUTEX_INITIALIZER;
static int32_t m_Made[GBUF_NUM_MALLOC_TYPES];
#endif

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
#if defined(__DREAMCAST__)
    mutex_lock(&m_Lock);
#endif
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
        if (chunks > 0 || m_Made[i] > 0) {
            printf(
                "[gamebuf] %d: %d chunks freed, %d made\n", (int)i,
                (int)chunks, (int)m_Made[i]);
        }
        m_Made[i] = 0;
    #endif
        Memory_ArenaFree(&m_Allocator[i]);
#else
        Memory_ArenaReset(&m_Allocator[i]);
#endif
    }
#if defined(__DREAMCAST__)
    mutex_unlock(&m_Lock);
#endif
}

#if defined(__DREAMCAST__)
// A diagnostic: the chunks of an arena that its list holds against the ones
// made for it.
void GameBuf_Check(const char *const tag, const GAME_BUFFER buffer)
{
    int32_t chunks = 0;
    for (const MEMORY_ARENA_CHUNK *c = m_Allocator[buffer].first_chunk;
         c != nullptr; c = c->next) {
        chunks++;
    }
    printf(
        "[gamebuf-check] %s: %d listed of %d made\n", tag, (int)chunks,
        (int)m_Made[buffer]);
}
#endif

void GameBuf_ResetSingle(const GAME_BUFFER buffer)
{
    Memory_ArenaReset(&m_Allocator[buffer]);
}

void *GameBuf_Alloc(const size_t alloc_size, const GAME_BUFFER buffer)
{
    const size_t aligned_size = Memory_Align(alloc_size);
#if defined(__DREAMCAST__)
    // The level's, even when an item is made in play.
    mutex_lock(&m_Lock);
    DC_LevelHeap_Enter();
    const MEMORY_ARENA_CHUNK *const before = m_Allocator[buffer].current_chunk;
    void *const result = Memory_ArenaAlloc(&m_Allocator[buffer], aligned_size);
    m_Made[buffer] += m_Allocator[buffer].current_chunk != before;
    DC_LevelHeap_Leave();
    mutex_unlock(&m_Lock);
    return result;
#else
    return Memory_ArenaAlloc(&m_Allocator[buffer], aligned_size);
#endif
}

REGISTER_BASE_SUBSYSTEM(.init = M_Init, .shutdown = M_Shutdown)
