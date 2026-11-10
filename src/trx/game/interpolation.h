#pragma once

#include <trx/core/math/func.h>
#include <trx/game/items/types.h>

void Interpolation_Enable(void);
void Interpolation_Disable(void);
bool Interpolation_IsEnabled(void);
bool Interpolation_IsActive(void);

TRX_REAL Interpolation_GetWorldRate(void);
TRX_REAL Interpolation_GetCameraRate(void);
TRX_REAL Interpolation_GetRate(void);
void Interpolation_SetRate(TRX_REAL rate);

// Returns how far the frame being drawn stands between the last two ticks of
// the interface. This matches the world rate, except while the game exits:
// the world then stands still and only the interface moves.
TRX_REAL Interpolation_GetUIRate(void);
// Sets the interface rate on its own. Interpolation_SetRate sets both.
void Interpolation_SetUIRate(TRX_REAL rate);

void Interpolation_Interpolate(void);
void Interpolation_Remember(void);
void Interpolation_RememberItem(ITEM *item);
void Interpolation_RememberBraid(void);

// Instantly discard interpolation data
void Interpolation_CommitLara(void);
void Interpolation_CommitBraid(void);

#if defined(__DREAMCAST__)
// The logic on its own thread (platform/dreamcast/dc_world.c): at the end of
// each tick the ends of the interpolation (the camera, Lara, the items, the
// effects, the braid) are kept, and a frame drawn while the next tick runs
// swaps them in and back out, so that it shows the last two whole ticks.
void Interpolation_DC_Publish(void);
void Interpolation_DC_Swap(bool in);
void Interpolation_DC_Forget(void);
#endif
