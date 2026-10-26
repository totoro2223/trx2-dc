#include "dc_save_arena.h"

#include "dc_level_heap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// The first chunks are small so that they fit the holes the level's load
// leaves; later ones grow, as the arena has to hold a couple of megabytes and
// a chunk's own tags cost the region a block each.
#define M_CHUNK_MIN (64 * 1024)
// A chunk that does not fit the region falls back to newlib's heap, and the
// heap is what the next level loads into: 256K is the size the fuller levels
// still have a hole for, in one piece or halved.
#define M_CHUNK_MAX (256 * 1024)
// A save is dumped a section at a time, and the chunks are kept from one to
// the next (they are let go together at the end): the size the fullest section
// needs, not the sum of all of them.
#define M_CHUNK_COUNT 24

typedef struct M_CHUNK {
    size_t size;
    size_t used;
    // The chunk came from the level's region, not from newlib.
    size_t from_region;
    // So that data[] starts on a double's alignment.
    size_t padding;
    uint8_t data[];
} M_CHUNK;

static struct {
    bool active;
    // Begin and End nest: a save's tree can be read from inside another one.
    int32_t depth;
    // In the order they were taken, smallest first: a section fills them in
    // turn.
    M_CHUNK *chunks[M_CHUNK_COUNT];
    int32_t count;
    int32_t current;
    // What the next chunk to be taken would ask for.
    size_t chunk_size;
    size_t nodes;
    size_t bytes;
    size_t from_heap;
} m_Arena = {};

static void M_Report(const char *const what)
{
    printf(
        "[arena] %s: %d nodes, %dK in the region, %dK from the heap (%d "
        "chunks)\n",
        what, (int)m_Arena.nodes,
        (int)((m_Arena.bytes - m_Arena.from_heap) / 1024),
        (int)(m_Arena.from_heap / 1024), (int)m_Arena.count);
}

void DC_SaveArena_Begin(void)
{
    m_Arena.depth++;
    m_Arena.active = true;
}

bool DC_SaveArena_IsOpen(void)
{
    return m_Arena.active;
}

void DC_SaveArena_Reset(void)
{
    if (!m_Arena.active) {
        return;
    }
    // TEMP: what this section's tree took.
    M_Report("section");
    for (int32_t i = 0; i < m_Arena.count; i++) {
        m_Arena.chunks[i]->used = 0;
    }
    m_Arena.current = 0;
    m_Arena.nodes = 0;
    m_Arena.bytes = 0;
    m_Arena.from_heap = 0;
}

void DC_SaveArena_End(void)
{
    if (!m_Arena.active) {
        return;
    }
    if (--m_Arena.depth > 0) {
        return;
    }
    // TEMP: what the last section took.
    M_Report("end");
    for (int32_t i = 0; i < m_Arena.count; i++) {
        M_CHUNK *const chunk = m_Arena.chunks[i];
        if (chunk->from_region) {
            DC_LevelHeap_Free(chunk);
        } else {
            free(chunk);
        }
        m_Arena.chunks[i] = nullptr;
    }
    m_Arena.active = false;
    m_Arena.count = 0;
    m_Arena.current = 0;
    m_Arena.chunk_size = 0;
    m_Arena.nodes = 0;
    m_Arena.bytes = 0;
    m_Arena.from_heap = 0;
}

// One chunk, from the level's region when it has a hole that size, else from
// newlib.
static void *M_Take(const size_t size, bool *const from_region)
{
    void *const memory = DC_LevelHeap_Alloc(sizeof(M_CHUNK) + size);
    if (memory != nullptr) {
        *from_region = true;
        return memory;
    }
    *from_region = false;
    return malloc(sizeof(M_CHUNK) + size);
}

static bool M_Grow(const size_t need)
{
    if (m_Arena.count >= M_CHUNK_COUNT) {
        return false;
    }
    const size_t want = m_Arena.chunk_size < need ? need : m_Arena.chunk_size;
    // The level's region first, at the size asked for or at the nearest
    // smaller size that still holds a node: by the time the save is due, the
    // region is not one stretch (TR3's level 12 had a 1195K hole, but its
    // sections fill it as they are built). Newlib is likewise halved, as what
    // is left of it is fragmented too, and the nodes are small.
    uint8_t *memory = nullptr;
    size_t got = want;
    bool from_region = false;
    for (size_t size = want;; size /= 2) {
        if (size < need) {
            size = need;
        }
        memory = M_Take(size, &from_region);
        if (memory != nullptr) {
            got = size;
            break;
        }
        if (size == need) {
            break;
        }
    }
    if (memory == nullptr) {
        return false;
    }
    M_CHUNK *const chunk = (M_CHUNK *)memory;
    chunk->size = got;
    chunk->used = 0;
    chunk->from_region = from_region;
    m_Arena.chunks[m_Arena.count++] = chunk;
    m_Arena.current = m_Arena.count - 1;
    if (m_Arena.chunk_size < M_CHUNK_MAX) {
        m_Arena.chunk_size = m_Arena.chunk_size == 0
            ? M_CHUNK_MIN
            : m_Arena.chunk_size * 2;
    }
    return true;
}

void *DC_SaveArena_Alloc(const size_t size)
{
    if (!m_Arena.active) {
        return nullptr;
    }
    size_t need = (size + 7) & ~(size_t)7;
    if (need == 0) {
        need = 8;
    }
    if (m_Arena.count == 0) {
        if (!M_Grow(need)) {
            return nullptr;
        }
    }
    M_CHUNK *chunk = m_Arena.chunks[m_Arena.current];
    while (chunk->used + need > chunk->size) {
        if (m_Arena.current + 1 < m_Arena.count) {
            // The chunks after this one are the ones taken later, and larger.
            m_Arena.current++;
            chunk = m_Arena.chunks[m_Arena.current];
            continue;
        }
        if (!M_Grow(need)) {
            return nullptr;
        }
        chunk = m_Arena.chunks[m_Arena.current];
    }
    void *const result = chunk->data + chunk->used;
    chunk->used += need;
    m_Arena.nodes++;
    m_Arena.bytes += need;
    if (!chunk->from_region) {
        m_Arena.from_heap += need;
    }
    return result;
}

bool DC_SaveArena_Free(const void *const ptr)
{
    if (!m_Arena.active || ptr == nullptr) {
        return false;
    }
    const uint8_t *const at = ptr;
    for (int32_t i = 0; i < m_Arena.count; i++) {
        const M_CHUNK *const chunk = m_Arena.chunks[i];
        if (at >= chunk->data && at < chunk->data + chunk->size) {
            return true;
        }
    }
    return false;
}
