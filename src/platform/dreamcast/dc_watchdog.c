// Dreamcast platform layer: a watchdog for finding hangs without a debugger.
//
// Every few seconds a low-priority thread prints where the watched thread is:
// its saved PC and PR, and the words on its stack that look like return
// addresses. Feed them to sh-elf-addr2line -f -e TRX.elf to read them back.

#include "dc_watchdog.h"

#include "dc_lua_alloc.h"

#include <arch/irq.h>
#include <arch/timer.h>
#include <kos/thread.h>

#include <malloc.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

// Writing to the serial port keeps the CPU busy until the bytes are out, so
// the report stays short and rare: a stall the player could see otherwise.
#define M_INTERVAL_MS 10000
#define M_STACK_WORDS 512
#define M_MAX_FRAMES 24

// The link script names the bounds of the code (C names lose the leading
// underscore the SH ELF symbols carry).
extern char _executable_start[];
extern char etext[];

static kthread_t *m_Watched = nullptr;

extern int32_t g_DC_BatcherMeshCount;
extern int32_t g_DC_BatcherVertexCount;
int32_t LUA_DC_GetMemoryKB(void);

static bool M_IsCode(const uint32_t addr)
{
    return addr >= (uint32_t)_executable_start && addr < (uint32_t)etext
        && (addr & 1) == 0;
}

static void *M_Run(void *const param)
{
    while (true) {
        thd_sleep(M_INTERVAL_MS);
        kthread_t *const thread = m_Watched;
        if (thread == nullptr) {
            continue;
        }
        const irq_context_t *const ctx = &thread->context;
        const struct mallinfo mem = mallinfo();
        DC_LUA_ALLOC_STATS lua;
        DC_LuaAlloc_GetStats(&lua);
        // The stack grows down; the marker left at its bottom is the part
        // never used.
        const uint8_t *const stack_bottom = thread->stack;
        size_t untouched = 0;
        while (untouched < thread->stack_size
               && stack_bottom[untouched] == DC_STACK_MARKER) {
            untouched++;
        }
        printf(
            "[watchdog] heap=%uK used=%uK lua=%dK (small=%dK big=%dK "
            "spilled=%d) brk=%08lx meshes=%d/%d stack=%uK %s pc=%08lx pr=%08lx "
            "sp=%08lx trace:",
            (unsigned)(mem.arena / 1024), (unsigned)(mem.uordblks / 1024),
            (int)LUA_DC_GetMemoryKB(), (int)lua.small_kb, (int)lua.big_kb,
            (int)lua.spilled, (unsigned long)sbrk(0),
            (int)g_DC_BatcherMeshCount,
            (int)g_DC_BatcherVertexCount,
            (unsigned)((thread->stack_size - untouched) / 1024), thread->label,
            (unsigned long)ctx->pc, (unsigned long)ctx->pr,
            (unsigned long)ctx->r[15]);
        const uint32_t *sp = (const uint32_t *)ctx->r[15];
        const uint32_t *const stack_top = (const uint32_t *)((uintptr_t)
            thread->stack + thread->stack_size);
        int frames = 0;
        for (int i = 0; i < M_STACK_WORDS && sp + i < stack_top
             && frames < M_MAX_FRAMES;
             i++) {
            if (M_IsCode(sp[i])) {
                printf(" %08lx", (unsigned long)sp[i]);
                frames++;
            }
        }
        printf("\n");
    }
    return nullptr;
}

// A sampling profiler: every 1 to 3 milliseconds the watched thread's saved PC
// is counted, and every few seconds the counts go to the console as
// "[prof] pc count" lines (tools/dreamcast/prof_report.py reads them back).
// A thread that is waiting shows up in the scheduler's code.
#if defined(TRX_DC_PROFILE)
#define M_PROF_SLOTS 4096
#define M_PROF_DUMP_MS 10000

typedef struct {
    uint32_t pc;
    uint32_t count;
} M_PROF_SLOT;

// The saved PC, and the return address (PR): the caller of a function that
// waits or runs with interrupts off shows there.
static M_PROF_SLOT m_Prof[M_PROF_SLOTS];
static M_PROF_SLOT m_ProfPR[M_PROF_SLOTS];

static void M_ProfCount(M_PROF_SLOT *const table, const uint32_t pc)
{
    uint32_t slot = (pc >> 1) * 2654435761u % M_PROF_SLOTS;
    for (int32_t i = 0; i < M_PROF_SLOTS; i++) {
        if (table[slot].pc == pc || table[slot].pc == 0) {
            table[slot].pc = pc;
            table[slot].count++;
            return;
        }
        slot = (slot + 1) % M_PROF_SLOTS;
    }
}

static void M_ProfDump(M_PROF_SLOT *const table, const char *const tag)
{
    for (int32_t i = 0; i < M_PROF_SLOTS; i++) {
        if (table[i].count >= 2) {
            printf(
                "%s %08lx %lu\n", tag, (unsigned long)table[i].pc,
                (unsigned long)table[i].count);
        }
        table[i].pc = 0;
        table[i].count = 0;
    }
}

static void *M_Profile(void *const param)
{
    uint64_t last_dump = timer_ms_gettime64();
    uint32_t samples = 0;
    uint32_t seed = 2463534242u;
    while (true) {
        // 1 to 3 ms at random: a fixed period locks to the 30 fps frame and
        // keeps hitting the same spots of it.
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        thd_sleep(1 + seed % 3);
        kthread_t *const thread = m_Watched;
        if (thread == nullptr) {
            continue;
        }
        M_ProfCount(m_Prof, thread->context.pc);
        M_ProfCount(m_ProfPR, thread->context.pr);
        samples++;
        const uint64_t now = timer_ms_gettime64();
        if (now - last_dump < M_PROF_DUMP_MS) {
            continue;
        }
        printf("[prof] begin %lu samples\n", (unsigned long)samples);
        M_ProfDump(m_Prof, "[prof]");
        M_ProfDump(m_ProfPR, "[prof] pr");
        printf("[prof] end\n");
        samples = 0;
        last_dump = timer_ms_gettime64();
    }
    return nullptr;
}
#endif

// Text for the console from the game's thread, written out by a thread of
// its own: the serial port takes ~0.1 ms a character, and a frame's
// statistics (M_StatsEndScene) would otherwise hold the game up for a frame.
// One writer and one reader, so the ring needs no lock; what does not fit
// is dropped.
#define M_LOG_BYTES 8192
static char m_Log[M_LOG_BYTES];
static volatile uint32_t m_LogHead = 0;
static volatile uint32_t m_LogTail = 0;

void DC_Watchdog_Print(const char *text)
{
    uint32_t head = m_LogHead;
    for (; *text != '\0'; text++) {
        const uint32_t next = (head + 1) % M_LOG_BYTES;
        if (next == m_LogTail) {
            break;
        }
        m_Log[head] = *text;
        head = next;
    }
    m_LogHead = head;
}

static void *M_LogRun(void *const param)
{
    while (true) {
        thd_sleep(20);
        const uint32_t head = m_LogHead;
        uint32_t tail = m_LogTail;
        while (tail != head) {
            const uint32_t end = head > tail ? head : M_LOG_BYTES;
            fwrite(&m_Log[tail], 1, end - tail, stdout);
            tail = end % M_LOG_BYTES;
        }
        m_LogTail = tail;
        fflush(stdout);
    }
    return nullptr;
}

void DC_Watchdog_Start(kthread_t *const thread)
{
    m_Watched = thread;
    const kthread_attr_t attr = {
        .create_detached = true,
        .stack_size = 16 * 1024,
        .label = "trx-watchdog",
    };
    thd_create_ex(&attr, M_Run, nullptr);
    const kthread_attr_t log_attr = {
        .create_detached = true,
        .stack_size = 8 * 1024,
        .label = "trx-log",
    };
    thd_create_ex(&log_attr, M_LogRun, nullptr);
#if defined(TRX_DC_PROFILE)
    const kthread_attr_t prof_attr = {
        .create_detached = true,
        .stack_size = 8 * 1024,
        // Above the game, so that it wakes when due.
        .prio = PRIO_DEFAULT - 1,
        .label = "trx-profiler",
    };
    thd_create_ex(&prof_attr, M_Profile, nullptr);
#endif
}

// Ticks between two loads: long enough for the level to settle.
#define M_TEST_LOAD_TICKS 360

static int32_t m_TestLoads = 0;
static int32_t m_TestLoadsDone = 0;
static int32_t m_TestTicks = 0;

void DC_Watchdog_SetTestLoads(const int32_t count)
{
    m_TestLoads = count;
}

bool DC_Watchdog_IsTesting(void)
{
    return m_TestLoads > 0;
}

int32_t DC_Watchdog_TakeTestLoad(void)
{
    if (m_TestLoadsDone >= m_TestLoads || ++m_TestTicks < M_TEST_LOAD_TICKS) {
        return -1;
    }
    m_TestTicks = 0;
    const struct mallinfo mem = mallinfo();
    DC_LUA_ALLOC_STATS lua;
    DC_LuaAlloc_GetStats(&lua);
    printf(
        "[test] load %d of %d: used=%uK free=%uK lua=%dK small=%dK big=%dK "
        "spilled=%d brk=%08lx\n",
        (int)m_TestLoadsDone + 1, (int)m_TestLoads, mem.uordblks / 1024,
        mem.fordblks / 1024, (int)LUA_DC_GetMemoryKB(), (int)lua.small_kb,
        (int)lua.big_kb, (int)lua.spilled, (unsigned long)sbrk(0));
    return m_TestLoadsDone++ % 2;
}
