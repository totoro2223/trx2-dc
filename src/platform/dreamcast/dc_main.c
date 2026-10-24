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

#include "dc_bench.h"
#include "dc_level_select.h"
#include "dc_mode.h"
#include "dc_watchdog.h"
#include "vmu.h"

#include <arch/arch.h>
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
    // The RAM disk cannot make directories (no mkdir), so the saves sit at its root.
    setenv("TRX_SAVES_DIR", "/ram", 0);
    setenv("TRX_SCREENSHOTS_DIR", "/ram/trx/screenshots", 0);
    DC_VMU_RestoreSaves(getenv("TRX_SAVES_DIR"));
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
#else
    // The game on the disc (make_disc.sh's TRX_DC_GAME): TR2 unless it
    // says otherwise.
    int engine = 2;
    FILE *const game = fopen("/cd/GAME.TXT", "r");
    if (game != nullptr) {
        if (fscanf(game, "%d", &engine) != 1) {
            engine = 2;
        }
        fclose(game);
    }
    // An expansion on the disc (make_disc.sh's TRX_DC_MOD): the engine runs
    // it by name, as --mod does.
    static char mod_id[32];
    FILE *const mod_file = fopen("/cd/MOD.TXT", "r");
    if (mod_file != nullptr) {
        if (fscanf(mod_file, "%31s", mod_id) != 1) {
            mod_id[0] = '\0';
        }
        fclose(mod_file);
    }
    // A test disc that starts in one of the title's demos (make_disc.sh's
    // TRX_DC_DEMO).
    FILE *const demo = fopen("/cd/DEMO.TXT", "r");
    if (demo != nullptr) {
        int number = -1;
        while (g_DC_DemoQueueCount < DC_DEMO_QUEUE_MAX
               && fscanf(demo, "%d", &number) == 1) {
            g_DC_DemoQueue[g_DC_DemoQueueCount++] = number;
        }
        fclose(demo);
    }
    // A test disc that runs the benchmark straight away, with no keys
    // (make_disc.sh's TRX_DC_BENCH): the demo's number.
    FILE *const bench = fopen("/cd/BENCH.TXT", "r");
    if (bench != nullptr) {
        int number = -1;
        if (fscanf(bench, "%d", &number) == 1 && number >= 0) {
            g_DC_StartDemo = number;
            g_DC_BenchRequested = true;
        }
        fclose(bench);
    }
    // A test disc that starts in a mode whatever the settings say
    // (make_disc.sh's TRX_DC_MODE): the mode's number.
    FILE *const mode = fopen("/cd/MODE.TXT", "r");
    if (mode != nullptr) {
        int number = -1;
        if (fscanf(mode, "%d", &number) == 1) {
            g_DC_TestMode = number;
        }
        fclose(mode);
    }
    // A test disc that fixes the shadow type (make_disc.sh's TRX_DC_SHADOW):
    // the SHADOW_TYPE's number.
    FILE *const shadow = fopen("/cd/SHADOW.TXT", "r");
    if (shadow != nullptr) {
        int number = -1;
        if (fscanf(shadow, "%d", &number) == 1) {
            g_DC_TestShadow = number;
        }
        fclose(shadow);
    }
    // A test disc that changes the mode on a frame (make_disc.sh's
    // TRX_DC_SWITCH): the mode's number and the frame.
    FILE *const change = fopen("/cd/SWITCH.TXT", "r");
    if (change != nullptr) {
        if (fscanf(
                change, "%d %d", (int *)&g_DC_TestSwitchMode,
                (int *)&g_DC_TestSwitchFrame)
            != 2) {
            g_DC_TestSwitchMode = -1;
        }
        fclose(change);
    }
    // A test disc that goes on through the levels (make_disc.sh's
    // TRX_DC_LEVELS): the last one and how long to stay in each.
    FILE *const levels = fopen("/cd/LEVELS.TXT", "r");
    if (levels != nullptr) {
        if (fscanf(
                levels, "%d %d", (int *)&g_DC_TestLevelLast,
                (int *)&g_DC_TestLevelFrames)
            != 2) {
            g_DC_TestLevelFrames = 0;
        }
        fclose(levels);
    }
    // Test discs name their level (make_disc.sh's fourth argument).
    int level = -1;
    FILE *const start = fopen("/cd/START.TXT", "r");
    if (start != nullptr) {
        if (fscanf(start, "%d", &level) != 1) {
            level = -1;
        }
        // And, optionally, how many saved games to load in turn.
        int loads = 0;
        if (fscanf(start, "%d", &loads) == 1) {
            DC_Watchdog_SetTestLoads(loads);
        }
        fclose(start);
    } else if (g_DC_DemoQueueCount == 0 && g_DC_StartDemo < 0) {
        level = DC_LevelSelect_Run(engine, mod_id);
    }
    g_DC_TestLevelStart = level;
    static char engine_arg[12];
    static char level_arg[12];
    static char *level_argv[] = {
        "trx", "-e", engine_arg, "-l", level_arg, nullptr,
    };
    snprintf(engine_arg, sizeof(engine_arg), "%d", engine);
    if (mod_id[0] != '\0') {
        static char *mod_argv[] = {
            "trx", "--mod", mod_id, "-l", level_arg, nullptr,
        };
        snprintf(level_arg, sizeof(level_arg), "%d", (int)level);
        argc = level >= 0 ? 5 : 3;
        argv = mod_argv;
    } else if (level >= 0) {
        snprintf(level_arg, sizeof(level_arg), "%d", (int)level);
        argc = 5;
        argv = level_argv;
    } else if (engine != 2) {
        // The title of the game on the disc.
        static char *title_argv[] = { "trx", "-e", engine_arg, nullptr };
        argc = 3;
        argv = title_argv;
    }
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
#if !defined(TRX_DC_START_LEVEL)
    if (args.result == 0) {
        // The game ended (the player left to the list of levels): the console
        // starts the disc over, so the list comes with all the memory clean,
        // however the last game used it. Leaving it to the exit path worked
        // the first time and not the second.
        printf("TRX: the game ended; starting the disc over\n");
        arch_reboot();
    }
#endif
    return args.result;
}
