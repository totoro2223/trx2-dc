// Compresses a film for the Dreamcast, for the disc (make_disc.sh). Built and
// run on the host, fed raw frames by ffmpeg:
//
//   ffmpeg -i film.rpl -vf fps=15,scale=256:192 -f rawvideo -pix_fmt rgb24 - |
//       fmvenc film.fmv 256 192 15
//
// Each frame is vector quantised on its own, as the textures are: a codebook
// of 256 blocks of 2x2 texels (RGB565) and a byte per block, which the game
// unpacks (src/platform/dreamcast/video.c). Frames are 2048 + w/2 * h/2 bytes
// each, so any frame can be reached by its number.
//
// Output: "TRXV", u16 width, u16 height, u16 fps, u16 0, u32 frames, then the
// frames: 256 x 4 u16 texels (the block's top left, top right, bottom left and
// bottom right), then the block indices in rows.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <fcntl.h>
    #include <io.h>
#endif

#define CODES 256
#define DIMS 12
#define ITERATIONS 3

static void Put16(uint8_t *const p, const uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static float Dist(const float *const a, const float *const b)
{
    float d = 0.0f;
    for (int i = 0; i < DIMS; i++) {
        const float t = a[i] - b[i];
        d += t * t;
    }
    return d;
}

static int Nearest(
    const float *const v, float (*const codes)[DIMS], const int count)
{
    int best = 0;
    float best_d = 1e30f;
    for (int c = 0; c < count; c++) {
        const float d = Dist(v, codes[c]);
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    return best;
}

// Splits the codebook in two at a time (LBG) until it has 256 codes.
static void Train(
    float (*const blocks)[DIMS], const int n, float (*const codes)[DIMS],
    int *const index)
{
    int count = 1;
    for (int i = 0; i < DIMS; i++) {
        float sum = 0.0f;
        for (int b = 0; b < n; b++) {
            sum += blocks[b][i];
        }
        codes[0][i] = sum / n;
    }
    while (count < CODES) {
        for (int c = 0; c < count; c++) {
            for (int i = 0; i < DIMS; i++) {
                codes[count + c][i] = codes[c][i] - 2.0f;
                codes[c][i] += 2.0f;
            }
        }
        count *= 2;
        for (int it = 0; it < ITERATIONS; it++) {
            static __thread float sums[CODES][DIMS];
            static __thread int counts[CODES];
            memset(sums, 0, sizeof(sums));
            memset(counts, 0, sizeof(counts));
            for (int b = 0; b < n; b++) {
                const int c = Nearest(blocks[b], codes, count);
                counts[c]++;
                for (int i = 0; i < DIMS; i++) {
                    sums[c][i] += blocks[b][i];
                }
            }
            for (int c = 0; c < count; c++) {
                if (counts[c] == 0) {
                    // An empty code takes the block that fits worst.
                    int worst = 0;
                    float worst_d = -1.0f;
                    for (int b = 0; b < n; b += 7) {
                        const float d = Dist(
                            blocks[b], codes[Nearest(blocks[b], codes, count)]);
                        if (d > worst_d) {
                            worst_d = d;
                            worst = b;
                        }
                    }
                    memcpy(codes[c], blocks[worst], sizeof(codes[c]));
                    continue;
                }
                for (int i = 0; i < DIMS; i++) {
                    codes[c][i] = sums[c][i] / counts[c];
                }
            }
        }
    }
    for (int b = 0; b < n; b++) {
        index[b] = Nearest(blocks[b], codes, CODES);
    }
}

static uint16_t To565(const float r, const float g, const float b)
{
    const int ri = r < 0 ? 0 : r > 255 ? 255 : (int)(r + 0.5f);
    const int gi = g < 0 ? 0 : g > 255 ? 255 : (int)(g + 0.5f);
    const int bi = b < 0 ? 0 : b > 255 ? 255 : (int)(b + 0.5f);
    return (uint16_t)(((ri >> 3) << 11) | ((gi >> 2) << 5) | (bi >> 3));
}

static void EncodeFrame(
    const uint8_t *const rgb, const int w, const int h, uint8_t *const out)
{
    const int bw = w / 2;
    const int bh = h / 2;
    const int n = bw * bh;
    float(*const blocks)[DIMS] = malloc((size_t)n * sizeof(*blocks));
    int *const index = malloc((size_t)n * sizeof(int));
    float codes[CODES][DIMS];
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            float *const v = blocks[by * bw + bx];
            for (int t = 0; t < 4; t++) {
                const uint8_t *const p =
                    rgb + ((by * 2 + t / 2) * w + bx * 2 + t % 2) * 3;
                v[t * 3] = p[0];
                v[t * 3 + 1] = p[1];
                v[t * 3 + 2] = p[2];
            }
        }
    }
    Train(blocks, n, codes, index);
    for (int c = 0; c < CODES; c++) {
        for (int t = 0; t < 4; t++) {
            Put16(
                out + c * 8 + t * 2,
                To565(codes[c][t * 3], codes[c][t * 3 + 1], codes[c][t * 3 + 2]));
        }
    }
    for (int b = 0; b < n; b++) {
        out[CODES * 8 + b] = (uint8_t)index[b];
    }
    free(blocks);
    free(index);
}

int main(const int argc, char **const argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: fmvenc <out.fmv> <width> <height> <fps>\n");
        return 1;
    }
    const int w = atoi(argv[2]);
    const int h = atoi(argv[3]);
    const int fps = atoi(argv[4]);
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1) || fps <= 0) {
        fprintf(stderr, "fmvenc: even width and height, and a frame rate\n");
        return 1;
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    const size_t rgb_size = (size_t)w * h * 3;
    const size_t frame_size = CODES * 8 + (size_t)(w / 2) * (h / 2);
    size_t capacity = 64;
    size_t frames = 0;
    uint8_t *raw = malloc(capacity * rgb_size);
    while (raw != NULL) {
        if (frames == capacity) {
            capacity *= 2;
            uint8_t *const grown = realloc(raw, capacity * rgb_size);
            if (grown == NULL) {
                break;
            }
            raw = grown;
        }
        if (fread(raw + frames * rgb_size, 1, rgb_size, stdin) != rgb_size) {
            break;
        }
        frames++;
    }
    if (raw == NULL || frames == 0) {
        fprintf(stderr, "fmvenc: no frames\n");
        return 1;
    }
    uint8_t *const coded = malloc(frames * frame_size);
    if (coded == NULL) {
        fprintf(stderr, "fmvenc: out of memory\n");
        return 1;
    }
#pragma omp parallel for schedule(dynamic)
    for (long f = 0; f < (long)frames; f++) {
        EncodeFrame(raw + (size_t)f * rgb_size, w, h, coded + f * frame_size);
    }
    FILE *const fp = fopen(argv[1], "wb");
    if (fp == NULL) {
        fprintf(stderr, "fmvenc: cannot write %s\n", argv[1]);
        return 1;
    }
    uint8_t head[16] = { 'T', 'R', 'X', 'V' };
    Put16(head + 4, (uint32_t)w);
    Put16(head + 6, (uint32_t)h);
    Put16(head + 8, (uint32_t)fps);
    Put16(head + 12, (uint32_t)frames);
    Put16(head + 14, (uint32_t)(frames >> 16));
    fwrite(head, 1, sizeof(head), fp);
    fwrite(coded, 1, frames * frame_size, fp);
    fclose(fp);
    fprintf(stderr, "fmvenc: %zu frames, %zu bytes\n", frames, frames * frame_size + 16);
    return 0;
}
