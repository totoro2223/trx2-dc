#pragma once

#include <trx/game/output/mesh_batcher/batcher.h>
#include <trx/game/rooms/types.h>

void OutputSource_Rooms_Init(MESH_BATCHER *batcher);
void OutputSource_Rooms_Shutdown(void);
void OutputSource_Rooms_ObserveLevelLoad(void);
void OutputSource_Rooms_ObserveLevelUnload(void);
void OutputSource_Rooms_ObserveRoomFlip(const ROOM *room);

void OutputSource_Rooms_StageRoom(const ROOM *room);
#if defined(__DREAMCAST__)
// A static merged into the mesh of the room that draws it (rooms.c): not
// drawn as an object.
bool OutputSource_Rooms_IsStaticMerged(int32_t draw_num);
bool OutputSource_Rooms_RayBlocked(
    const ROOM *room, const XYZ_32 *eye, const XYZ_32 *to);
#endif
