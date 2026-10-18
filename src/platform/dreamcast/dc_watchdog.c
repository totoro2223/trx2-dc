// Dreamcast platform layer: a watchdog for finding hangs without a debugger.
//
// Every few seconds a low-priority thread prints where the watched thread is:
// its saved PC and PR, and the words on its stack that look like return
// addresses. Feed them to sh-elf-addr2line -f -e TRX.elf to read them back.

#include "dc_watchdog.h"

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
        // The stack grows down; the marker left at its bottom is the part
        // never used.
        const uint8_t *const stack_bottom = thread->stack;
        size_t untouched = 0;
        while (untouched < thread->stack_size
               && stack_bottom[untouched] == DC_STACK_MARKER) {
            untouched++;
        }
        printf(
            "[watchdog] heap=%uK used=%uK lua=%dK brk=%08lx meshes=%d/%d stack=%uK %s pc=%08lx pr=%08lx "
            "sp=%08lx trace:",
            (unsigned)(mem.arena / 1024), (unsigned)(mem.uordblks / 1024),
            (int)LUA_DC_GetMemoryKB(), (unsigned long)sbrk(0),
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

// A sampling profiler: every millisecond or so the watched thread's saved PC
// is counted, and every few seconds the counts go to the console as
// "[prof] pc count" lines (tools/dreamcast/prof_report.py reads them back).
// A thread that is waiting shows up in the scheduler's code.
#if defined(TRX_DC_PROFILE)
#define M_PROF_SLOTS 4096
#define M_PROF_DUMP_MS 10000

static struct {
    uint32_t pc;
    uint32_t count;
} m_Prof[M_PROF_SLOTS];

static void M_ProfCount(const uint32_t pc)
{
    uint32_t slot = (pc >> 1) * 2654435761u % M_PROF_SLOTS;
    for (int32_t i = 0; i < M_PROF_SLOTS; i++) {
        if (m_Prof[slot].pc == pc || m_Prof[slot].pc == 0) {
            m_Prof[slot].pc = pc;
            m_Prof[slot].count++;
            return;
        }
        slot = (slot + 1) % M_PROF_SLOTS;
    }
}

static void *M_Profile(void *const param)
{
    uint64_t last_dump = timer_ms_gettime64();
    uint32_t samples = 0;
    while (true) {
        thd_sleep(1);
        kthread_t *const thread = m_Watched;
        if (thread == nullptr) {
            continue;
        }
        M_ProfCount(thread->context.pc);
        samples++;
        const uint64_t now = timer_ms_gettime64();
        if (now - last_dump < M_PROF_DUMP_MS) {
            continue;
        }
        printf("[prof] begin %lu samples\n", (unsigned long)samples);
        for (int32_t i = 0; i < M_PROF_SLOTS; i++) {
            if (m_Prof[i].count >= 2) {
                printf(
                    "[prof] %08lx %lu\n", (unsigned long)m_Prof[i].pc,
                    (unsigned long)m_Prof[i].count);
            }
            m_Prof[i].pc = 0;
            m_Prof[i].count = 0;
        }
        printf("[prof] end\n");
        samples = 0;
        last_dump = timer_ms_gettime64();
    }
    return nullptr;
}
#endif

void DC_Watchdog_Start(kthread_t *const thread)
{
    m_Watched = thread;
    const kthread_attr_t attr = {
        .create_detached = true,
        .stack_size = 16 * 1024,
        .label = "trx-watchdog",
    };
    thd_create_ex(&attr, M_Run, nullptr);
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
