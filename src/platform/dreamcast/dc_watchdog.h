#pragma once

#include <kos/thread.h>

// Byte a thread's stack is filled with before it starts, so that the part it
// never reached can be told from the rest.
#define DC_STACK_MARKER 0xA5

// Prints where the given thread is every few seconds (debug builds).
void DC_Watchdog_Start(kthread_t *thread);
