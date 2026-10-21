#pragma once

#include <kos/thread.h>

// Byte a thread's stack is filled with before it starts, so that the part it
// never reached can be told from the rest.
#define DC_STACK_MARKER 0xA5

// Prints where the given thread is every few seconds (debug builds).
void DC_Watchdog_Start(kthread_t *thread);

// Test discs (START.TXT's second number) load a saved game this many times,
// one after another, to show what each load leaves behind.
void DC_Watchdog_SetTestLoads(int32_t count);
// Called once a game tick: the saved game to load now (0 or 1, in turn), or
// -1. Also prints the heap as each load starts.
int32_t DC_Watchdog_TakeTestLoad(void);
// Whether this is such a disc.
bool DC_Watchdog_IsTesting(void);
