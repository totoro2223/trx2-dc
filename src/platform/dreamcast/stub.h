#pragma once

#include <stdio.h>

// Marks a placeholder of the Dreamcast platform layer. The first call of each
// one is reported on the debug console, so the log shows which parts of the
// engine reach code that does nothing yet.
#define DC_STUB()                                                              \
    do {                                                                       \
        static bool reported_ = false;                                         \
        if (!reported_) {                                                      \
            reported_ = true;                                                  \
            printf("[stub] %s\n", __func__);                                   \
        }                                                                      \
    } while (0)
