// Dreamcast video: plays the discs' films (tools/dreamcast/fmvenc.c makes
// them, make_disc.sh puts them in /cd/fmv) for the game's FMV player
// (trx/game/fmv.c), in the place of the desktop's FFmpeg reader.
//
// A film is a run of frames, each a codebook of 256 blocks of 2x2 texels and a
// byte per block. Unpacking one is a table look-up per block, into the BGRA
// surface the game gave; the game draws it. Shortcuts: a film runs on the
// clock of the console, not the sound's (the sound's position runs ahead of
// what is heard by its buffers), and it drops frames when it falls behind.

#include <trx/av/video.h>
#include <trx/core/log.h>
#include <trx/core/memory.h>

#include <arch/timer.h>
#include <kos/fs.h>

#include <stdlib.h>
#include <string.h>

#define M_CODES 256
#define M_CODEBOOK_BYTES (M_CODES * 8)
#define M_HEAD_BYTES 16

typedef void (*M_SURFACE_FUNC)(void *surface, void *user_data);
typedef void *(*M_LOCK_FUNC)(void *surface, void *user_data);

typedef struct {
    file_t fd;
    int32_t width;
    int32_t height;
    int32_t fps;
    int32_t frames;
    size_t frame_bytes;
    uint8_t *frame;
    int32_t shown;
    void *surface;
    uint64_t start_us;
    uint64_t paused_at_us;
    bool paused;

    VIDEO_SURFACE_ALLOCATOR_FUNC allocate;
    void *allocate_data;
    M_SURFACE_FUNC deallocate;
    void *deallocate_data;
    M_SURFACE_FUNC clear;
    void *clear_data;
    M_LOCK_FUNC lock;
    void *lock_data;
    M_SURFACE_FUNC unlock;
    void *unlock_data;
    M_SURFACE_FUNC upload;
    void *upload_data;
    M_SURFACE_FUNC render_begin;
    void *render_begin_data;
    M_SURFACE_FUNC render_end;
    void *render_end_data;
} M_PRIV;

static uint32_t M_ReadLE(const uint8_t *const p, const int32_t bytes)
{
    uint32_t v = 0;
    for (int32_t i = bytes - 1; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

// A texel of the codebook as the game's BGRA surface holds it.
static uint32_t M_Texel(const uint16_t t)
{
    const uint32_t r = (t >> 11) & 31;
    const uint32_t g = (t >> 5) & 63;
    const uint32_t b = t & 31;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16)
        | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

static void M_Unpack(const M_PRIV *const p, uint32_t *const out, const int32_t pitch)
{
    uint32_t colours[M_CODES][4];
    for (int32_t c = 0; c < M_CODES; c++) {
        for (int32_t t = 0; t < 4; t++) {
            colours[c][t] = M_Texel(
                (uint16_t)M_ReadLE(p->frame + c * 8 + t * 2, 2));
        }
    }
    const uint8_t *index = p->frame + M_CODEBOOK_BYTES;
    const int32_t stride = pitch / 4;
    for (int32_t by = 0; by < p->height / 2; by++) {
        uint32_t *const top = out + by * 2 * stride;
        uint32_t *const bottom = top + stride;
        for (int32_t bx = 0; bx < p->width / 2; bx++) {
            const uint32_t *const c = colours[*index++];
            top[bx * 2] = c[0];
            top[bx * 2 + 1] = c[1];
            bottom[bx * 2] = c[2];
            bottom[bx * 2 + 1] = c[3];
        }
    }
}

RESULT Video_Open(const char *const path, VIDEO **const out_video)
{
    const file_t fd = fs_open(path, O_RDONLY);
    FAIL_IF(fd < 0, "cannot open %s", path);
    uint8_t head[M_HEAD_BYTES];
    if (fs_read(fd, head, sizeof(head)) != sizeof(head)
        || memcmp(head, "TRXV", 4) != 0) {
        fs_close(fd);
        return FAIL("%s is not a film this console plays", path);
    }
    M_PRIV *const p = Memory_Alloc(sizeof(M_PRIV));
    p->fd = fd;
    p->width = (int32_t)M_ReadLE(head + 4, 2);
    p->height = (int32_t)M_ReadLE(head + 6, 2);
    p->fps = (int32_t)M_ReadLE(head + 8, 2);
    p->frames = (int32_t)M_ReadLE(head + 12, 4);
    p->frame_bytes =
        M_CODEBOOK_BYTES + (size_t)(p->width / 2) * (size_t)(p->height / 2);
    p->shown = -1;
    if (p->width <= 0 || p->height <= 0 || p->fps <= 0 || p->frames <= 0) {
        fs_close(fd);
        Memory_Free(p);
        return FAIL("%s has a broken header", path);
    }
    p->frame = Memory_Alloc(p->frame_bytes);

    VIDEO *const video = Memory_Alloc(sizeof(VIDEO));
    video->path = path;
    video->priv = p;
    *out_video = video;
    return OK;
}

void Video_Close(VIDEO *const video)
{
    if (video == nullptr) {
        return;
    }
    M_PRIV *const p = video->priv;
    if (p->surface != nullptr && p->deallocate != nullptr) {
        p->deallocate(p->surface, p->deallocate_data);
    }
    fs_close(p->fd);
    Memory_Free(p->frame);
    Memory_Free(p);
    Memory_Free(video);
}

void Video_Start(VIDEO *const video)
{
    M_PRIV *const p = video->priv;
    if (p->allocate != nullptr) {
        p->surface = p->allocate(p->width, p->height, p->allocate_data);
    }
    if (p->surface != nullptr && p->clear != nullptr) {
        p->clear(p->surface, p->clear_data);
    }
    p->start_us = timer_us_gettime64();
    video->is_playing = p->surface != nullptr;
}

void Video_Stop(VIDEO *const video)
{
    video->is_playing = false;
}

void Video_SetPaused(VIDEO *const video, const bool paused)
{
    M_PRIV *const p = video->priv;
    if (paused == p->paused) {
        return;
    }
    const uint64_t now = timer_us_gettime64();
    if (paused) {
        p->paused_at_us = now;
    } else {
        // The film's clock stood still while it was paused.
        p->start_us += now - p->paused_at_us;
    }
    p->paused = paused;
}

void Video_PumpEvents(VIDEO *const video)
{
    M_PRIV *const p = video->priv;
    if (!video->is_playing || p->paused) {
        return;
    }
    const uint64_t elapsed_us = timer_us_gettime64() - p->start_us;
    const int32_t frame = (int32_t)(elapsed_us * (uint64_t)p->fps / 1000000u);
    if (frame >= p->frames) {
        video->is_playing = false;
        return;
    }
    if (frame == p->shown) {
        return;
    }
    if (fs_seek(
            p->fd, M_HEAD_BYTES + (off_t)frame * (off_t)p->frame_bytes,
            SEEK_SET)
            < 0
        || fs_read(p->fd, p->frame, p->frame_bytes) != (ssize_t)p->frame_bytes) {
        LOG_WARNING("%s: frame %d cannot be read", video->path, frame);
        video->is_playing = false;
        return;
    }
    p->shown = frame;
    uint32_t *const pixels =
        p->lock != nullptr ? p->lock(p->surface, p->lock_data) : nullptr;
    if (pixels == nullptr) {
        return;
    }
    M_Unpack(p, pixels, p->width * 4);
    if (p->unlock != nullptr) {
        p->unlock(p->surface, p->unlock_data);
    }
    if (p->render_begin != nullptr) {
        p->render_begin(p->surface, p->render_begin_data);
    }
    if (p->upload != nullptr) {
        p->upload(p->surface, p->upload_data);
    }
    if (p->render_end != nullptr) {
        p->render_end(p->surface, p->render_end_data);
    }
}

void Video_SetAudioEnabled(VIDEO *const video, const bool enabled)
{
}

void Video_SetVolume(VIDEO *const video, const double volume)
{
}

void Video_SetExternalAudioClock(VIDEO *const video, const double timestamp)
{
}

void Video_SetSurfacePixelFormat(
    VIDEO *const video, const enum AVPixelFormat pixel_format)
{
}

void Video_SetSurfaceStride(VIDEO *const video, const int32_t stride)
{
}

#define M_SETTER(name, field, type)                                                void name(VIDEO *const video, type func, void *const user_data)                {                                                                                  M_PRIV *const p = video->priv;                                                 p->field = func;                                                               p->field##_data = user_data;                                               }

M_SETTER(Video_SetSurfaceAllocatorFunc, allocate, VIDEO_SURFACE_ALLOCATOR_FUNC)
M_SETTER(Video_SetSurfaceDeallocatorFunc, deallocate, M_SURFACE_FUNC)
M_SETTER(Video_SetSurfaceClearFunc, clear, M_SURFACE_FUNC)
M_SETTER(Video_SetSurfaceLockFunc, lock, M_LOCK_FUNC)
M_SETTER(Video_SetSurfaceUnlockFunc, unlock, M_SURFACE_FUNC)
M_SETTER(Video_SetSurfaceUploadFunc, upload, M_SURFACE_FUNC)
M_SETTER(Video_SetRenderBeginFunc, render_begin, M_SURFACE_FUNC)
M_SETTER(Video_SetRenderEndFunc, render_end, M_SURFACE_FUNC)
