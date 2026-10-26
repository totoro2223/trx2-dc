// Dreamcast platform layer: the strict build and the hang report. See
// dc_strict.h.

#include "dc_strict.h"

#include "vmu.h"

#include <arch/timer.h>
#include <dc/biosfont.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <kos/thread.h>

#include <malloc.h>
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
    const uint8_t *const colour = m_Colours[stage];
    vid_border_color(colour[0], colour[1], colour[2]);
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
    const uint32_t count = m_Count;
    const uint32_t first = count > M_RING ? count - M_RING : 0;
    for (uint32_t i = first; i < count && at < size; i++) {
        const M_CRUMB *const crumb = &m_Ring[i % M_RING];
        at += (size_t)snprintf(
            out + at, size - at, "%u %d %s f%u\n", (unsigned)crumb->ms,
            (int)crumb->stage, crumb->text, (unsigned)crumb->frame);
    }
    return at < size ? at : size - 1;
}

// Writes the report on the screen as the PowerVR last showed it: if the game
// has hung, nothing draws over it any more.
static void M_ShowOnScreen(const char *const report)
{
    if (vid_mode == nullptr) {
        return;
    }
    const uint32_t offset = PVR_GET(PVR_FB_ADDR) & 0x00FFFFFFu;
    uint16_t *const fb = (uint16_t *)((uintptr_t)vram_l + offset);
    const uint32_t width = (uint32_t)vid_mode->width;
    char line[64];
    uint32_t y = 8;
    const char *p = report;
    while (*p != '\0' && y + BFONT_HEIGHT < (uint32_t)vid_mode->height) {
        size_t n = 0;
        while (p[n] != '\0' && p[n] != '\n' && n < sizeof(line) - 1) {
            n++;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        bfont_draw_str_ex(fb + y * width + 8, width, 0xFFFF, 0x0000, 16, true, line);
        y += BFONT_HEIGHT;
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
}

static void *M_Watch(void *const param)
{
    (void)param;
    bool written = false;
    uint32_t shown_at = 0;
    while (true) {
        thd_sleep(1000);
        if (!m_Running) {
            continue;
        }
        const uint32_t now = M_Now();
        const uint32_t idle = (now - m_BeatMs) / 1000u;
        if (idle < DC_STALL_SECONDS) {
            written = false;
            shown_at = 0;
            continue;
        }
        char report[M_REPORT_BYTES];
        M_BuildReport(report, sizeof(report), idle);
        // Again every few seconds, the count of seconds moving.
        if (idle >= shown_at + 5) {
            shown_at = idle;
            printf("[crumb] %s", report);
            M_ShowOnScreen(report);
        }
        if (!written && idle >= 2 * DC_STALL_SECONDS) {
            written = true;
            DC_VMU_WriteLog("TRXLOG", report);
        }
    }
    return nullptr;
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
