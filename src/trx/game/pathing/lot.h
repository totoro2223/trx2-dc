#pragma once

#include <trx/game/creature/types.h>

LOT_SETUP LOT_Setup(LOT_SETUP_TYPE type);
void LOT_InitialiseArray(void);
void LOT_InitialiseSlot(int16_t item_num, int32_t slot);
void LOT_CreateZone(ITEM *item);
void LOT_InitialiseLOT(LOT_INFO *LOT);
bool LOT_EnableBaddieAI(int16_t item_num, bool always);
void LOT_DisableBaddieAI(int16_t item_num);
CREATURE *LOT_GetBaddieSlot(int32_t i);

// Dreamcast: whether this level takes the original games' cuts
// (rendering.dc_original_ai, read when the level loads): the linear pose
// blend, one pose for far enemies, 128 visible rooms at most and the frames
// of an item worked out once. The AI itself is TRX's, so that a demo ends
// where it did.
bool LOT_IsOriginalSim(void);
void LOT_ClearLOT(LOT_INFO *LOT);

// Drops the box every creature is heading for, so that each finds its way
// again against the world as it now stands. TR4 does this whenever a door
// blocks or unblocks a box.
void LOT_ClearRoutes(void);
