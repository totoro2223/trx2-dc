#include <trx/config.h>
#include <trx/core/file.h>
#include <trx/core/subsystem.h>
#include <trx/debug.h>
#include <trx/game/game.h>
#include <trx/game/game_flow.h>
#include <trx/game/items.h>
#include <trx/game/lara.h>
#include <trx/game/objects.h>
#include <trx/game/rooms.h>
#include <trx/game/savegame.h>
#include <trx/game/savegame/file.h>
#include <trx/game/savegame/identity.h>
#include <trx/version.h>

#if defined(__DREAMCAST__)
    #include <platform/dreamcast/vmu.h>
    #include <trx/game/output/sources/objects.h>

int32_t LUA_DC_CollectGarbage(void);
void DC_PVR_ReleaseVertexPool(void);
void DC_Render_FreeSavingRoom(void);
#endif

static SAVEGAME_VERSION m_InitialVersion = SG_VERSION_LEGACY;

static void M_LoadPreprocess(void)
{
    SG_Resume_ResetAllEntries();
}

static void M_LoadPostprocess(void)
{
    // TODO: tidy this; skidoo drivers currently require handle_save_func to be
    // called immediately on load within the strategies.
    for (int32_t i = 0; i < Item_GetTotalCount(); i++) {
        if (!Savegame_IsSavedItem(i)) {
            continue;
        }
        ITEM *const item = Item_Get(i);
        const OBJECT *const obj = Object_Get(item->object_id);
        // TODO: make this engine-agnostic
        if (g_TRVersion == 1 && obj->handle_save_func != nullptr) {
            obj->handle_save_func(item, SAVEGAME_STAGE_AFTER_LOAD);
        }
    }

    LARA_INFO *const lara = Lara_GetLaraInfo();
    // The cached mesh positions describe Lara as she stood before the load.
    lara->mesh_pos_matrices_valid = false;
    if (Game_GetBonusFlag() != GBF_NONE) {
        CONFIG_SET(g_Config.profile.new_game_plus_unlock, true);
        Config_Update();
    }
    if (lara->burn && !g_Config.gameplay.enable_enhanced_saves) {
        lara->burn = false;
        Lara_CatchFire();
    }
}

static void M_Shutdown(void)
{
    SG_Manager_Shutdown();
    SG_Resume_Shutdown();
}

SAVEGAME_VERSION Savegame_GetInitialVersion(void)
{
    return m_InitialVersion;
}

void Savegame_SetInitialVersion(const SAVEGAME_VERSION version)
{
    m_InitialVersion = version;
}

void Savegame_Init(void)
{
    SG_Resume_Init();
    SG_Manager_Init();
}

bool Savegame_IsManualSaveAllowed(void)
{
    const SAVE_CRYSTAL_MODE crystal_mode = g_Config.gameplay.save_crystal_mode;
    return !g_Config.flow.load_save_disabled
        && crystal_mode != SAVE_CRYSTAL_SAVE
        && crystal_mode != SAVE_CRYSTAL_SAVE_PICKUP;
}

bool Savegame_IsSavedItem(const int32_t item_num)
{
    if (item_num == Lara_GetLaraInfo()->gun_item_num) {
        return false;
    }
    return item_num < Item_GetLevelCount() || !Item_Get(item_num)->is_destroyed;
}

void Savegame_ProcessItemsBeforeSave(void)
{
    for (int32_t i = 0; i < Item_GetTotalCount(); i++) {
        if (!Savegame_IsSavedItem(i)) {
            continue;
        }
        ITEM *const item = Item_Get(i);
        const OBJECT *const obj = Object_Get(item->object_id);
        if (obj->handle_save_func != nullptr) {
            obj->handle_save_func(item, SAVEGAME_STAGE_BEFORE_SAVE);
        }
    }
}

void Savegame_ProcessItemsBeforeLoad(void)
{
    for (int32_t i = 0; i < Item_GetTotalCount(); i++) {
        if (!Savegame_IsSavedItem(i)) {
            continue;
        }
        ITEM *const item = Item_Get(i);
        const OBJECT *const obj = Object_Get(item->object_id);
        if (obj->handle_save_func != nullptr) {
            obj->handle_save_func(item, SAVEGAME_STAGE_BEFORE_LOAD);
        }
    }
}

#if defined(__DREAMCAST__)
// The saved game's copies of the game's state take up to 400 KiB at once, and
// the fuller levels (Jungle) have none free: what a scene can build again goes
// first (the object meshes it drew some frames ago, the cache down to a few
// thousand vertices), the scripts' garbage, and the batcher's per-vertex
// scratch of the frame (a couple of megabytes in Catacombs).
static void M_FreeMemoryForSaving(void)
{
    DC_Render_FreeSavingRoom();
    OutputSource_Objects_EvictUnused(1);
    OutputSource_Objects_EvictToBudget(6000);
    LUA_DC_CollectGarbage();
    // And the polygon lists' pool, 384 KiB idle between frames.
    DC_PVR_ReleaseVertexPool();
}
#endif

bool Savegame_Save(const SAVEGAME_SLOT_REF slot)
{
    if (!SG_Manager_IsValidSlotRef(slot)) {
        return false;
    }
#if defined(__DREAMCAST__)
    M_FreeMemoryForSaving();
#endif
    SG_Manager_BindSlot(slot);

    const GF_LEVEL *const current_level = Game_GetCurrentLevel();
    SG_Resume_StoreGameToEntry(current_level);
    SG_Resume_MirrorCurrentEntry(current_level);

    return SG_Manager_WriteSlot(slot);
}

RESULT Savegame_Load(const SAVEGAME_SLOT_REF slot)
{
#if defined(__DREAMCAST__)
    M_FreeMemoryForSaving();
#endif
    const SAVEGAME_INFO *const savegame_info = SG_Manager_GetSavegameInfo(slot);
    FAIL_IF(savegame_info == nullptr, "the slot holds no saved game");
    ASSERT(savegame_info->full_path != nullptr);

    M_LoadPreprocess();
    SaveGame_ResetDropped();

    TRX_FILE *fp = nullptr;
    RESULT result =
        File_OpenPath(savegame_info->full_path, FILE_OPEN_READ, &fp);
    if (IS_OK(result)) {
        result = SG_File_LoadFromFile(fp);
        File_Close(fp);
    }

    M_LoadPostprocess();
    const int32_t dropped = SaveGame_GetDroppedCount();
    if (dropped > 0) {
        LOG_WARNING(
            "%d records in this save name something the game no longer holds",
            dropped);
    }
    m_InitialVersion = savegame_info->initial_version;
    return result;
}

RESULT Savegame_UpdateDeathCounters(
    const SAVEGAME_SLOT_REF slot, const int32_t death_count)
{
    const SAVEGAME_INFO *const savegame_info = SG_Manager_GetSavegameInfo(slot);
    FAIL_IF(savegame_info == nullptr, "the slot holds no saved game");
    ASSERT(savegame_info->full_path != nullptr);

    TRX_FILE *fp = nullptr;
    MUST(File_OpenPath(savegame_info->full_path, FILE_OPEN_READ_WRITE, &fp));
    const RESULT result = SG_File_UpdateDeathCounters(
        fp, savegame_info->level_num, death_count, savegame_info->is_quick);
    File_Close(fp);
#if defined(__DREAMCAST__)
    if (IS_OK(result)) {
        DC_VMU_StoreSave(savegame_info->full_path, savegame_info->level_title);
    }
#endif
    return result;
}

RESULT Savegame_LoadOnlyResumeInfo(const SAVEGAME_SLOT_REF slot)
{
    const SAVEGAME_INFO *const savegame_info = SG_Manager_GetSavegameInfo(slot);
    FAIL_IF(savegame_info == nullptr, "the slot holds no saved game");
    ASSERT(savegame_info->full_path != nullptr);

    TRX_FILE *fp = nullptr;
    RESULT result =
        File_OpenPath(savegame_info->full_path, FILE_OPEN_READ, &fp);
    if (IS_OK(result)) {
        result = SG_File_LoadOnlyResumeInfo(fp);
        File_Close(fp);
    }

    Savegame_SetInitialVersion(savegame_info->initial_version);
    return result;
}

RESULT Savegame_ReadReachedLevels(
    const SAVEGAME_SLOT_REF slot, VECTOR *const levels)
{
#if defined(__DREAMCAST__)
    M_FreeMemoryForSaving();
#endif
    RESUME_INFO *const snapshot = SG_Resume_Snapshot();
    const SAVEGAME_VERSION initial_version = m_InitialVersion;
    SG_Resume_ClearEntries();
    const RESULT result = Savegame_LoadOnlyResumeInfo(slot);
    if (IS_OK(result)) {
        const GF_LEVEL_TABLE *const level_table = GF_GetLevelTable(GFLT_MAIN);
        const int32_t last_level_num = SG_Manager_GetLevelNumber(slot);
        for (int32_t i = 0; i <= last_level_num && i < level_table->count;
             i++) {
            const GF_LEVEL *const level = &level_table->levels[i];
            if (SG_Resume_GetEntry(level)->flags.available) {
                Vector_Add(levels, &level);
            }
        }
    }
    m_InitialVersion = initial_version;
    SG_Resume_Restore(snapshot);
    return result;
}

bool Savegame_RestartAvailable(const SAVEGAME_SLOT_REF slot)
{
    if (!SG_Manager_IsValidSlotRef(slot)) {
        return true;
    }
    const SAVEGAME_INFO *const savegame_info = SG_Manager_GetSavegameInfo(slot);
    return savegame_info->features.restart;
}

REGISTER_SUBSYSTEM(.shutdown = M_Shutdown)
