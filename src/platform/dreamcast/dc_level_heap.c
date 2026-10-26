// Dreamcast platform layer: the game's memory, in one region with the
// level's data at one end and the rest at the other.
//
// The PlayStation's Tomb Raider took one block for the level at boot and
// emptied it whole at each load, so a level always loaded into one clean
// span. Here everything shared newlib's heap instead: what outlives a level
// (strings, settings, the save's buffers) sat in the middle of the room the
// next level loaded into, and after a few loads a big block no longer fit.
// A region of its own for the level did not fit either: 16 MiB has no room
// to split in two fixed parts (DC build 265).
//
// So the region takes, at the first level load, all the heap has but for
// what newlib's own users need (KOS, Lua's spills), and Memory_Alloc takes
// from it from then on: what a level loads and builds (between
// DC_LevelHeap_Enter and Leave: Level_Initialise, the game buffer, the object
// meshes built as they are first drawn) from its foot, the rest from its
// head. The two grow towards each other and never mix, so when a level
// unloads its end is one clean span again, but for what was allocated there
// and kept on purpose, which the next load reports.
//
// Each block carries its size before and after it (boundary tags), so a
// block let go joins the free ones either side, as in dc_lua_alloc.c.
// A lock guards it: a level's injections are read on the thread pool.

#include "dc_level_heap.h"

#include <kos/mutex.h>

// A diagnostic, built in with -DTRX_DC_HEAP_CENSUS: who holds what the region
// keeps between levels (tools/dreamcast/live_census.py reads the log).
#if defined(TRX_DC_HEAP_CENSUS)
    #define DC_LEVELHEAP_TRACE 1
#else
    #define DC_LEVELHEAP_TRACE 0
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// What newlib's heap keeps for its own users: KOS's file buffers, Lua's
// spills, the PowerVR's scratch. Half a megabyte was tried for the save's sake
// (DC build 791) and a level load died for the lack of it: a megabyte is what a
// load needs at its peak.
#define M_HEAP_RESERVE (1024 * 1024)
#define M_PROBE_MAX (12 * 1024 * 1024)
#define M_PROBE_STEP (64 * 1024)
#define M_GRAIN 8
#define M_USED 1u
#define M_TAG 4
#define M_MIN 16
// A shrunk block gives its tail back past this.
#define M_SPLIT_MIN 1024
#define M_BINS 24
// The most a block that outlives a level may be to stay out of the region.
#define M_SMALL_MAX (16 * 1024)
// Small blocks asked for while a level loads go to a pool of their own at the
// region's top, walled off by a block never freed: a few of them outlive the
// level (the rooms' overlap map, made at the end of the load and freed by the
// next level's; the thread pool), and anywhere else they would sit where the
// load left them, splitting the span the next level needs (DC build 311:
// TR1's largest free span fell from 6.6 to 4.4 MB over a 94-byte block at
// 2.1 MB; TR3's to 2.4 MB; at the head among the rest, build 316, the same
// block stayed 0.8-1.1 MB under the top, below the load's other small
// blocks). A pool that fills spills into the head side as before; the ones
// that survive are all under 256 bytes, and a level loaded holds ~100 KiB of
// blocks that small (TR3's Jungle, 1088 of them), and a load twice that: a
// pool of 256 K overflowed each load, and what spilled under the wall put the
// next survivor 90 K lower every load (DC build 320).
#define M_LEVEL_SMALL_MAX 256
#define M_POOL_SIZE (384 * 1024)

// Where a block goes: the level's data from the foot, what outlives a level
// from the head, the level's small blocks to the pool.
typedef enum {
    M_LEVEL,
    M_HEAD,
    M_POOL,
} M_KIND;

typedef struct M_FREE {
    struct M_FREE *next;
    struct M_FREE *prev;
} M_FREE;

static struct {
    bool tried;
    uint8_t *base;
    uint8_t *start;
    uint8_t *end;
    // The pool's first block; the wall sits just below it.
    uint8_t *pool;
    M_FREE *bins[M_BINS];
    size_t used;
    size_t peak;
    int32_t blocks;
    int32_t spilled;
    int32_t depth;
} m_Heap = {};
static mutex_t m_Lock = MUTEX_INITIALIZER;

#if DC_LEVELHEAP_TRACE
// A TR3 level holds ~2200 blocks once loaded and more while it loads: 3072
// slots overflowed and left the survivors unnamed (DC build 315).
#define M_TRACE_SLOTS 8192
#define M_TRACE_FRAMES 7
typedef struct {
    void *ptr;
    uint32_t size;
    uint32_t pc[M_TRACE_FRAMES];
} M_TRACE;
static M_TRACE m_Trace[M_TRACE_SLOTS];
static int m_TraceDropped = 0;
extern char _executable_start[];
extern char etext[];

static const M_TRACE *M_TraceFind(const void *const ptr)
{
    uint32_t at = ((uint32_t)ptr >> 3) % M_TRACE_SLOTS;
    for (int probe = 0; probe < M_TRACE_SLOTS; probe++) {
        const M_TRACE *const slot = &m_Trace[at];
        if (slot->ptr == nullptr) {
            return nullptr;
        }
        if (slot->ptr == ptr) {
            return slot;
        }
        at = (at + 1) % M_TRACE_SLOTS;
    }
    return nullptr;
}

static void M_TraceAdd(void *const ptr, const size_t size)
{
    uint32_t sp;
    __asm__ volatile("mov r15, %0" : "=r"(sp));
    const uint32_t *const stack = (const uint32_t *)sp;
    uint32_t pc[M_TRACE_FRAMES] = {};
    int frames = 0;
    for (int i = 0; i < 160 && frames < M_TRACE_FRAMES; i++) {
        const uint32_t word = stack[i];
        if (word >= (uint32_t)_executable_start && word < (uint32_t)etext
            && (word & 1) == 0 && (frames == 0 || pc[frames - 1] != word)) {
            pc[frames++] = word;
        }
    }
    uint32_t at = ((uint32_t)ptr >> 3) % M_TRACE_SLOTS;
    for (int probe = 0; probe < M_TRACE_SLOTS; probe++) {
        M_TRACE *const slot = &m_Trace[at];
        if (slot->ptr == nullptr || slot->ptr == (void *)1) {
            slot->ptr = ptr;
            slot->size = (uint32_t)size;
            for (int i = 0; i < M_TRACE_FRAMES; i++) {
                slot->pc[i] = pc[i];
            }
            return;
        }
        at = (at + 1) % M_TRACE_SLOTS;
    }
    m_TraceDropped++;
}

static void M_TraceDel(void *const ptr)
{
    uint32_t at = ((uint32_t)ptr >> 3) % M_TRACE_SLOTS;
    for (int probe = 0; probe < M_TRACE_SLOTS; probe++) {
        M_TRACE *const slot = &m_Trace[at];
        if (slot->ptr == nullptr) {
            return;
        }
        if (slot->ptr == ptr) {
            slot->ptr = (void *)1;
            return;
        }
        at = (at + 1) % M_TRACE_SLOTS;
    }
}

void DC_LevelHeap_DumpLive(const char *const label)
{
    printf("[live-begin] %s\n", label);
    for (int i = 0; i < M_TRACE_SLOTS; i++) {
        const M_TRACE *const slot = &m_Trace[i];
        if (slot->ptr == nullptr || slot->ptr == (void *)1) {
            continue;
        }
        printf("[live] %u", (unsigned)slot->size);
        for (int k = 0; k < M_TRACE_FRAMES && slot->pc[k] != 0; k++) {
            printf(" %08x", (unsigned)slot->pc[k]);
        }
        printf("\n");
    }
    printf("[live-end] %d not traced\n", m_TraceDropped);
}
#else
void DC_LevelHeap_DumpLive(const char *const label)
{
    (void)label;
}
#endif

static inline uint8_t *M_Block(void *const payload)
{
    return (uint8_t *)payload - M_TAG;
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
    while (size > M_MIN && bin < M_BINS - 1) {
        size >>= 1;
        bin++;
    }
    return bin;
}

static void M_BinInsert(uint8_t *const block)
{
    M_FREE *const node = (M_FREE *)(block + M_TAG);
    M_FREE **const head = &m_Heap.bins[M_Bin(M_SizeAt(block))];
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
        m_Heap.bins[M_Bin(M_SizeAt(block))] = node->next;
    }
    if (node->next != nullptr) {
        node->next->prev = node->prev;
    }
}

// Takes the largest block the heap has, less what it keeps.
static void M_Init(void)
{
    m_Heap.tried = true;
    size_t size = M_PROBE_MAX;
    void *probe = nullptr;
    while (size > M_HEAP_RESERVE + M_PROBE_STEP
           && (probe = malloc(size)) == nullptr) {
        size -= M_PROBE_STEP;
    }
    if (probe == nullptr) {
        return;
    }
    free(probe);
    size -= M_HEAP_RESERVE;
    m_Heap.base = malloc(size);
    if (m_Heap.base == nullptr) {
        return;
    }
    // A used tag at either end, so that no block looks past the region for
    // free ones to join; blocks start 4 bytes off the grain, so that their
    // payloads lie on it.
    uint8_t *const aligned = (uint8_t *)(((uintptr_t)m_Heap.base + M_GRAIN - 1)
                                         & ~(uintptr_t)(M_GRAIN - 1));
    *(uint32_t *)aligned = M_USED;
    m_Heap.start = aligned + M_TAG;
    m_Heap.end = (uint8_t *)(((uintptr_t)(m_Heap.base + size) - M_TAG)
                             & ~(uintptr_t)(M_GRAIN - 1))
        - M_TAG;
    *(uint32_t *)m_Heap.end = M_USED;
    // The pool at the top, behind a used block of its own that is never let
    // go, so that no free block joins across it.
    m_Heap.pool = m_Heap.end - M_POOL_SIZE;
    uint8_t *const wall = m_Heap.pool - M_MIN;
    M_SetTags(m_Heap.start, (size_t)(wall - m_Heap.start), false);
    M_BinInsert(m_Heap.start);
    M_SetTags(wall, M_MIN, true);
    M_SetTags(m_Heap.pool, (size_t)(m_Heap.end - m_Heap.pool), false);
    M_BinInsert(m_Heap.pool);
    printf(
        "[levelheap] %uK for the level's data, %uK of it for its small "
        "blocks\n",
        (unsigned)(size / 1024), (unsigned)(M_POOL_SIZE / 1024));
}

static size_t M_Need(const size_t size)
{
    const size_t need =
        (size + 2 * M_TAG + M_GRAIN - 1) & ~(size_t)(M_GRAIN - 1);
    return need < M_MIN ? M_MIN : need;
}

// Marks need bytes of a free block used, the rest of it free again: the
// level's from the block's foot, the rest from its head.
static void *M_Carve(uint8_t *block, size_t need, const bool level)
{
    const size_t have = M_SizeAt(block);
    if (have - need >= M_MIN) {
        if (level) {
            M_SetTags(block + need, have - need, false);
            M_BinInsert(block + need);
        } else {
            M_SetTags(block, have - need, false);
            M_BinInsert(block);
            block += have - need;
        }
    } else {
        need = have;
    }
    M_SetTags(block, need, true);
    m_Heap.used += need;
    if (m_Heap.used > m_Heap.peak) {
        m_Heap.peak = m_Heap.used;
    }
    m_Heap.blocks++;
    return block + M_TAG;
}

// The highest free block of at least need bytes that starts in [from, to).
static uint8_t *M_Highest(
    const size_t need, const uint8_t *const from, const uint8_t *const to)
{
    uint8_t *best = nullptr;
    for (int32_t bin = M_Bin(need); bin < M_BINS; bin++) {
        for (M_FREE *node = m_Heap.bins[bin]; node != nullptr;
             node = node->next) {
            uint8_t *const block = (uint8_t *)node - M_TAG;
            if (M_SizeAt(block) >= need && block >= from && block < to
                && block > best) {
                best = block;
            }
        }
    }
    return best;
}

// The level's data: from the first bin that fits, its lowest block. The
// rest: the highest block of every bin that fits, so that what outlives a
// level never lands in a hole the level left in the middle, where it would
// split the next level's span in two (DC build 283: 6.7 MB free became
// 1.6 MB at most in four loads, and Jungle's 1.6 MB of frames no longer fit).
// The head's keep below the pool and the pool's inside it, each falling back
// on the whole region when its own part is full.
static void *M_Take(const size_t size, const M_KIND kind)
{
    const size_t need = M_Need(size);
    uint8_t *best = nullptr;
    if (kind == M_LEVEL) {
        for (int32_t bin = M_Bin(need); bin < M_BINS && best == nullptr;
             bin++) {
            for (M_FREE *node = m_Heap.bins[bin]; node != nullptr;
                 node = node->next) {
                uint8_t *const block = (uint8_t *)node - M_TAG;
                if (M_SizeAt(block) >= need
                    && (best == nullptr || block < best)) {
                    best = block;
                }
            }
        }
    } else {
        best = kind == M_POOL ? M_Highest(need, m_Heap.pool, m_Heap.end)
                              : M_Highest(need, m_Heap.start, m_Heap.pool);
        if (best == nullptr) {
            best = M_Highest(need, m_Heap.start, m_Heap.end);
        }
    }
    if (best == nullptr) {
        return nullptr;
    }
    M_BinRemove(best);
    return M_Carve(best, need, kind == M_LEVEL);
}

static M_KIND M_KindFor(const size_t size)
{
    if (m_Heap.depth <= 0) {
        return M_HEAD;
    }
    return size > M_LEVEL_SMALL_MAX ? M_LEVEL : M_POOL;
}

// Lets a free stretch go, joined with the free ones either side.
static void M_Release(uint8_t *block, size_t size)
{
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

void DC_LevelHeap_Enter(void)
{
    m_Heap.depth++;
}

void DC_LevelHeap_Leave(void)
{
    m_Heap.depth--;
}


void *DC_LevelHeap_Alloc(const size_t size)
{
    // Before the first level the heap serves everything: the region is taken
    // only once Lua and the boot have had theirs.
    if (size == 0 || (!m_Heap.tried && m_Heap.depth <= 0)) {
        return nullptr;
    }
    // What outlives a level and is small (names, tables, a vector's array)
    // goes to newlib's heap: in the region such a block lands under some big
    // block the level has taken from the top, which is let go at its end,
    // and then sits in the middle of the span the next level loads into. TR3's
    // fourth level found its largest free span at 1.9 MB of 6.4.
    if (m_Heap.depth <= 0 && size <= M_SMALL_MAX) {
        return nullptr;
    }
    mutex_lock(&m_Lock);
    if (!m_Heap.tried) {
        M_Init();
    }
    void *result = nullptr;
    if (m_Heap.base != nullptr) {
        result = M_Take(size, M_KindFor(size));
        if (result == nullptr) {
            m_Heap.spilled++;
        }
#if DC_LEVELHEAP_TRACE
        else {
            M_TraceAdd(result, size);
        }
#endif
    }
    mutex_unlock(&m_Lock);
    return result;
}

bool DC_LevelHeap_Owns(const void *const ptr)
{
    return (const uint8_t *)ptr >= m_Heap.start
        && (const uint8_t *)ptr < m_Heap.end;
}

static void M_Free(void *const ptr)
{
#if DC_LEVELHEAP_TRACE
    M_TraceDel(ptr);
#endif
    uint8_t *const block = M_Block(ptr);
    const size_t size = M_SizeAt(block);
    m_Heap.used -= size;
    m_Heap.blocks--;
    M_Release(block, size);
}

void DC_LevelHeap_Free(void *const ptr)
{
    mutex_lock(&m_Lock);
    M_Free(ptr);
    mutex_unlock(&m_Lock);
}

static void *M_Realloc(void *const ptr, const size_t size)
{
    uint8_t *const block = M_Block(ptr);
    const size_t have = M_SizeAt(block);
    const size_t need = M_Need(size);
    if (need <= have) {
        // Shrunk in place, the tail given back when worth it.
        if (have - need >= M_SPLIT_MIN) {
            M_SetTags(block, need, true);
            m_Heap.used -= have - need;
            M_Release(block + need, have - need);
        }
        return ptr;
    }
    // Grown into the free block after it.
    uint8_t *const next = block + have;
    if (!M_UsedAt(next) && have + M_SizeAt(next) >= need) {
        M_BinRemove(next);
        const size_t total = have + M_SizeAt(next);
        m_Heap.used -= have;
        m_Heap.blocks--;
        M_SetTags(block, total, false);
        // Kept where its data already is.
        return M_Carve(block, need, true);
    }
    void *result = M_Take(size, M_KindFor(size));
#if DC_LEVELHEAP_TRACE
    if (result != nullptr) {
        M_TraceAdd(result, size);
    }
#endif
    if (result == nullptr) {
        result = malloc(size);
        m_Heap.spilled++;
    }
    if (result != nullptr) {
        memcpy(result, ptr, have - 2 * M_TAG);
        M_Free(ptr);
    }
    return result;
}

void *DC_LevelHeap_Realloc(void *const ptr, const size_t size)
{
    mutex_lock(&m_Lock);
    void *const result = M_Realloc(ptr, size);
    mutex_unlock(&m_Lock);
    return result;
}

// A diagnostic: the region as runs of used blocks (offset and size in KiB from
// its start, and how many blocks), with the free spans between them.
void DC_LevelHeap_DumpLayout(void)
{
    mutex_lock(&m_Lock);
    printf("[layout-begin] %uK\n", (unsigned)((m_Heap.end - m_Heap.start) / 1024));
    const uint8_t *at = m_Heap.start;
    while (at < m_Heap.end) {
        const bool used = M_UsedAt(at);
        const uint8_t *const from = at;
        int count = 0;
        while (at < m_Heap.end && M_UsedAt(at) == used) {
            at += M_SizeAt(at);
            count++;
        }
        printf(
            "[layout] %s %uK at %uK (%d)\n", used ? "used" : "free",
            (unsigned)((at - from) / 1024), (unsigned)((from - m_Heap.start) / 1024),
            count);
    }
    // A few of the small ones, and the ones the census has no trace for,
    // with what they hold.
    int shown = 0;
    for (const uint8_t *b = m_Heap.start; b < m_Heap.end && shown < 40;
         b += M_SizeAt(b)) {
        if (!M_UsedAt(b)) {
            continue;
        }
        const bool untraced =
#if DC_LEVELHEAP_TRACE
            M_TraceFind(b + M_TAG) == nullptr;
#else
            false;
#endif
        if (M_SizeAt(b) <= 48 || untraced) {
            const uint32_t *w = (const uint32_t *)(b + M_TAG);
            printf(
                "[small] %uK size %u%s: %08x %08x %08x %08x\n",
                (unsigned)((b - m_Heap.start) / 1024), (unsigned)M_SizeAt(b),
                untraced ? " untraced" : "", (unsigned)w[0], (unsigned)w[1],
                (unsigned)w[2], (unsigned)w[3]);
            shown++;
        }
    }
    printf("[layout-end]\n");
    mutex_unlock(&m_Lock);
}

void DC_LevelHeap_GetStats(DC_LEVEL_HEAP_STATS *const out)
{
    mutex_lock(&m_Lock);
    size_t largest = 0;
    for (int32_t bin = M_BINS - 1; bin >= 0 && largest == 0; bin--) {
        for (const M_FREE *node = m_Heap.bins[bin]; node != nullptr;
             node = node->next) {
            const size_t size = M_SizeAt((const uint8_t *)node - M_TAG);
            largest = size > largest ? size : largest;
        }
    }
    *out = (DC_LEVEL_HEAP_STATS) {
        .size_kb = (int32_t)((m_Heap.end - m_Heap.start) / 1024),
        .used_kb = (int32_t)(m_Heap.used / 1024),
        .peak_kb = (int32_t)(m_Heap.peak / 1024),
        .blocks = m_Heap.blocks,
        .largest_free_kb = (int32_t)(largest / 1024),
        .spilled = m_Heap.spilled,
    };
    mutex_unlock(&m_Lock);
}
