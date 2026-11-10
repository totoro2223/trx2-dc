// The logic on its own thread (see dc_world.h, PLAN_60_FIJO.md phase 4).

#include "dc_world.h"

#include <trx/core/log.h>
#include <trx/game/interpolation.h>

#include <arch/timer.h>
#include <dc/pvr.h>
#include <dc/vblank.h>
#include <kos/sem.h>
#include <kos/mutex.h>
#include <kos/thread.h>

#include <stdatomic.h>

// One frame at 60, and the most the render thread may run ahead of it.
#define M_FRAME_US 16667
#define M_TICK_US 33333
#define M_STACK_SIZE (64 * 1024)
// Above the executor's thread (PRIO_DEFAULT, 10): lower runs first.
#define M_PRIORITY 8

static mutex_t m_Lock = MUTEX_INITIALIZER;
// Signalled at each vertical blank: a frame starts as the screen does.
static semaphore_t m_Vblank;
static int m_VblankHandle = -1;

static void M_OnVblank(const uint32_t code, void *const data)
{
    (void)code;
    (void)data;
    sem_signal(&m_Vblank);
}
static kthread_t *m_Thread = nullptr;
static DC_WORLD_DRAW m_Draw = nullptr;
static void *m_Arg = nullptr;
static atomic_bool m_Running = false;
static atomic_bool m_Paused = false;
// The render thread is waiting for the lock: the logic lets go at the next
// item.
static atomic_bool m_Waiting = false;
// The render thread is in the PowerVR's wait (outside the lock).
static atomic_bool m_InPvr = false;
// When the last tick ended, and whether a frame has shown it yet.
static uint64_t m_TickUs = 0;
static bool m_TickShown = true;

void DC_World_Lock(void)
{
    mutex_lock(&m_Lock);
}

void DC_World_Unlock(void)
{
    mutex_unlock(&m_Lock);
}

bool DC_World_IsRendering(void)
{
    return m_Thread != nullptr && !atomic_load(&m_Paused);
}

void DC_World_Yield(void)
{
    if (m_Thread == nullptr || atomic_load(&m_Paused)
        || !atomic_load(&m_Waiting)) {
        return;
    }
    mutex_unlock(&m_Lock);
    thd_pass();
    mutex_lock(&m_Lock);
}

// What the log tells every 600 frames: how long the render thread waited for
// the logic to let go, how long a frame took to draw, how long a tick took
// from start to end (with the frames drawn meanwhile), and the frames that
// started late.
static struct {
    uint32_t frames;
    uint32_t ticks;
    uint32_t late;
    uint64_t wait_sum;
    uint32_t wait_max;
    uint64_t draw_sum;
    uint32_t draw_max;
    uint64_t tick_sum;
    uint32_t tick_max;
    uint64_t tick_start;
} m_Stats = {};

void DC_World_TickBegin(void)
{
    m_Stats.tick_start = timer_us_gettime64();
}

void DC_World_TickDone(void)
{
    const uint32_t took = (uint32_t)(timer_us_gettime64() - m_Stats.tick_start);
    m_Stats.ticks++;
    m_Stats.tick_sum += took;
    m_Stats.tick_max = took > m_Stats.tick_max ? took : m_Stats.tick_max;
    Interpolation_DC_Publish();
    m_TickUs = timer_us_gettime64();
    m_TickShown = false;
}

static void *M_RenderThread(void *const unused)
{
    (void)unused;
    while (atomic_load(&m_Running)) {
        // The next vertical blank (the timeout only to see the stop); blanks
        // gone by while the last frame was drawn count as late, and are not
        // made up for.
        if (sem_wait_timed(&m_Vblank, 50) != 0) {
            continue;
        }
        while (sem_trywait(&m_Vblank) == 0) {
            m_Stats.late++;
        }
        if (atomic_load(&m_Paused)) {
            continue;
        }
        // The last frame's render, waited for with the world free.
        atomic_store(&m_InPvr, true);
        pvr_wait_ready();
        atomic_store(&m_InPvr, false);

        const uint64_t want = timer_us_gettime64();
        atomic_store(&m_Waiting, true);
        mutex_lock(&m_Lock);
        atomic_store(&m_Waiting, false);
        const uint64_t got = timer_us_gettime64();
        const uint32_t waited = (uint32_t)(got - want);
        m_Stats.wait_sum += waited;
        m_Stats.wait_max = waited > m_Stats.wait_max ? waited : m_Stats.wait_max;
        if (atomic_load(&m_Paused) || !atomic_load(&m_Running)) {
            mutex_unlock(&m_Lock);
            continue;
        }
        double frac = (double)(timer_us_gettime64() - m_TickUs) / M_TICK_US;
        frac = frac < 0.0 ? 0.0 : frac > 1.0 ? 1.0 : frac;
        const bool first = !m_TickShown;
        m_TickShown = true;
        Interpolation_DC_Swap(true);
        m_Draw(m_Arg, frac, first);
        Interpolation_DC_Swap(false);
        mutex_unlock(&m_Lock);
        // The logic behind by more than a tick (a frame longer than the
        // logic leaves room for, as in TR3's Antarctica): the render thread,
        // above it, holds off until a tick ends, so that frames are dropped,
        // never ticks.
        const uint64_t behind_since = timer_us_gettime64();
        while (atomic_load(&m_Running) && !atomic_load(&m_Paused)
               && m_TickShown
               && timer_us_gettime64() - m_TickUs > M_TICK_US
               && timer_us_gettime64() - behind_since < 4 * M_TICK_US) {
            thd_sleep(1);
        }
        const uint32_t drew = (uint32_t)(behind_since - got);
        m_Stats.draw_sum += drew;
        m_Stats.draw_max = drew > m_Stats.draw_max ? drew : m_Stats.draw_max;
        if (++m_Stats.frames == 600) {
            LOG_INFO(
                "[world] 600 frames, %u ticks, %u late; lock wait %u/%u us, "
                "draw %u/%u us, tick %u/%u us (average/most)",
                (unsigned)m_Stats.ticks, (unsigned)m_Stats.late,
                (unsigned)(m_Stats.wait_sum / 600), (unsigned)m_Stats.wait_max,
                (unsigned)(m_Stats.draw_sum / 600), (unsigned)m_Stats.draw_max,
                (unsigned)(m_Stats.ticks ? m_Stats.tick_sum / m_Stats.ticks : 0),
                (unsigned)m_Stats.tick_max);
            m_Stats = (typeof(m_Stats)) { .tick_start = m_Stats.tick_start };
        }
    }
    return nullptr;
}

void DC_World_StartRender(const DC_WORLD_DRAW draw, void *const arg)
{
    if (m_Thread != nullptr) {
        return;
    }
    m_Draw = draw;
    m_Arg = arg;
    DC_Replay_Forget();
    m_TickUs = timer_us_gettime64();
    m_TickShown = true;
    atomic_store(&m_Paused, false);
    atomic_store(&m_Running, true);
    sem_init(&m_Vblank, 0);
    m_VblankHandle = vblank_handler_add(M_OnVblank, nullptr);
    const kthread_attr_t attr = {
        .stack_size = M_STACK_SIZE,
        .prio = M_PRIORITY,
        .label = "trx-render",
    };
    m_Thread = thd_create_ex(&attr, M_RenderThread, nullptr);
    if (m_Thread == nullptr) {
        atomic_store(&m_Running, false);
    }
    LOG_INFO("[world] render thread %s", m_Thread != nullptr ? "started" : "failed");
}

void DC_World_StopRender(void)
{
    if (m_Thread == nullptr) {
        return;
    }
    atomic_store(&m_Running, false);
    // The caller holds the lock: the thread finishes its wait and leaves.
    mutex_unlock(&m_Lock);
    thd_join(m_Thread, nullptr);
    mutex_lock(&m_Lock);
    m_Thread = nullptr;
    if (m_VblankHandle >= 0) {
        vblank_handler_remove(m_VblankHandle);
        m_VblankHandle = -1;
    }
    sem_destroy(&m_Vblank);
    Interpolation_DC_Forget();
    DC_Replay_Forget();
}

void DC_World_Pause(const bool pause)
{
    if (m_Thread == nullptr) {
        return;
    }
    atomic_store(&m_Paused, pause);
    // What the phase draws meanwhile may change the world the recording
    // was of.
    DC_Replay_Forget();
    // Not while the render thread is in the PowerVR's wait: the phase
    // drawing now uses the PowerVR itself.
    while (pause && atomic_load(&m_InPvr)) {
        thd_pass();
    }
}
