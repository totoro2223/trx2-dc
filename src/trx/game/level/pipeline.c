#include <trx/config.h>
#include <trx/core/benchmark.h>
#include <trx/core/file.h>
#include <trx/core/filesystem.h>
#include <trx/core/log.h>
#include <trx/core/memory.h>
#include <trx/game/camera.h>
#include <trx/game/game_buf.h>
#include <trx/game/inject.h>
#include <trx/game/items/carrier.h>
#include <trx/game/level.h>
#include <trx/game/level/format/format.h>
#include <trx/game/objects.h>
#include <trx/game/objects/creatures/atlantean.h>
#include <trx/game/paths.h>
#include <trx/game/rooms.h>
#include <trx/game/sound.h>
#include <trx/game/stats.h>
#include <trx/version.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__DREAMCAST__)
#include <platform/dreamcast/dc_alloc_trace.h>
#include <platform/dreamcast/dc_level_heap.h>
#include <platform/dreamcast/loading.h>
#include <trx/core/filesystem.h>
#include <trx/game/items.h>
#include <trx/game/objects/families.h>
#include <trx/game/objects/general/pickup.h>
#endif

typedef struct {
    int32_t game_index;
    int32_t file_index;
} M_SAMPLE_ENTRY;

static int32_t M_CompareSampleOffsets(const void *const a, const void *const b)
{
    const M_SAMPLE_ENTRY *const entry_a = (M_SAMPLE_ENTRY *)a;
    const M_SAMPLE_ENTRY *const entry_b = (M_SAMPLE_ENTRY *)b;
    return entry_a->file_index - entry_b->file_index;
}

static void M_InitialiseSamplesFromFile(
    LEVEL_CONTEXT *const ctx, const char *file_name)
{
    BENCHMARK benchmark = Benchmark_Start();
    M_SAMPLE_ENTRY *entries = nullptr;
    LEVEL_CONTEXT_INFO *const info = &ctx->info;

    TRX_FILE *fp = nullptr;
    if (file_name == nullptr) {
        goto finish;
    }

    if (!SHOULD(
            File_OpenPath(file_name, FILE_OPEN_READ, &fp),
            "the level plays without its sounds")) {
        goto finish;
    }
    File_SetSoftFailure(fp, true);
    LOG_DEBUG("Loading samples from %s", file_name);
#if defined(__DREAMCAST__)
    {
        // The remaster's main.sfx starts with a table of its own (740 bytes
        // in TR2 and TR3) before the first WAV: skipped to the first RIFF.
        uint8_t head[4096];
        File_ReadData(fp, head, sizeof(head));
        int32_t skip = 0;
        while (skip + 4 <= (int32_t)sizeof(head)
               && memcmp(head + skip, "RIFF", 4) != 0) {
            skip += 2;
        }
        File_Seek(fp, skip + 4 <= (int32_t)sizeof(head) ? skip : 0, FILE_SEEK_SET);
    }
#endif

#if defined(__DREAMCAST__)
    // make_disc.sh writes the byte offset of every WAV of main.sfx beside it
    // (main.sfx.idx, uint32 little endian), so that only the samples the
    // level uses are read, not the whole 8 MB through a chain of headers.
    uint32_t *wav_offsets = nullptr;
    int32_t wav_count = 0;
    {
        TRX_FILE *idx = nullptr;
        char idx_path[256];
        snprintf(idx_path, sizeof(idx_path), "%s.idx", file_name);
        if (IS_OK(File_OpenPath(idx_path, FILE_OPEN_READ, &idx))) {
            wav_count = (int32_t)(File_Size(idx) / sizeof(uint32_t));
            wav_offsets = Memory_Alloc(sizeof(uint32_t) * (wav_count + 1));
            File_ReadData(idx, wav_offsets, sizeof(uint32_t) * wav_count);
            File_Close(idx);
        }
    }
#endif
    const int32_t sample_count = info->samples.offset_count;
    entries = Memory_Alloc(sizeof(M_SAMPLE_ENTRY) * sample_count);
    for (int32_t i = 0; i < sample_count; i++) {
        entries[i].game_index = i;
        entries[i].file_index = info->samples.offsets[i];
    }
    qsort(
        entries, sample_count, sizeof(M_SAMPLE_ENTRY), M_CompareSampleOffsets);

#if defined(__DREAMCAST__)
    for (int32_t k = 0; wav_offsets != nullptr && k < sample_count; k++) {
        const M_SAMPLE_ENTRY *const entry = &entries[k];
        if (entry->file_index < 0 || entry->file_index >= wav_count) {
            LOG_ERROR("Sample %d is not in main.sfx.idx", entry->file_index);
            continue;
        }
        File_Seek(fp, wav_offsets[entry->file_index], FILE_SEEK_SET);
        uint32_t header[11] = {};
        File_ReadData(fp, header, 11 * sizeof(uint32_t));
        if (header[0] != MKTAG('R', 'I', 'F', 'F')
            || header[9] != MKTAG('d', 'a', 't', 'a')) {
            LOG_ERROR("Unexpected sample header for sample %d", k);
            break;
        }
        const size_t header_size = 11 * sizeof(uint32_t);
        const size_t aligned_size = (header[10] + 1) & ~1;
        char *sample_data = Memory_Alloc(aligned_size + header_size);
        memcpy(sample_data, header, header_size);
        File_ReadData(fp, sample_data + header_size, aligned_size);
        SHOULD(Sound_LoadSampleData(
            entry->game_index, sample_data, aligned_size + header_size));
        Memory_FreePointer(&sample_data);
    }
    if (wav_offsets != nullptr) {
        Memory_FreePointer(&wav_offsets);
        goto finish;
    }
#endif
    for (int32_t i = 0, current_sample = 0; current_sample < sample_count;
         i++) {
        uint32_t header[11] = {};
        File_ReadData(fp, header, 11 * sizeof(uint32_t));
        if (header[0] != MKTAG('R', 'I', 'F', 'F')
            || header[2] != MKTAG('W', 'A', 'V', 'E')
            || header[9] != MKTAG('d', 'a', 't', 'a')) {
            LOG_ERROR("Unexpected sample header for sample %d", i);
            goto finish;
        }

        const size_t header_size = 11 * sizeof(uint32_t);
        const size_t aligned_size = (header[10] + 1) & ~1;
        const size_t size = aligned_size + header_size;
        const M_SAMPLE_ENTRY *const entry = &entries[current_sample];
        if (entry->file_index != i) {
            File_Seek(fp, aligned_size, FILE_SEEK_CUR);
            continue;
        }

        char *sample_data = Memory_Alloc(size);
        memcpy(sample_data, header, header_size);
        File_ReadData(fp, sample_data + header_size, aligned_size);
        SHOULD(Sound_LoadSampleData(entry->game_index, sample_data, size));
        Memory_FreePointer(&sample_data);

        current_sample++;
    }

finish:
    if (fp != nullptr) {
        File_Close(fp);
    }
    Memory_FreePointer(&entries);
    Memory_FreePointer(&info->samples.offsets);
    Benchmark_End(&benchmark, nullptr);
}

static void M_InitialiseSamplesFromLevelInfo(LEVEL_CONTEXT *const ctx)
{
    BENCHMARK benchmark = Benchmark_Start();
    LEVEL_CONTEXT_INFO *const info = &ctx->info;
    const int32_t sample_count = info->samples.offset_count;

    // TODO: this assumes that sample pointers are sorted - adopt TR2's approach
    // of sorting by index, verifying WAV headers and using WAV sample size.
    for (int32_t i = 0; i < sample_count; i++) {
        const int32_t current_offset = info->samples.offsets[i];
        const int32_t next_offset = i + 1 >= sample_count
            ? info->samples.data_size
            : info->samples.offsets[i + 1];

        const char *const sample_data = &info->samples.data[current_offset];
        const size_t sample_size = next_offset - current_offset;
        SHOULD(Sound_LoadSampleData(i, sample_data, sample_size));
    }

    Memory_FreePointer(&info->samples.offsets);
    Benchmark_End(&benchmark, nullptr);
}

static void M_MarkWaterEdgeVertices(void)
{
    if (!g_Config.visuals.fix_texture_issues) {
        return;
    }

    BENCHMARK benchmark = Benchmark_Start();
    for (int32_t i = 0; i < Room_GetCount(); i++) {
        const ROOM *const room = Room_Get(i);
        const int32_t y_test =
            room->flags.underwater ? room->max_ceiling : room->min_floor;
        for (int32_t j = 0; j < room->mesh.num_vertices; j++) {
            ROOM_VERTEX *const vertex = &room->mesh.vertices[j];
            if (vertex->pos.y == y_test) {
                vertex->flags.disable_wibble = true;
            }
        }
    }

    Benchmark_End(&benchmark, nullptr);
}

static void M_CompleteSetup(
    const LEVEL_FORMAT_LOADER *const loader, const GF_LEVEL *const level)
{
    BENCHMARK benchmark = Benchmark_Start();
    LEVEL_CONTEXT *const ctx = Level_Context_Get();

    // We inject explosions sprites and sounds, although in the original game,
    // some levels lack them, resulting in no audio or visual effects when
    // killing mutants. This is to maintain that feature.
    Atlantean_ToggleExplosions(Object_Get(O_EXPLOSION_1)->loaded);

#if defined(__DREAMCAST__)
    DC_Loading_SetStep("Applying injections");
#endif
    Inject_AllInjections();
    // Every injection has been applied; their data need not wait for the
    // rest of the level to be set up to be let go.
    Inject_ReleasePayloads();

#if defined(__DREAMCAST__)
    DC_Loading_SetStep("Preparing the level");
#endif
    Level_Finalize_LoadAnimFrames(ctx);
    Level_Finalize_LoadAnimCommands(ctx);
    if (g_TRVersion == 1) {
        M_MarkWaterEdgeVertices();
    }
    Level_Finalize_LoadObjectsAndItems(ctx);

    // Configure enemies who carry and drop items
    Carrier_InitialiseLevel(level);

#if defined(__DREAMCAST__)
    // The Dreamcast does not count every level at startup
    // (Stats_CalculateMaxStats), which leaves the secrets without their bits:
    // none would count, nor play its music. This level is counted now, as the
    // startup does it (items and carriers in place), and its secret items,
    // set up already, are handed the bits they were given.
    if ((level->type == GFL_NORMAL || level->type == GFL_BONUS)
        && Stats_HasLevelMaxStats(level)) {
        Stats_ScanLevel(level);
        for (int32_t i = 0; i < Item_GetLevelCount(); i++) {
            ITEM *const item = Item_Get(i);
            if (item->priv != nullptr
                && ObjectFamily_Has(item->object_id, OBJ_FAMILY_SECRET)) {
                Pickup_SetSecretMask(
                    item, Stats_GetSecretMaskForItem(level, (int16_t)i));
            }
        }
    }
#endif

    Level_Finalize_LoadRooms(ctx);
    Level_Finalize_LoadTextures(ctx);
    Level_Finalize_LoadTexturePages(ctx);
    Level_Finalize_LoadPalettes(ctx);
    Camera_SetupSequences();

    if (loader->game_version == 1 || loader->game_version >= 4) {
        M_InitialiseSamplesFromLevelInfo(ctx);
    } else {
        M_InitialiseSamplesFromFile(ctx, level->settings.sfx_path);
    }
    SHOULD(Sound_CheckSamples(), "those sounds stay silent");

    Benchmark_End(&benchmark, nullptr);
}

#if defined(__DREAMCAST__)
static uint64_t M_GetFileSize(const char *const path)
{
    uint64_t size = 0;
    if (path == nullptr || !FS_GetMeta(path, &size, nullptr)) {
        return 0;
    }
    return size;
}

// What the load reads from the disc: the level, its injections and its
// sounds, for the loading screen's percentage.
static uint64_t M_GetLoadSize(const GF_LEVEL *const level)
{
    uint64_t total = M_GetFileSize(level->path);
    for (int32_t i = 0; i < level->injections.count; i++) {
        total += M_GetFileSize(level->injections.data_paths[i]);
    }
    for (int32_t i = 0; i < Inject_GetDeclaredCount(); i++) {
        total += M_GetFileSize(Inject_GetDeclaredPath(i));
    }
    if (level->settings.sfx_path != nullptr) {
        char idx_path[256];
        snprintf(idx_path, sizeof(idx_path), "%s.idx", level->settings.sfx_path);
        // With its index only the level's own samples are read (~1 MB).
        total += M_GetFileSize(idx_path) > 0
            ? 1024 * 1024
            : M_GetFileSize(level->settings.sfx_path);
    }
    return total;
}
#endif

#if defined(__DREAMCAST__)
int32_t LUA_DC_CollectGarbage(void);
#endif

RESULT Level_Pipeline_Load(const GF_LEVEL *const level)
{
    LOG_INFO("%d (%s)", level->num, level->path);
    BENCHMARK benchmark = Benchmark_Start();
#if defined(__DREAMCAST__)
    #if defined(TRX_DC_WATCHDOG)
    // What the previous level (or the title) left behind.
    DC_AllocTrace_DumpLive(level->path);
    #endif
    // The previous level's data goes before the injections are read: both at
    // once do not fit, and a restart or a level change ran out of memory.
    GameBuf_Reset();
    // Lua's garbage is spread over what the previous level held; letting it
    // go leaves room for the next level's big blocks.
    LUA_DC_CollectGarbage();
    {
        // The room the level loads into: the largest free span is what
        // counts, and should stay near the whole of what is free.
        DC_LEVEL_HEAP_STATS heap;
        DC_LevelHeap_GetStats(&heap);
#if defined(TRX_DC_WATCHDOG) && defined(TRX_DC_HEAP_CENSUS)
        DC_LevelHeap_DumpLive("level load");
        DC_LevelHeap_DumpLayout();
#endif
        LOG_INFO(
            "game heap: %dK, %dK in %d blocks, largest free %dK, "
            "peak %dK, %d spilled",
            heap.size_kb, heap.used_kb, heap.blocks, heap.largest_free_kb,
            heap.peak_kb, heap.spilled);
    }
    #if defined(TRX_DC_WATCHDOG)
    DC_AllocTrace_DumpKept(level->path);
    DC_AllocTrace_DumpHeapMap(level->path);
    // From here on: what the loads leave is listed at the next.
    DC_AllocTrace_StartKeeping();
    #endif
    // The picture the level's loading screen would show, behind the bar.
    const char *picture = nullptr;
    for (int32_t i = 0; i < level->sequence.length; i++) {
        const GF_SEQUENCE_EVENT *const event = &level->sequence.events[i];
        if (event->type == GFS_LOADING_SCREEN) {
            picture = ((const GF_DISPLAY_PICTURE_DATA *)event->data)->path;
            break;
        }
    }
    if (picture == nullptr && level->type == GFL_TITLE) {
        // Back to the menu: its own picture.
        picture = g_GameFlow.main_menu_background_path;
    }
    DC_Loading_Begin(M_GetLoadSize(level), picture);
#endif

    Inject_InitLevel(level, INJECTION_MODE_FULL);
#if defined(__DREAMCAST__) && defined(TRX_DC_WATCHDOG)
    {
        // What the injections take while the level reads beside them: the
        // load's peak is the two together.
        DC_LEVEL_HEAP_STATS heap;
        DC_LevelHeap_GetStats(&heap);
        LOG_INFO(
            "game heap after the injections: %dK in %d blocks, largest free "
            "%dK",
            heap.used_kb, heap.blocks, heap.largest_free_kb);
    }
#endif
    const LEVEL_FORMAT_LOADER *loader = nullptr;
    const RESULT result = Level_Format_LoadFromFile(level, &loader);
    if (IS_OK(result)) {
        M_CompleteSetup(loader, level);
    }
    Inject_Cleanup();

#if defined(__DREAMCAST__)
    DC_Loading_End();
    #if defined(TRX_DC_WATCHDOG) && defined(TRX_DC_HEAP_CENSUS)
    // What the level is made of, by who asked for it (live_census.py), and
    // where it lies.
    DC_LevelHeap_DumpLive("level loaded");
    DC_LevelHeap_DumpLayout();
    #endif
#endif
    Benchmark_End(&benchmark, nullptr);
    return result;
}
