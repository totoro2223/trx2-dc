// Dreamcast platform layer: start-up.
//
// The engine finds its data relative to its base directory (see
// SDL_GetBasePath), which on the Dreamcast is the root of the disc. The disc
// cannot be written, so the directories the engine writes to are pointed at
// the RAM disk, through the environment variables the engine already honours
// for split installs.
//
// The link wraps main (-Wl,--wrap=main), so this runs once KOS is up and
// before the engine's own main.

#include "dc_watchdog.h"

#include <arch/mmu.h>
#include <kos/dbgio.h>
#include <kos/init.h>
#include <kos/thread.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DC_MAIN_STACK_SIZE (128 * 1024)

KOS_INIT_FLAGS(INIT_DEFAULT);

int __real_main(int argc, char *argv[]);

static void M_EnsureDir(const char *const path)
{
    if (mkdir(path, 0777) != 0) {
        // Already there, or the RAM disk cannot hold directories; the engine
        // reports whatever it then fails to write.
    }
}

static void M_SetupEnvironment(void)
{
    M_EnsureDir("/ram/trx");
    M_EnsureDir("/ram/trx/cache");
    M_EnsureDir("/ram/trx/saves");
    M_EnsureDir("/ram/trx/screenshots");

    setenv("TRX_DIR", "/cd", 0);
    setenv("TRX_CACHE_DIR", "/ram/trx/cache", 0);
    setenv("TRX_SAVES_DIR", "/ram/trx/saves", 0);
    setenv("TRX_SCREENSHOTS_DIR", "/ram/trx/screenshots", 0);
}

typedef struct {
    int argc;
    char **argv;
    int result;
} M_MAIN_ARGS;

static void *M_MainThread(void *const param)
{
    M_MAIN_ARGS *const args = param;
    args->result = __real_main(args->argc, args->argv);
    return nullptr;
}

int __wrap_main(int argc, char *argv[])
{
#if defined(TRX_DC_FB_CONSOLE)
    // Until the renderer owns the screen, show the log on it.
    dbgio_dev_select("fb");
#endif
    M_SetupEnvironment();
#if defined(TRX_DC_MMU_NULL_TRAP)
    // Without the MMU a null pointer reads the boot ROM and a null call runs
    // it, which crashes far from the cause. With it on and nothing mapped in
    // P0 (the engine runs in P1), both raise a TLB miss that KOS reports.
    mmu_init_basic();
#endif
    printf("TRX: Dreamcast platform layer ready\n");

    // The KOS main thread has a 64 KiB stack; the engine was written for
    // desktop stacks of a megabyte or more, so it runs on a thread of its own.
#if defined(TRX_DC_START_LEVEL)
    // Development builds can skip the title and start in a level of TR2.
    #define M_STR2(x) #x
    #define M_STR(x) M_STR2(x)
    static char *start_argv[] = {
        "trx", "-e", "2", "-l", M_STR(TRX_DC_START_LEVEL), nullptr,
    };
    argc = 5;
    argv = start_argv;
#endif
    M_MAIN_ARGS args = { .argc = argc, .argv = argv, .result = 0 };
    // The stack is filled with a marker so that the watchdog can tell how
    // deep it has been used.
    void *const stack = memalign(32, DC_MAIN_STACK_SIZE);
    if (stack == nullptr) {
        printf("TRX: could not allocate the main stack\n");
        return 1;
    }
    memset(stack, DC_STACK_MARKER, DC_MAIN_STACK_SIZE);
    const kthread_attr_t attr = {
        .stack_size = DC_MAIN_STACK_SIZE,
        .stack_ptr = stack,
        .label = "trx-main",
    };
    kthread_t *const thread = thd_create_ex(&attr, M_MainThread, &args);
    if (thread == nullptr) {
        printf("TRX: could not create the main thread\n");
        return 1;
    }
#if defined(TRX_DC_WATCHDOG)
    DC_Watchdog_Start(thread);
#endif
    thd_join(thread, nullptr);
    return args.result;
}
