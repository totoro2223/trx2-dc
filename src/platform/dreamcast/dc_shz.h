#pragma once

// SH4ZAM's fast scalar math (fsrra, fsca), behind plain functions: its headers
// are GNU C (raw strings in the assembly), which the engine's c2x does not
// accept, so they are compiled apart (dc_shz.c).

float DC_Shz_InvSqrt(float x); // x > 0
float DC_Shz_Acos(float x);
float DC_Shz_Sin(float radians);
