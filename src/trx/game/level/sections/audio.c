#include <trx/core/benchmark.h>
#include <trx/core/file.h>
#include <trx/core/log.h>
#include <trx/core/memory.h>
#include <trx/debug.h>
#include <trx/game/const.h>
#include <trx/game/game_buf.h>
#include <trx/game/inject.h>
#include <trx/game/level/format/format.h>
#include <trx/game/level/sections/read.h>
#include <trx/game/sound.h>

static size_t M_GetSampleCount(const LEVEL_FORMAT_LOADER *const loader)
{
    switch (loader->game_version) {
    case 1:
        return 256;
    case 2:
    case 3:
    case 4:
        return 370;
    default:
        ASSERT_FAIL();
    }
    return 0;
}

RESULT Level_Section_ReadSamples(LEVEL_CONTEXT *const ctx, TRX_FILE *const file)
{
    BENCHMARK benchmark = Benchmark_Start();
    const LEVEL_FORMAT_LOADER *const loader = ctx->loader;

    const int32_t sample_count = M_GetSampleCount(loader);
    int16_t *const sample_lut = Memory_Alloc(sizeof(int16_t) * sample_count);
    int16_t *const sample_lut_inv =
        Memory_Alloc(sizeof(int16_t) * sample_count);
    File_ReadData(file, sample_lut, sizeof(int16_t) * sample_count);
    for (int32_t i = 0; i < sample_count; i++) {
        if (sample_lut[i] != -1) {
            sample_lut_inv[sample_lut[i]] = i;
        }
    }

    const int32_t num_sample_infos = File_ReadCountS32(file);
    LOG_INFO("sample infos: %d", num_sample_infos);
    for (int32_t i = 0; i < num_sample_infos; i++) {
        SAMPLE_INFO *const sample_info =
            Sound_GetOrCreateSample(sample_lut_inv[i]);
        ASSERT(sample_info != nullptr);
        sample_info->number = File_ReadS16(file);

        if (loader->game_version >= 4) {
            sample_info->volume = File_ReadU8(file) << 6;
            sample_info->range = (File_ReadU8(file) + 1) * WALL_L;
        } else if (loader->game_version >= 3) {
            sample_info->volume = File_ReadU8(file) << 7;
            sample_info->range = File_ReadU8(file) * WALL_L;
        } else {
            sample_info->volume = File_ReadU16(file);
            sample_info->range = 10 * WALL_L;
        }

        if (loader->game_version >= 3) {
            sample_info->randomness = File_ReadU8(file);
            sample_info->pitch = File_ReadS8(file);
        } else {
            sample_info->randomness = File_ReadU16(file);
            sample_info->pitch = 0;
        }

        sample_info->flags.all = File_ReadU16(file);

        Sound_ReserveSampleData(
            sample_info->number, sample_info->flags.num_samples);
        if (loader->game_version == 1) {
            switch (sample_info->flags.mode_bits) {
            case 0:
                sample_info->mode = SAMPLE_MODE_WAIT;
                break;
            case 1:
                sample_info->mode = SAMPLE_MODE_RESTART;
                break;
            case 2:
                sample_info->mode = SAMPLE_MODE_LOOPED;
                break;
            case 3:
                LOG_WARNING(
                    "Unexpected sample mode for sample %d. flags=%0X", i,
                    sample_info->flags);
                break;
            }
        } else {
            switch (sample_info->flags.mode_bits) {
            case 0:
                sample_info->mode = SAMPLE_MODE_NORMAL;
                break;
            case 1:
                sample_info->mode = SAMPLE_MODE_WAIT;
                break;
            case 2:
                sample_info->mode = SAMPLE_MODE_RESTART;
                break;
            case 3:
                sample_info->mode = SAMPLE_MODE_LOOPED;
                break;
            }
        }
    }

    LEVEL_CONTEXT_INFO *const info = &ctx->info;
#if defined(__DREAMCAST__)
    size_t dc_samples_pos = 0;
#endif
    if (loader->game_version == 1) {
        const int32_t data_size = File_ReadS32(file);
        info->samples.data_size = data_size;
        LOG_INFO("%d sample data size", data_size);

#if defined(__DREAMCAST__)
        // The samples are not held: TR1's gym alone has 2.2 MiB of them, and
        // the level's RAM is 6.7 MiB. They are let into the sound memory one
        // at a time once the offsets after them are known (below).
        dc_samples_pos = File_Pos(file);
        File_Skip(file, data_size);
#else
        info->samples.data = GameBuf_Alloc(
            data_size + Inject_GetDataCount(IDT_SAMPLE_DATA), GBUF_SAMPLES);
        File_ReadData(file, info->samples.data, sizeof(char) * data_size);
#endif
    }

    const int32_t num_offsets = File_ReadCountS32(file);
    LOG_INFO("samples: %d", num_offsets);
    info->samples.offset_count = num_offsets;

    info->samples.offsets = Memory_Alloc(
        sizeof(int32_t)
        * (num_offsets + Inject_GetDataCount(IDT_SAMPLE_INDICES)));
    File_ReadData(file, info->samples.offsets, sizeof(int32_t) * num_offsets);

#if defined(__DREAMCAST__)
    if (loader->game_version == 1) {
        // Each sample goes from the file to the sound memory by itself.
        const size_t after = File_Pos(file);
        const int32_t *const offsets = info->samples.offsets;
        for (int32_t i = 0; i < num_offsets; i++) {
            const int32_t next = i + 1 >= num_offsets
                ? info->samples.data_size
                : offsets[i + 1];
            const int32_t size = next - offsets[i];
            if (size <= 0) {
                continue;
            }
            char *const sample = Memory_Alloc(size);
            File_Seek(file, dc_samples_pos + offsets[i], FILE_SEEK_SET);
            File_ReadData(file, sample, size);
            SHOULD(Sound_LoadSampleData(i, sample, size));
            Memory_FreePointer(&sample);
        }
        File_Seek(file, after, FILE_SEEK_SET);
        // Nothing is left for the level's setup to load.
        info->samples.offset_count = 0;
        Memory_FreePointer(&info->samples.offsets);
    }
#endif

    Memory_Free(sample_lut);
    Memory_Free(sample_lut_inv);
    Benchmark_End(&benchmark, nullptr);
    return OK;
}
