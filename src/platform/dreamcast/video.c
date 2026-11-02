// Dreamcast film player: the engine's Video_* interface (trx/av/video.h) over
// the discs' own format, made on the host from the games' films by
// tools/dreamcast/fmvmux.c and make_disc.sh. A file is a header and frames,
// each the size of the picture (u32), the picture (MPEG-1, as Tomb Raider 4's)
// and the sound of its time (Yamaha ADPCM), so the disc is read in one pass.
//
// The SH4 decodes the pictures (mpeg1.c) and the PowerVR's YUV converter turns
// them into the texture (DC_PVR_SetVideoYUV). The sound goes to the AICA as a
// stream of the film's name, which audio.c feeds from the queue here
// (DC_Video_ReadAudio). The film's clock is what the AICA has played, read
// from its channel (DC_Audio_FilmSeconds): the picture cannot get ahead of the
// sound or fall behind it.
//
// The film plays before the level loads (game_flow/sequencer.c), so the memory
// it needs, some 600 KB of pictures read ahead and the decoder's, is free.

#include "dc_mpeg.h"
#include "dc_video.h"
#include "render/pvr.h"
#include "stub.h"

#include <trx/av/video.h>
#include <trx/core/log.h>
#include <trx/core/memory.h>

#include <arch/timer.h>
#include <kos/mutex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M_HEADER_BYTES 24
// KOS asks the stream for half of its buffer at a time (32704 bytes) and plays
// old data again when it is given less, so the queue keeps more than that.
#define M_SOUND_QUEUE 65536
#define M_SOUND_AHEAD 40000
// Pictures read and not yet in the decoder, and pictures kept in it: a picture
// is decoded once the next is in the decoder's hands.
#define M_PICTURE_SLOTS 128
#define M_DECODER_AHEAD 3
// The AICA going on without its position moving this long means its sound has
// ended: the film's clock goes on by the timer.
#define M_SOUND_STALL_US 500000

typedef struct {
    uint8_t *data;
    uint32_t size;
} M_PICTURE;

typedef struct {
    FILE *fp;
    uint32_t frames;
    uint32_t fps;
    uint16_t width;
    uint16_t height;
    uint32_t sound_per_frame;

    DC_MPEG *mpeg;
    // The pictures read from the file, in order, until the decoder takes them.
    M_PICTURE pictures[M_PICTURE_SLOTS];
    uint32_t picture_head;
    uint32_t picture_count;
    // Frames read from the file, handed to the decoder, and decoded.
    uint32_t read;
    uint32_t fed;
    uint32_t decoded;
    bool eof;
    bool ended;
    uint8_t *record;
    uint32_t record_capacity;
    uint32_t presented;

    // The sound not yet handed to the AICA.
    uint8_t *queue;
    uint32_t queue_head;
    uint32_t queue_count;
    mutex_t queue_lock;

    // The film's clock: the sound played, and when it last moved.
    double heard;
    uint64_t heard_us;

    void *surface;
    VIDEO_SURFACE_ALLOCATOR_FUNC allocate;
    void *allocate_data;
    void (*deallocate)(void *surface, void *user_data);
    void *deallocate_data;
    void (*upload)(void *surface, void *user_data);
    void *upload_data;
    void (*render_begin)(void *surface, void *user_data);
    void *render_begin_data;
    void (*render_end)(void *surface, void *user_data);
    void *render_end_data;

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
    mutex_lock(&m->queue_lock);
    // The AICA takes stereo sound in pieces of 32 bytes a channel.
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

// Puts the sound of a frame in the queue the AICA's stream reads.
static void M_QueueSound(M_VIDEO *const m, const uint8_t *const sound)
{
    mutex_lock(&m->queue_lock);
    uint32_t room = M_SOUND_QUEUE - m->queue_count;
    if (room > m->sound_per_frame) {
        room = m->sound_per_frame;
    }
    const uint32_t tail = (m->queue_head + m->queue_count) % M_SOUND_QUEUE;
    const uint32_t run =
        room < M_SOUND_QUEUE - tail ? room : M_SOUND_QUEUE - tail;
    memcpy(m->queue + tail, sound, run);
    memcpy(m->queue, sound + run, room - run);
    m->queue_count += room;
    mutex_unlock(&m->queue_lock);
}

// Reads frames of the file while the sound's queue wants more: the picture
// goes to a slot, the sound to the queue.
static void M_Fill(M_VIDEO *const m)
{
    while (!m->eof && m->picture_count < M_PICTURE_SLOTS) {
        mutex_lock(&m->queue_lock);
        const uint32_t queued = m->queue_count;
        mutex_unlock(&m->queue_lock);
        const bool need =
            m->sound_per_frame > 0 ? queued < M_SOUND_AHEAD : m->picture_count < 8;
        if (!need) {
            return;
        }
        uint32_t size = 0;
        if (fread(&size, 4, 1, m->fp) != 1 || size == 0 || size > (1u << 20)) {
            m->eof = true;
            return;
        }
        const uint32_t total = size + m->sound_per_frame;
        if (total > m->record_capacity) {
            uint8_t *const grown = realloc(m->record, total);
            if (grown == nullptr) {
                m->eof = true;
                return;
            }
            m->record = grown;
            m->record_capacity = total;
        }
        if (fread(m->record, 1, total, m->fp) != total) {
            m->eof = true;
            return;
        }
        uint8_t *const data = malloc(size);
        if (data == nullptr) {
            m->eof = true;
            return;
        }
        memcpy(data, m->record, size);
        M_PICTURE *const slot =
            &m->pictures[(m->picture_head + m->picture_count) % M_PICTURE_SLOTS];
        slot->data = data;
        slot->size = size;
        m->picture_count++;
        if (m->sound_per_frame > 0) {
            M_QueueSound(m, m->record + size);
        }
        m->read++;
        if (m->read >= m->frames) {
            m->eof = true;
        }
    }
}

// Hands the decoder the pictures it needs to decode the next, and tells it
// when there are no more.
static void M_Feed(M_VIDEO *const m)
{
    while (m->fed < m->decoded + M_DECODER_AHEAD && m->picture_count > 0) {
        M_PICTURE *const slot = &m->pictures[m->picture_head];
        DC_Mpeg_Write(m->mpeg, slot->data, slot->size);
        free(slot->data);
        slot->data = nullptr;
        m->picture_head = (m->picture_head + 1) % M_PICTURE_SLOTS;
        m->picture_count--;
        m->fed++;
    }
    if (m->eof && m->picture_count == 0 && !m->ended) {
        DC_Mpeg_End(m->mpeg);
        m->ended = true;
    }
}

RESULT Video_Open(const char *const path, VIDEO **const out_video)
{
    FILE *const fp = fopen(path, "rb");
    FAIL_IF(fp == nullptr, "cannot open %s", path);
    // A frame is a few KB: read through a big buffer, so the drive is asked
    // for many sectors at a time and not for a few at every frame (a seek
    // each, from the loop that draws).
    setvbuf(fp, nullptr, _IOFBF, 64 * 1024);
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
    if (version == 3) {
        m->mpeg = DC_Mpeg_Create();
    }
    m->queue = malloc(M_SOUND_QUEUE);
    VIDEO *const video = calloc(1, sizeof(VIDEO));
    if (version != 3 || m->frames == 0 || m->fps == 0 || m->mpeg == nullptr
        || m->queue == nullptr || video == nullptr) {
        fclose(fp);
        DC_Mpeg_Destroy(m->mpeg);
        free(m->queue);
        free(m);
        free(video);
        return FAIL("%s cannot be played (it must be MPEG-1)", path);
    }
    m->fp = fp;
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
        "film %s: %u frames shown of %u in %.1f s", video->path,
        (unsigned)m->presented, (unsigned)m->frames,
        (double)(timer_us_gettime64() - m->start_us - m->paused_us) / 1e6);
    if (m->surface != nullptr && m->deallocate != nullptr) {
        m->deallocate(m->surface, m->deallocate_data);
    }
    // The sound stream is closed before this (fmv.c), so nothing reads the
    // queue any more.
    m_Film = nullptr;
    mutex_destroy(&m->queue_lock);
    fclose(m->fp);
    DC_Mpeg_Destroy(m->mpeg);
    for (uint32_t i = 0; i < m->picture_count; i++) {
        free(m->pictures[(m->picture_head + i) % M_PICTURE_SLOTS].data);
    }
    free(m->record);
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
}

void Video_SetSurfaceUnlockFunc(
    VIDEO *const video, void (*const func)(void *surface, void *user_data),
    void *const user_data)
{
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
    M_Fill(m);
    m->start_us = timer_us_gettime64();
    m->heard_us = m->start_us;
    video->is_playing = true;
}

void Video_Stop(VIDEO *const video)
{
    video->is_playing = false;
}

// How far the film has played: the sound the AICA has played. Without sound,
// or once the AICA's sound has ended, the timer.
static double M_Seconds(M_VIDEO *const m, const uint64_t now)
{
    double heard = 0.0;
    if (m->sound_per_frame > 0 && DC_Audio_FilmSeconds(&heard)) {
        heard -= DC_Audio_FilmSilenceSeconds();
        if (heard != m->heard) {
            m->heard = heard;
            m->heard_us = now;
        } else if (m->heard > 0.0 && now - m->heard_us > M_SOUND_STALL_US) {
            return m->heard + (double)(now - m->heard_us) / 1e6;
        }
        return heard;
    }
    return (double)(now - m->start_us - m->paused_us) / 1e6;
}

// Puts a decoded picture on the screen.
static void M_Show(M_VIDEO *const m, const uint32_t *const picture)
{
    if (m->render_begin != nullptr) {
        m->render_begin(m->surface, m->render_begin_data);
    }
    DC_PVR_SetVideoYUV(picture, m->width, m->height);
    m->upload(m->surface, m->upload_data);
    if (m->render_end != nullptr) {
        m->render_end(m->surface, m->render_end_data);
    }
}

void Video_PumpEvents(VIDEO *const video)
{
    M_VIDEO *const m = M_Get(video);
    if (!video->is_playing || m->paused || m->surface == nullptr) {
        return;
    }
    const uint64_t now = timer_us_gettime64();
    double seconds = M_Seconds(m, now);
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    const int64_t frame = (int64_t)(seconds * m->fps);
    if (frame >= (int64_t)m->frames) {
        video->is_playing = false;
        return;
    }

    M_Fill(m);
    // A picture a turn, each one shown; only a film far behind decodes two
    // (each picture needs the last, so none is skipped, and the sound's queue
    // is kept full between them).
    const int limit = frame - (int64_t)m->decoded > 4 ? 2 : 1;
    const uint32_t *picture = nullptr;
    for (int n = 0; n < limit && (int64_t)m->decoded <= frame; n++) {
        M_Feed(m);
        const uint32_t *const next = DC_Mpeg_Decode(m->mpeg);
        if (next == nullptr) {
            break;
        }
        picture = next;
        m->decoded++;
        M_Fill(m);
    }
    if (picture == nullptr) {
        // Everything decoded: the film is over.
        if (m->ended && m->decoded >= m->read) {
            video->is_playing = false;
        }
        return;
    }
    m->presented++;
    if (m->presented % 600 == 1) {
        LOG_INFO(
            "film: picture %u of %u at %.2f s, sound %.2f s", m->decoded,
            m->frames, (double)m->decoded / (double)m->fps, seconds);
    }
    M_Show(m, picture);
}
