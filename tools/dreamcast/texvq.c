// Compresses the texture pages of TR2 levels and TRX injections to the
// PowerVR's VQ format, for the disc (make_disc.sh), and with -b makes the
// relief of the level pages the rooms use. Built and run on the host:
//
//   texvq [-b <output.bump>] <output.vq> <file.tr2|file.bin>...
//
// Each ARGB1555 page becomes a 2 KiB codebook of 256 2x2 texel blocks and a
// byte per block (18 KiB instead of 128). The game finds a page here by the
// FNV-1a hash of its 16-bit texels as it holds them once the level is loaded
// (DC_PVR_CommitTexturePages); pages changed after that (texture edits) and
// ARGB4444 pages (soft-edged, which VQ would smear) have no entry and stay as
// they are.
//
// Output: "TVQ2", u32 count, count x {u32 hash, u32 position}, sorted by
// hash, then count x 23896 bytes (the codebook, the indices of the mipmaps
// from 1x1 up to 128x128, the page's own, two bytes of padding; a page with
// no mipmaps would take 18432) in the order the pages first came (a
// level's pages one after another, so that loading it reads ahead, not back
// and forth); position counts those.
//
// The relief (pvr.c's M_LoadDiscBumps): "TBP1" and the same index, then
// count x 32768 bytes, each page's relief as the PowerVR takes it (bump
// texels, twiddled), made by bump_page.h as the game makes it, only for the
// pages some room face uses (the game puts relief on room faces only).

#include "../../src/platform/dreamcast/render/bump_page.h"

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
// The indices of the levels under the whole page (1x1 to 128x128), which the
// PowerVR keeps in front of the whole page's: 1 + 1 + 4 + ... + 4096.
#define MIP_BYTES 5462
// A page on the disc: the codebook, the small levels, the whole page, and two
// bytes so that the next starts on a word.
#define MIP_PAGE_BYTES (CODES * 8 + MIP_BYTES + BLOCKS + 2)
// Alpha weighs more than colour: a cut-out edge that moves shows more than a
// shade that does.
#define ALPHA_WEIGHT 4.0f

typedef struct {
    uint32_t hash;
    uint32_t format;
    uint16_t texels[PAGE_SIZE];
    uint8_t vq[VQ_BYTES];
    uint8_t mip[MIP_BYTES];
} PAGE;

static PAGE *m_Pages = NULL;
static int m_Count = 0;

#define BUMP_TEXELS (DC_BUMP_SIZE * DC_BUMP_SIZE)
#define MAX_TEXTURES 32768

typedef struct {
    uint32_t hash;
    uint16_t texels[BUMP_TEXELS];
} BUMP;

static const char *m_BumpPath = NULL;
static BUMP *m_Bumps = NULL;
static int m_BumpCount = 0;

// An object texture as the game holds it once the level is loaded.
typedef struct {
    uint16_t page;
    uint16_t u[4];
    uint16_t v[4];
    int corners;
} TEXTURE;

// What the relief needs of a level: which textures its rooms use (1 on a
// quad, 2 on a triangle) and the object textures.
typedef struct {
    uint8_t used[MAX_TEXTURES];
    TEXTURE *textures;
    int32_t count;
} LEVEL;

static LEVEL m_Level;

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

// A level read from the start, as level/format/format_tr*.c walk it.
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

// The rooms, noting the textures their faces use (sections/rooms.c): each
// room's mesh is vertices (8 bytes in TR1, 12 after), quads and triangles
// (their vertices and a texture, whose top bit is double-sidedness), and
// sprites.
static void ReadRooms(CURSOR *const c, const int version)
{
    memset(m_Level.used, 0, sizeof(m_Level.used));
    const int32_t rooms = CursorU16(c);
    for (int32_t i = 0; i < rooms && !c->bad; i++) {
        CursorSkip(c, 16);
        const int64_t mesh_size = (int64_t)CursorS32(c) * 2;
        if (mesh_size < 0 || c->at + (uint64_t)mesh_size > c->size) {
            c->bad = 1;
            return;
        }
        CURSOR mesh = { c->data, c->at + (size_t)mesh_size, c->at, 0 };
        CursorSkip(&mesh, (int64_t)CursorU16(&mesh) * (version == 1 ? 8 : 12));
        for (int corners = 4; corners >= 3; corners--) {
            const int32_t faces = CursorU16(&mesh);
            for (int32_t j = 0; j < faces && !mesh.bad; j++) {
                CursorSkip(&mesh, corners * 2);
                m_Level.used[CursorU16(&mesh) & 0x7FFF] |= corners == 4 ? 1 : 2;
            }
        }
        c->bad |= mesh.bad;
        CursorSkip(c, mesh_size);
        SkipArrU16(c, 32); // portals
        const int32_t size_z = (int16_t)CursorU16(c);
        const int32_t size_x = (int16_t)CursorU16(c);
        CursorSkip(c, (int64_t)size_z * size_x * 8); // sectors
        if (version == 1) {
            CursorSkip(c, 2);
            SkipArrU16(c, 18); // lights
            SkipArrU16(c, 18); // static meshes
            CursorSkip(c, 4);
        } else {
            CursorSkip(c, version == 2 ? 6 : 4);
            SkipArrU16(c, 24); // lights
            SkipArrU16(c, 20); // static meshes
            CursorSkip(c, version == 2 ? 4 : 7);
        }
    }
}

// As sections/textures.c's M_DecodeTR3ObjectTextureUVs.
static void DecodeTR3UVs(int16_t *const uv)
{
    uint8_t flags = 0;
    for (int i = 0; i < 8; i++) {
        if ((uv[i] & 0x80) != 0) {
            uv[i] |= 0x00FF;
            flags |= 1 << i;
        } else {
            uv[i] &= 0xFF00;
        }
    }
    for (int i = 0; i < 8; i++) {
        uv[i] += (flags & 1) != 0 ? -256 : 256;
        flags >>= 1;
    }
}

// The object textures (sections/textures.c). A texture is a triangle's
// (three corners) when a room triangle uses it, as finalize/render_assets.c
// makes it, or when its fourth corner is nil, as the triangles of the
// objects' meshes, which are not read here, leave it.
// shortcut: a texture with a fourth corner that only an object mesh's
// triangle uses keeps four corners here (the game drops the fourth); read the
// meshes if a relief ever looks off at such a texture's edge.
static void ReadTextures(CURSOR *const c, const int version)
{
    const int32_t count = CursorS32(c);
    if (count < 0 || count > MAX_TEXTURES
        || c->at + (uint64_t)count * 20 > c->size) {
        c->bad = 1;
        return;
    }
    m_Level.textures = realloc(m_Level.textures, sizeof(TEXTURE) * (count + 1));
    m_Level.count = count;
    for (int32_t i = 0; i < count; i++) {
        TEXTURE *const texture = &m_Level.textures[i];
        CursorSkip(c, 2); // draw type
        texture->page = (uint16_t)CursorU16(c);
        int16_t uv[8];
        for (int k = 0; k < 8; k++) {
            uv[k] = (int16_t)CursorU16(c);
        }
        texture->corners =
            (m_Level.used[i] & 2) != 0 || (uv[6] == 0 && uv[7] == 0) ? 3 : 4;
        if (version == 3) {
            DecodeTR3UVs(uv);
        }
        for (int k = 0; k < 4; k++) {
            texture->u[k] = (uint16_t)uv[k * 2];
            texture->v[k] = (uint16_t)uv[k * 2 + 1];
        }
    }
}

static int Twiddle(int x, int y);

// The relief of one of the level's pages, if its rooms use it and no page
// alike has one yet (by the hash the game keys pages by), as pvr.c's
// M_GetBumpPage makes it.
// shortcut: a page alike in two levels gets the relief of the first, made
// with that level's textures; key by the textures too if they ever differ.
static void AddBump(const int32_t page, const uint16_t *const texels)
{
    if (m_BumpPath == NULL) {
        return;
    }
    int used = 0;
    for (int32_t i = 0; i < m_Level.count && !used; i++) {
        used = m_Level.textures[i].page == page && m_Level.used[i] != 0;
    }
    const uint32_t hash = Hash(texels, PAGE_SIZE * 2);
    for (int i = 0; used && i < m_BumpCount; i++) {
        used = m_Bumps[i].hash != hash;
    }
    if (!used) {
        return;
    }
    static uint8_t height[BUMP_TEXELS];
    static uint16_t owner[BUMP_TEXELS];
    int32_t lo = 255;
    int32_t hi = 0;
    for (int y = 0; y < DC_BUMP_SIZE; y++) {
        for (int x = 0; x < DC_BUMP_SIZE; x++) {
            const uint16_t *const t = &texels[y * 2 * PAGE_W + x * 2];
            const uint16_t block[4] = { t[0], t[PAGE_W], t[1], t[PAGE_W + 1] };
            int32_t luma;
            const uint8_t h =
                DC_Bump_Height(DC_Bump_HalfTexel(block, false), false, &luma);
            height[y * DC_BUMP_SIZE + x] = h;
            if (h != 0) {
                lo = luma < lo ? luma : lo;
                hi = luma > hi ? luma : hi;
            }
        }
    }
    DC_Bump_Stretch(height, lo, hi);
    memset(owner, 0, sizeof(owner));
    for (int32_t i = 0; i < m_Level.count; i++) {
        const TEXTURE *const texture = &m_Level.textures[i];
        if (texture->page == page) {
            DC_Bump_Own(
                owner, (uint16_t)(i + 1), texture->u, texture->v,
                texture->corners);
        }
    }
    m_Bumps = realloc(m_Bumps, sizeof(BUMP) * (m_BumpCount + 1));
    BUMP *const bump = &m_Bumps[m_BumpCount++];
    bump->hash = hash;
    for (int y = 0; y < DC_BUMP_SIZE; y++) {
        for (int x = 0; x < DC_BUMP_SIZE; x++) {
            float nx;
            float ny;
            bump->texels[Twiddle(x, y)] =
                DC_Bump_Slope(height, owner, x, y, &nx, &ny)
                ? DC_Bump_Encode(nx, ny)
                : DC_BUMP_FLAT;
        }
    }
}

// TR2 and TR3: version, 8-bit palette, 16-bit palette, page count, 8-bit
// pages, then the 16-bit pages the game keeps; for the relief, on to the
// object textures (format_tr2.c and format_tr3.c's probes).
static void ReadLevel(
    const uint8_t *const data, const size_t size, const int version)
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
    m_Level.count = 0;
    if (m_BumpPath != NULL) {
        CURSOR c = { data, size, pages_16 + (size_t)count * PAGE_SIZE * 2 + 4,
                     0 };
        ReadRooms(&c, version);
        // From the floor data to the static objects.
        static const int items[] = { 2, 2, 4, 32, 6, 8, 2, 4, 2, 18, 32 };
        for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
            SkipArrS32(&c, items[i]);
        }
        if (version == 3) {
            SkipArrS32(&c, 16); // sprite textures
            SkipArrS32(&c, 8); // sprite sequences
            SkipArrS32(&c, 16); // cameras
            SkipArrS32(&c, 16); // sound sources
            const int32_t boxes = CursorS32(&c);
            CursorSkip(&c, (int64_t)boxes * 8);
            SkipArrS32(&c, 2); // overlaps
            CursorSkip(&c, (int64_t)boxes * 20); // zones
            SkipArrS32(&c, 2); // animated textures
        }
        ReadTextures(&c, version);
        if (c.bad) {
            fprintf(
                stderr, "texvq: a TR%d level that does not add up; no relief\n",
                version);
            m_Level.count = 0;
        }
    }
    for (int32_t i = 0; i < count; i++) {
        uint16_t texels[PAGE_SIZE];
        memcpy(texels, data + pages_16 + (size_t)i * PAGE_SIZE * 2, sizeof(texels));
        AddPage(texels, 0);
        AddBump(i, texels);
    }
}

// TR1: version 32, the 8-bit pages, and further on the palette, which the game
// needs to make the pages 16-bit before it keys them (level/format/format_tr1.c
// walks the same layout; sections/textures.c does the conversion).
static void ReadLevelTR1(const uint8_t *const data, const size_t size)
{
    CURSOR c = { data, size, 4, 0 };
    m_Level.count = 0;
    const int32_t count = CursorS32(&c);
    const size_t tiles = c.at;
    CursorSkip(&c, (int64_t)count * PAGE_SIZE);
    CursorSkip(&c, 4);
    ReadRooms(&c, 1);
    // From the floor data to the sound sources; the object textures are the
    // twelfth.
    static const int items[] = { 2, 2, 4, 32, 6, 8, 2, 4, 2, 18, 32, 20, 16, 8,
                                 16, 16 };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]) && !c.bad; i++) {
        if (i == 11) {
            ReadTextures(&c, 1);
        } else {
            SkipArrS32(&c, items[i]);
        }
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
        AddBump(i, texels);
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

// The levels under the page, each halved by averaging, in the page's own
// codebook (a block of the 2x2 level is one byte, the 1x1 level is that same
// block). The PowerVR keeps their indices from the smallest up, each twiddled:
// the 2x2 level at 1, 4x4 at 2, 8x8 at 6, 16x16 at 0x16, and so on.
// Averaged across the whole page: a texture of the atlas bleeds into its
// neighbours in the far levels.
static void Mipmaps(PAGE *const page, float (*const codes)[DIMS], const int code_count)
{
    float(*cur)[4] = malloc(sizeof(float) * 4 * PAGE_SIZE);
    float(*next)[4] = malloc(sizeof(float) * 4 * PAGE_SIZE);
    for (int i = 0; i < PAGE_SIZE; i++) {
        Decode(page->texels[i], cur[i]);
    }
    int offset = MIP_BYTES;
    for (int size = PAGE_W; size > 2; size /= 2) {
        const int half = size / 2;
        for (int y = 0; y < half; y++) {
            for (int x = 0; x < half; x++) {
                // The colour of a see-through texel is whatever the artist left
                // (black): only the solid ones give theirs to the average.
                const float *const t[4] = {
                    cur[2 * y * size + 2 * x],
                    cur[2 * y * size + 2 * x + 1],
                    cur[(2 * y + 1) * size + 2 * x],
                    cur[(2 * y + 1) * size + 2 * x + 1],
                };
                float solid = 0.0f;
                for (int c = 0; c < 4; c++) {
                    solid += t[c][3] > 0.0f ? 1.0f : 0.0f;
                }
                for (int k = 0; k < 3; k++) {
                    float sum = 0.0f;
                    for (int c = 0; c < 4; c++) {
                        sum += (solid > 0.0f ? (t[c][3] > 0.0f ? 1.0f : 0.0f)
                                             : 1.0f)
                            * t[c][k];
                    }
                    next[y * half + x][k] = sum / (solid > 0.0f ? solid : 4.0f);
                }
                next[y * half + x][3] =
                    (t[0][3] + t[1][3] + t[2][3] + t[3][3]) * 0.25f;
            }
        }
        // next is a level of half x half texels, in blocks of 2x2.
        const int blocks = half / 2;
        offset -= blocks * blocks;
        for (int by = 0; by < blocks; by++) {
            for (int bx = 0; bx < blocks; bx++) {
                float v[DIMS];
                const float *const t[4] = {
                    next[2 * by * half + 2 * bx],
                    next[(2 * by + 1) * half + 2 * bx],
                    next[2 * by * half + 2 * bx + 1],
                    next[(2 * by + 1) * half + 2 * bx + 1],
                };
                for (int c = 0; c < 4; c++) {
                    memcpy(&v[c * 4], t[c], sizeof(float) * 4);
                }
                page->mip[offset + Twiddle(bx, by)] =
                    (uint8_t)Nearest(v, codes, code_count, NULL);
            }
        }
        float(*const swap)[4] = cur;
        cur = next;
        next = swap;
    }
    page->mip[0] = page->mip[1];
    free(cur);
    free(next);
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
    Mipmaps(page, codes, code_count);
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

// The reliefs, in the order the pages first came, under the VQ file's index.
static int WriteBumps(void)
{
    uint32_t (*const index)[2] = malloc(sizeof(*index) * (m_BumpCount + 1));
    for (int i = 0; i < m_BumpCount; i++) {
        index[i][0] = m_Bumps[i].hash;
        index[i][1] = i;
    }
    qsort(index, m_BumpCount, sizeof(*index), CompareHash);
    FILE *const fp = fopen(m_BumpPath, "wb");
    if (fp == NULL) {
        fprintf(stderr, "texvq: cannot write %s\n", m_BumpPath);
        free(index);
        return 1;
    }
    const uint32_t count = m_BumpCount;
    fwrite("TBP1", 1, 4, fp);
    fwrite(&count, 4, 1, fp);
    fwrite(index, sizeof(*index), m_BumpCount, fp);
    for (int i = 0; i < m_BumpCount; i++) {
        fwrite(m_Bumps[i].texels, 2, BUMP_TEXELS, fp);
    }
    fclose(fp);
    free(index);
    printf("texvq: %d page reliefs\n", m_BumpCount);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "-b") == 0) {
        m_BumpPath = argv[2];
        argc -= 2;
        argv += 2;
    }
    if (argc < 3) {
        fprintf(
            stderr,
            "usage: texvq [-b <output.bump>] <output.vq> "
            "<file.tr2|file.bin>...\n");
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
            ReadLevel(data, size, S32(data) == 0x2D ? 2 : 3);
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
    // TRX_MIP=1 writes the pages with their mipmaps ("TVQ2"). Off by default:
    // in Flycast the pages with see-through texels (Lara, the font, the
    // passport) came out corrupt with them, even with the level bias at 0.25,
    // while the data decodes right (29/10/2026, unsolved).
    const bool no_mip = getenv("TRX_MIP") == NULL;
    fwrite(no_mip ? "TVQ1" : "TVQ2", 1, 4, fp);
    fwrite(&count, 4, 1, fp);
    fwrite(index, sizeof(*index), m_Count, fp);
    for (int i = 0; i < m_Count; i++) {
        fwrite(m_Pages[i].vq, 1, CODES * 8, fp);
        if (!no_mip) {
            fwrite(m_Pages[i].mip, 1, MIP_BYTES, fp);
        }
        fwrite(m_Pages[i].vq + CODES * 8, 1, BLOCKS, fp);
        if (!no_mip)
        fwrite("\0\0", 1, 2, fp);
    }
    fclose(fp);
    return m_BumpPath != NULL ? WriteBumps() : 0;
}
