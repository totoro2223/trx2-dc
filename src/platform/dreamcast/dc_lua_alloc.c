// Dreamcast platform layer: Lua's memory, in a region of its own.
//
// Lua takes and lets go of blocks all the time, and in the shared heap they
// end up all over it: what is still alive when a level unloads sits in the
// middle of the room the next level loads into, and splits it until a big
// block no longer fits (a second loaded game ran out of memory so). Here
// Lua's blocks stay in one region taken once at boot, and spill into the
// heap only when it is full.
//
// The region has two parts. Blocks up to M_SMALL_MAX bytes, which are most
// of Lua's, come from the first, a free list per size: Lua tells a block's
// size when it frees or resizes it, so they carry no header. The rest come
// from the second, where each block carries its size before and after it
// (boundary tags), so that a block let go joins the free ones either side.
// Which part a block is in tells how it was taken.
//
// Only the game thread runs Lua, so none of it needs a lock (newlib's malloc
// takes one on every call, which KOS makes a mutex and a look for threads to
// wake: a fifth of the inventory's time).

#include "dc_lua_alloc.h"

#include <trx/game/output/sources/objects.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define M_SMALL_SIZE (1280 * 1024)
// The graphics dialog, the largest Lua the Dreamcast opens, peaks at ~950K
// here (DC build 149); past this, blocks spill into the heap.
#define M_BIG_SIZE (1024 * 1024)
#define M_GRAIN 8
#define M_SMALL_MAX 256
#define M_SMALL_CLASSES (M_SMALL_MAX / M_GRAIN)

// A big block: its size in bytes with M_USED, its payload, and its size
// again in its last word. A free one holds the free list's links in its
// payload.
#define M_USED 1u
#define M_TAG 4
#define M_BIG_MIN 16
// The free blocks, by the power of two below their size.
#define M_BINS 22

typedef struct M_FREE {
    struct M_FREE *next;
    struct M_FREE *prev;
} M_FREE;

static struct {
    uint8_t *base;
    uint8_t *end;
    struct {
        uint8_t *top;
        uint8_t *end;
        void *free[M_SMALL_CLASSES];
    } small;
    struct {
        uint8_t *start;
        uint8_t *end;
        M_FREE *bins[M_BINS];
        size_t used;
    } big;
    int32_t spilled;
} m_Region = {};

static inline uint32_t *M_Head(void *const payload)
{
    return (uint32_t *)((uint8_t *)payload - M_TAG);
}

static inline size_t M_SizeAt(const uint8_t *const block)
{
    return *(const uint32_t *)block & ~M_USED;
}

static inline bool M_UsedAt(const uint8_t *const block)
{
    return (*(const uint32_t *)block & M_USED) != 0;
}

static void M_SetTags(uint8_t *const block, const size_t size, const bool used)
{
    const uint32_t tag = (uint32_t)size | (used ? M_USED : 0);
    *(uint32_t *)block = tag;
    *(uint32_t *)(block + size - M_TAG) = tag;
}

static int32_t M_Bin(size_t size)
{
    int32_t bin = 0;
    while (size > M_BIG_MIN && bin < M_BINS - 1) {
        size >>= 1;
        bin++;
    }
    return bin;
}

static void M_BinInsert(uint8_t *const block)
{
    M_FREE *const node = (M_FREE *)(block + M_TAG);
    M_FREE **const head = &m_Region.big.bins[M_Bin(M_SizeAt(block))];
    node->prev = nullptr;
    node->next = *head;
    if (*head != nullptr) {
        (*head)->prev = node;
    }
    *head = node;
}

static void M_BinRemove(uint8_t *const block)
{
    M_FREE *const node = (M_FREE *)(block + M_TAG);
    if (node->prev != nullptr) {
        node->prev->next = node->next;
    } else {
        m_Region.big.bins[M_Bin(M_SizeAt(block))] = node->next;
    }
    if (node->next != nullptr) {
        node->next->prev = node->prev;
    }
}

static void M_Init(void)
{
    m_Region.base = malloc(M_SMALL_SIZE + M_BIG_SIZE);
    if (m_Region.base == nullptr) {
        return;
    }
    m_Region.end = m_Region.base + M_SMALL_SIZE + M_BIG_SIZE;
    m_Region.small.top = m_Region.base;
    m_Region.small.end = m_Region.base + M_SMALL_SIZE;

    // A used tag at either end, so that no block looks past the part for
    // free ones to join; blocks start 4 bytes off the grain, so that their
    // payloads lie on it.
    uint8_t *const start = m_Region.small.end;
    *(uint32_t *)start = M_USED;
    *(uint32_t *)(m_Region.end - M_TAG) = M_USED;
    uint8_t *const first = start + M_TAG;
    m_Region.big.start = first;
    m_Region.big.end = m_Region.end - M_TAG;
    M_SetTags(first, (size_t)(m_Region.big.end - first), false);
    M_BinInsert(first);
}

static void *M_SmallAlloc(const size_t size)
{
    const int32_t c = (int32_t)((size - 1) / M_GRAIN);
    void *const block = m_Region.small.free[c];
    if (block != nullptr) {
        m_Region.small.free[c] = *(void **)block;
        return block;
    }
    const size_t bytes = (size_t)(c + 1) * M_GRAIN;
    if ((size_t)(m_Region.small.end - m_Region.small.top) < bytes) {
        return nullptr;
    }
    void *const fresh = m_Region.small.top;
    m_Region.small.top += bytes;
    return fresh;
}

static void M_SmallFree(void *const ptr, const size_t size)
{
    const int32_t c = (int32_t)((size - 1) / M_GRAIN);
    *(void **)ptr = m_Region.small.free[c];
    m_Region.small.free[c] = ptr;
}

static void *M_BigAlloc(const size_t size)
{
    size_t need = (size + 2 * M_TAG + M_GRAIN - 1) & ~(size_t)(M_GRAIN - 1);
    if (need < M_BIG_MIN) {
        need = M_BIG_MIN;
    }
    // The first that fits, from the bin of its size up; a bin above holds
    // only blocks that fit.
    for (int32_t bin = M_Bin(need); bin < M_BINS; bin++) {
        for (M_FREE *node = m_Region.big.bins[bin]; node != nullptr;
             node = node->next) {
            uint8_t *const block = (uint8_t *)node - M_TAG;
            const size_t have = M_SizeAt(block);
            if (have < need) {
                continue;
            }
            M_BinRemove(block);
            if (have - need >= M_BIG_MIN) {
                M_SetTags(block + need, have - need, false);
                M_BinInsert(block + need);
            } else {
                need = have;
            }
            M_SetTags(block, need, true);
            m_Region.big.used += need;
            return block + M_TAG;
        }
    }
    return nullptr;
}

static void M_BigFree(void *const ptr)
{
    uint8_t *block = (uint8_t *)M_Head(ptr);
    size_t size = M_SizeAt(block);
    m_Region.big.used -= size;
    uint8_t *const next = block + size;
    if (!M_UsedAt(next)) {
        M_BinRemove(next);
        size += M_SizeAt(next);
    }
    const uint32_t prev_tag = *(const uint32_t *)(block - M_TAG);
    if ((prev_tag & M_USED) == 0) {
        uint8_t *const prev = block - (prev_tag & ~M_USED);
        M_BinRemove(prev);
        size += M_SizeAt(prev);
        block = prev;
    }
    M_SetTags(block, size, false);
    M_BinInsert(block);
}

// How many bytes a big block's payload holds.
static size_t M_BigCapacity(void *const ptr)
{
    return M_SizeAt((const uint8_t *)M_Head(ptr)) - 2 * M_TAG;
}

static bool M_InSmall(const void *const ptr)
{
    return (const uint8_t *)ptr >= m_Region.base
        && (const uint8_t *)ptr < m_Region.small.end;
}

static bool M_InBig(const void *const ptr)
{
    return (const uint8_t *)ptr >= m_Region.big.start
        && (const uint8_t *)ptr < m_Region.big.end;
}

// malloc's, which before failing lets go of the object meshes not drawn this
// frame (Lua collects its own garbage then by itself).
static void *M_HeapRealloc(void *const ptr, const size_t size)
{
    void *result = realloc(ptr, size);
    if (result == nullptr && OutputSource_Objects_EvictUnused(1) > 0) {
        result = realloc(ptr, size);
    }
    return result;
}

static void *M_Take(const size_t size)
{
    if (m_Region.base == nullptr) {
        M_Init();
    }
    if (m_Region.base != nullptr) {
        void *const block =
            size <= M_SMALL_MAX ? M_SmallAlloc(size) : nullptr;
        if (block != nullptr) {
            return block;
        }
        void *const big = M_BigAlloc(size);
        if (big != nullptr) {
            return big;
        }
    }
    m_Region.spilled++;
    return M_HeapRealloc(nullptr, size);
}

static void M_Give(void *const ptr, const size_t size)
{
    if (M_InSmall(ptr)) {
        M_SmallFree(ptr, size);
    } else if (M_InBig(ptr)) {
        M_BigFree(ptr);
    } else {
        free(ptr);
    }
}

void *DC_LuaAlloc(
    void *const ud, void *const ptr, const size_t osize, const size_t nsize)
{
    if (nsize == 0) {
        if (ptr != nullptr) {
            M_Give(ptr, osize);
        }
        return nullptr;
    }
    if (ptr == nullptr) {
        return M_Take(nsize);
    }

    // Kept where it is when it still fits: a small block of the same size
    // class, or a big block not to be left mostly empty.
    if (M_InSmall(ptr) && nsize <= M_SMALL_MAX
        && (nsize - 1) / M_GRAIN == (osize - 1) / M_GRAIN) {
        return ptr;
    }
    if (M_InBig(ptr)) {
        const size_t capacity = M_BigCapacity(ptr);
        if (nsize <= capacity && nsize > M_SMALL_MAX && nsize * 2 >= capacity) {
            return ptr;
        }
    }
    if (!M_InSmall(ptr) && !M_InBig(ptr)) {
        // A block from before the region, or one that spilled: back into
        // the region if it now fits, else resized in the heap.
        void *const block = m_Region.base != nullptr
            ? (nsize <= M_SMALL_MAX ? M_SmallAlloc(nsize) : M_BigAlloc(nsize))
            : nullptr;
        if (block == nullptr) {
            return M_HeapRealloc(ptr, nsize);
        }
        memcpy(block, ptr, osize < nsize ? osize : nsize);
        free(ptr);
        return block;
    }

    void *const block = M_Take(nsize);
    if (block == nullptr) {
        if (nsize < osize) {
            // Shrinking may not fail, and the block is big enough as it is.
            // (A small one goes back by its new size, a little of it lost.)
            return ptr;
        }
        return nullptr;
    }
    memcpy(block, ptr, osize < nsize ? osize : nsize);
    M_Give(ptr, osize);
    return block;
}

void DC_LuaAlloc_GetStats(DC_LUA_ALLOC_STATS *const out)
{
    out->small_kb =
        (int32_t)((m_Region.small.top - m_Region.base) / 1024);
    out->big_kb = (int32_t)(m_Region.big.used / 1024);
    out->spilled = m_Region.spilled;
}
