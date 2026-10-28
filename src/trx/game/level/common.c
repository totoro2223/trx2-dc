#include <trx/config.h>
#include <trx/core/benchmark.h>
#include <trx/core/log.h>
#include <trx/game/camera.h>
#include <trx/game/cutseq/playback.h>
#include <trx/game/demo.h>
#include <trx/game/effects.h>
#include <trx/game/fx.h>
#include <trx/game/game.h>
#include <trx/game/game_strings/entries.h>
#include <trx/game/game_strings/table.h>
#include <trx/game/gym.h>
#include <trx/game/items.h>
#include <trx/game/lara.h>
#include <trx/game/lara/mesh.h>
#include <trx/game/lara/skin/common.h>
#include <trx/game/level.h>
#include <trx/game/lua.h>
#include <trx/game/lua/store.h>
#include <trx/game/music.h>
#include <trx/game/objects.h>
#include <trx/game/output.h>
#include <trx/game/output/overlay.h>
#include <trx/game/pathing.h>
#include <trx/game/savegame.h>
#include <trx/game/sound.h>
#include <trx/game/sparks.h>
#include <trx/game/stats.h>
#include <trx/game/ui.h>

#if defined(__DREAMCAST__)
    #include <platform/dreamcast/dc_level_heap.h>
    #include <platform/dreamcast/dc_settings.h>
    #include <trx/game/output/sources/objects.h>

int32_t LUA_DC_CollectGarbage(void);
#endif

static bool m_WorldLoaded = false;

void Level_Unload(void)
{
    m_WorldLoaded = false;

    // First, so the end event a dropped cutscene fires reaches a script that
    // can still read the world it played in.
    CutSeq_Reset();

    // And then the script itself, before the world it was written against is
    // taken apart under it.
    LUA_DropLevelScript();

    Music_ResetTrackStates();
    Sound_ResetSamples();

    Lara_InitialiseLoad(NO_ITEM);
    Lara_Mesh_Reset();
    Lara_Vehicle_SetIndex(NO_ITEM);

    Gym_TrackManager_Reset(GYM_TRACK_ASSAULT);
    Gym_TrackManager_Reset(GYM_TRACK_QUAD);
    Creature_Reset();
    Anim_Reset();
    Item_Reset();
    // Before the objects go: what it holds are objects this level minted.
    Lara_Skin_Reset();
    Object_Reset();
    Camera_Reset();
    Walkable_Reset();

    Output_SetTimeInGame(0.0f);
    Output_Overlay_SetLetterbox(0.0f);
    Output_Overlay_SetFade(0.0f);
    Output_DispatchLevelUnload();

    Sound_StopAll();
    Viewport_AlterFOV(-1, FOV_MODE_GAME);
#if defined(__DREAMCAST__)
    // The level's own strings go with it, before the next level loads into
    // the memory they would otherwise sit in the middle of; the next level
    // applies its own (GameStringTable_Apply).
    GameString_RevertOverrides();
#endif
}

bool Level_IsWorldLoaded(void)
{
    return m_WorldLoaded;
}

RESULT Level_Initialise(
    const GF_LEVEL *const level, const GF_SEQUENCE_CONTEXT seq_ctx)
{
    BENCHMARK benchmark = Benchmark_Start();
    LOG_DEBUG("num=%d (%s)", level->num, level->path);
    if (level->type == GFL_DEMO) {
        Demo_Initialise();
    }
#if defined(__DREAMCAST__)
    // Whatever the render options changed since they were last written goes
    // to the memory card before any level, menu or demo loads, where the
    // next one starts with them.
    DC_Settings_Save();
#endif

    // Before the incoming level takes over as the current one, so a script
    // hearing the unload still finds the level it was written for.
    Level_Unload();
#if defined(__DREAMCAST__)
    // A clean slate before the next level loads, whatever it costs in time
    // (about a second): every object mesh built so far goes (they build again
    // as they are drawn), the scripts' garbage is collected twice (what the
    // first collection frees may hold others). The loads that aborted in
    // Jungle, in TR1's levels and on opening the save menu were all at the
    // end of the memory.
    OutputSource_Objects_EvictUnused(0);
    LUA_DC_CollectGarbage();
    LUA_DC_CollectGarbage();
#endif
#if defined(__DREAMCAST__)
    // What the level loads and builds goes in its own memory, empty again
    // now (dc_level_heap.c).
    // shortcut: a failed load leaves it entered, the game exits on one.
    DC_LevelHeap_Enter();
#endif

    Game_SetIsLevelComplete(false);
    if (level->type != GFL_TITLE && level->type != GFL_DEMO) {
        Gym_SetInventoryOpenEnabled(false);
    }
    if (level->type != GFL_TITLE && level->type != GFL_CUTSCENE) {
        Game_SetCurrentLevel(level);
    }
    GF_SetCurrentLevel(level);

    if (level->type != GFL_TITLE) {
        Stats_ResetLevel(level);
    }

    LUA_Store_ClearLevel();

    if (level == nullptr) {
        return FAIL("there is no level to load");
    }

    // Read here rather than at the first cutscene trigger, so a scene starts
    // without waiting on the file.
    CutSeq_Load();

    // After the unload, so what the outgoing level torn down reaches the script
    // that set it up, and here rather than in the sequencer, so a level loaded
    // by any other path - the title screen among them - still runs its own.
    LUA_RunLevelScript(level);

    MUST(Level_Pipeline_Load(level));
    m_WorldLoaded = true;

    // The level script ran before any of this was here, so the watchers it
    // attached are still owed the call for the value in force.
    LUA_Config_FlushPendingWatchers();
    // Resolve script-declared flip groups before savegame loading replays
    // their moved state.
    LUA_Rooms_ApplyFlipGroups();

    UI_LoadText();
    Output_SetSkyboxEnabled(Object_Get(O_SKYBOX)->loaded);
    Output_DispatchLevelLoad();

    GameStringTable_Apply(level);

    Effect_InitialiseArray();
    LOT_InitialiseArray();
    FX_Reset();
    FX_Weather_SetWeather(level->weather_type);
    Level_ResetSettingOverrides();
    Sparks_Reset();

    // Every other level type reaches Lara_Initialise through the sequencer;
    // the title is loaded on its own, and still has to dress her for the
    // cutscenes it plays behind the menu.
    if (level->type == GFL_TITLE && Lara_GetItem() != nullptr) {
        Lara_Skin_Initialise();
        Lara_Mesh_Initialise(level);
    }

    // A title runs behind the menu rather than reaching live play, so nothing
    // else would ever say its item setup is over and let the events flow. It
    // has no save to overlay either, which is what the quiet period is for.
    if (level->type == GFL_TITLE) {
        Game_SetIsSettingUpItems(false);
    }

#if defined(__DREAMCAST__)
    DC_LevelHeap_Leave();
#endif
    Benchmark_End(&benchmark, nullptr);
    return OK;
}
