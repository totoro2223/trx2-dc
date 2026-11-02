#pragma once

#define NO_ITEM (-1)
#if defined(__DREAMCAST__)
    // The original games held 256 (Barkhang's file has ~213); the desktop's
    // 10240 sizes pools the Dreamcast's 16 MiB cannot spare, and 1024 still
    // took 180K of items a level plus the static tables sized by it.
    #define MAX_ITEMS 512
#else
    #define MAX_ITEMS 10240
#endif
