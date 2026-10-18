#pragma once

#define NO_ITEM (-1)
#if defined(__DREAMCAST__)
    // The original games held 256; the desktop's 10240 sizes pools the
    // Dreamcast's 16 MiB cannot spare.
    #define MAX_ITEMS 1024
#else
    #define MAX_ITEMS 10240
#endif
