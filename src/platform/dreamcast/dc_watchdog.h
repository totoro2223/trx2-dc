#pragma once

#include <kos/thread.h>

// Prints where the given thread is every few seconds (debug builds).
void DC_Watchdog_Start(kthread_t *thread);
