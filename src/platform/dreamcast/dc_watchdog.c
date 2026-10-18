// Dreamcast platform layer: a watchdog for finding hangs without a debugger.
//
// Every few seconds a low-priority thread prints where the watched thread is:
// its saved PC and PR, and the words on its stack that look like return
// addresses. Feed them to sh-elf-addr2line -f -e TRX.elf to read them back.

#include "dc_watchdog.h"

#include <arch/irq.h>
#include <kos/thread.h>

#include <malloc.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

#define M_INTERVAL_MS 1000
#define M_STACK_WORDS 512
#define M_MAX_FRAMES 24

// The link script names the bounds of the code (C names lose the leading
// underscore the SH ELF symbols carry).
extern char _executable_start[];
extern char etext[];

static kthread_t *m_Watched = nullptr;

extern int32_t g_DC_BatcherMeshCount;
extern int32_t g_DC_BatcherVertexCount;

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
            "[watchdog] heap=%uK used=%uK brk=%08lx meshes=%d/%d stack=%uK %s pc=%08lx pr=%08lx "
            "sp=%08lx trace:",
            (unsigned)(mem.arena / 1024), (unsigned)(mem.uordblks / 1024),
            (unsigned long)sbrk(0), (int)g_DC_BatcherMeshCount,
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

void DC_Watchdog_Start(kthread_t *const thread)
{
    m_Watched = thread;
    const kthread_attr_t attr = {
        .create_detached = true,
        .stack_size = 16 * 1024,
        .label = "trx-watchdog",
    };
    thd_create_ex(&attr, M_Run, nullptr);
}
