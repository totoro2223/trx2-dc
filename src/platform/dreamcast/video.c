// Dreamcast film player: the engine's Video_* interface (trx/av/video.h) over
// the discs' own format, made on the host from the games' films by
// tools/dreamcast/fmvconv.c and make_disc.sh. A file is a header and frames,
// each a VQ texture the PowerVR decodes itself and the sound of its time
// (Yamaha ADPCM), so the disc is read in one pass: a few frames at a time into
// a ring of pictures and a queue of sound. Reading the sound from a file of
// its own made the drive seek between the two and the film crawl.
//
// The engine's surface is the frame's carrier: the frame is copied into the
// surface's buffer and uploaded (Output_Quad_Upload sends it on to the
// PowerVR). The sound goes to the AICA as a stream of the film's name, which
// audio.c feeds from this queue (DC_Video_ReadAudio); the stream's place is
// the film's clock.

#include "dc_video.h"
#include "stub.h"

#include <trx/av/video.h>
#include <trx/core/log.h>
#include <trx/core/memory.h>

#include <arch/timer.h>
#include <kos/mutex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M_FRAME_BYTES 18432
#define M_HEADER_BYTES 24
// Frames read at a time, and frames the ring keeps.
#define M_BLOCK_FRAMES 4
#define M_SLOTS 20
// The sound is kept this far ahead (bytes), about a third of a second.
#define M_SOUND_AHEAD 24000
#define M_SOUND_QUEUE 40960

typedef struct {
    FILE *fp;
    uint32_t frames;
    uint32_t fps;
    uint16_t width;
    uint16_t height;
    uint32_t sound_per_frame;

    // The ring of pictures: slot f % M_SLOTS holds frame f while f lies
    // within M_SLOTS of the next one to read.
    uint8_t *slots;
    uint8_t *block;
    uint32_t next;
    bool eof;
    int64_t shown;
    uint32_t presented;
    bool used_audio_clock;
    uint64_t sound_start_us;

    // The sound not yet handed to the AICA.
    uint8_t *queue;
    uint32_t queue_head;
    uint32_t queue_count;
    mutex_t queue_lock;

    void *surface;
    VIDEO_SURFACE_ALLOCATOR_FUNC allocate;
    void *allocate_data;
    void (*deallocate)(void *surface, void *user_data);
    void *deallocate_data;
    void *(*lock)(void *surface, void *user_data);
    void *lock_data;
    void (*unlock)(void *surface, void *user_data);
    void *unlock_data;
    void (*upload)(void *surface, void *user_data);
    void *upload_data;
    void (*render_begin)(void *surface, void *user_data);
    void *render_begin_data;
    void (*render_end)(void *surface, void *user_data);
    void *render_end_data;

    double audio_clock;
    uint64_t start_us;
    uint64_t paused_at_us;
    uint64_t paused_us;
    bool paused;
} M_VIDEO;

// The film being played, which the sound stream finds its queue in.
static M_VIDEO *m_Film = nullptr;

static M_VIDEO *M_Get(VIDEO *const video)
{
    return video->priv;
}

bool DC_Video_GetAudio(
    uint32_t *const rate, int32_t *const channels, uint32_t *const bytes)
{
    if (m_Film == nullptr || m_Film->sound_per_frame == 0) {
        return false;
    }
    *rate = 44100;
    *channels = 2;
    *bytes = m_Film->frames * m_Film->sound_per_frame;
    return true;
}

size_t DC_Video_ReadAudio(
    uint8_t *const dst, const size_t size, bool *const finished)
{
    M_VIDEO *const m = m_Film;
    *finished = true;
    if (m == nullptr) {
        return 0;
    }
    // The AICA takes stereo sound in pieces of 32 bytes a channel.
    mutex_lock(&m->queue_lock);
    size_t n = size < m->queue_count ? size : m->queue_count;
    n &= ~(size_t)63;
    const size_t first =
        n < M_SOUND_QUEUE - m->queue_head ? n : M_SOUND_QUEUE - m->queue_head;
    memcpy(dst, m->queue + m->queue_head, first);
    memcpy(dst + first, m->queue, n - first);
    m->queue_head = (m->queue_head + n) % M_SOUND_QUEUE;
    m->queue_count -= n;
    *finished = m->eof && m->queue_count < 64;
    mutex_unlock(&m->queue_lock);
    return n;
}

// Reads the next frames of the file, as the clock needs them.
static void M_Fill(M_VIDEO *const m)
{
    const uint32_t frame_bytes = M_FRAME_BYTES + m->sound_per_frame;
    while (!m->eof) {
        mutex_lock(&m->queue_lock);
        const uint32_t queued = m->queue_count;
        mutex_unlock(&m->queue_lock);
        const bool need = m->sound_per_frame > 0 && m->used_audio_clock
            ? queued < M_SOUND_AHEAD
            : (int64_t)m->next < m->shown + 8 || m->shown < 0;
        // The ring must keep what is yet to be shown.
        if (!need || (int64_t)m->next + M_BLOCK_FRAMES > m->shown + M_SLOTS) {
            return;
        }
        uint32_t count = m->frames - m->next;
        if (count > M_BLOCK_FRAMES) {
            count = M_BLOCK_FRAMES;
        }
        if (fread(m->block, frame_bytes, count, m->fp) != count) {
            m->eof = true;
            return;
        }
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *const at = m->block + i * frame_bytes;
            memcpy(
                m->slots + (size_t)((m->next + i) % M_SLOTS) * M_FRAME_BYTES,
                at, M_FRAME_BYTES);
            mutex_lock(&m->queue_lock);
            uint32_t room = M_SOUND_QUEUE - m->queue_count;
            if (room > m->sound_per_frame) {
                room = m->sound_per_frame;
            }
            const uint32_t tail = (m->queue_head + m->queue_count) % M_SOUND_QUEUE;
            const uint32_t run =
                room < M_SOUND_QUEUE - tail ? room : M_SOUND_QUEUE - tail;
            memcpy(m->queue + tail, at + M_FRAME_BYTES, run);
            memcpy(m->queue, at + M_FRAME_BYTES + run, room - run);
            m->queue_count += room;
            mutex_unlock(&m->queue_lock);
        }
        m->next += count;
        if (m->next >= m->frames) {
            m->eof = true;
        }
    }
}

RESULT Video_Open(const char *const path, VIDEO **const out_video)
{
    FILE *const fp = fopen(path, "rb");
    FAIL_IF(fp == nullptr, "cannot open %s", path);
    uint8_t head[M_HEADER_BYTES];
    uint32_t version = 0;
    M_VIDEO *const m = calloc(1, sizeof(M_VIDEO));
    if (m == nullptr || fread(head, 1, sizeof(head), fp) != sizeof(head)
        || memcmp(head, "TRXV", 4) != 0) {
        fclose(fp);
        free(m);
        return FAIL("%s is not a film of this disc", path);
    }
    memcpy(&version, head + 4, 4);
    memcpy(&m->frames, head + 8, 4);
    memcpy(&m->fps, head + 12, 4);
    memcpy(&m->width, head + 16, 2);
    memcpy(&m->height, head + 18, 2);
    memcpy(&m->sound_per_frame, head + 20, 4);
    m->slots = malloc((size_t)M_SLOTS * M_FRAME_BYTES);
    m->block =
        malloc((size_t)M_BLOCK_FRAMES * (M_FRAME_BYTES + m->sound_per_frame));
    m->queue = malloc(M_SOUND_QUEUE);
    VIDEO *const video = calloc(1, sizeof(VIDEO));
    if (version != 2 || m->frames == 0 || m->fps == 0 || m->slots == nullptr
        || m->block == nullptr || m->queue == nullptr || video == nullptr) {
        fclose(fp);
        free(m->slots);
        free(m->block);
        free(m->queue);
        free(m);
        free(video);
        return FAIL("%s cannot be played", path);
    }
    m->fp = fp;
    m->shown = -1;
    m->audio_clock = -1.0;
    mutex_init(&m->queue_lock, MUTEX_TYPE_NORMAL);
    video->path = path;
    video->priv = m;
    m_Film = m;
    *out_video = video;
    return OK;
}

void Video_Close(VIDEO *const video)
{
    if (video == nullptr) {
        return;
    }
    M_VIDEO *const m = M_Get(video);
    LOG_INFO(
        "film %s: %u frames shown of %u in %.1f s, %s clock", video->path,
        (unsigned)m->presented, (unsigned)m->frames,
        (double)(timer_us_gettime64() - m->start_us - m->paused_us) / 1e6,
        m->used_audio_clock ? "sound" : "wall");
    if (m->surface != nullptr && m->deallocate != nullptr) {
        m->deallocate(m->surface, m->deallocate_data);
    }
    // The sound stream is closed before this (fmv.c), so nothing reads the
    // queue any more.
    m_Film = nullptr;
    mutex_destroy(&m->queue_lock);
    fclose(m->fp);
    free(m->slots);
    free(m->block);
    free(m->queue);
    free(m);
    free(video);
}

void Video_SetAudioEnabled(VIDEO *video, bool enabled)
{
}

void Video_SetVolume(VIDEO *video, double volume)
{
}

void Video_SetSurfacePixelFormat(VIDEO *video, enum AVPixelFormat pixel_format)
{
}

void Video_SetSurfaceStride(VIDEO *video, int32_t stride)
{
}

void Video_SetSurfaceAllocatorFunc(
    VIDEO *const video, const VIDEO_SURFACE_ALLOCATOR_FUNC func,
    void *const user_data)
{
    M_Get(video)->allocate = func;
    M_Get(video)->allocate_data = user_data;
}

void Video_SetSurfaceDeallocatorFunc(
    VIDEO *const video, void (*const func)(void *surface, void *user_data),
    void *const user_data)
{
    M_Get(video)->deallocate = func;
    M_Get(video)->deallocate_data = user_data;
}

void Video_SetSurfaceClearFunc(
    VIDEO *video, void (*func)(void *surface, void *user_data),
    void *user_data)
{
}

void Video_SetSurfaceLockFunc(
    VIDEO *const video, void *(*const func)(void *surface, void *user_data),
    void *const user_data)
{
    M_Get(video)->lock = func;
    M_Get(video)->lock_data = user_data;
}

void Video_SetSurfaceUnlockFunc(
    VIDEO *const video, void (*const func)(void *surface, void *user_data),
    void *const user_data)
{
    M_Get(video)->unlock = func;
    M_Get(video)->unlock_data = user_data;
}

void Video_SetSurfaceUploadFunc(
    VIDEO *const video, void (*const func)(void *surface, void *user_data),
    void *const user_data)
{
    M_Get(video)->upload = func;
    M_Get(video)->upload_data = user_data;
}

void Video_SetRenderBeginFunc(
    VIDEO *const video, void (*const func)(void *surface, void *user_data),
    void *const user_data)
{
    M_Get(video)->render_begin = func;
    M_Get(video)->render_begin_data = user_data;
}

void Video_SetRenderEndFunc(
    VIDEO *const video, void (*const func)(void *surface, void *user_data),
    void *const user_data)
{
    M_Get(video)->render_end = func;
    M_Get(video)->render_end_data = user_data;
}

void Video_SetExternalAudioClock(VIDEO *const video, const double timestamp)
{
    M_Get(video)->audio_clock = timestamp;
}

void Video_SetPaused(VIDEO *const video, const bool paused)
{
    M_VIDEO *const m = M_Get(video);
    if (paused == m->paused) {
        return;
    }
    const uint64_t now = timer_us_gettime64();
    if (paused) {
        m->paused_at_us = now;
    } else {
        m->paused_us += now - m->paused_at_us;
    }
    m->paused = paused;
}

void Video_Start(VIDEO *const video)
{
    M_VIDEO *const m = M_Get(video);
    if (m->allocate != nullptr) {
        m->surface = m->allocate(m->width, m->height, m->allocate_data);
    }
    // The sound's first moments are in the queue before the stream asks.
    m->used_audio_clock = m->sound_per_frame > 0;
    M_Fill(m);
    m->start_us = timer_us_gettime64();
    video->is_playing = true;
}

void Video_Stop(VIDEO *const video)
{
    video->is_playing = false;
}

void Video_PumpEvents(VIDEO *const video)
{
    M_VIDEO *const m = M_Get(video);
    if (!video->is_playing || m->paused || m->surface == nullptr) {
        return;
    }
    // The film's own clock. The sound's place is known only in steps of what
    // the AICA was last handed (half a second), too coarse to time frames
    // by: the clock runs from the moment the sound begins instead, as the
    // two hold the same pace.
    const uint64_t now = timer_us_gettime64();
    if (m->sound_per_frame > 0 && m->sound_start_us == 0) {
        if (m->audio_clock > 0.0) {
            m->sound_start_us = now;
        }
    }
    double seconds = 0.0;
    if (m->sound_per_frame == 0) {
        seconds = (double)(now - m->start_us - m->paused_us) / 1e6;
    } else if (m->sound_start_us != 0) {
        seconds = (double)(now - m->sound_start_us - m->paused_us) / 1e6;
    }
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    int64_t frame = (int64_t)(seconds * m->fps);
    if (frame >= (int64_t)m->frames) {
        video->is_playing = false;
        return;
    }
    M_Fill(m);
    // Not read yet (the disc is behind), or gone from the ring: the nearest.
    if (frame >= (int64_t)m->next) {
        frame = (int64_t)m->next - 1;
    }
    if (frame < (int64_t)m->next - M_SLOTS) {
        frame = (int64_t)m->next - M_SLOTS;
    }
    if (frame < 0 || frame == m->shown) {
        return;
    }
    m->shown = frame;
    m->presented++;
    if (m->presented % 60 == 1) {
        LOG_INFO(
            "film: frame %d at %.1f s, %u shown, %.1f s since the start",
            (int)frame, seconds, (unsigned)m->presented,
            (double)(timer_us_gettime64() - m->start_us - m->paused_us) / 1e6);
    }
    if (m->render_begin != nullptr) {
        m->render_begin(m->surface, m->render_begin_data);
    }
    uint8_t *const dst = m->lock(m->surface, m->lock_data);
    memcpy(
        dst, m->slots + (size_t)(frame % M_SLOTS) * M_FRAME_BYTES,
        M_FRAME_BYTES);
    m->unlock(m->surface, m->unlock_data);
    m->upload(m->surface, m->upload_data);
    if (m->render_end != nullptr) {
        m->render_end(m->surface, m->render_end_data);
    }
}
