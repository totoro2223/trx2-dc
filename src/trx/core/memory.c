#include <trx/core/memory.h>

#include <trx/core/utils.h>
#include <trx/debug.h>

#include <stdlib.h>
#include <string.h>

#if defined(__DREAMCAST__)
    #include <malloc.h>
    #include <platform/dreamcast/dc_level_heap.h>
#endif

static MEMORY_ARENA_CHUNK *M_ArenaAllocChunk(
    MEMORY_ARENA_ALLOCATOR *const allocator, const size_t size)
{
    const size_t new_chunk_size = MAX(allocator->default_chunk_size, size);
    MEMORY_ARENA_CHUNK *const new_chunk =
        Memory_Alloc(sizeof(MEMORY_ARENA_CHUNK) + new_chunk_size);
    new_chunk->memory = (char *)new_chunk + sizeof(MEMORY_ARENA_CHUNK);
    new_chunk->size = new_chunk_size;
    new_chunk->offset = 0;
    new_chunk->next = nullptr;
    return new_chunk;
}

size_t Memory_Align(const size_t size)
{
    return (size + 7) & ~7;
}

#if defined(__DREAMCAST__)
// The game's memory (platform/dreamcast/dc_level_heap.c).
void *DC_LevelHeap_Alloc(size_t size);
bool DC_LevelHeap_Owns(const void *ptr);
void *DC_LevelHeap_Realloc(void *ptr, size_t size);
void DC_LevelHeap_Free(void *ptr);

// The Dreamcast runs close to the end of its memory: before an allocation
// gives up, whatever can be built again later is let go and it is tried once
// more.
static bool (*m_LowMemoryHandler)(void) = nullptr;
static bool m_InLowMemoryHandler = false;

void Memory_SetLowMemoryHandler(bool (*const handler)(void))
{
    m_LowMemoryHandler = handler;
}

bool Memory_HandleLowMemory(void)
{
    if (m_LowMemoryHandler == nullptr || m_InLowMemoryHandler) {
        return false;
    }
    m_InLowMemoryHandler = true;
    const bool freed = m_LowMemoryHandler();
    m_InLowMemoryHandler = false;
    return freed;
}
#endif

void *Memory_Alloc(const size_t size)
{
#if defined(__DREAMCAST__)
    // From the game's region once there is one (dc_level_heap.c), else or
    // when it is full from the heap.
    void *result = DC_LevelHeap_Alloc(size);
    if (result == nullptr) {
        result = malloc(size);
    }
    if (result == nullptr && Memory_HandleLowMemory()) {
        result = DC_LevelHeap_Alloc(size);
        if (result == nullptr) {
            result = malloc(size);
        }
    }
#else
    void *result = malloc(size);
#endif
    ASSERT(result != nullptr);
    memset(result, 0, size);
    return result;
}

void *Memory_Realloc(void *const memory, const size_t size)
{
#if defined(__DREAMCAST__)
    if (memory == nullptr) {
        return Memory_Alloc(size);
    }
    if (DC_LevelHeap_Owns(memory)) {
        void *const result = DC_LevelHeap_Realloc(memory, size);
        ASSERT(result != nullptr);
        return result;
    }
#endif
    void *result = realloc(memory, size);
#if defined(__DREAMCAST__)
    // realloc returned nullptr, so the block is still ours; GCC 13 reads the
    // retries below as uses of a block it believes was given back.
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wuse-after-free"
    // The fuller levels leave newlib's top pinned while the level's region has
    // megabytes free (TR3's mines: 4.6 MB); a big block newlib cannot grow is
    // moved there whole, and lives in the region from then on.
    if (result == nullptr && size >= 128 * 1024) {
        void *const moved = DC_LevelHeap_Alloc(size);
        if (moved != nullptr) {
            memcpy(moved, memory, MIN(size, malloc_usable_size(memory)));
            free(memory);
            return moved;
        }
    }
    if (result == nullptr && size > 0 && Memory_HandleLowMemory()) {
        result = realloc(memory, size);
    }
    #pragma GCC diagnostic pop
#endif
    ASSERT(result != nullptr);
    return result;
}

void Memory_Free(void *const memory)
{
#if defined(__DREAMCAST__)
    if (DC_LevelHeap_Owns(memory)) {
        DC_LevelHeap_Free(memory);
        return;
    }
#endif
    if (memory != nullptr) {
        free(memory);
    }
}

void Memory_FreePointer(void *arg)
{
    ASSERT(arg != nullptr);
    void *memory;
    memcpy(&memory, arg, sizeof(void *));
    memcpy(arg, &(void *) { nullptr }, sizeof(void *));
    Memory_Free(memory);
}

void *Memory_Dup(const void *const buffer, const size_t size)
{
    ASSERT(buffer != nullptr);
    char *memory = Memory_Alloc(size);
    memcpy(memory, buffer, size);
    return memory;
}

char *Memory_DupStr(const char *const string)
{
    if (string == nullptr) {
        return nullptr;
    }
    char *memory = Memory_Alloc(strlen(string) + 1);
    strcpy(memory, string);
    return memory;
}

void *Memory_ArenaAlloc(
    MEMORY_ARENA_ALLOCATOR *const allocator, const size_t size)
{
    // Ensure a default chunk size is set.
    if (allocator->default_chunk_size == 0) {
        allocator->default_chunk_size = 1024 * 4; // default to 4K
    }

    // Find first chunk that has enough space.
    MEMORY_ARENA_CHUNK *chunk = allocator->current_chunk;
    while (chunk != nullptr && chunk->offset + size > chunk->size) {
        chunk = chunk->next;
    }

    // If no chunk satisfies this criteria, append a new chunk.
    if (chunk == nullptr) {
        chunk = M_ArenaAllocChunk(allocator, size);
        if (allocator->current_chunk != nullptr) {
            chunk->next = allocator->current_chunk->next;
            allocator->current_chunk->next = chunk;
        }
        allocator->current_chunk = chunk;
        if (allocator->first_chunk == nullptr) {
            allocator->first_chunk = chunk;
        }
    }

    ASSERT(chunk != nullptr);

    // Allocate from the current chunk.
    void *const result = (char *)chunk->memory + chunk->offset;
    chunk->offset += size;
    return result;
}

void Memory_ArenaReset(MEMORY_ARENA_ALLOCATOR *const allocator)
{
    MEMORY_ARENA_CHUNK *chunk = allocator->first_chunk;
    while (chunk != nullptr) {
        memset(chunk->memory, 0, chunk->size);
        chunk->offset = 0;
        chunk = chunk->next;
    }
    allocator->current_chunk = allocator->first_chunk;
}

void Memory_ArenaFree(MEMORY_ARENA_ALLOCATOR *const allocator)
{
    MEMORY_ARENA_CHUNK *chunk = allocator->first_chunk;
    while (chunk != nullptr) {
        MEMORY_ARENA_CHUNK *const next = chunk->next;
        Memory_Free(chunk);
        chunk = next;
    }
    allocator->first_chunk = nullptr;
    allocator->current_chunk = nullptr;
}
