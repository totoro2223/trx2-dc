// Compresses the texture pages of TR2 levels and TRX injections to the
// PowerVR's VQ format, for the disc (make_disc.sh). Built and run on the
// host:
//
//   texvq <output.vq> <file.tr2|file.bin>...
//
// Each ARGB1555 page becomes a 2 KiB codebook of 256 2x2 texel blocks and a
// byte per block (18 KiB instead of 128). The game finds a page here by the
// FNV-1a hash of its 16-bit texels as it holds them once the level is loaded
// (DC_PVR_CommitTexturePages); pages changed after that (texture edits) and
// ARGB4444 pages (soft-edged, which VQ would smear) have no entry and stay as
// they are.
//
// Output: "TVQ1", u32 count, count x {u32 hash, u32 position}, sorted by
// hash, then count x 18432 bytes in the order the pages first came (a
// level's pages one after another, so that loading it reads ahead, not back
// and forth); position counts those.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define PAGE_W 256
#define PAGE_SIZE (PAGE_W * PAGE_W)
#define BLOCKS (PAGE_SIZE / 4)
#define CODES 256
#define DIMS 16
#define VQ_BYTES (CODES * 8 + BLOCKS)
#define ITERATIONS 16
// Alpha weighs more than colour: a cut-out edge that moves shows more than a
// shade that does.
#define ALPHA_WEIGHT 4.0f

typedef struct {
    uint32_t hash;
    uint32_t format;
    uint16_t texels[PAGE_SIZE];
    uint8_t vq[VQ_BYTES];
} PAGE;

static PAGE *m_Pages = NULL;
static int m_Count = 0;

// FNV-1a a 32-bit word at a time (little-endian, as on the Dreamcast).
static uint32_t Hash(const void *const data, const size_t size)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; i += 4) {
        uint32_t w;
        memcpy(&w, (const uint8_t *)data + i, 4);
        h = (h ^ w) * 16777619u;
    }
    return h;
}

static void AddPage(const uint16_t *const texels, const uint32_t format)
{
    const uint32_t hash = Hash(texels, PAGE_SIZE * 2);
    for (int i = 0; i < m_Count; i++) {
        if (m_Pages[i].hash == hash && m_Pages[i].format == format) {
            return;
        }
    }
    m_Pages = realloc(m_Pages, sizeof(PAGE) * (m_Count + 1));
    PAGE *const page = &m_Pages[m_Count++];
    page->hash = hash;
    page->format = format;
    memcpy(page->texels, texels, PAGE_SIZE * 2);
}

// As texture_pages_16.h does it.
static uint16_t ToARGB1555(const uint8_t *const c)
{
    return (c[3] >= 128 ? 0x8000 : 0) | ((c[0] >> 3) << 10)
        | ((c[1] >> 3) << 5) | (c[2] >> 3);
}

static uint16_t ToARGB4444(const uint8_t *const c)
{
    return ((c[3] >> 4) << 12) | ((c[0] >> 4) << 8) | ((c[1] >> 4) << 4)
        | (c[2] >> 4);
}

static void AddRGBAPage(const uint8_t *const rgba)
{
    uint32_t format = 0;
    for (int i = 0; i < PAGE_SIZE; i++) {
        if (rgba[i * 4 + 3] != 0 && rgba[i * 4 + 3] != 255) {
            format = 1;
            break;
        }
    }
    static uint16_t texels[PAGE_SIZE];
    for (int i = 0; i < PAGE_SIZE; i++) {
        texels[i] = format ? ToARGB4444(&rgba[i * 4]) : ToARGB1555(&rgba[i * 4]);
    }
    AddPage(texels, format);
}

static uint8_t *ReadFile(const char *const path, size_t *const size)
{
    FILE *const fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    *size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *const data = malloc(*size);
    if (fread(data, 1, *size, fp) != *size) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return data;
}

static int32_t S32(const uint8_t *const p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

// TR2: version, 8-bit palette, 16-bit palette, page count, 8-bit pages, then
// the 16-bit pages the game keeps.
static void ReadLevel(const uint8_t *const data, const size_t size)
{
    const size_t at = 4 + 768 + 1024;
    if (size < at + 4) {
        return;
    }
    const int32_t count = S32(data + at);
    const size_t pages_16 = at + 4 + (size_t)count * PAGE_SIZE;
    if (count <= 0 || size < pages_16 + (size_t)count * PAGE_SIZE * 2) {
        return;
    }
    for (int32_t i = 0; i < count; i++) {
        uint16_t texels[PAGE_SIZE];
        memcpy(texels, data + pages_16 + (size_t)i * PAGE_SIZE * 2, sizeof(texels));
        AddPage(texels, 0);
    }
}

// TR1: version 32, the 8-bit pages, and further on the palette, which the game
// needs to make the pages 16-bit before it keys them (level/format/format_tr1.c
// walks the same layout; sections/textures.c does the conversion).
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t at;
    int bad;
} CURSOR;

static int32_t CursorS32(CURSOR *const c)
{
    if (c->at + 4 > c->size) {
        c->bad = 1;
        return 0;
    }
    const int32_t v = S32(c->data + c->at);
    c->at += 4;
    return v;
}

static int32_t CursorU16(CURSOR *const c)
{
    if (c->at + 2 > c->size) {
        c->bad = 1;
        return 0;
    }
    const int32_t v = c->data[c->at] | (c->data[c->at + 1] << 8);
    c->at += 2;
    return v;
}

static void CursorSkip(CURSOR *const c, const int64_t bytes)
{
    if (bytes < 0 || c->at + (uint64_t)bytes > c->size) {
        c->bad = 1;
        return;
    }
    c->at += (size_t)bytes;
}

// A count of s32, then that many items of the size.
static void SkipArrS32(CURSOR *const c, const int64_t item)
{
    CursorSkip(c, (int64_t)CursorS32(c) * item);
}

static void SkipArrU16(CURSOR *const c, const int64_t item)
{
    CursorSkip(c, (int64_t)CursorU16(c) * item);
}

static void ReadLevelTR1(const uint8_t *const data, const size_t size)
{
    CURSOR c = { data, size, 4, 0 };
    const int32_t count = CursorS32(&c);
    const size_t tiles = c.at;
    CursorSkip(&c, (int64_t)count * PAGE_SIZE);
    CursorSkip(&c, 4);
    const int32_t rooms = CursorU16(&c);
    for (int32_t i = 0; i < rooms && !c.bad; i++) {
        CursorSkip(&c, 16);
        SkipArrS32(&c, 2);
        SkipArrU16(&c, 32);
        const int32_t size_z = (int16_t)CursorU16(&c);
        const int32_t size_x = (int16_t)CursorU16(&c);
        CursorSkip(&c, (int64_t)size_z * size_x * 8);
        CursorSkip(&c, 2);
        SkipArrU16(&c, 18);
        SkipArrU16(&c, 18);
        CursorSkip(&c, 4);
    }
    static const int items[] = { 2, 2, 4, 32, 6, 8, 2, 4, 2, 18, 32, 20, 16, 8,
                                 16, 16 };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]) && !c.bad; i++) {
        SkipArrS32(&c, items[i]);
    }
    const int32_t boxes = CursorS32(&c);
    CursorSkip(&c, (int64_t)boxes * 20);
    SkipArrS32(&c, 2); // overlaps
    CursorSkip(&c, (int64_t)boxes * 12); // zones
    SkipArrS32(&c, 2); // animated textures
    SkipArrS32(&c, 22); // items
    CursorSkip(&c, 32 * 256); // light table
    const size_t palette = c.at;
    CursorSkip(&c, 768);
    if (c.bad || count <= 0) {
        fprintf(stderr, "texvq: a TR1 level that does not add up\n");
        return;
    }
    uint8_t rgb[256][3];
    for (int i = 0; i < 256; i++) {
        for (int k = 0; k < 3; k++) {
            const uint8_t v = i == 0 ? 0 : data[palette + i * 3 + k];
            rgb[i][k] = i == 0 ? 0 : (uint8_t)((v << 2) | (v >> 4));
        }
    }
    for (int32_t i = 0; i < count; i++) {
        uint16_t texels[PAGE_SIZE];
        const uint8_t *const page = data + tiles + (size_t)i * PAGE_SIZE;
        for (int j = 0; j < PAGE_SIZE; j++) {
            const uint8_t index = page[j];
            const uint8_t rgba[4] = { rgb[index][0], rgb[index][1],
                                      rgb[index][2], index == 0 ? 0 : 255 };
            texels[j] = ToARGB1555(rgba);
        }
        AddPage(texels, 0);
    }
}

// The texture pages of an injection (see inject/common.c and
// inject/data/textures.c for the layout).
static void ReadInjection(const uint8_t *const data, const size_t size)
{
    const int trxi = memcmp(data, "TRXI", 4) == 0;
    const uLongf unpacked_size = (uint32_t)S32(data + 12);
    const uLong packed_size = (uint32_t)S32(data + 16);
    if (20 + packed_size > size) {
        return;
    }
    uint8_t *const payload = malloc(unpacked_size);
    uLongf out_size = unpacked_size;
    if (uncompress(payload, &out_size, data + 20, packed_size) != Z_OK) {
        free(payload);
        return;
    }
    const uint8_t *p = payload;
    if (trxi) {
        const int32_t tests = S32(p);
        p += 4;
        for (int32_t i = 0; i < tests; i++) {
            p += 8;
            p += 4 + S32(p);
        }
    } else {
        p += 4;
        p += 4 + S32(p);
    }
    const int32_t chunks = S32(p);
    p += 4;
    for (int32_t i = 0; i < chunks; i++) {
        const int32_t type = S32(p);
        p += trxi ? 8 : 4;
        const int32_t blocks = S32(p);
        const int32_t chunk_size = S32(p + 4);
        p += 8;
        const uint8_t *const end = p + chunk_size;
        // ICT_TEXTURE_DATA, and in it IDT_TEXTURE_PAGES: RGBA pages first.
        for (int32_t j = 0; type == 0 && j < blocks; j++) {
            const int32_t data_type = S32(p);
            const int32_t count = S32(p + 4);
            const int32_t block_size = S32(p + 8);
            p += 12;
            for (int32_t k = 0; data_type == 1 && k < count; k++) {
                AddRGBAPage(p + (size_t)k * PAGE_SIZE * 4);
            }
            p += block_size;
        }
        p = end;
    }
    free(payload);
}

// The bits of y and x in turn, y's lowest first (pvr.c's M_Twiddle).
static int Twiddle(const int x, const int y)
{
    int r = 0;
    for (int bit = 0; bit < 8; bit++) {
        r |= ((y >> bit) & 1) << (2 * bit);
        r |= ((x >> bit) & 1) << (2 * bit + 1);
    }
    return r;
}

static void Decode(const uint16_t t, float *const out)
{
    out[0] = (float)((t >> 10) & 31) * (255.0f / 31.0f);
    out[1] = (float)((t >> 5) & 31) * (255.0f / 31.0f);
    out[2] = (float)(t & 31) * (255.0f / 31.0f);
    out[3] = (t & 0x8000) ? 255.0f * ALPHA_WEIGHT : 0.0f;
}

static uint16_t Encode(const float *const c)
{
    int v[3];
    for (int k = 0; k < 3; k++) {
        v[k] = (int)(c[k] * (31.0f / 255.0f) + 0.5f);
        v[k] = v[k] < 0 ? 0 : (v[k] > 31 ? 31 : v[k]);
    }
    return (c[3] >= 127.5f * ALPHA_WEIGHT ? 0x8000 : 0) | (v[0] << 10)
        | (v[1] << 5) | v[2];
}

static float Distance(const float *const a, const float *const b, const float limit)
{
    float d = 0.0f;
    for (int k = 0; k < DIMS; k++) {
        const float e = a[k] - b[k];
        d += e * e;
        if (d >= limit) {
            break;
        }
    }
    return d;
}

static int Nearest(const float *const v, float (*const codes)[DIMS], const int n, float *const dist)
{
    int best = 0;
    float best_d = 3.4e38f;
    for (int c = 0; c < n; c++) {
        const float d = Distance(v, codes[c], best_d);
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    if (dist != NULL) {
        *dist = best_d;
    }
    return best;
}

static int CompareU64(const void *const a, const void *const b)
{
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void Compress(PAGE *const page)
{
    // Blocks in the order the PowerVR keeps them: block i holds texels
    // (0,0), (0,1), (1,0), (1,1) of its 2x2, as a twiddled page would.
    uint64_t *const blocks = malloc(sizeof(uint64_t) * BLOCKS);
    for (int by = 0; by < PAGE_W / 2; by++) {
        for (int bx = 0; bx < PAGE_W / 2; bx++) {
            const uint16_t *const t = &page->texels[by * 2 * PAGE_W + bx * 2];
            blocks[Twiddle(bx, by)] = (uint64_t)t[0] | (uint64_t)t[PAGE_W] << 16
                | (uint64_t)t[1] << 32 | (uint64_t)t[PAGE_W + 1] << 48;
        }
    }
    // The distinct blocks and how often each comes.
    uint64_t *const unique = malloc(sizeof(uint64_t) * BLOCKS);
    memcpy(unique, blocks, sizeof(uint64_t) * BLOCKS);
    qsort(unique, BLOCKS, sizeof(uint64_t), CompareU64);
    float *const weight = malloc(sizeof(float) * BLOCKS);
    int n = 0;
    for (int i = 0; i < BLOCKS; i++) {
        if (n > 0 && unique[n - 1] == unique[i]) {
            weight[n - 1] += 1.0f;
        } else {
            unique[n] = unique[i];
            weight[n++] = 1.0f;
        }
    }
    float (*const vec)[DIMS] = malloc(sizeof(float) * DIMS * n);
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 4; k++) {
            Decode((uint16_t)(unique[i] >> (16 * k)), &vec[i][k * 4]);
        }
    }

    static _Thread_local float codes[CODES][DIMS];
    int code_count = n < CODES ? n : CODES;
    float *const near = malloc(sizeof(float) * n);
    int *const label = malloc(sizeof(int) * n);
    if (n <= CODES) {
        memcpy(codes, vec, sizeof(float) * DIMS * n);
    } else {
        // Seeds far from those chosen, the common blocks first.
        int first = 0;
        for (int i = 1; i < n; i++) {
            first = weight[i] > weight[first] ? i : first;
        }
        memcpy(codes[0], vec[first], sizeof(codes[0]));
        for (int i = 0; i < n; i++) {
            near[i] = Distance(vec[i], codes[0], 3.4e38f);
        }
        for (int c = 1; c < CODES; c++) {
            int pick = 0;
            for (int i = 1; i < n; i++) {
                pick = near[i] * weight[i] > near[pick] * weight[pick] ? i : pick;
            }
            memcpy(codes[c], vec[pick], sizeof(codes[0]));
            for (int i = 0; i < n; i++) {
                const float d = Distance(vec[i], codes[c], near[i]);
                near[i] = d < near[i] ? d : near[i];
            }
        }
        for (int it = 0; it < ITERATIONS; it++) {
            static _Thread_local double sum[CODES][DIMS];
            static _Thread_local double total[CODES];
            memset(sum, 0, sizeof(sum));
            memset(total, 0, sizeof(total));
            int worst = 0;
            for (int i = 0; i < n; i++) {
                label[i] = Nearest(vec[i], codes, CODES, &near[i]);
                worst = near[i] * weight[i] > near[worst] * weight[worst] ? i : worst;
                total[label[i]] += weight[i];
                for (int k = 0; k < DIMS; k++) {
                    sum[label[i]][k] += vec[i][k] * weight[i];
                }
            }
            for (int c = 0; c < CODES; c++) {
                if (total[c] == 0.0) {
                    // An empty code takes the block worst served.
                    memcpy(codes[c], vec[worst], sizeof(codes[0]));
                    near[worst] = 0.0f;
                    continue;
                }
                for (int k = 0; k < DIMS; k++) {
                    codes[c][k] = (float)(sum[c][k] / total[c]);
                }
            }
        }
    }
    // The codebook as the page's texels, and each block given the nearest of
    // those.
    uint16_t book[CODES][4] = {};
    for (int c = 0; c < code_count; c++) {
        for (int k = 0; k < 4; k++) {
            book[c][k] = Encode(&codes[c][k * 4]);
            Decode(book[c][k], &codes[c][k * 4]);
        }
    }
    memcpy(page->vq, book, sizeof(book));
    for (int i = 0; i < BLOCKS; i++) {
        float v[DIMS];
        for (int k = 0; k < 4; k++) {
            Decode((uint16_t)(blocks[i] >> (16 * k)), &v[k * 4]);
        }
        page->vq[CODES * 8 + i] = (uint8_t)Nearest(v, codes, code_count, NULL);
    }
    free(blocks);
    free(unique);
    free(weight);
    free(vec);
    free(near);
    free(label);
}

static int CompareHash(const void *const a, const void *const b)
{
    const uint32_t x = *(const uint32_t *)a;
    const uint32_t y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

int main(const int argc, char **const argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: texvq <output.vq> <file.tr2|file.bin>...\n");
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        size_t size;
        uint8_t *const data = ReadFile(argv[i], &size);
        if (data == NULL || size < 20) {
            fprintf(stderr, "texvq: cannot read %s\n", argv[i]);
            free(data);
            return 1;
        }
        if (memcmp(data, "TRXJ", 4) == 0 || memcmp(data, "TRXI", 4) == 0) {
            ReadInjection(data, size);
        } else if (
            S32(data) == 0x2D || (uint32_t)S32(data) == 0xFF080038u
            || (uint32_t)S32(data) == 0xFF180038u) {
            // TR2, and TR3 (level/format/format_tr3.c), which lays its pages
            // out the same way. DC build 313 shipped TR3 with only its
            // injections' pages in VQ.
            ReadLevel(data, size);
        } else if (S32(data) == 32) {
            ReadLevelTR1(data, size);
        }
        free(data);
    }
    // Only the pages VQ suits (see the top).
    int kept = 0;
    for (int i = 0; i < m_Count; i++) {
        if (m_Pages[i].format == 0) {
            m_Pages[kept++] = m_Pages[i];
        }
    }
    printf("texvq: %d pages (%d ARGB4444 left out)\n", kept, m_Count - kept);
    m_Count = kept;
    #pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < m_Count; i++) {
        Compress(&m_Pages[i]);
    }
    uint32_t (*const index)[2] = malloc(sizeof(*index) * m_Count);
    for (int i = 0; i < m_Count; i++) {
        index[i][0] = m_Pages[i].hash;
        index[i][1] = i;
    }
    qsort(index, m_Count, sizeof(*index), CompareHash);

    FILE *const fp = fopen(argv[1], "wb");
    if (fp == NULL) {
        fprintf(stderr, "texvq: cannot write %s\n", argv[1]);
        return 1;
    }
    const uint32_t count = m_Count;
    fwrite("TVQ1", 1, 4, fp);
    fwrite(&count, 4, 1, fp);
    fwrite(index, sizeof(*index), m_Count, fp);
    for (int i = 0; i < m_Count; i++) {
        fwrite(m_Pages[i].vq, 1, VQ_BYTES, fp);
    }
    fclose(fp);
    return 0;
}
