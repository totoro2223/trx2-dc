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
#include "dc_games.h"
#include "dc_settings.h"
#include "dc_level_select.h"
#include "dc_mode.h"
#include "dc_watchdog.h"
#include "vmu.h"

#include <arch/arch.h>
#include <arch/irq.h>
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

// A file of the running game's folder.
static FILE *M_Open(const char *const name)
{
    char path[96];
    DC_Games_Path(path, sizeof(path), name);
    return fopen(path, "r");
}

// A crash (a failed assertion, a bad address, an out of memory) would stop the
// console with the screen as it was; the disc starts over instead, at the list
// of levels of the same game, which is all the flow a game that cannot go on
// can keep. Three in a row stop it for good, since they would only loop.
static void M_Recover(
    const irq_t code, irq_context_t *const context, void *const data)
{
    (void)data;
    printf(
        "TRX: exception %04x at %08lx: starting the disc over\n",
        (unsigned)code, (unsigned long)context->pc);
    if (DC_Games_Recover()) {
        arch_reboot();
    }
    arch_abort();
}

static void M_InstallRecovery(void)
{
    static const irq_t codes[] = {
        EXC_ILLEGAL_INSTR, EXC_SLOT_ILLEGAL_INSTR, EXC_DATA_ADDRESS_READ,
        EXC_DATA_ADDRESS_WRITE, EXC_TRAPA,
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        irq_set_handler(codes[i], M_Recover, nullptr);
    }
}

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

    setenv("TRX_DIR", g_DC_Root, 1);
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

// Where the engine's thread says how it ended (see DC_Main_EndGame).
static M_MAIN_ARGS *m_Running = nullptr;

void DC_Main_EndGame(const int result)
{
    if (m_Running != nullptr) {
        m_Running->result = result;
    }
    // The engine has shut down: its thread ends here, and dc_main goes on
    // (exit() from this thread crashed the console in TR2).
    thd_exit(nullptr);
}

// Runs the game of the disc's folder once, from the choice of level to the
// engine's end. Returns what the engine returned.
static int M_RunGame(int argc, char *argv[])
{
    // A run begins with none of the last one's test switches.
    g_DC_Run++;
    g_DC_TestMode = -1;
    g_DC_TestShadow = -1;
    g_DC_TestSwitchMode = -1;
    g_DC_TestSwitchFrame = 0;
    g_DC_TestLevelStart = 0;
    g_DC_TestLevelLast = -1;
    g_DC_TestLevelFrames = 0;
    g_DC_TestFire = 0;
    g_DC_TestGive = 0;
    g_DC_TestExit = 0;
    g_DC_TestSave = 0;
    g_DC_DemoQueueCount = 0;
    g_DC_DemoQueueNext = 0;
    g_DC_StartDemo = -1;
    g_DC_StartCutscene = -1;
    g_DC_BenchRequested = false;

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
    FILE *const game = M_Open("GAME.TXT");
    if (game != nullptr) {
        if (fscanf(game, "%d", &engine) != 1) {
            engine = 2;
        }
        fclose(game);
    }
    // An expansion on the disc (make_disc.sh's TRX_DC_MOD): the engine runs
    // it by name, as --mod does.
    static char mod_id[32];
    FILE *const mod_file = M_Open("MOD.TXT");
    if (mod_file != nullptr) {
        if (fscanf(mod_file, "%31s", mod_id) != 1) {
            mod_id[0] = '\0';
        }
        fclose(mod_file);
    }
    // An expansion the menu of a disc of several games asked for.
    if (DC_Games_Expansion()[0] != '\0') {
        snprintf(mod_id, sizeof(mod_id), "%s", DC_Games_Expansion());
    }
    // A test disc that starts in one of the title's demos (make_disc.sh's
    // TRX_DC_DEMO).
    FILE *const demo = M_Open("DEMO.TXT");
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
    FILE *const bench = M_Open("BENCH.TXT");
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
    FILE *const mode = M_Open("MODE.TXT");
    if (mode != nullptr) {
        int number = -1;
        if (fscanf(mode, "%d", &number) == 1) {
            g_DC_TestMode = number;
        }
        fclose(mode);
    }
    // A test disc that fixes the shadow type (make_disc.sh's TRX_DC_SHADOW):
    // the SHADOW_TYPE's number.
    FILE *const shadow = M_Open("SHADOW.TXT");
    if (shadow != nullptr) {
        int number = -1;
        if (fscanf(shadow, "%d", &number) == 1) {
            g_DC_TestShadow = number;
        }
        fclose(shadow);
    }
    // A test disc that makes the pistols smoke every n frames
    // (make_disc.sh's TRX_DC_FIRE).
    FILE *const fire = M_Open("FIRE.TXT");
    if (fire != nullptr) {
        int number = 0;
        if (fscanf(fire, "%d", &number) == 1) {
            g_DC_TestFire = number;
        }
        fclose(fire);
    }
    // A test disc that leaves the game after n frames (make_disc.sh's
    // TRX_DC_EXIT).
    FILE *const leave = M_Open("EXIT.TXT");
    if (leave != nullptr) {
        int number = 0;
        if (fscanf(leave, "%d", &number) == 1) {
            g_DC_TestExit = number;
        }
        fclose(leave);
    }
    // A test disc that saves and loads games (make_disc.sh's TRX_DC_SAVE).
    FILE *const saves = M_Open("SAVE.TXT");
    if (saves != nullptr) {
        int number = 0;
        if (fscanf(saves, "%d", &number) == 1) {
            g_DC_TestSave = number;
        }
        fclose(saves);
    }
    // A test disc that gives Lara a weapon (make_disc.sh's TRX_DC_GIVE).
    FILE *const give = M_Open("GIVE.TXT");
    if (give != nullptr) {
        int number = 0;
        if (fscanf(give, "%d", &number) == 1) {
            g_DC_TestGive = number;
        }
        fclose(give);
    }
    // A test disc that changes the mode on a frame (make_disc.sh's
    // TRX_DC_SWITCH): the mode's number and the frame.
    FILE *const change = M_Open("SWITCH.TXT");
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
    FILE *const levels = M_Open("LEVELS.TXT");
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
    FILE *const start = M_Open("START.TXT");
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
        // The expansion's level that the list of the game before picked.
        const int picked = DC_Games_TakeStartLevel();
        level = picked >= -1 ? picked : DC_LevelSelect_Run(engine, mod_id);
        if (level == DC_LEVEL_SELECT_RESTART) {
            // Another game or the expansion: the main loop starts it (the
            // choice is waiting in dc_games.c).
            return 1;
        }
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
    } else if (engine != 2 || g_DC_Run > 1) {
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
    m_Running = &args;
    thd_join(thread, nullptr);
    m_Running = nullptr;
    // The thread is over: its stack goes back.
    free(stack);
    return args.result;
}

int __wrap_main(int argc, char *argv[])
{
#if defined(TRX_DC_FB_CONSOLE)
    // Until the renderer owns the screen, show the log on it.
    dbgio_dev_select("fb");
#endif
    DC_Games_Init();
    M_InstallRecovery();
#if defined(TRX_DC_MMU_NULL_TRAP)
    // Without the MMU a null pointer reads the boot ROM and a null call runs
    // it, which crashes far from the cause. With it on and nothing mapped in
    // P0 (the engine runs in P1), both raise a TLB miss that KOS reports.
    mmu_init_basic();
#endif
    int result;
    while (true) {
        M_SetupEnvironment();
        printf("TRX: Dreamcast platform layer ready\n");
        result = M_RunGame(argc, argv);
        // The title or the level list asked for another game: it starts here,
        // in the same run of the console, with the last one's memory given
        // back.
        if (!DC_Games_ApplyPending()) {
            break;
        }
    }
#if !defined(TRX_DC_START_LEVEL)
    if (result == 0) {
        // The game ended (the player left to the list of levels): the console
        // starts the disc over, so the list comes with all the memory clean.
        printf("TRX: the game ended; starting the disc over\n");
        fflush(stdout);
        DC_Games_Remember();
        arch_reboot();
    }
#endif
    return result;
}
