// Dreamcast platform layer: the strict build and the hang report. See
// dc_strict.h.

#include "dc_strict.h"
#include "dc_level_heap.h"

#include "render/pvr.h"
#include "vmu.h"

#include <arch/irq.h>
#include <arch/timer.h>
#include <dc/biosfont.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <kos/thread.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define M_RING 16
#define M_TEXT 40
#define M_REPORT_BYTES 1024
// What the heap is asked for at the start, from the most down.
#define M_POISON_FROM (14u * 1024u * 1024u)
#define M_POISON_TO (64u * 1024u)
// The colour of the screen's border at each stage (red, orange, yellow,
// green, cyan, blue, magenta, then black).
static const uint8_t m_Colours[][3] = {
    { 0, 0, 0 },     { 255, 0, 0 },   { 255, 128, 0 }, { 255, 255, 0 },
    { 0, 255, 0 },   { 0, 255, 255 }, { 0, 0, 255 },   { 255, 0, 255 },
    { 0, 0, 0 },
};

typedef struct {
    uint32_t ms;
    uint32_t frame;
    uint8_t stage;
    char text[M_TEXT];
} M_CRUMB;

static M_CRUMB m_Ring[M_RING];
static volatile uint32_t m_Count = 0;
static volatile uint32_t m_Frames = 0;
static volatile uint32_t m_BeatMs = 0;
static volatile bool m_Running = false;
static volatile DC_CRUMB m_Stage = 0;
static volatile uint32_t m_StageFrame = 0;

// The trail of steps (DC_Trail), last ones kept.
static char m_TrailText[600];
static size_t m_TrailLength = 0;

static uint32_t M_Now(void)
{
    return (uint32_t)timer_ms_gettime64();
}

void DC_Crumb(const DC_CRUMB stage, const char *const text)
{
    const uint32_t at = m_Count % M_RING;
    M_CRUMB *const crumb = &m_Ring[at];
    crumb->ms = M_Now();
    crumb->frame = m_Frames;
    crumb->stage = (uint8_t)stage;
    snprintf(crumb->text, sizeof(crumb->text), "%s", text);
    m_Count++;
    m_Stage = stage;
    m_StageFrame = m_Frames;
    // (The border stays black: it takes the stage's colour only when the game
    // stalls, in M_Watch.)
    // A stage is a sign of life too.
    m_BeatMs = crumb->ms;
    printf("[crumb] %d %s (frame %u)\n", (int)stage, text, (unsigned)m_Frames);
}

void DC_Crumb_Beat(void)
{
    m_BeatMs = M_Now();
}

void DC_Crumb_Frame(void)
{
    m_Frames++;
    m_BeatMs = M_Now();
    // The first frame the engine draws after starting or after a level
    // loads, and a second later the border goes black: running.
    const uint32_t since = m_Frames - m_StageFrame;
    if (since == 1 && (m_Stage == DC_CRUMB_ENGINE || m_Stage == DC_CRUMB_LOADED)) {
        DC_Crumb(DC_CRUMB_FIRST_FRAME, "first frame");
    } else if (since == 60 && m_Stage == DC_CRUMB_FIRST_FRAME) {
        DC_Crumb(DC_CRUMB_RUNNING, "running");
    }
#if defined(TRX_DC_TEST_HANG_FRAMES)
    // A hang to try the report with: the game stops here.
    if (m_Frames == TRX_DC_TEST_HANG_FRAMES) {
        for (;;) {
            __asm__ volatile("" ::: "memory");
        }
    }
#endif
}

// What the report says: the last stages, newest last.
static size_t M_BuildReport(char *const out, const size_t size, const uint32_t idle)
{
    size_t at = 0;
    at += (size_t)snprintf(
        out + at, size - at, "TRX STALLED %us, frame %u\n", (unsigned)idle,
        (unsigned)m_Frames);
    // The last steps of the trail, whole lines, then the last stages.
    const char *steps =
        m_TrailText + (m_TrailLength > 360 ? m_TrailLength - 360 : 0);
    const char *const eol = strchr(steps, '\n');
    if (steps != m_TrailText && eol != NULL) {
        steps = eol + 1;
    }
    at += (size_t)snprintf(out + at, size - at, "%s", steps);
    const struct mallinfo info = mallinfo();
    at += (size_t)snprintf(
        out + at, size - at, "heap %uK free %uK vram %uK\n",
        (unsigned)(info.uordblks / 1024), (unsigned)(info.fordblks / 1024),
        (unsigned)(pvr_mem_available() / 1024));
    const uint32_t count = m_Count;
    const uint32_t first = count > 6 ? count - 6 : 0;
    for (uint32_t i = first; i < count && at < size; i++) {
        const M_CRUMB *const crumb = &m_Ring[i % M_RING];
        at += (size_t)snprintf(
            out + at, size - at, "%d %s f%u\n", (int)crumb->stage,
            crumb->text, (unsigned)crumb->frame);
    }
    return at < size ? at : size - 1;
}

// The most characters of a line the loading screen's text takes (the BIOS
// font is 12 wide, the text 512).
#define M_LINE_CHARS 41
#define M_PAGE_LINES 16
// How long each step of the trail stays on the screen.
#define DC_TRAIL_HOLD_MS 500

// Writes the report on the screen, two lines at a time, white on black in the
// BIOS font, through the renderer's loading screen (the one that shows the
// loading bar and the memory card's notices). Writing into the frame buffer by
// hand put garbage over the screens instead. Not from an exception, where
// the PowerVR cannot be driven, and only from the engine's thread.
// The report in the top left corner of the screen, in the BIOS font on black,
// written straight into the frame buffer being shown: it needs neither the
// renderer nor the engine's thread, so it works from the hang watcher and from
// an exception, where the PowerVR may be stuck.
static void M_DrawBios(const char *const report)
{
    if (vid_mode == NULL || vid_mode->pm != PM_RGB565) {
        return;
    }
    const int width = vid_mode->width;
    const int cols = width / 12;
    uint16_t *const fb = (uint16_t *)(0xA5000000u
        + (*(volatile uint32_t *)0xA05F8050u & 0x00FFFFFFu));
    int rows = 0;
    for (const char *p = report; *p != '\0'; p++) {
        rows += *p == '\n';
    }
    rows = rows > 19 ? 19 : rows + 1;
    memset(fb, 0, (size_t)width * (size_t)(rows * 24) * 2u);
    bfont_set_foreground_color(0xFFFFFFFFu);
    bfont_set_background_color(0xFF000000u);
    const char *p = report;
    for (int row = 0; row < rows && *p != '\0'; row++) {
        char line[64];
        size_t len = 0;
        while (p[len] != '\0' && p[len] != '\n' && len < (size_t)cols
               && len < sizeof(line) - 1) {
            line[len] = p[len];
            len++;
        }
        line[len] = '\0';
        bfont_draw_str(fb + row * 24 * width, (uint32_t)width, 1, line);
        p += len;
        while (*p != '\0' && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
    }
}

static void M_ShowOnScreen(const char *const report)
{
    if (irq_inside_int()) {
        return;
    }
    char lines[M_PAGE_LINES][M_LINE_CHARS + 1];
    int n = 0;
    const char *p = report;
    while (*p != '\0' && n < M_PAGE_LINES) {
        size_t len = 0;
        while (p[len] != '\0' && p[len] != '\n' && len < M_LINE_CHARS) {
            len++;
        }
        memcpy(lines[n], p, len);
        lines[n][len] = '\0';
        n++;
        p += len;
        if (*p == '\n') {
            p++;
        }
    }
    for (int i = 0; i < n; i += 2) {
        DC_PVR_DrawLoadingScreen(
            lines[i], i + 1 < n ? lines[i + 1] : "", (i + 2) * 100 / (n + 1));
        thd_sleep(3000);
    }
}

// TEMP: steps taken since the end of the level, counted even inside a frame.
static volatile int m_Step = 0;

static void *M_Watch(void *const param)
{
    (void)param;
    bool written = false;
    bool drawn = false;
    uint32_t shown_at = 0;
    while (true) {
        thd_sleep(1000);
        if (!m_Running) {
            continue;
        }
        const uint32_t now = M_Now();
        const uint32_t idle = (now - m_BeatMs) / 1000u;
        if (idle < DC_STALL_SECONDS) {
            if (shown_at != 0) {
                vid_border_color(0, 0, 0);
            }
            written = false;
            drawn = false;
            shown_at = 0;
            continue;
        }
        // TEMP: the true last step, from outside the stuck engine thread.
        DC_VMU_ShowStep(m_Step);
        const uint8_t *const colour = m_Colours[m_Stage];
        vid_border_color(colour[0], colour[1], colour[2]);
        char report[M_REPORT_BYTES];
        M_BuildReport(report, sizeof(report), idle);
        // On the screen every second (the count of seconds moving), whatever
        // the engine thread is stuck in.
        M_DrawBios(report);
        // Again every few seconds, in the serial log.
        if (idle >= shown_at + 5) {
            shown_at = idle;
            printf("[crumb] %s", report);
            // Drawn from here too, once: if the engine is stuck inside a frame the
            // renderer refuses, and if it is stuck anywhere else this shows where.
            if (!drawn) {
                drawn = true;
                M_ShowOnScreen(report);
            }
        }
        if (!written && idle >= 2 * DC_STALL_SECONDS) {
            written = true;
            DC_VMU_WriteLog("TRXLOG", report);
        }
    }
    return nullptr;
}

// An assertion failed (trx/debug.h): the file and line, what failed and the
// last stages, on the screen and in TRXLOG, for 20 s before the restart.
void DC_Crumb_Fatal(const char *const file, const int line, const char *const message)
{
    static bool m_Done = false;
    if (m_Done) {
        return;
    }
    m_Done = true;
    // The file's name without its directories.
    const char *name = file;
    for (const char *p = file; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    char report[M_REPORT_BYTES];
    size_t at = (size_t)snprintf(
        report, sizeof(report), "TRX STOPPED, frame %u\n%s:%d\n%.120s\n",
        (unsigned)m_Frames, name, line, message);
    // The memory when it stopped: the level's region (used / size, the largest
    // free span) and the heap's (in use, free): a failed allocation is told
    // apart from a bad pointer by it.
    DC_LEVEL_HEAP_STATS heap;
    DC_LevelHeap_GetStats(&heap);
    const struct mallinfo info = mallinfo();
    at += (size_t)snprintf(
        report + at, sizeof(report) - at, "region %d/%dK big %dK\nheap %uK free %uK\n",
        (int)heap.used_kb, (int)heap.size_kb, (int)heap.largest_free_kb,
        (unsigned)(info.uordblks / 1024), (unsigned)(info.fordblks / 1024));
    // The last stages, the newest last.
    const uint32_t count = m_Count;
    const uint32_t first = count > 8 ? count - 8 : 0;
    for (uint32_t i = first; i < count && at < sizeof(report); i++) {
        const M_CRUMB *const crumb = &m_Ring[i % M_RING];
        at += (size_t)snprintf(
            report + at, sizeof(report) - at, "%u %d %s f%u\n",
            (unsigned)crumb->ms, (int)crumb->stage, crumb->text,
            (unsigned)crumb->frame);
    }
    printf("[fatal] %s", report);
    M_DrawBios(report);
    if (irq_inside_int()) {
        // An exception: nothing can sleep, draw or reach the card here.
        const uint32_t until = M_Now() + 20000;
        while ((int32_t)(M_Now() - until) < 0) {
        }
        return;
    }
    DC_VMU_WriteLog("TRXLOG", report);
    // The report goes round on the screen for a minute.
    for (int round = 0; round < 6; round++) {
        M_ShowOnScreen(report);
    }
}

// What a run leaves for the next boot: the soft reset keeps RAM, as dc_games.c
// relies on, so a note a few bytes above its own survives the restart.
#define M_NOTE_ADDRESS 0xACFF0200u
#define M_NOTE_MAGIC 0x54525845u

typedef struct {
    uint32_t magic;
    uint32_t kind;
    uint32_t code;
    uint32_t pc;
    uint32_t pr;
    uint32_t sp;
    uint32_t tea;
    uint32_t frame;
    char text[400];
} M_NOTE;

static const char *m_ErrorMessage = NULL;

static void M_NoteSave(
    const uint32_t kind, const uint32_t code, const uint32_t pc,
    const uint32_t pr, const uint32_t sp, const uint32_t tea,
    const char *const text)
{
    volatile M_NOTE *const note = (volatile M_NOTE *)M_NOTE_ADDRESS;
    note->magic = 0;
    note->kind = kind;
    note->code = code;
    note->pc = pc;
    note->pr = pr;
    note->sp = sp;
    note->tea = tea;
    note->frame = m_Frames;
    size_t i = 0;
    for (; i < sizeof(note->text) - 1 && text[i] != '\0'; i++) {
        note->text[i] = text[i];
    }
    note->text[i] = '\0';
    note->magic = M_NOTE_MAGIC;
}

// A processor exception (a bad address, an illegal instruction): from inside
// the handler nothing can sleep or reach the memory card, so it is the screen
// only, held with a spin for 20 s.
void DC_Crumb_Exception(
    const unsigned code, const unsigned long pc, const unsigned long pr,
    const unsigned long sp, const unsigned long tea)
{
    char report[M_REPORT_BYTES];
    size_t at = (size_t)snprintf(
        report, sizeof(report),
        "TRX EXCEPTION %04x, frame %u\npc %08lx pr %08lx\nsp %08lx addr %08lx\n",
        code, (unsigned)m_Frames, pc, pr, sp, tea);
    const size_t crumbs_at = at;
    const uint32_t count = m_Count;
    const uint32_t first = count > 8 ? count - 8 : 0;
    for (uint32_t i = first; i < count && at < sizeof(report); i++) {
        const M_CRUMB *const crumb = &m_Ring[i % M_RING];
        at += (size_t)snprintf(
            report + at, sizeof(report) - at, "%d %s f%u\n",
            (int)crumb->stage, crumb->text, (unsigned)crumb->frame);
    }
    // The screen may be black or not the one shown, and the restart that
    // follows wipes it: the next boot shows the same report (DC_Crumb_ReportLast).
    M_NoteSave(1, code, pc, pr, sp, tea, report + crumbs_at);
    M_DrawBios(report);
    M_ShowOnScreen(report);
    const uint32_t until = M_Now() + 20000;
    while ((int32_t)(M_Now() - until) < 0) {
    }
}

// The report of the last run, kept until the engine has the PowerVR up and the
// screen can show it (DC_Crumb_ShowPrevious).
static char m_Previous[M_REPORT_BYTES];
static bool m_HasPrevious = false;

void DC_Crumb_ShowPrevious(void)
{
    if (m_HasPrevious) {
        m_HasPrevious = false;
        M_ShowOnScreen(m_Previous);
    }
}

// The engine's fatal error (Shell_ExitSystem): it ends the game with no
// exception, so only this note tells why.
void DC_Crumb_Error(const char *const message)
{
    M_NoteSave(2, 0, 0, 0, 0, 0, message);
    m_ErrorMessage = message;
}

// TEMP: a line of the trail the game leaves on the memory card as it goes
// through the end of a level: a console that restarts with nothing on the
// screen shows, next boot, how far it got (DC_Crumb_ReportLast).
void DC_Trail(const char *const text)
{
    char *const trail = m_TrailText;
    size_t length = m_TrailLength;
    // The oldest half goes when the buffer is nearly full.
    if (length > sizeof(m_TrailText) - 80) {
        memmove(trail, trail + 300, length - 300);
        length -= 300;
    }
    ++m_Step;
    // TEMP: the card is only written outside a frame; the console may stop on
    // a maple transfer while the PowerVR has a scene open.
    if (!DC_PVR_IsInFrame()) {
        DC_VMU_ShowStep(m_Step);
    }
    const int step = m_Step;
    length += (size_t)snprintf(
        trail + length, sizeof(m_TrailText) - length, "%u %s\n", (unsigned)M_Now(), text);
    if (length >= sizeof(m_TrailText)) {
        length = sizeof(m_TrailText) - 1;
    }
    m_TrailLength = length;
    printf("[trail] %s\n", text);
    // In RAM, which the restart keeps: the card can be full, and the console
    // may restart before a write to it ends.
    M_NoteSave(3, 0, 0, 0, 0, 0, trail + (length > 380 ? length - 380 : 0));
    // And on the screen, held a moment: the step, and the one before it. Whatever
    // is the last one read before the console restarts says where it did.
    // No longer drawn on the loading screen: the steps stay in the VMU's
    // blocks, the RAM note and the serial log.
    (void)step;
}


// TEMP: a second kind of step, for what happens often: it is only taken from the
// end of a level on, and at most so many (a step costs half a second).
static bool m_Tracing = false;
static int m_TraceBudget = 0;

void DC_TraceOn(const bool on)
{
    if (on) {
        m_Step = 0;
    }
    m_Tracing = on;
    m_TraceBudget = on ? 160 : 0;
}

void DC_TraceF(const char *const fmt, ...)
{
    if (!m_Tracing || m_TraceBudget <= 0) {
        return;
    }
    m_TraceBudget--;
    char text[64];
    va_list va;
    va_start(va, fmt);
    vsnprintf(text, sizeof(text), fmt, va);
    va_end(va);
    DC_Trail(text);
}

void DC_TrailF(const char *const fmt, ...)
{
    char text[64];
    va_list va;
    va_start(va, fmt);
    vsnprintf(text, sizeof(text), fmt, va);
    va_end(va);
    DC_Trail(text);
}

// TEMP: how much of the engine thread's stack has been touched so far (its
// marker, left by dc_main.c, is gone from what was used), on the serial line.
void DC_Crumb_StackUse(const char *const where)
{
    const kthread_t *const thread = thd_get_current();
    const uint8_t *const bottom = (const uint8_t *)thread->stack;
    size_t untouched = 0;
    while (untouched < thread->stack_size && bottom[untouched] == 0xA5) {
        untouched++;
    }
    printf(
        "[stack] %s: %uK of %uK used\n", where,
        (unsigned)((thread->stack_size - untouched) / 1024),
        (unsigned)(thread->stack_size / 1024));
}

// TEMP: the engine ends (Shell_Terminate), on the screen for 15 s and in
// TRXLOG with the error that caused it, if any, and the last stages: a console
// that restarts at the end of a level leaves no other trace.
void DC_Crumb_Exit(const int code)
{
    char report[M_REPORT_BYTES];
    size_t at = (size_t)snprintf(
        report, sizeof(report), "TRX ENDED, code %d, frame %u\n%.200s\n", code,
        (unsigned)m_Frames, m_ErrorMessage != NULL ? m_ErrorMessage : "no error: ended early");
    const uint32_t count = m_Count;
    const uint32_t first = count > 8 ? count - 8 : 0;
    for (uint32_t i = first; i < count && at < sizeof(report); i++) {
        const M_CRUMB *const crumb = &m_Ring[i % M_RING];
        at += (size_t)snprintf(
            report + at, sizeof(report) - at, "%d %s f%u\n", (int)crumb->stage,
            crumb->text, (unsigned)crumb->frame);
    }
    printf("[exit] %s", report);
    // A clean end that comes before the game has run for a second after a load
    // is not the player leaving: it is shown like an error, or the console
    // would just start over with nothing on the screen.
    const bool early = m_Stage == DC_CRUMB_LOADING || m_Stage == DC_CRUMB_LOADED
        || m_Stage == DC_CRUMB_FIRST_FRAME || m_Stage == DC_CRUMB_ENGINE;
    if (code == 0 && m_ErrorMessage == NULL && !early) {
        // A way out the player took: nothing for the next boot to report.
        ((volatile M_NOTE *)M_NOTE_ADDRESS)->magic = 0;
        return;
    }
    M_ShowOnScreen(report);
    DC_VMU_WriteLog("TRXLOG", report);
}

// At the start of the disc: if the run before ended in an exception or an
// error, says so on the screen for 15 s and in TRXLOG.
void DC_Crumb_ReportLast(void)
{
    volatile M_NOTE *const note = (volatile M_NOTE *)M_NOTE_ADDRESS;
    printf("[crash] note at boot: magic %08x\n", (unsigned)note->magic);
    if (note->magic != M_NOTE_MAGIC) {
        return;
    }
    note->magic = 0;
    char text[sizeof(note->text)];
    for (size_t i = 0; i < sizeof(text); i++) {
        text[i] = note->text[i];
    }
    text[sizeof(text) - 1] = '\0';
    char report[M_REPORT_BYTES];
    if (note->kind == 1) {
        snprintf(
            report, sizeof(report),
            "LAST RUN: EXCEPTION %04x, frame %u\npc %08x pr %08x\nsp %08x addr %08x\n%s",
            (unsigned)note->code, (unsigned)note->frame, (unsigned)note->pc,
            (unsigned)note->pr, (unsigned)note->sp, (unsigned)note->tea, text);
    } else {
        snprintf(
            report, sizeof(report), "LAST RUN: %s, frame %u\n%s",
            note->kind == 3 ? "TRAIL" : "ERROR", (unsigned)note->frame, text);
    }
    printf("[crash] %s", report);
    snprintf(m_Previous, sizeof(m_Previous), "%s", report);
    m_HasPrevious = true;
}

// abort() (Lua's panic, an assert of the C library) returns to the loader at
// once: this puts the reason on the screen first (linked with --wrap=abort).
void __real_abort(void) __attribute__((noreturn));

void __wrap_abort(void)
{
    DC_Crumb_Fatal("abort", 0, "abort() was called");
    __real_abort();
}

// What KOS does with an exception nobody handled, and with its own panics.
void __real_arch_panic(const char *fmt, ...) __attribute__((noreturn));
void __real_arch_abort(void) __attribute__((noreturn));

void __wrap_arch_panic(const char *const fmt, ...)
{
    char message[160];
    va_list va;
    va_start(va, fmt);
    vsnprintf(message, sizeof(message), fmt, va);
    va_end(va);
    DC_Crumb_Fatal("kos panic", 0, message);
    __real_arch_panic("%s", message);
}

void __wrap_arch_abort(void)
{
    DC_Crumb_Fatal("arch_abort", 0, "arch_abort() was called");
    __real_arch_abort();
}

void DC_Crumb_StartWatch(void)
{
    m_BeatMs = M_Now();
    m_Running = true;
    const kthread_attr_t attr = {
        .create_detached = true,
        .stack_size = 8 * 1024,
        .label = "trx-hang",
    };
    thd_create_ex(&attr, M_Watch, nullptr);
}

#if defined(TRX_DC_STRICT)
void __real_free(void *ptr);

void DC_Strict_PoisonRAM(void)
{
    // The biggest block the heap can give, filled and handed back whole:
    // what the game takes from it next is garbage, as it is on the console.
    for (size_t size = M_POISON_FROM; size >= M_POISON_TO; size -= 256u * 1024u) {
        void *const block = malloc(size);
        if (block == nullptr) {
            continue;
        }
        memset(block, DC_STRICT_FRESH, size);
        __real_free(block);
        printf("[strict] %u KiB of the heap filled with %02x\n",
            (unsigned)(size / 1024), DC_STRICT_FRESH);
        return;
    }
}

void DC_Strict_FillNew(void *const ptr, const size_t size)
{
    if (ptr != nullptr) {
        memset(ptr, DC_STRICT_FRESH, size);
    }
}

void DC_Strict_FillFreed(void *const ptr)
{
    if (ptr != nullptr) {
        memset(ptr, DC_STRICT_FREED, malloc_usable_size(ptr));
    }
}
#else
void DC_Strict_PoisonRAM(void) {}
void DC_Strict_FillNew(void *const ptr, const size_t size) { (void)ptr; (void)size; }
void DC_Strict_FillFreed(void *const ptr) { (void)ptr; }
#endif
