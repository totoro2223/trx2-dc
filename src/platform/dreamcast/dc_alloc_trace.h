#pragma once

// Lists on the serial console the large heap blocks still alive, with the
// code that asked for them (debug builds only; see dc_alloc_trace.c).
void DC_AllocTrace_DumpLive(const char *label);

// Prints how full each stretch of the heap is, and its largest hole.
void DC_AllocTrace_DumpHeapMap(const char *label);

// From the end of a level's load: remembers the blocks taken above where the
// first load began, and lists at the next load those still there.
void DC_AllocTrace_StartKeeping(void);
void DC_AllocTrace_DumpKept(const char *label);

#if defined(TRX_DC_HEAP_CENSUS)
    #include <stdint.h>

// Lists every block newlib holds, by caller (sampled; census builds only).
void DC_AllocTrace_DumpCensus(const char *label);

// Sums blocks by their callers, then prints the sums as "[live] <bytes>
// x<blocks> <callers>" between head and "[live-end]" (live_census.py).
void DC_AllocTrace_Group(uint32_t size, const uint32_t *frames, int count);
void DC_AllocTrace_PrintGroups(const char *head, int dropped);
#endif
