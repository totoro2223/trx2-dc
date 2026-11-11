#pragma once

#include <trx/core/log.h>

#if defined(__DREAMCAST__)
    #include <stdbool.h>
    #include <stdio.h>

// A failed assertion reaches the console as a restart with no trace: this puts
// where it was on the screen and in a file on the memory card (TRXLOG) before
// the restart (platform/dreamcast/dc_strict.c).
void DC_Crumb_Fatal(const char *file, int line, const char *message);
// Who called the function that failed (its return address), for addr2line.
extern void *g_DC_FatalCaller;
    #define M_DC_FATAL(message)                                                \
        (g_DC_FatalCaller = __builtin_return_address(0),                       \
         DC_Crumb_Fatal(__FILE__, __LINE__, message))
// TEMP: a step of the end of a level, on the screen for a moment and kept in
// RAM: a console that restarts leaves the last one read as where it did.
void DC_TrailF(const char *fmt, ...);
void DC_TraceOn(bool on);
void DC_TraceF(const char *fmt, ...);
// TEMP: where the first moments of a level are, and the last asm routine
// entered, on the memory card (TRXLOG) each time: the first few dozen only.
void DC_Where(const char *what, int n);
    #define DC_WHERE(what, n) DC_Where(what, n)
    #define DC_TRAIL(...) DC_TrailF(__VA_ARGS__)
    // The same, for what happens often: only from DC_TraceOn(true) on, and a
    // few dozen at most.
    #define DC_TRACE(...) DC_TraceF(__VA_ARGS__)
#else
    #define M_DC_FATAL(message)
    #define DC_WHERE(what, n) ((void)0)
    #define DC_TRAIL(...) ((void)0)
    #define DC_TRACE(...) ((void)0)
#endif

#define ASSERT(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            M_DC_FATAL(#x);                                                    \
            LOG_ERROR("Assertion failed: %s", #x);                             \
            __builtin_trap();                                                  \
        }                                                                      \
    } while (0)

#define ASSERT_FAIL()                                                          \
    do {                                                                       \
        M_DC_FATAL("ASSERT_FAIL");                                             \
        LOG_ERROR("Assertion failed");                                         \
        __builtin_trap();                                                      \
    } while (0)

#if defined(__DREAMCAST__)
    #define ASSERT_FAIL_FMT(fmt, ...)                                          \
        do {                                                                   \
            char m_dc_fatal[160];                                              \
            snprintf(m_dc_fatal, sizeof(m_dc_fatal), fmt __VA_OPT__(, )        \
                         __VA_ARGS__);                                         \
            M_DC_FATAL(m_dc_fatal);                                            \
            LOG_ERROR("Assertion failed: " fmt __VA_OPT__(, ) __VA_ARGS__);    \
            __builtin_trap();                                                  \
        } while (0)
#else
    #define ASSERT_FAIL_FMT(fmt, ...)                                          \
        do {                                                                   \
            LOG_ERROR("Assertion failed: " fmt __VA_OPT__(, ) __VA_ARGS__);    \
            __builtin_trap();                                                  \
        } while (0)
#endif

#define SOFT_ASSERT(x, msg)                                                    \
    do {                                                                       \
        if (!(x)) {                                                            \
            LOG_ERROR("Warning: %s (%s)", msg, #x);                            \
        }                                                                      \
    } while (0)
