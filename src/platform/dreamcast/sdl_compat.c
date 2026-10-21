// Dreamcast platform layer: the part of SDL2 the engine uses, implemented on
// KallistiOS.
//
// The headers are the stock SDL2 ones (include/SDL2), so the engine compiles
// against the real types and constants. What lives here is just enough
// behaviour behind them: timers, threads and locks map onto the KOS kernel,
// the first Dreamcast controller is presented as an SDL game controller
// through the usual events, and the window and display calls describe the
// fixed 640x480 screen.

#include <SDL2/SDL.h>

#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <kos/cond.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include <kos/timer.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DC_SCREEN_WIDTH 640
#define DC_SCREEN_HEIGHT 480
#define DC_EVENT_QUEUE_SIZE 64
#define DC_THREAD_STACK_SIZE (128 * 1024)
// Analog trigger travel (0-255) past which the trigger also counts as the
// shoulder button of the same side.
#define DC_TRIGGER_BUTTON_THRESHOLD 128

struct SDL_mutex {
    mutex_t mutex;
};

struct SDL_cond {
    condvar_t cond;
};

struct SDL_Thread {
    kthread_t *thread;
    SDL_ThreadFunction func;
    void *data;
    int status;
};

struct SDL_Window {
    Uint32 flags;
};

struct _SDL_GameController {
    int index;
};

static Uint32 m_InitFlags = 0;
static char m_Error[256] = "";
static struct SDL_Window m_Window = {};
static struct _SDL_GameController m_Controller = {};
static bool m_ControllerOpen = false;

static SDL_Event m_EventQueue[DC_EVENT_QUEUE_SIZE];
static int m_EventHead = 0;
static int m_EventCount = 0;
static bool m_EventsPumped = false;

static bool m_ButtonDown[SDL_CONTROLLER_BUTTON_MAX] = {};
static Sint16 m_AxisValue[SDL_CONTROLLER_AXIS_MAX] = {};

// The game reads the controller once a frame; a press let go of before the
// next read would be lost. A thread watching the controller between frames
// keeps every button it saw down until the game has read it once.
#define DC_LATCH_INTERVAL_MS 8
static uint32_t m_LatchedButtons = 0;
static bool m_LatchStarted = false;

static const char *const m_ButtonNames[SDL_CONTROLLER_BUTTON_MAX] = {
    [SDL_CONTROLLER_BUTTON_A] = "a",
    [SDL_CONTROLLER_BUTTON_B] = "b",
    [SDL_CONTROLLER_BUTTON_X] = "x",
    [SDL_CONTROLLER_BUTTON_Y] = "y",
    [SDL_CONTROLLER_BUTTON_BACK] = "back",
    [SDL_CONTROLLER_BUTTON_GUIDE] = "guide",
    [SDL_CONTROLLER_BUTTON_START] = "start",
    [SDL_CONTROLLER_BUTTON_LEFTSTICK] = "leftstick",
    [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = "rightstick",
    [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = "leftshoulder",
    [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = "rightshoulder",
    [SDL_CONTROLLER_BUTTON_DPAD_UP] = "dpup",
    [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = "dpdown",
    [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = "dpleft",
    [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = "dpright",
    [SDL_CONTROLLER_BUTTON_MISC1] = "misc1",
    [SDL_CONTROLLER_BUTTON_PADDLE1] = "paddle1",
    [SDL_CONTROLLER_BUTTON_PADDLE2] = "paddle2",
    [SDL_CONTROLLER_BUTTON_PADDLE3] = "paddle3",
    [SDL_CONTROLLER_BUTTON_PADDLE4] = "paddle4",
    [SDL_CONTROLLER_BUTTON_TOUCHPAD] = "touchpad",
};

static const char *const m_AxisNames[SDL_CONTROLLER_AXIS_MAX] = {
    [SDL_CONTROLLER_AXIS_LEFTX] = "leftx",
    [SDL_CONTROLLER_AXIS_LEFTY] = "lefty",
    [SDL_CONTROLLER_AXIS_RIGHTX] = "rightx",
    [SDL_CONTROLLER_AXIS_RIGHTY] = "righty",
    [SDL_CONTROLLER_AXIS_TRIGGERLEFT] = "lefttrigger",
    [SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = "righttrigger",
};

static void M_SetError(const char *const fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(m_Error, sizeof(m_Error), fmt, args);
    va_end(args);
}

static void M_PushEvent(const SDL_Event *const event)
{
    if (m_EventCount >= DC_EVENT_QUEUE_SIZE) {
        return;
    }
    const int tail = (m_EventHead + m_EventCount) % DC_EVENT_QUEUE_SIZE;
    m_EventQueue[tail] = *event;
    m_EventCount++;
}

static void M_SetButton(const SDL_GameControllerButton button, const bool down)
{
    if (m_ButtonDown[button] == down) {
        return;
    }
    m_ButtonDown[button] = down;
#if defined(TRX_DC_INPUT_LOG)
    printf("[input] %u %s %s\n", (unsigned)timer_ms_gettime64(),
        m_ButtonNames[button], down ? "down" : "up");
#endif
    const SDL_Event event = {
        .cbutton = {
            .type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP,
            .timestamp = SDL_GetTicks(),
            .which = 0,
            .button = button,
            .state = down ? SDL_PRESSED : SDL_RELEASED,
        },
    };
    M_PushEvent(&event);
}

static void M_SetAxis(const SDL_GameControllerAxis axis, const Sint16 value)
{
    if (m_AxisValue[axis] == value) {
        return;
    }
    m_AxisValue[axis] = value;
#if defined(TRX_DC_INPUT_LOG)
    printf("[input] %u %s %d\n", (unsigned)timer_ms_gettime64(),
        m_AxisNames[axis], (int)value);
#endif
    const SDL_Event event = {
        .caxis = {
            .type = SDL_CONTROLLERAXISMOTION,
            .timestamp = SDL_GetTicks(),
            .which = 0,
            .axis = axis,
            .value = value,
        },
    };
    M_PushEvent(&event);
}

// Stick values are -128..127; SDL axes run -32768..32767.
static Sint16 M_StickToAxis(const int value)
{
    const int scaled = value * 256;
    return scaled > SDL_JOYSTICK_AXIS_MAX ? SDL_JOYSTICK_AXIS_MAX : scaled;
}

// Trigger values are 0..255; SDL trigger axes run 0..32767.
static Sint16 M_TriggerToAxis(const int value)
{
    return (Sint16)(value * SDL_JOYSTICK_AXIS_MAX / 255);
}

static void *M_LatchButtons(void *const param)
{
    while (true) {
        maple_device_t *const device =
            maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        const cont_state_t *const state = device != nullptr
            ? (const cont_state_t *)maple_dev_status(device)
            : nullptr;
        if (state != nullptr) {
            __atomic_fetch_or(
                &m_LatchedButtons, (uint32_t)state->buttons, __ATOMIC_RELAXED);
        }
        thd_sleep(DC_LATCH_INTERVAL_MS);
    }
    return nullptr;
}

static void M_StartLatch(void)
{
    if (m_LatchStarted) {
        return;
    }
    m_LatchStarted = true;
    const kthread_attr_t attr = {
        .create_detached = true,
        .stack_size = 8 * 1024,
        .label = "trx-input",
    };
    thd_create_ex(&attr, M_LatchButtons, nullptr);
}

static void M_PollController(void)
{
    M_StartLatch();
    maple_device_t *const device = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *const state =
        device != nullptr ? (cont_state_t *)maple_dev_status(device) : nullptr;
    if (state == nullptr) {
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
            M_SetButton(i, false);
        }
        for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++) {
            M_SetAxis(i, 0);
        }
        return;
    }

    // What is held now, and what was pressed since the last read.
    const uint32_t latched =
        __atomic_exchange_n(&m_LatchedButtons, 0, __ATOMIC_RELAXED);
    const uint32_t b = state->buttons | latched;
    if ((state->buttons & CONT_RESET_BUTTONS) == CONT_RESET_BUTTONS) {
        const SDL_Event quit = { .quit = { .type = SDL_QUIT } };
        M_PushEvent(&quit);
    }

    M_SetButton(SDL_CONTROLLER_BUTTON_A, b & CONT_A);
    M_SetButton(SDL_CONTROLLER_BUTTON_B, b & CONT_B);
    M_SetButton(SDL_CONTROLLER_BUTTON_X, b & CONT_X);
    M_SetButton(SDL_CONTROLLER_BUTTON_Y, b & CONT_Y);
    M_SetButton(SDL_CONTROLLER_BUTTON_START, b & CONT_START);
    // Pads with the extra buttons (arcade sticks, the twin-stick) offer C, Z
    // and D; the stock controller has none of them.
    M_SetButton(SDL_CONTROLLER_BUTTON_BACK, b & CONT_D);
    M_SetButton(SDL_CONTROLLER_BUTTON_LEFTSTICK, b & CONT_Z);
    M_SetButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK, b & CONT_C);
    M_SetButton(SDL_CONTROLLER_BUTTON_DPAD_UP, b & CONT_DPAD_UP);
    M_SetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, b & CONT_DPAD_DOWN);
    M_SetButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT, b & CONT_DPAD_LEFT);
    M_SetButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, b & CONT_DPAD_RIGHT);
    M_SetButton(
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        state->ltrig >= DC_TRIGGER_BUTTON_THRESHOLD);
    M_SetButton(
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        state->rtrig >= DC_TRIGGER_BUTTON_THRESHOLD);

    M_SetAxis(SDL_CONTROLLER_AXIS_LEFTX, M_StickToAxis(state->joyx));
    M_SetAxis(SDL_CONTROLLER_AXIS_LEFTY, M_StickToAxis(state->joyy));
    M_SetAxis(SDL_CONTROLLER_AXIS_RIGHTX, M_StickToAxis(state->joy2x));
    M_SetAxis(SDL_CONTROLLER_AXIS_RIGHTY, M_StickToAxis(state->joy2y));
    M_SetAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, M_TriggerToAxis(state->ltrig));
    M_SetAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, M_TriggerToAxis(state->rtrig));
}

// ----------------------------------------------------------------------------
// Initialisation and errors

int SDL_Init(const Uint32 flags)
{
    return SDL_InitSubSystem(flags);
}

int SDL_InitSubSystem(const Uint32 flags)
{
    m_InitFlags |= flags;
    return 0;
}

void SDL_QuitSubSystem(const Uint32 flags)
{
    m_InitFlags &= ~flags;
}

Uint32 SDL_WasInit(const Uint32 flags)
{
    return flags == 0 ? m_InitFlags : (m_InitFlags & flags);
}

void SDL_Quit(void)
{
    m_InitFlags = 0;
}

const char *SDL_GetError(void)
{
    return m_Error;
}

void SDL_free(void *const mem)
{
    free(mem);
}

// ----------------------------------------------------------------------------
// Time

Uint32 SDL_GetTicks(void)
{
    return (Uint32)timer_ms_gettime64();
}

Uint64 SDL_GetPerformanceCounter(void)
{
    return timer_ns_gettime64();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return 1000000000ULL;
}

// Time this thread spent in SDL_Delay, for the frame statistics.
_Thread_local uint64_t g_DC_SleepUs = 0;

void SDL_Delay(const Uint32 ms)
{
    const uint64_t start = timer_us_gettime64();
    thd_sleep(ms);
    g_DC_SleepUs += timer_us_gettime64() - start;
}

// ----------------------------------------------------------------------------
// Threads and locks

int SDL_GetCPUCount(void)
{
    return 1;
}

SDL_mutex *SDL_CreateMutex(void)
{
    SDL_mutex *const result = malloc(sizeof(SDL_mutex));
    if (result == nullptr) {
        M_SetError("out of memory");
        return nullptr;
    }
    // SDL mutexes may be locked again by the thread that holds them.
    mutex_init(&result->mutex, MUTEX_TYPE_RECURSIVE);
    return result;
}

void SDL_DestroyMutex(SDL_mutex *const mutex)
{
    if (mutex != nullptr) {
        mutex_destroy(&mutex->mutex);
        free(mutex);
    }
}

int SDL_LockMutex(SDL_mutex *const mutex)
{
    return mutex != nullptr ? mutex_lock(&mutex->mutex) : -1;
}

int SDL_UnlockMutex(SDL_mutex *const mutex)
{
    return mutex != nullptr ? mutex_unlock(&mutex->mutex) : -1;
}

SDL_cond *SDL_CreateCond(void)
{
    SDL_cond *const result = malloc(sizeof(SDL_cond));
    if (result == nullptr) {
        M_SetError("out of memory");
        return nullptr;
    }
    cond_init(&result->cond);
    return result;
}

void SDL_DestroyCond(SDL_cond *const cond)
{
    if (cond != nullptr) {
        cond_destroy(&cond->cond);
        free(cond);
    }
}

int SDL_CondSignal(SDL_cond *const cond)
{
    return cond != nullptr ? cond_signal(&cond->cond) : -1;
}

int SDL_CondBroadcast(SDL_cond *const cond)
{
    return cond != nullptr ? cond_broadcast(&cond->cond) : -1;
}

int SDL_CondWait(SDL_cond *const cond, SDL_mutex *const mutex)
{
    if (cond == nullptr || mutex == nullptr) {
        return -1;
    }
    return cond_wait(&cond->cond, &mutex->mutex);
}

static void *M_ThreadEntry(void *const param)
{
    SDL_Thread *const thread = param;
    thread->status = thread->func(thread->data);
    return nullptr;
}

SDL_Thread *SDL_CreateThread(
    const SDL_ThreadFunction fn, const char *const name, void *const data)
{
    SDL_Thread *const result = malloc(sizeof(SDL_Thread));
    if (result == nullptr) {
        M_SetError("out of memory");
        return nullptr;
    }
    result->func = fn;
    result->data = data;
    result->status = 0;
    // KOS threads default to a 32 KiB stack, short of what engine code run
    // on a worker expects.
    const kthread_attr_t attr = {
        .stack_size = DC_THREAD_STACK_SIZE,
        .label = name,
    };
    result->thread = thd_create_ex(&attr, M_ThreadEntry, result);
    if (result->thread == nullptr) {
        M_SetError("thread %s could not be created", name);
        free(result);
        return nullptr;
    }
    return result;
}

void SDL_WaitThread(SDL_Thread *const thread, int *const status)
{
    if (thread == nullptr) {
        return;
    }
    thd_join(thread->thread, nullptr);
    if (status != nullptr) {
        *status = thread->status;
    }
    free(thread);
}

// ----------------------------------------------------------------------------
// Events and the controller

int SDL_PollEvent(SDL_Event *const event)
{
    // Read the controller once per drain of the queue, so a caller looping
    // until no events are left sees each change once.
    if (m_EventCount == 0 && !m_EventsPumped) {
        M_PollController();
        m_EventsPumped = true;
    }
    if (m_EventCount == 0) {
        m_EventsPumped = false;
        return 0;
    }
    if (event != nullptr) {
        *event = m_EventQueue[m_EventHead];
        m_EventHead = (m_EventHead + 1) % DC_EVENT_QUEUE_SIZE;
        m_EventCount--;
    }
    return 1;
}

int SDL_NumJoysticks(void)
{
    return maple_enum_type(0, MAPLE_FUNC_CONTROLLER) != nullptr ? 1 : 0;
}

SDL_bool SDL_IsGameController(const int joystick_index)
{
    return joystick_index == 0 && SDL_NumJoysticks() > 0;
}

const char *SDL_GameControllerNameForIndex(const int joystick_index)
{
    return joystick_index == 0 ? "Dreamcast Controller" : nullptr;
}

SDL_GameControllerType SDL_GameControllerTypeForIndex(const int joystick_index)
{
    // A, B, X and Y sit where they do on an Xbox pad.
    return SDL_CONTROLLER_TYPE_XBOX360;
}

SDL_GameController *SDL_GameControllerOpen(const int joystick_index)
{
    if (!SDL_IsGameController(joystick_index)) {
        M_SetError("no controller at index %d", joystick_index);
        return nullptr;
    }
    m_Controller.index = joystick_index;
    m_ControllerOpen = true;
    return &m_Controller;
}

void SDL_GameControllerClose(SDL_GameController *const gamecontroller)
{
    if (gamecontroller == &m_Controller) {
        m_ControllerOpen = false;
    }
}

SDL_GameControllerButton SDL_GameControllerGetButtonFromString(
    const char *const str)
{
    if (str == nullptr) {
        return SDL_CONTROLLER_BUTTON_INVALID;
    }
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        if (m_ButtonNames[i] != nullptr && strcasecmp(str, m_ButtonNames[i]) == 0) {
            return i;
        }
    }
    return SDL_CONTROLLER_BUTTON_INVALID;
}

const char *SDL_GameControllerGetStringForButton(
    const SDL_GameControllerButton button)
{
    if (button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX) {
        return nullptr;
    }
    return m_ButtonNames[button];
}

SDL_GameControllerAxis SDL_GameControllerGetAxisFromString(const char *const str)
{
    if (str == nullptr) {
        return SDL_CONTROLLER_AXIS_INVALID;
    }
    // SDL also accepts a leading '+' or '-' half-axis marker.
    const char *name = str;
    if (*name == '+' || *name == '-') {
        name++;
    }
    for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++) {
        if (m_AxisNames[i] != nullptr && strcasecmp(name, m_AxisNames[i]) == 0) {
            return i;
        }
    }
    return SDL_CONTROLLER_AXIS_INVALID;
}

const char *SDL_GameControllerGetStringForAxis(const SDL_GameControllerAxis axis)
{
    if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) {
        return nullptr;
    }
    return m_AxisNames[axis];
}

// ----------------------------------------------------------------------------
// Keyboard. No keyboard is read on the Dreamcast; the names are still asked
// for when the bindings are described or loaded.

SDL_Keymod SDL_GetModState(void)
{
    return KMOD_NONE;
}

SDL_Keycode SDL_GetKeyFromScancode(const SDL_Scancode scancode)
{
    return SDL_SCANCODE_TO_KEYCODE(scancode);
}

SDL_Scancode SDL_GetScancodeFromKey(const SDL_Keycode key)
{
    if (key & SDLK_SCANCODE_MASK) {
        return (SDL_Scancode)(key & ~SDLK_SCANCODE_MASK);
    }
    return SDL_SCANCODE_UNKNOWN;
}

const char *SDL_GetScancodeName(const SDL_Scancode scancode)
{
    return "";
}

SDL_Scancode SDL_GetScancodeFromName(const char *const name)
{
    return SDL_SCANCODE_UNKNOWN;
}

const char *SDL_GetKeyName(const SDL_Keycode key)
{
    return "";
}

SDL_Keycode SDL_GetKeyFromName(const char *const name)
{
    return SDLK_UNKNOWN;
}

SDL_bool SDL_HasClipboardText(void)
{
    return SDL_FALSE;
}

char *SDL_GetClipboardText(void)
{
    return strdup("");
}

int SDL_SetClipboardText(const char *const text)
{
    return 0;
}

int SDL_GetNumTouchDevices(void)
{
    return 0;
}

SDL_TouchID SDL_GetTouchDevice(const int index)
{
    M_SetError("no touch devices");
    return 0;
}

SDL_TouchDeviceType SDL_GetTouchDeviceType(const SDL_TouchID touch_id)
{
    return SDL_TOUCH_DEVICE_INVALID;
}

// ----------------------------------------------------------------------------
// Video. There is one screen, always 640x480, and one window covering it.

int SDL_GetNumVideoDisplays(void)
{
    return 1;
}

int SDL_GetDisplayBounds(const int display_index, SDL_Rect *const rect)
{
    if (rect != nullptr) {
        *rect = (SDL_Rect) { 0, 0, DC_SCREEN_WIDTH, DC_SCREEN_HEIGHT };
    }
    return 0;
}

int SDL_GetCurrentDisplayMode(
    const int display_index, SDL_DisplayMode *const mode)
{
    if (mode != nullptr) {
        *mode = (SDL_DisplayMode) {
            .format = SDL_PIXELFORMAT_RGB565,
            .w = DC_SCREEN_WIDTH,
            .h = DC_SCREEN_HEIGHT,
            .refresh_rate = 60,
        };
    }
    return 0;
}

SDL_Window *SDL_CreateWindow(
    const char *const title, const int x, const int y, const int w,
    const int h, const Uint32 flags)
{
    m_Window.flags = (flags & ~SDL_WINDOW_HIDDEN) | SDL_WINDOW_FULLSCREEN
        | SDL_WINDOW_INPUT_FOCUS;
    return &m_Window;
}

void SDL_DestroyWindow(SDL_Window *const window)
{
}

Uint32 SDL_GetWindowFlags(SDL_Window *const window)
{
    return window != nullptr ? window->flags : 0;
}

void SDL_GetWindowPosition(SDL_Window *const window, int *const x, int *const y)
{
    if (x != nullptr) {
        *x = 0;
    }
    if (y != nullptr) {
        *y = 0;
    }
}

void SDL_GetWindowSize(SDL_Window *const window, int *const w, int *const h)
{
    if (w != nullptr) {
        *w = DC_SCREEN_WIDTH;
    }
    if (h != nullptr) {
        *h = DC_SCREEN_HEIGHT;
    }
}

void SDL_SetWindowPosition(SDL_Window *const window, const int x, const int y)
{
}

void SDL_SetWindowSize(SDL_Window *const window, const int w, const int h)
{
}

int SDL_SetWindowFullscreen(SDL_Window *const window, const Uint32 flags)
{
    return 0;
}

void SDL_ShowWindow(SDL_Window *const window)
{
    if (window != nullptr) {
        window->flags |= SDL_WINDOW_SHOWN;
    }
}

void SDL_RaiseWindow(SDL_Window *const window)
{
}

void SDL_MaximizeWindow(SDL_Window *const window)
{
}

int SDL_GL_SetAttribute(const SDL_GLattr attr, const int value)
{
    return 0;
}

int SDL_ShowCursor(const int toggle)
{
    return SDL_DISABLE;
}

int SDL_ShowSimpleMessageBox(
    const Uint32 flags, const char *const title, const char *const message,
    SDL_Window *const window)
{
    printf("%s: %s\n", title != nullptr ? title : "TRX", message);
    return 0;
}

// ----------------------------------------------------------------------------
// System

char *SDL_GetBasePath(void)
{
    // The game ships on the disc; the engine's own data sits at its root.
    return strdup("/cd/");
}

SDL_Locale *SDL_GetPreferredLocales(void)
{
    M_SetError("no locale information");
    return nullptr;
}
