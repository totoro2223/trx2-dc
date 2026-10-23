// Turns a film into the format the Dreamcast plays (src/platform/dreamcast/
// video.c), for make_disc.sh. Built and run on the host, with raw frames from
// FFmpeg on its input:
//
//   ffmpeg -i X.FMV -vf scale=256:256 -f rawvideo -pix_fmt rgb24 - \
//     | fmvconv <output.fmv> <fps> <display width> <display height> \
//         [sound.wav]
//
// Each frame becomes a PowerVR VQ texture of 256x256 RGB565: a 2 KiB
// codebook of 256 2x2 blocks, as the PowerVR lays them out, and a byte per
// block (18 KiB a frame), which the chip decodes by itself. The codebook is
// made afresh for each frame, from the last frame's (the films change little
// from one frame to the next, so a few rounds of k-means are enough, and the
// pictures hold together in time). The frame is the film squeezed to a
// square: the Dreamcast stretches it back.
//
// The sound is the WAV's Yamaha ADPCM (stereo, 44.1 kHz, as the music is),
// cut into as many bytes as a frame lasts and put after each frame, so that
// the player reads picture and sound in one pass over the disc.
//
// Output: "TRXV", u32 version (2), u32 frames, u32 fps, u16 display width,
// u16 display height, u32 sound bytes per frame, then frames x (18432 bytes
// of picture and that many of sound).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <fcntl.h>
    #include <io.h>
#endif

#define W 256
#define BW (W / 2)
#define BLOCKS (BW * BW)
#define CODES 256
#define DIMS 12
#define FRAME_BYTES (CODES * 8 + BLOCKS)
#define FIRST_ROUNDS 8
#define NEXT_ROUNDS 3

// The bits of y and x in turn, y's lowest first (texvq.c's Twiddle).
static int Twiddle(const int x, const int y)
{
    int r = 0;
    for (int bit = 0; bit < 7; bit++) {
        r |= ((y >> bit) & 1) << (2 * bit);
        r |= ((x >> bit) & 1) << (2 * bit + 1);
    }
    return r;
}

static uint16_t Encode(const float *const c)
{
    int r = (int)(c[0] * (31.0f / 255.0f) + 0.5f);
    int g = (int)(c[1] * (63.0f / 255.0f) + 0.5f);
    int b = (int)(c[2] * (31.0f / 255.0f) + 0.5f);
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 63 ? 63 : g;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static void Decode(const uint16_t t, float *const out)
{
    out[0] = (float)(t >> 11) * (255.0f / 31.0f);
    out[1] = (float)((t >> 5) & 63) * (255.0f / 63.0f);
    out[2] = (float)(t & 31) * (255.0f / 31.0f);
}

static int Nearest(const float *const v, float (*const codes)[DIMS])
{
    int best = 0;
    float best_d = 1e30f;
    for (int c = 0; c < CODES; c++) {
        float d = 0.0f;
        for (int k = 0; k < DIMS; k++) {
            const float e = v[k] - codes[c][k];
            d += e * e;
        }
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    return best;
}

int main(const int argc, char **const argv)
{
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: fmvconv <out.fmv> <fps> <dw> <dh> [sound.wav]\n");
        return 1;
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    FILE *const out = fopen(argv[1], "wb");
    if (out == NULL) {
        perror(argv[1]);
        return 1;
    }
    const uint32_t fps = (uint32_t)atoi(argv[2]);
    const uint16_t dw = (uint16_t)atoi(argv[3]);
    const uint16_t dh = (uint16_t)atoi(argv[4]);
    // The sound's bytes: after the WAV's "data" chunk header.
    uint8_t *sound = NULL;
    size_t sound_size = 0;
    const uint32_t per_frame = argc == 6 ? (44100u / fps) & ~1u : 0;
    if (argc == 6) {
        FILE *const wav = fopen(argv[5], "rb");
        if (wav == NULL) {
            perror(argv[5]);
            return 1;
        }
        fseek(wav, 12, SEEK_SET);
        uint8_t chunk[8];
        while (fread(chunk, 1, 8, wav) == 8) {
            uint32_t size;
            memcpy(&size, chunk + 4, 4);
            if (memcmp(chunk, "data", 4) == 0) {
                sound = malloc(size);
                sound_size = fread(sound, 1, size, wav);
                break;
            }
            fseek(wav, (long)((size + 1) & ~1u), SEEK_CUR);
        }
        fclose(wav);
    }
    fwrite("TRXV", 1, 4, out);
    const uint32_t version = 2;
    fwrite(&version, 4, 1, out);
    fwrite(&(uint32_t) { 0 }, 4, 1, out);
    fwrite(&fps, 4, 1, out);
    fwrite(&dw, 2, 1, out);
    fwrite(&dh, 2, 1, out);
    fwrite(&per_frame, 4, 1, out);

    uint8_t *const rgb = malloc(W * W * 3);
    float(*const vec)[DIMS] = malloc(sizeof(float) * DIMS * BLOCKS);
    // The blocks in the order the PowerVR keeps them (see Twiddle).
    uint8_t *const index = malloc(BLOCKS);
    int *const label = malloc(sizeof(int) * BLOCKS);
    static float codes[CODES][DIMS];
    uint32_t frames = 0;
    while (fread(rgb, 1, W * W * 3, stdin) == (size_t)W * W * 3) {
        for (int by = 0; by < BW; by++) {
            for (int bx = 0; bx < BW; bx++) {
                float *const v = vec[by * BW + bx];
                // (0,0), (0,1), (1,0), (1,1) of the block: texels in the
                // order a twiddled page keeps them.
                const int off[4][2] = { { 0, 0 }, { 0, 1 }, { 1, 0 },
                                        { 1, 1 } };
                for (int k = 0; k < 4; k++) {
                    const uint8_t *const p = &rgb
                        [((by * 2 + off[k][1]) * W + bx * 2 + off[k][0]) * 3];
                    for (int c = 0; c < 3; c++) {
                        v[k * 3 + c] = p[c];
                    }
                }
            }
        }
        if (frames == 0) {
            // Evenly spread across the picture to begin with.
            for (int c = 0; c < CODES; c++) {
                memcpy(codes[c], vec[(c * BLOCKS) / CODES + 17], sizeof(codes[c]));
            }
        }
        const int rounds = frames == 0 ? FIRST_ROUNDS : NEXT_ROUNDS;
        for (int round = 0; round < rounds; round++) {
#pragma omp parallel for schedule(static)
            for (int i = 0; i < BLOCKS; i++) {
                label[i] = Nearest(vec[i], codes);
            }
            static double sum[CODES][DIMS];
            static double count[CODES];
            memset(sum, 0, sizeof(sum));
            memset(count, 0, sizeof(count));
            for (int i = 0; i < BLOCKS; i++) {
                count[label[i]] += 1.0;
                for (int k = 0; k < DIMS; k++) {
                    sum[label[i]][k] += vec[i][k];
                }
            }
            for (int c = 0; c < CODES; c++) {
                if (count[c] > 0.0) {
                    for (int k = 0; k < DIMS; k++) {
                        codes[c][k] = (float)(sum[c][k] / count[c]);
                    }
                } else {
                    // A code nobody uses goes to a block the others fit worst.
                    memcpy(codes[c], vec[(rand() % BLOCKS)], sizeof(codes[c]));
                }
            }
        }
        // The codebook as texels, and each block given the nearest of those.
        uint16_t book[CODES][4];
        for (int c = 0; c < CODES; c++) {
            for (int k = 0; k < 4; k++) {
                book[c][k] = Encode(&codes[c][k * 3]);
                Decode(book[c][k], &codes[c][k * 3]);
            }
        }
#pragma omp parallel for schedule(static)
        for (int i = 0; i < BLOCKS; i++) {
            label[i] = Nearest(vec[i], codes);
        }
        for (int by = 0; by < BW; by++) {
            for (int bx = 0; bx < BW; bx++) {
                index[Twiddle(bx, by)] = (uint8_t)label[by * BW + bx];
            }
        }
        fwrite(book, 1, sizeof(book), out);
        fwrite(index, 1, BLOCKS, out);
        // This frame's share of the sound, silence past its end.
        for (uint32_t i = 0; i < per_frame; i++) {
            const size_t at = (size_t)frames * per_frame + i;
            fputc(at < sound_size ? sound[at] : 0, out);
        }
        frames++;
    }
    fseek(out, 8, SEEK_SET);
    fwrite(&frames, 4, 1, out);
    fclose(out);
    fprintf(stderr, "%u frames\n", frames);
    return frames > 0 ? 0 : 1;
}
