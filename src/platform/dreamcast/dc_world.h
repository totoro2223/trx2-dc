#pragma once

// The logic on its own thread (PLAN_60_FIJO.md, phase 4). In the native mode
// a frame is drawn by a render thread of higher priority every 1/60 s, while
// the executor's thread runs the 30 ticks a second in what time is left: no
// frame waits for a whole tick. The world has one lock: the logic holds it
// through a tick and lets go between items (DC_World_Yield) and while it
// waits for the next one; the render thread takes it for a frame, drawing
// the last two whole ticks (Interpolation_DC_Swap).

#include <stdbool.h>
#include <stdint.h>

// The frame the render thread draws, given the share of the way from the
// tick before last to the last one.
typedef void (*DC_WORLD_DRAW)(void *arg, double frac, bool first);

void DC_World_Lock(void);
void DC_World_Unlock(void);

// From the logic, between items: lets a frame due be drawn.
void DC_World_Yield(void);
// TEMP: what the logic is about to run (an object number), to say what a long
// stretch without a yield was.
void DC_World_Tag(int32_t tag);

// The render thread draws from now on (the caller holds the lock), until
// stopped; a stop waits for the frame being drawn.
void DC_World_StartRender(DC_WORLD_DRAW draw, void *arg);
void DC_World_StopRender(void);
bool DC_World_IsRendering(void);

// A phase run inside a tick (the inventory) draws itself: the render thread
// holds off until it is over.
void DC_World_Pause(bool pause);

// A tick starts, and has ended: its ends are published for the frames.
void DC_World_TickBegin(void);
void DC_World_TickDone(void);

// The frame between two ticks (PLAN_60_FIJO.md, "stage por tick"): the
// first frame after a tick stages the world once, at the tick's own state,
// and draws it halfway back to the tick before (each instance's world matrix
// and the camera blended with the last tick's); the frames after it draw the
// same staging again, as the tick left it, without staging anything.
// Game_Draw asks DC_Replay_GameDraw first: true when it was replayed.
typedef enum {
    DC_REPLAY_OFF,
    DC_REPLAY_RECORD,
    DC_REPLAY_PLAY,
} DC_REPLAY_MODE;
#define DC_REPLAY_SEGMENTS 4

void DC_Replay_SetMode(DC_REPLAY_MODE mode);
bool DC_Replay_GameDraw(void);
void DC_Replay_GameDrawDone(void);
// The render thread stopped: no tick before to blend with.
void DC_Replay_Forget(void);
