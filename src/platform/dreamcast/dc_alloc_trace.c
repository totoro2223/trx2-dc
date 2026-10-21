// Dreamcast platform layer: reports large heap allocations (debug builds).
//
// With 16 MiB of RAM the question is always where it went. The link wraps
// malloc, calloc, realloc and free (-Wl,--wrap=...) so that every allocation
// of at least DC_ALLOC_TRACE_MIN bytes is printed with its caller, and the
// ones still alive can be listed (DC_AllocTrace_DumpLive), to find what one
// level leaves behind for the next; feed the addresses to
// sh-elf-addr2line -f -e TRX.elf (tools/dreamcast/alloc_census.py).

#include "dc_alloc_trace.h"

#include "dc_watchdog.h"

#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DC_ALLOC_TRACE_MIN (24 * 1024)
#define M_LIVE_SLOTS 512
#define M_LIVE_FRAMES 8

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);

extern char _executable_start[];
extern char etext[];

typedef struct {
    void *ptr;
    size_t size;
    uint32_t frames[M_LIVE_FRAMES];
} M_LIVE;

static M_LIVE m_Live[M_LIVE_SLOTS];

// Words on the stack that point into the code: likely return addresses.
static int M_Trace(uint32_t *const out, const int max)
{
    uint32_t sp;
    __asm__ volatile("mov r15, %0" : "=r"(sp));
    const uint32_t *const stack = (const uint32_t *)sp;
    int frames = 0;
    for (int i = 0; i < 256 && frames < max; i++) {
        const uint32_t word = stack[i];
        // (The same word again is one saved twice, not another caller.)
        if (word >= (uint32_t)_executable_start && word < (uint32_t)etext
            && (word & 1) == 0 && (frames == 0 || out[frames - 1] != word)) {
            out[frames++] = word;
        }
    }
    return frames;
}

static void M_Report(
    const char *const what, const size_t size, const void *const result,
    const void *const caller)
{
    const struct mallinfo mem = mallinfo();
    printf(
        "[alloc] %s %uK -> %p from %p (in use %uK) trace:", what,
        (unsigned)(size / 1024), result, caller,
        (unsigned)(mem.uordblks / 1024));
    uint32_t frames[10];
    const int count = M_Trace(frames, 10);
    for (int i = 0; i < count; i++) {
        printf(" %08lx", (unsigned long)frames[i]);
    }
    printf("\n");
}

static void M_Forget(void *const ptr)
{
    // Only large blocks are remembered; the many small ones Lua frees every
    // frame skip the search (it took a third of the inventory's time).
    if (ptr == nullptr || malloc_usable_size(ptr) < DC_ALLOC_TRACE_MIN) {
        return;
    }
    for (int i = 0; i < M_LIVE_SLOTS; i++) {
        if (m_Live[i].ptr == ptr) {
            m_Live[i].ptr = nullptr;
            return;
        }
    }
}

static void M_Remember(void *const ptr, const size_t size)
{
    if (ptr == nullptr || size < DC_ALLOC_TRACE_MIN) {
        return;
    }
    for (int i = 0; i < M_LIVE_SLOTS; i++) {
        if (m_Live[i].ptr == nullptr) {
            m_Live[i].ptr = ptr;
            m_Live[i].size = size;
            const int count = M_Trace(m_Live[i].frames, M_LIVE_FRAMES);
            for (int j = count; j < M_LIVE_FRAMES; j++) {
                m_Live[i].frames[j] = 0;
            }
            return;
        }
    }
}

void DC_AllocTrace_DumpLive(const char *const label)
{
    const struct mallinfo mem = mallinfo();
    size_t total = 0;
    for (int i = 0; i < M_LIVE_SLOTS; i++) {
        if (m_Live[i].ptr != nullptr) {
            total += m_Live[i].size;
        }
    }
    printf(
        "[live] %s: in use %uK, of which %uK in large blocks\n", label,
        (unsigned)(mem.uordblks / 1024), (unsigned)(total / 1024));
    for (int i = 0; i < M_LIVE_SLOTS; i++) {
        if (m_Live[i].ptr == nullptr) {
            continue;
        }
        printf("[live] %uK trace:", (unsigned)(m_Live[i].size / 1024));
        for (int j = 0; j < M_LIVE_FRAMES && m_Live[i].frames[j] != 0; j++) {
            printf(" %08lx", (unsigned long)m_Live[i].frames[j]);
        }
        printf("\n");
    }
}

// From the end of one level's load to the start of the next, every block
// taken above where that load began is remembered with its callers: what of
// it is still there at the next load sits in the middle of the room the next
// level needs, and splits it.
#define M_KEPT_SLOTS 4093
#define M_KEPT_FRAMES 12
#define M_KEPT_GONE ((void *)1)

typedef struct {
    void *ptr;
    uint32_t size;
    uint32_t frames[M_KEPT_FRAMES];
} M_KEPT;

// Taken only on test discs that load games in turn (dc_watchdog.c).
static M_KEPT *m_Kept = nullptr;
static bool m_Keeping = false;
static uintptr_t m_KeptFrom = 0;
static int m_KeptLost = 0;

static int M_KeptSlot(const void *const ptr)
{
    return (int)(((uintptr_t)ptr >> 3) % M_KEPT_SLOTS);
}

static void M_Keep(void *const ptr, const size_t size)
{
    if (!m_Keeping || ptr == nullptr || (uintptr_t)ptr < m_KeptFrom) {
        return;
    }
    int slot = M_KeptSlot(ptr);
    for (int i = 0; i < M_KEPT_SLOTS; i++) {
        M_KEPT *const kept = &m_Kept[slot];
        if (kept->ptr == nullptr || kept->ptr == M_KEPT_GONE) {
            kept->ptr = ptr;
            kept->size = (uint32_t)size;
            const int count = M_Trace(kept->frames, M_KEPT_FRAMES);
            for (int j = count; j < M_KEPT_FRAMES; j++) {
                kept->frames[j] = 0;
            }
            return;
        }
        slot = (slot + 1) % M_KEPT_SLOTS;
    }
    m_KeptLost++;
}

static void M_Unkeep(void *const ptr)
{
    if (!m_Keeping || ptr == nullptr || (uintptr_t)ptr < m_KeptFrom) {
        return;
    }
    int slot = M_KeptSlot(ptr);
    for (int i = 0; i < M_KEPT_SLOTS && m_Kept[slot].ptr != nullptr; i++) {
        if (m_Kept[slot].ptr == ptr) {
            m_Kept[slot].ptr = M_KEPT_GONE;
            return;
        }
        slot = (slot + 1) % M_KEPT_SLOTS;
    }
}

void DC_AllocTrace_StartKeeping(void)
{
    if (m_Keeping || !DC_Watchdog_IsTesting()) {
        return;
    }
    if (m_Kept == nullptr) {
        m_Kept = __real_malloc(sizeof(M_KEPT) * M_KEPT_SLOTS);
        if (m_Kept == nullptr) {
            return;
        }
    }
    for (int i = 0; i < M_KEPT_SLOTS; i++) {
        m_Kept[i].ptr = nullptr;
    }
    m_KeptLost = 0;
    m_Keeping = true;
}

void DC_AllocTrace_DumpKept(const char *const label)
{
    if (!m_Keeping) {
        return;
    }
    int count = 0;
    uint32_t bytes = 0;
    for (int i = 0; i < M_KEPT_SLOTS; i++) {
        const M_KEPT *const kept = &m_Kept[i];
        if (kept->ptr == nullptr || kept->ptr == M_KEPT_GONE) {
            continue;
        }
        count++;
        bytes += kept->size;
        printf("[kept] %p %u:", kept->ptr, (unsigned)kept->size);
        for (int j = 0; j < M_KEPT_FRAMES && kept->frames[j] != 0; j++) {
            printf(" %08lx", (unsigned long)kept->frames[j]);
        }
        printf("\n");
    }
    printf(
        "[kept] %s: %d blocks, %uK above %08lx (%d not followed)\n", label,
        count, (unsigned)(bytes / 1024), (unsigned long)m_KeptFrom,
        m_KeptLost);
}

void *__wrap_malloc(const size_t size)
{
    void *const result = __real_malloc(size);
    if (size >= DC_ALLOC_TRACE_MIN || result == nullptr) {
        M_Report("malloc", size, result, __builtin_return_address(0));
    }
    M_Remember(result, size);
    M_Keep(result, size);
    return result;
}

void *__wrap_calloc(const size_t count, const size_t size)
{
    void *const result = __real_calloc(count, size);
    if (count * size >= DC_ALLOC_TRACE_MIN || result == nullptr) {
        M_Report("calloc", count * size, result, __builtin_return_address(0));
    }
    M_Remember(result, count * size);
    M_Keep(result, count * size);
    return result;
}

void *__wrap_realloc(void *const ptr, const size_t size)
{
    void *const result = __real_realloc(ptr, size);
    if (size >= DC_ALLOC_TRACE_MIN || result == nullptr) {
        M_Report("realloc", size, result, __builtin_return_address(0));
    }
    if (result != nullptr || size == 0) {
        M_Forget(ptr);
        M_Remember(result, size);
        M_Unkeep(ptr);
        M_Keep(result, size);
    }
    return result;
}

void __wrap_free(void *const ptr)
{
    M_Forget(ptr);
    M_Unkeep(ptr);
    __real_free(ptr);
}

// Walks the heap (KOS's dlmalloc: each chunk is a previous size, then its
// own size with bit 0 telling whether the chunk before is in use) and prints
// how full each stretch of it is, and its largest hole: what a big block
// asked for next can count on.
#define M_MAP_BUCKET (256 * 1024)

extern char end[];
void *sbrk(intptr_t increment);

// Where the last walk met a chunk that makes no sense, and its size.
static uintptr_t m_WalkBreak = 0;
static size_t m_WalkBreakSize = 0;

static bool M_WalkHeap(
    const uintptr_t start, const uintptr_t top, const bool print,
    size_t *const largest_free)
{
    uint32_t used[64] = {};
    uint32_t chunks[64] = {};
    uintptr_t c = start;
    *largest_free = 0;
    while (c + 8 <= top) {
        const size_t size = *(const uint32_t *)(c + 4) & ~(size_t)7;
        if (size < 16 || c + size > top) {
            m_WalkBreak = c;
            m_WalkBreakSize = size;
            return false;
        }
        // The last chunk, which runs to what sbrk gave, is free.
        const uintptr_t next = c + size;
        const bool in_use =
            next + 8 <= top && (*(const uint32_t *)(next + 4) & 1) != 0;
        const int bucket = (int)((c - start) / M_MAP_BUCKET);
        if (in_use && bucket < 64) {
            used[bucket] += size;
            chunks[bucket]++;
        } else if (!in_use && size > *largest_free) {
            *largest_free = size;
        }
        c = next;
    }
    if (c != top) {
        return false;
    }
    if (print) {
        for (int b = 0; b < 64 && start + (uintptr_t)b * M_MAP_BUCKET < top;
             b++) {
            printf(
                "[heap] %08lx %3uK in %u\n",
                (unsigned long)(start + (uintptr_t)b * M_MAP_BUCKET),
                (unsigned)(used[b] / 1024), (unsigned)chunks[b]);
        }
    }
    return true;
}

void DC_AllocTrace_DumpHeapMap(const char *const label)
{
    if (!DC_Watchdog_IsTesting()) {
        return;
    }
    const uintptr_t top = (uintptr_t)sbrk(0);
    if (m_KeptFrom == 0) {
        m_KeptFrom = top;
    }
    // Where the first chunk lies is looked for once: a walk from anywhere
    // else soon meets a size that makes no sense. A heap that can no longer
    // be walked from there is reported, not looked over again.
    static uintptr_t start = 0;
    size_t largest = 0;
    if (start == 0) {
        const uintptr_t base = ((uintptr_t)end + 7) & ~(uintptr_t)7;
        for (uintptr_t at = base; at < base + 8192 && start == 0; at += 8) {
            if (M_WalkHeap(at, top, false, &largest)) {
                start = at;
            }
        }
        if (start == 0) {
            printf("[heap] %s: could not walk the heap\n", label);
            return;
        }
    } else if (!M_WalkHeap(start, top, false, &largest)) {
        const uint32_t *const words = (const uint32_t *)m_WalkBreak;
        printf(
            "[heap] %s: the walk from %08lx breaks at %08lx (size %u): "
            "%08lx %08lx %08lx %08lx\n",
            label, (unsigned long)start, (unsigned long)m_WalkBreak,
            (unsigned)m_WalkBreakSize, (unsigned long)words[0],
            (unsigned long)words[1], (unsigned long)words[2],
            (unsigned long)words[3]);
        return;
    }
    printf(
        "[heap] %s: %08lx-%08lx, largest hole %uK\n", label,
        (unsigned long)start, (unsigned long)top, (unsigned)(largest / 1024));
    M_WalkHeap(start, top, true, &largest);
}
