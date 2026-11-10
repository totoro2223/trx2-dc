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
#include "dc_strict.h"
#include "dc_level_select.h"
#include "dc_splash.h"
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

// Newlib declares it only for POSIX builds.
int setenv(const char *name, const char *value, int overwrite);

#define DC_MAIN_STACK_SIZE (128 * 1024)

KOS_INIT_FLAGS(INIT_DEFAULT);

int __real_main(int argc, char *argv[]);

// A file of the running game's folder.
bool g_DC_QuietLog = false;

static FILE *M_Open(const char *const name)
{
    char path[96];
    DC_Games_Path(path, sizeof(path), name);
    return fopen(path, "r");
}

static bool M_Flag(const char *const name)
{
    FILE *const file = M_Open(name);
    if (file == nullptr) {
        return false;
    }
    fclose(file);
    return true;
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
    DC_Crumb_Exception(
        (unsigned)code, (unsigned long)context->pc, (unsigned long)context->pr,
        (unsigned long)context->r[15], *(volatile unsigned long *)0xFF00000Cu);
    if (DC_Games_Recover()) {
        arch_reboot();
    }
    arch_abort();
}

static void M_InstallRecovery(void)
{
    static const irq_t codes[] = {
        EXC_ILLEGAL_INSTR, EXC_SLOT_ILLEGAL_INSTR, EXC_DATA_ADDRESS_READ,
        EXC_DATA_ADDRESS_WRITE, EXC_TRAPA, EXC_DMA_DMAE, EXC_GENERAL_FPU,
        EXC_SLOT_FPU, EXC_FPU, EXC_NMI, EXC_DTLB_MISS_READ, EXC_DTLB_MISS_WRITE,
        EXC_DTLB_PV_READ, EXC_DTLB_PV_WRITE, EXC_ITLB_PV,
        EXC_INITIAL_PAGE_WRITE,
        // A jump to a bad or odd address (a corrupted return address, a bad
        // function pointer) lands here, and the KOS default for these gave no
        // report.
        EXC_INSTR_ADDRESS, EXC_ITLB_MISS, EXC_DTLB_MULTIPLE,
        EXC_ITLB_MULTIPLE, EXC_USER_BREAK_PRE, EXC_USER_BREAK_POST,
        EXC_DOUBLE_FAULT, EXC_UNHANDLED_EXC,
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
    DC_VMU_DrawLogo();
}

typedef struct {
    int argc;
    char **argv;
    int result;
} M_MAIN_ARGS;

static void *M_MainThread(void *const param)
{
    M_MAIN_ARGS *const args = param;
    DC_Crumb(DC_CRUMB_ENGINE, "engine thread");
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

// Set when the last run ended with the player leaving: the next starts at the
// game's title.
static bool m_ToTitle = false;

// Runs the game of the disc's folder once, from the choice of level to the
// engine's end. Returns what the engine returned.
static int M_RunGame(int argc, char *argv[])
{
    // A run begins with none of the last one's test switches.
    g_DC_Run++;
    g_DC_TestMode = -1;
    g_DC_TestShadow = -1;
    g_DC_TestCel = -1;
    g_DC_TestOriginalAI = -1;
    g_DC_TestFps = -1;
    g_DC_TestWide = -1;
    g_DC_TestSnow = -1;
    g_DC_TestFogStart = -1;
    g_DC_TestFogEnd = -1;
    g_DC_TestRoomCel = -1;
    g_DC_TestBump = -1;
    g_DC_TestObjectShadows = -1;
    g_DC_TestResetOptions = 0;
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
    // Static for the arguments below, and not to remember the last game's.
    mod_id[0] = 0;
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
    // A test disc that fixes the cel shading and the room cel shading
    // (make_disc.sh's TRX_DC_CEL): "<cel> <room cel>", each 0 or 1.
    FILE *const cel = M_Open("CEL.TXT");
    if (cel != nullptr) {
        int a = -1;
        int b = -1;
        int c = -1;
        if (fscanf(cel, "%d %d %d", &a, &b, &c) >= 2) {
            g_DC_TestCel = a;
            g_DC_TestRoomCel = b;
            g_DC_TestBump = c;
        }
        fclose(cel);
    }
    // A test disc that fixes the frame rate (make_disc.sh's TRX_DC_FPS).
    FILE *const fps = M_Open("FPS.TXT");
    if (fps != nullptr) {
        int n = -1;
        if (fscanf(fps, "%d", &n) == 1) {
            g_DC_TestFps = n;
        }
        fclose(fps);
    }
    // A test disc that starts in one of the game's cutscenes (make_disc.sh's
    // TRX_DC_CUTSCENE): the cutscene's number.
    FILE *const cine = M_Open("CUTSCENE.TXT");
    if (cine != nullptr) {
        int number = -1;
        if (fscanf(cine, "%d", &number) == 1 && number >= 0) {
            g_DC_StartCutscene = number;
        }
        fclose(cine);
    }
    // A test disc that fixes the screen (make_disc.sh's TRX_DC_WIDE).
    FILE *const wide = M_Open("WIDE.TXT");
    if (wide != nullptr) {
        int on = -1;
        if (fscanf(wide, "%d", &on) == 1) {
            g_DC_TestWide = on;
        }
        fclose(wide);
    }
    // A test disc that fixes the enemy simulation (make_disc.sh's TRX_DC_AI).
    FILE *const ai = M_Open("AI.TXT");
    if (ai != nullptr) {
        int on = -1;
        if (fscanf(ai, "%d", &on) == 1) {
            g_DC_TestOriginalAI = on;
        }
        fclose(ai);
    }
    // A test disc that caps the weather particles (make_disc.sh's
    // TRX_DC_SNOW).
    FILE *const snow = M_Open("SNOW.TXT");
    if (snow != nullptr) {
        int n = -1;
        if (fscanf(snow, "%d", &n) == 1) {
            g_DC_TestSnow = n;
        }
        fclose(snow);
    }
    // A test disc that leaves assembly routines out (make_disc.sh's
    // TRX_DC_ASMOFF).
    g_DC_AsmOff = 0;
    // dc_turn, dc_sphere_touch and dc_bounds_collide stay in C: on a real console
    // the box collision in assembly killed Lara's first frame (build 1327).
    g_DC_AsmOff2 = 1024 | 2048 | 4096;
    FILE *const asm_off = M_Open("ASMOFF.TXT");
    if (asm_off != nullptr) {
        unsigned mask = 0;
        unsigned mask2 = 0;
        const int got = fscanf(asm_off, "%u %u", &mask, &mask2);
        if (got >= 1) {
            g_DC_AsmOff = mask;
        }
        if (got >= 2) {
            g_DC_AsmOff2 = mask2;
        }
        fclose(asm_off);
    }
    // A test disc with its own fog distances (make_disc.sh's TRX_DC_FOG).
    FILE *const fog = M_Open("FOG.TXT");
    if (fog != nullptr) {
        int a = -1;
        int b = -1;
        if (fscanf(fog, "%d %d", &a, &b) == 2) {
            g_DC_TestFogStart = a;
            g_DC_TestFogEnd = b;
        }
        fclose(fog);
    }
    // A test disc with the object shadows on (make_disc.sh's
    // TRX_DC_OBJECT_SHADOWS).
    FILE *const objshadow = M_Open("OBJSHADOW.TXT");
    if (objshadow != nullptr) {
        int on = -1;
        if (fscanf(objshadow, "%d", &on) == 1) {
            g_DC_TestObjectShadows = on;
        }
        fclose(objshadow);
    }
    // A test disc that wipes the render options off the memory card
    // (make_disc.sh's TRX_DC_RESETOPT).
    // The serial log blocks the game at the port's speed (~4 s of a level's
    // load at 115200 baud, with or without a cable): only a test disc keeps it
    // (make_disc.sh, LOG.TXT).
    FILE *const log = M_Open("LOG.TXT");
    FILE *const quiet = M_Open("NOLOG.TXT");
    if (quiet != nullptr) {
        // Off until the benchmark ends (DC_Bench_End).
        fclose(quiet);
        g_DC_QuietLog = true;
        dbgio_disable();
        if (log != nullptr) {
            fclose(log);
        }
    } else if (log != nullptr) {
        fclose(log);
    } else {
        dbgio_disable();
    }
    FILE *const reset = M_Open("RESETOPT.TXT");
    if (reset != nullptr) {
        g_DC_TestResetOptions = 1;
        fclose(reset);
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
    FILE *const debugkeys = M_Open("DEBUGKEYS.TXT");
    if (debugkeys != nullptr) {
        g_DC_TestDebugKeys = 1;
        fclose(debugkeys);
    }
    FILE *const showfps = M_Open("SHOWFPS.TXT");
    if (showfps != nullptr) {
        g_DC_TestShowFps = 1;
        fclose(showfps);
    }
    FILE *const nohud = M_Open("NOHUD.TXT");
    if (nohud != nullptr) {
        g_DC_TestNoHud = 1;
        fclose(nohud);
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
    // A test disc that completes the level after n frames (make_disc.sh's
    // TRX_DC_COMPLETE).
    FILE *const complete = M_Open("COMPLETE.TXT");
    if (complete != nullptr) {
        int number = 0;
        if (fscanf(complete, "%d", &number) == 1) {
            g_DC_TestComplete = number;
        }
        fclose(complete);
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
    // Only the disc's first start: a later one (the level list of this game,
    // asked for from its menu) has a choice of its own.
    FILE *const start = g_DC_Run <= 1 ? M_Open("START.TXT") : nullptr;
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
    } else if (m_ToTitle && !M_Flag("TITLELIST.TXT")) {
        // The player left a level: the game's own title, not the list (a disc
        // made with make_disc.sh's TRX_DC_TITLELIST goes back to the list).
        m_ToTitle = false;
    } else if (
        g_DC_DemoQueueCount == 0 && g_DC_StartDemo < 0
        && g_DC_StartCutscene < 0) {
        // The expansion's level that the list of the game before picked.
        const int picked = DC_Games_TakeStartLevel();
        DC_Splash_Show(engine);
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
        // By name: with no level the engine would take the game it ran last
        // (the expansion).
        static char base_id[8];
        snprintf(base_id, sizeof(base_id), "tr%d", engine);
        static char *title_argv[] = { "trx", "--mod", base_id, nullptr };
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
    DC_Crumb(DC_CRUMB_BOOT, "boot");
    DC_Crumb_StartWatch();
    DC_Strict_PoisonRAM();
    DC_Games_Init();
    DC_Crumb(DC_CRUMB_GAMES, "games ready");
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
        DC_Crumb_ReportLast();
        printf("TRX: Dreamcast platform layer ready\n");
        result = M_RunGame(argc, argv);
        // The title or the level list asked for another game: it starts here,
        // in the same run of the console, with the last one's memory given
        // back.
        if (!DC_Games_ApplyPending()) {
#if !defined(TRX_DC_START_LEVEL)
            // The player left to the list of levels: it comes back in this run.
            // arch_reboot() sent a DreamShell ISO back to DreamShell, so the
            // old restart is gone. shortcut: memory is not reset between
            // games, reboot again if long sessions run out of it.
            if (result == 0) {
                m_ToTitle = true;
                continue;
            }
#endif
            break;
        }
    }
    return result;
}
