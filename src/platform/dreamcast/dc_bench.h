#pragma once

// A demo started from the level select, and the benchmark: one of them run
// whole, every frame of it measured, the results shown and logged at its end.

#include <stdbool.h>
#include <stdint.h>

// The demo the game starts in instead of the title, or -1 (dc_main.c).
extern int32_t g_DC_StartDemo;
// That demo is the benchmark.
extern bool g_DC_BenchRequested;

void DC_Bench_Begin(void);
// Logs the results as a [bench] line and shows them for a while.
void DC_Bench_End(void);
