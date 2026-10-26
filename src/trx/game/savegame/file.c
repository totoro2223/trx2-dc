#include <trx/game/savegame/file.h>

#include <trx/core/bson.h>
#include <trx/core/file.h>
#include <trx/core/memory.h>
#include <trx/core/strings.h>
#include <trx/core/utils.h>
#include <trx/game/game.h>
#include <trx/game/game_flow.h>
#include <trx/game/lua/store.h>
#include <trx/game/savegame.h>
#include <trx/game/sparks/manager.h>
#include <trx/version.h>

#if defined(__DREAMCAST__)
    #include <platform/dreamcast/dc_level_heap.h>
    #include <platform/dreamcast/dc_save_arena.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zconf.h>
#include <zlib.h>

#define M_MAGIC_TR1X MKTAG('T', '1', 'M', 'B') // TOOD: remove me after TRX 1.5
#define M_MAGIC_TR2X MKTAG('T', '2', 'X', 'B') // TOOD: remove me after TRX 1.5
#define M_MAGIC_TRX MKTAG('T', 'R', 'X', 'S')

static RESULT M_ParseFromBuffer(
    const char *const buffer, int32_t *const version_out,
    JSON_VALUE **const out_root)
{
    *out_root = nullptr;

    const SAVEGAME_BSON_HEADER *const header = (SAVEGAME_BSON_HEADER *)buffer;
    if (header->magic != M_MAGIC_TR1X && header->magic != M_MAGIC_TR2X
        && header->magic != M_MAGIC_TRX) {
        return FAIL("the file is not a savegame");
    }

    if (version_out != nullptr) {
        *version_out = header->version;
    }

    const char *const compressed = buffer + sizeof(SAVEGAME_BSON_HEADER);
    char *uncompressed = Memory_Alloc(header->uncompressed_size);

    uLongf uncompressed_size = header->uncompressed_size;
    const int32_t error_code = uncompress(
        (Bytef *)uncompressed, &uncompressed_size, (const Bytef *)compressed,
        (uLongf)header->compressed_size);
    if (error_code != Z_OK) {
        Memory_FreePointer(&uncompressed);
        return FAIL(
            "the savegame data would not decompress (error %d)", error_code);
    }

    JSON_VALUE *root = nullptr;
    const RESULT result = BSON_Parse(uncompressed, uncompressed_size, &root);
    Memory_FreePointer(&uncompressed);
    MUST(result, "the savegame data does not read as BSON");
    *out_root = root;
    return OK;
}

static RESULT M_ReadRaw(
    TRX_FILE *const fp, int32_t *const version_out, JSON_VALUE **const out_root)
{
    File_SetSoftFailure(fp, true);
    const size_t buffer_size = File_Size(fp);
    AUTO_FREE char *buffer = Memory_Alloc(buffer_size);
    File_Seek(fp, 0, FILE_SEEK_SET);
    File_ReadData(fp, buffer, buffer_size);
    if (File_HasFailed(fp)) {
        return FAIL("%s: the saved game ends early", File_GetPath(fp));
    }

    const RESULT result = M_ParseFromBuffer(buffer, version_out, out_root);
    return Result_Prefix(result, "%s", File_GetPath(fp));
}

#if defined(__DREAMCAST__)
// The save's BSON and its compressed copy are 350 KiB at once, and the fuller
// levels have no hole that size left (Barkhang aborted on saving): the copy is
// written as zlib makes it, 16 KiB at a time, and the header's sizes are
// patched at the end. Only the BSON itself is held.
#define M_SAVE_CHUNK (16 * 1024)

#if defined(__DREAMCAST__)
// zlib's own state is a couple of hundred kilobytes, and the heap of the
// fuller levels has none to spare (Barkhang aborted on saving): it comes from
// the level's region, where the tree of the save has just been let go.
static voidpf M_ZAlloc(voidpf const opaque, uInt const items, uInt const size)
{
    const size_t bytes = (size_t)items * size;
    void *const memory = DC_LevelHeap_Alloc(bytes);
    if (memory == nullptr) {
        return malloc(bytes);
    }
    memset(memory, 0, bytes);
    return memory;
}

static void M_ZFree(voidpf const opaque, voidpf const address)
{
    if (address == nullptr) {
        return;
    }
    if (DC_LevelHeap_Owns(address)) {
        DC_LevelHeap_Free(address);
        return;
    }
    free(address);
}
#endif

static RESULT M_SaveCompressed(
    TRX_FILE *const fp, const char *const uncompressed,
    const size_t uncompressed_size, SAVEGAME_BSON_HEADER header)
{
    header.compressed_size = 0;
    header.uncompressed_size = (int32_t)uncompressed_size;
    File_WriteData(fp, &header, sizeof(header));

    static Bytef chunk[M_SAVE_CHUNK];
    z_stream stream = {};
#if defined(__DREAMCAST__)
    stream.zalloc = M_ZAlloc;
    stream.zfree = M_ZFree;
#endif
#if defined(__DREAMCAST__)
    // A 32K window with the default memory level takes 256K of newlib, and
    // what is left of newlib is what the next level loads into: an 8K window
    // and a small buffer compress the save a little less and cost ~64K.
    const int init = deflateInit2(
        &stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 13, 6,
        Z_DEFAULT_STRATEGY);
#else
    const int init = deflateInit(&stream, Z_DEFAULT_COMPRESSION);
#endif
    if (init != Z_OK) {
        return FAIL("the savegame data would not compress");
    }
    stream.next_in = (Bytef *)uncompressed;
    stream.avail_in = (uInt)uncompressed_size;
    size_t written = 0;
    while (true) {
        stream.next_out = chunk;
        stream.avail_out = sizeof(chunk);
        const int32_t zlib_result = deflate(
            &stream, stream.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH);
        if (zlib_result != Z_OK && zlib_result != Z_STREAM_END) {
            deflateEnd(&stream);
            return FAIL("the savegame data would not compress");
        }
        const size_t have = sizeof(chunk) - stream.avail_out;
        if (have != 0) {
            File_WriteData(fp, chunk, have);
            written += have;
        }
        if (zlib_result == Z_STREAM_END) {
            break;
        }
    }
    deflateEnd(&stream);

    // Back over the data to write the sizes it turned out to have.
    header.compressed_size = (int32_t)written;
    File_Seek(fp, 0, FILE_SEEK_SET);
    File_WriteData(fp, &header, sizeof(header));
    File_Seek(fp, sizeof(header) + written, FILE_SEEK_SET);
    return OK;
}
#endif

#if defined(__DREAMCAST__)
// TEMP: the room the save's buffers find themselves with.
static void M_LogRegion(const char *const what)
{
#if defined(TRX_DC_HEAP_CENSUS)
    // TEMP: who is holding the region when the save begins.
    if (strcmp(what, "start") == 0) {
        DC_LevelHeap_DumpLive("save start");
    }
#endif
    DC_LEVEL_HEAP_STATS st;
    DC_LevelHeap_GetStats(&st);
    printf(
        "[save] %s: used %dK of %dK in %d blocks, largest %dK, peak %dK, "
        "spilled %d\n",
        what, (int)st.used_kb, (int)st.size_kb, (int)st.blocks,
        (int)st.largest_free_kb, (int)st.peak_kb, (int)st.spilled);
}
#endif

static RESULT M_SaveBSON(
    TRX_FILE *const fp, const char *const uncompressed,
    const size_t uncompressed_size, const int32_t counter,
    const int32_t level_num, const bool is_quick)
{
#if !defined(__DREAMCAST__)
    uLongf compressed_size = compressBound(uncompressed_size);
    char *compressed = Memory_Alloc(compressed_size);
    const int32_t result = compress(
        (Bytef *)compressed, &compressed_size, (const Bytef *)uncompressed,
        (uLongf)uncompressed_size);
    if (result != Z_OK) {
        Memory_FreePointer(&compressed);
        return FAIL("the savegame data would not compress");
    }
#endif

    const GF_LEVEL *const level = GF_GetLevel(GFLT_MAIN, level_num);
    const SAVEGAME_BSON_HEADER header = {
        .magic = M_MAGIC_TRX,
        .initial_version = Savegame_GetInitialVersion(),
        .version = SG_CURRENT_VERSION,
#if defined(__DREAMCAST__)
        .compressed_size = 0,
#else
        .compressed_size = compressed_size,
#endif
        .uncompressed_size = uncompressed_size,
    };
    const SAVEGAME_BSON_EXTENDED_HEADER extra_header = {
        .flags = Game_GetBonusFlag() | (is_quick ? SAVEGAME_EXT_FLAG_QUICK : 0)
            | SAVEGAME_EXT_FLAG_SAVED_AT,
        .counter = counter,
        .level_num = level->num,
        .title_size = level->title != nullptr ? strlen(level->title) : 0,
    };

#if defined(__DREAMCAST__)
    MUST(M_SaveCompressed(fp, uncompressed, uncompressed_size, header));
#else
    File_WriteData(fp, &header, sizeof(header));
    File_WriteData(fp, compressed, compressed_size);
#endif
    File_WriteData(fp, &extra_header, sizeof(extra_header));
    File_WriteData(
        fp, level->title, level->title != nullptr ? strlen(level->title) : 0);
    const int64_t saved_at = (int64_t)time(nullptr);
    File_WriteData(fp, &saved_at, sizeof(saved_at));

#if !defined(__DREAMCAST__)
    Memory_FreePointer(&compressed);
#endif
    return OK;
}

static RESULT M_SaveRaw(
    TRX_FILE *const fp, const JSON_VALUE *const root, const int32_t level_num,
    const bool is_quick)
{
    size_t uncompressed_size = 0;
    char *uncompressed = nullptr;
    MUST(BSON_Write(root, (void **)&uncompressed, &uncompressed_size));
    const int32_t counter = JSON_ObjectGetInt(
        JSON_ValueAsObject(root), "save_counter", SG_Manager_GetCounter());
    const RESULT result = M_SaveBSON(
        fp, uncompressed, uncompressed_size, counter, level_num, is_quick);
    Memory_FreePointer(&uncompressed);
    return result;
}

static RESULT M_Load(JSON_READ_IO *const io)
{
    MUST(SG_File_LoadResumeInfoList(io));
    MUST(SG_File_LoadMisc(io));
    MUST(SG_File_LoadInventory(io));
    MUST(SG_File_LoadFlipmaps(io));
    MUST(SG_File_LoadCameras(io));
    MUST(SG_File_LoadItems(io));
    MUST(SG_File_LoadEffects(io));
    MUST(SG_File_LoadFX(io));
    MUST(Sparks_Load(io));
    MUST(SG_File_LoadFlares(io));
    MUST(SG_File_LoadMusic(io));
    MUST(SG_File_LoadLara(io));
    MUST(SG_File_LoadRules(io));
    MUST(LUA_Store_Load(io));
    return OK;
}

const char *SG_File_GetSaveFilePattern(void)
{
    return g_GameFlow.savegame_file_fmt;
}

const char *SG_File_GetQuickSaveFilePattern(void)
{
    const char *const pattern = SG_File_GetSaveFilePattern();
    const char *const placeholder = strchr(pattern, '%');
    if (placeholder == nullptr) {
        return String_FormatStatic("%s_q", pattern);
    }
    const int32_t prefix_size = placeholder - pattern;
    return String_FormatStatic("%.*sq%s", prefix_size, pattern, placeholder);
}

RESULT SG_File_LoadFromFile(TRX_FILE *const fp)
{
    int32_t sg_version = -1;
    JSON_VALUE *root = nullptr;
#if defined(__DREAMCAST__)
    // The file, its decompressed copy and the tree they parse into are 400
    // KiB at once, and the main memory of the fuller levels has none: they go
    // in the level's region (the tree lives until it is freed below, before
    // any other level loads). The tree itself is read into the save arena's
    // chunks, as no hole is 700K wide right after a level load.
    DC_SaveArena_Begin();
    DC_LevelHeap_Enter();
#endif
    const RESULT read_result = M_ReadRaw(fp, &sg_version, &root);
#if defined(__DREAMCAST__)
    DC_LevelHeap_Leave();
#endif
    if (!IS_OK(read_result)) {
#if defined(__DREAMCAST__)
        DC_SaveArena_End();
#endif
        return read_result;
    }
    JSON_READ_IO *const io =
        JSON_ReadIO_Create(root, sg_version, File_GetPath(fp));
    const RESULT result = M_Load(io);
    JSON_ReadIO_Destroy(io);
    JSON_ValueFree(root);
#if defined(__DREAMCAST__)
    DC_SaveArena_End();
#endif
    return result;
}

RESULT SG_File_SaveToFile(TRX_FILE *const fp, SAVEGAME_INFO *const info)
{
#if defined(__DREAMCAST__)
    // One section at a time. The whole tree of the game's state is over a
    // megabyte in the fuller levels (Catacombs) and does not fit beside the
    // level's data; a section's tree does, so each is turned into its BSON and
    // let go before the next is built (dc_save_arena.c). What is kept is the
    // BSON, a couple of hundred kilobytes.
    typedef struct {
        const char *name;
        void (*dump)(JSON_WRITE_IO *io);
    } M_SECTION;
    static const M_SECTION sections[] = {
        { "resume_info", SG_File_DumpResumeInfoList },
        { "inventory", SG_File_DumpInventory },
        { "flipmaps", SG_File_DumpFlipmaps },
        { "cameras", SG_File_DumpCameras },
        { "items", SG_File_DumpItems },
        { "effects", SG_File_DumpEffects },
        { "fx", SG_File_DumpFX },
        { "sparks", Sparks_Save },
        { "lara", SG_File_DumpLara },
        { "music", SG_File_DumpMusic },
        { "rules", SG_File_DumpRules },
        { "misc", SG_File_DumpMisc },
        { "lua", LUA_Store_Dump },
    };

    DC_LevelHeap_Enter();
    size_t capacity = 64 * 1024;
    char *bson = Memory_Alloc(capacity);
    // The document's size goes in at the end, when the sections are in.
    size_t bson_size = sizeof(int32_t);
    int32_t counter = SG_Manager_GetCounter();
    RESULT result = OK;
    DC_SaveArena_Begin();
    M_LogRegion("start");
    for (int32_t i = 0; i < (int32_t)ARRAY_SIZE(sections) && IS_OK(result);
         i++) {
        JSON_WRITE_IO *const io = JSON_WriteIO_Create();
        sections[i].dump(io);
        JSON_VALUE *const root = JSON_WriteIO_GetRoot(io);
        counter = JSON_ObjectGetInt(
            JSON_ValueAsObject(root), "save_counter", counter);
        char *doc = nullptr;
        size_t doc_size = 0;
        result = BSON_Write(root, (void **)&doc, &doc_size);
        if (IS_OK(result)) {
            // A document is its size, its elements and a closing zero: the
            // elements are what the save's one document takes from each
            // section.
            const size_t have = doc_size >= 5 ? doc_size - 5 : 0;
            if (bson_size + have > capacity) {
                while (bson_size + have > capacity) {
                    capacity *= 2;
                }
                bson = Memory_Realloc(bson, capacity);
            }
            memcpy(bson + bson_size, doc + sizeof(int32_t), have);
            bson_size += have;
        }
        Memory_Free(doc);
        JSON_WriteIO_Destroy(io);
        DC_SaveArena_Reset();
        M_LogRegion(sections[i].name);
    }
    DC_SaveArena_End();
    if (IS_OK(result)) {
        const int32_t total = (int32_t)(bson_size + 1);
        memcpy(bson, &total, sizeof(total));
        bson[bson_size++] = 0;
        printf("[save] the BSON is %uK\n", (unsigned)(bson_size / 1024));
        result = M_SaveBSON(
            fp, bson, bson_size, counter, Game_GetCurrentLevel()->num,
            info != nullptr && info->is_quick);
    }
    Memory_Free(bson);
    DC_LevelHeap_Leave();
    return result;
#else
    JSON_WRITE_IO *const io = JSON_WriteIO_Create();

    SG_File_DumpResumeInfoList(io);
    SG_File_DumpInventory(io);
    SG_File_DumpFlipmaps(io);
    SG_File_DumpCameras(io);
    SG_File_DumpItems(io);
    SG_File_DumpEffects(io);
    SG_File_DumpFX(io);
    Sparks_Save(io);
    SG_File_DumpLara(io);
    SG_File_DumpMusic(io);
    SG_File_DumpRules(io);
    SG_File_DumpMisc(io);
    LUA_Store_Dump(io);

    JSON_VALUE *const root = JSON_WriteIO_GetRoot(io);
    size_t bson_size = 0;
    char *bson = nullptr;
    RESULT result = BSON_Write(root, (void **)&bson, &bson_size);
    const int32_t counter = JSON_ObjectGetInt(
        JSON_ValueAsObject(root), "save_counter", SG_Manager_GetCounter());
    JSON_WriteIO_Destroy(io);
    if (IS_OK(result)) {
        result = M_SaveBSON(
            fp, bson, bson_size, counter, Game_GetCurrentLevel()->num,
            info != nullptr && info->is_quick);
    }
    Memory_Free(bson);
    return result;
#endif
}

RESULT SG_File_FillInfo(TRX_FILE *const fp, SAVEGAME_INFO *const info)
{
    *info = (SAVEGAME_INFO) {};

    SAVEGAME_BSON_HEADER header = {};
    File_Seek(fp, 0, FILE_SEEK_SET);
    FAIL_IF(
        !File_TryReadData(fp, &header, sizeof(SAVEGAME_BSON_HEADER)),
        "%s: the savegame header could not be read", File_GetPath(fp));
    FAIL_IF(
        header.magic != M_MAGIC_TR1X && header.magic != M_MAGIC_TR2X
            && header.magic != M_MAGIC_TRX,
        "%s: the file is not a savegame", File_GetPath(fp));
    FAIL_IF(
        header.version < SG_MIN_SUPPORTED_VERSION,
        "%s: the savegame is version %d, older than the %d TRX reads",
        File_GetPath(fp), header.version, SG_MIN_SUPPORTED_VERSION);
    info->initial_version = header.initial_version;
    info->features.restart = header.initial_version >= SG_VERSION_LEGACY;
    info->features.select_level = header.initial_version >= SG_VERSION_1;

    // recover the slot information from the end of the file
    File_Skip(fp, header.compressed_size);
    SAVEGAME_BSON_EXTENDED_HEADER extra_header;
    if (File_TryReadData(fp, &extra_header, sizeof(extra_header))) {
        info->counter = extra_header.counter;
        info->level_num = extra_header.level_num;
        info->is_quick = (extra_header.flags & SAVEGAME_EXT_FLAG_QUICK) != 0;
        info->level_title = Memory_Alloc(extra_header.title_size + 1);
        File_ReadData(fp, info->level_title, extra_header.title_size);
        info->saved_at = 0;
        int64_t saved_at;
        if ((extra_header.flags & SAVEGAME_EXT_FLAG_SAVED_AT) != 0
            && File_TryReadData(fp, &saved_at, sizeof(saved_at))) {
            info->saved_at = saved_at;
        }
        return OK;
    }

    // recover the slot information from the savegame structures
    File_Seek(fp, 0, FILE_SEEK_SET);
    JSON_VALUE *root = nullptr;
#if defined(__DREAMCAST__)
    // The slot list is read while a level is loaded, so the tree goes in the
    // arena's chunks here too.
    DC_SaveArena_Begin();
#endif
    const RESULT read_result = M_ReadRaw(fp, nullptr, &root);
    if (!IS_OK(read_result)) {
#if defined(__DREAMCAST__)
        DC_SaveArena_End();
#endif
        return read_result;
    }
    JSON_OBJECT *const root_obj = JSON_ValueAsObject(root);
    RESULT result = OK;
    if (root_obj == nullptr) {
        result =
            FAIL("%s: the savegame holds no root object", File_GetPath(fp));
    } else {
        info->counter = JSON_ObjectGetInt(root_obj, "save_counter", -1);
        info->level_num = JSON_ObjectGetInt(root_obj, "level_num", -1);
        const char *level_title =
            JSON_ObjectGetString(root_obj, "level_title", nullptr);
        if (level_title != nullptr) {
            info->level_title = Memory_DupStr(level_title);
        }
        if (info->level_num == -1) {
            result = FAIL(
                "%s: the savegame names no level number", File_GetPath(fp));
        }
    }
    JSON_ValueFree(root);
#if defined(__DREAMCAST__)
    DC_SaveArena_End();
#endif

    return result;
}

RESULT SG_File_LoadOnlyResumeInfo(TRX_FILE *const fp)
{
    int32_t sg_version = -1;
    JSON_VALUE *root = nullptr;
#if defined(__DREAMCAST__)
    DC_SaveArena_Begin();
    DC_LevelHeap_Enter();
#endif
    const RESULT read_result = M_ReadRaw(fp, &sg_version, &root);
#if defined(__DREAMCAST__)
    DC_LevelHeap_Leave();
#endif
    if (!IS_OK(read_result)) {
#if defined(__DREAMCAST__)
        DC_SaveArena_End();
#endif
        return read_result;
    }
    JSON_READ_IO *const io =
        JSON_ReadIO_Create(root, sg_version, File_GetPath(fp));
    const RESULT result = SG_File_LoadResumeInfoList(io);
    JSON_ReadIO_Destroy(io);
    JSON_ValueFree(root);
#if defined(__DREAMCAST__)
    DC_SaveArena_End();
#endif
    return result;
}

RESULT SG_File_UpdateDeathCounters(
    TRX_FILE *const fp, int32_t level_num, const int32_t death_count,
    const bool is_quick)
{
    JSON_VALUE *root = nullptr;
#if defined(__DREAMCAST__)
    // Read into the arena's chunks, as a load is: the tree is rewritten here
    // and let go at the end.
    DC_SaveArena_Begin();
#endif
    const RESULT read_result = M_ReadRaw(fp, nullptr, &root);
    if (!IS_OK(read_result)) {
#if defined(__DREAMCAST__)
        DC_SaveArena_End();
#endif
        return read_result;
    }

    RESULT result = OK;
    JSON_OBJECT *const root_obj = JSON_ValueAsObject(root);
    if (root_obj == nullptr) {
        result =
            FAIL("%s: the savegame holds no root object", File_GetPath(fp));
        goto cleanup;
    }

    JSON_OBJECT *const misc_obj = JSON_ObjectGetObject(root_obj, "misc");
    if (misc_obj == nullptr) {
        result = FAIL("%s: the savegame holds no 'misc'", File_GetPath(fp));
        goto cleanup;
    }
    JSON_ObjectEvictKey(misc_obj, "death_count");
    JSON_ObjectAppendInt(misc_obj, "death_count", death_count);

    const GF_LEVEL_TABLE *const level_table = GF_GetLevelTable(GFLT_MAIN);
    int32_t resume_idx = -1;
    for (int32_t i = 0; i < level_table->count; i++) {
        if (level_table->levels[i].num == level_num) {
            resume_idx = i;
            break;
        }
    }

    JSON_ARRAY *const resume_arr = JSON_ObjectGetArray(root_obj, "resume_info");
    if (resume_arr != nullptr && resume_idx != -1) {
        JSON_OBJECT *const resume_obj =
            JSON_ArrayGetObject(resume_arr, resume_idx);
        if (resume_obj != nullptr) {
            JSON_ObjectEvictKey(resume_obj, "death_count");
            JSON_ObjectAppendInt(resume_obj, "death_count", death_count);
        }
    }

    File_Seek(fp, 0, FILE_SEEK_SET);
    result = M_SaveRaw(fp, root, level_num, is_quick);

cleanup:
    JSON_ValueFree(root);
#if defined(__DREAMCAST__)
    DC_SaveArena_End();
#endif
    return result;
}
