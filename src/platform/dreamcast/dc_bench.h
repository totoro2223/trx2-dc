#pragma once

// A demo started from the level select, and the benchmark: one of them run
// whole, every frame of it measured, the results shown and logged at its end.

#include <stdbool.h>
#include <stdint.h>

// The demo, or the cutscene, the game starts in instead of the title, or -1
// (the level select).
extern int32_t g_DC_StartDemo;
extern int32_t g_DC_StartCutscene;
// That demo is the benchmark.
extern bool g_DC_BenchRequested;

void DC_Bench_Begin(int32_t demo_num);
// Logs a detailed report, every line of it starting "[bench] " and short
// enough for the serial console not to wrap it, between "=== report" and
// "=== end" (tools/dreamcast/bench_archive.ps1 keeps it, dated), and shows
// the summary for a while.
void DC_Bench_End(void);
