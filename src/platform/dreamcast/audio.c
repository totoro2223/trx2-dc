// Dreamcast sound on the AICA.
//
// Effects: each sample goes to sound RAM as 4-bit Yamaha ADPCM, a quarter of
// its 16-bit size (a level's samples fit in the AICA's 2 MiB), and each sound
// plays on a channel of its own, kept for it, so that a looped sound is never
// taken over by another. Volume and pan are the desktop mixer's
// (audio_sample.c): a gain per side in the game's log units, sent to the AICA
// as one volume and a pan. The channels are started here rather than through
// the sound effect manager, so that a paused sound starts again where it
// stopped: from that place in its sample.
//
// Music: the tracks are WAV files of stereo Yamaha ADPCM, which make_disc.sh
// makes from the game's own; KallistiOS's streams play them, fed from the
// disc by a thread of their own.

#include <trx/av/audio.h>

#include <trx/core/memory.h>
#include <trx/version.h>

#include <dc/sound/aica_comm.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <dc/spu.h>
#include <kos/fs.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include <arch/timer.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "dc_video.h"

// The AICA counts a sample's position in 16 bits.
#define M_MAX_LENGTH 65534
#define M_MIN_FREQ 1000
#define M_MAX_FREQ 88200
#define M_MAX_STREAMS SND_STREAM_MAX
#define M_STREAM_BUFFER SND_STREAM_BUFFER_MAX_ADPCM
#define M_POLL_MS 20
// How long a stream's last data take to play out of its buffer.
#define M_TAIL_MS 600

typedef struct {
    uint32_t addr; // in sound RAM, 0 when none
    int32_t length; // in samples
    uint32_t rate;
} M_SAMPLE;

typedef struct {
    bool used;
    bool paused;
    bool looped;
    int32_t sample;
    int32_t volume;
    int32_t pan;
    float pitch;
    // Where the channel was last started from, in samples.
    int32_t from;
    // When a sound that does not loop runs out (us): the AICA keeps its
    // channel keyed on past the end, so the channel cannot tell.
    uint64_t ends_at;
    // What the channel was last told, so that only changes are sent.
    int32_t sent_vol;
    int32_t sent_pan;
    int32_t sent_freq;
} M_SOUND;

typedef struct {
    bool used;
    bool playing;
    bool looped;
    snd_stream_hnd_t hnd;
    file_t fd;
    // The sound of a film, handed over by the film player (video.c).
    bool film;
    int32_t channels;
    uint32_t rate;
    uint32_t data_start;
    uint32_t data_size;
    // Bytes of data handed to the stream so far, from data_start.
    uint32_t pos;
    // Where a loop starts again and where the track ends, in bytes.
    uint32_t loop_from;
    uint32_t end;
    float volume;
    float fade;
    float fade_step; // per poll
    bool fading_out;
    // When the last data will have played (ms), or 0 while there is more.
    uint64_t ends_at;
    void (*finish)(int32_t sound_id, void *user_data);
    void *finish_data;
} M_STREAM;

static bool m_Ready = false;
static bool m_Muted = false;
static uint8_t m_Reverb = 0;
static M_SAMPLE m_Samples[AUDIO_MAX_SAMPLES] = {};
static M_SOUND m_Sounds[AUDIO_MAX_ACTIVE_SAMPLES] = {};
// The channel each sound plays on, taken from the sound effects' own.
static int32_t m_Channels[AUDIO_MAX_ACTIVE_SAMPLES] = {};

static M_STREAM m_Streams[M_MAX_STREAMS] = {};
static mutex_t m_StreamLock = MUTEX_INITIALIZER;
static kthread_t *m_Thread = nullptr;
static volatile bool m_ThreadRun = false;
// What one poll asks for: half a channel's buffer, for both channels.
static uint8_t m_StreamBuffer[M_STREAM_BUFFER] __attribute__((aligned(32)));

// ----------------------------------------------------------------------------
// WAV files

static uint16_t M_Read16(const uint8_t *const p)
{
    return p[0] | (p[1] << 8);
}

static uint32_t M_Read32(const uint8_t *const p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    int32_t format;
    int32_t channels;
    int32_t bits;
    uint32_t rate;
    int32_t block_align;
    uint32_t fmt_at; // the format chunk's body, from the start of the file
    uint32_t fmt_size;
    uint32_t data_at; // from the start of the file
    uint32_t data_size;
} M_WAV;

// Reads a RIFF WAV's format and where its data lie, from as much of its
// start as is given; the data chunk need not be wholly there.
static bool M_ParseWAV(
    const uint8_t *const head, const size_t size, M_WAV *const out)
{
    if (size < 12 || memcmp(head, "RIFF", 4) != 0
        || memcmp(head + 8, "WAVE", 4) != 0) {
        return false;
    }
    *out = (M_WAV) {};
    for (size_t at = 12; at + 8 <= size;) {
        const uint8_t *const chunk = head + at;
        const uint32_t len = M_Read32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0 && len >= 16 && at + 24 <= size) {
            out->format = M_Read16(chunk + 8);
            out->channels = M_Read16(chunk + 10);
            out->rate = M_Read32(chunk + 12);
            out->block_align = M_Read16(chunk + 20);
            out->bits = M_Read16(chunk + 22);
            out->fmt_at = (uint32_t)at + 8;
            out->fmt_size = len;
        } else if (memcmp(chunk, "data", 4) == 0) {
            out->data_at = (uint32_t)at + 8;
            out->data_size = len;
            return out->channels > 0 && out->rate > 0;
        }
        at += 8 + len + (len & 1);
    }
    return false;
}

// Microsoft ADPCM (format 2, as some injected sounds are) as mono 16-bit
// samples, the channels averaged: a new buffer, or nullptr.
static int16_t *M_DecodeMSADPCM(
    const uint8_t *const data, const M_WAV *const wav, const uint8_t *src,
    const size_t src_size, int32_t *const out_count)
{
    static const int16_t adapt[16] = {
        230, 230, 230, 230, 307, 409, 512, 614,
        768, 614, 512, 409, 307, 230, 230, 230,
    };
    static const int16_t standard[7][2] = {
        { 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 },
        { 240, 0 }, { 460, -208 }, { 392, -232 },
    };
    const int32_t ch = wav->channels;
    const int32_t align = wav->block_align;
    if (ch < 1 || ch > 2 || align <= 7 * ch) {
        return nullptr;
    }
    // The file's own coefficients when it gives them.
    int16_t coef[32][2];
    int32_t num_coef = 7;
    memcpy(coef, standard, sizeof(standard));
    if (wav->fmt_size >= 22) {
        const uint8_t *const fmt = data + wav->fmt_at;
        const int32_t n = M_Read16(fmt + 20);
        if (n >= 1 && n <= 32 && wav->fmt_size >= 22 + (uint32_t)n * 4) {
            num_coef = n;
            for (int32_t i = 0; i < n; i++) {
                coef[i][0] = (int16_t)M_Read16(fmt + 22 + i * 4);
                coef[i][1] = (int16_t)M_Read16(fmt + 24 + i * 4);
            }
        }
    }
    const int32_t per_block = 2 + (align - 7 * ch) * 2 / ch;
    const int32_t blocks = (int32_t)(src_size / align);
    if (blocks <= 0) {
        return nullptr;
    }
    int16_t *const out = Memory_Alloc(sizeof(int16_t) * blocks * per_block);
    int32_t count = 0;
    for (int32_t b = 0; b < blocks; b++, src += align) {
        int32_t c1[2];
        int32_t c2[2];
        int32_t delta[2];
        int32_t s1[2];
        int32_t s2[2];
        const uint8_t *p = src;
        for (int32_t c = 0; c < ch; c++) {
            const int32_t idx = p[c] < num_coef ? p[c] : 0;
            c1[c] = coef[idx][0];
            c2[c] = coef[idx][1];
        }
        p += ch;
        for (int32_t c = 0; c < ch; c++, p += 2) {
            delta[c] = (int16_t)M_Read16(p);
        }
        for (int32_t c = 0; c < ch; c++, p += 2) {
            s1[c] = (int16_t)M_Read16(p);
        }
        for (int32_t c = 0; c < ch; c++, p += 2) {
            s2[c] = (int16_t)M_Read16(p);
        }
        // The header's two samples come out oldest first.
        int32_t sum2 = 0;
        int32_t sum1 = 0;
        for (int32_t c = 0; c < ch; c++) {
            sum2 += s2[c];
            sum1 += s1[c];
        }
        out[count++] = (int16_t)(sum2 / ch);
        out[count++] = (int16_t)(sum1 / ch);
        int32_t sum = 0;
        int32_t c = 0;
        for (; p < src + align; p++) {
            for (int32_t half = 0; half < 2; half++) {
                const int32_t nibble = half == 0 ? *p >> 4 : *p & 15;
                const int32_t signed_nibble = nibble >= 8 ? nibble - 16 : nibble;
                int32_t s = ((s1[c] * c1[c] + s2[c] * c2[c]) >> 8)
                    + signed_nibble * delta[c];
                s = s < -32768 ? -32768 : (s > 32767 ? 32767 : s);
                s2[c] = s1[c];
                s1[c] = s;
                delta[c] = (adapt[nibble] * delta[c]) >> 8;
                if (delta[c] < 16) {
                    delta[c] = 16;
                }
                sum += s;
                if (++c == ch) {
                    out[count++] = (int16_t)(sum / ch);
                    sum = 0;
                    c = 0;
                }
            }
        }
    }
    *out_count = count;
    return out;
}

// A PCM WAV of 8 or 16 bits, or a Microsoft ADPCM one, as mono 16-bit
// samples: a new buffer, which the caller frees, or nullptr.
static int16_t *M_DecodeWAV(
    const uint8_t *const data, const size_t size, int32_t *const out_count,
    uint32_t *const out_rate)
{
    M_WAV wav;
    if (!M_ParseWAV(data, size, &wav) || wav.data_at > size) {
        return nullptr;
    }
    const uint8_t *const pcm = data + wav.data_at;
    const size_t room = size - wav.data_at;
    const size_t pcm_size = wav.data_size < room ? wav.data_size : room;
    if (wav.format == 2) {
        *out_rate = wav.rate;
        return M_DecodeMSADPCM(data, &wav, pcm, pcm_size, out_count);
    }
    if (wav.format != 1 || (wav.bits != 8 && wav.bits != 16)) {
        return nullptr;
    }
    const int32_t frame = wav.channels * wav.bits / 8;
    const int32_t count = (int32_t)(pcm_size / frame);
    if (count <= 0) {
        return nullptr;
    }
    int16_t *const out = Memory_Alloc(sizeof(int16_t) * count);
    for (int32_t i = 0; i < count; i++) {
        int32_t sum = 0;
        for (int32_t c = 0; c < wav.channels; c++) {
            const uint8_t *const s = pcm + i * frame + c * wav.bits / 8;
            sum += wav.bits == 16 ? (int16_t)M_Read16(s) : (s[0] - 128) << 8;
        }
        out[i] = (int16_t)(sum / wav.channels);
    }
    *out_count = count;
    *out_rate = wav.rate;
    return out;
}

// One sample to the AICA's ADPCM (ffmpeg's adpcm_yamaha); the state starts as
// the AICA's does, step 127 and nothing predicted.
static uint8_t M_EncodeADPCM(
    const int32_t sample, int32_t *const predictor, int32_t *const step)
{
    static const int8_t diff[16] = {
        1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15,
    };
    static const int16_t scale[8] = {
        230, 230, 230, 230, 307, 409, 512, 614,
    };
    const int32_t delta = sample - *predictor;
    int32_t nibble = abs(delta) * 4 / *step;
    if (nibble > 7) {
        nibble = 7;
    }
    if (delta < 0) {
        nibble += 8;
    }
    const int32_t p = *predictor + (*step * diff[nibble]) / 8;
    *predictor = p < -32768 ? -32768 : (p > 32767 ? 32767 : p);
    const int32_t s = (*step * scale[nibble & 7]) >> 8;
    *step = s < 127 ? 127 : (s > 24576 ? 24576 : s);
    return (uint8_t)nibble;
}

// ----------------------------------------------------------------------------
// Samples into sound RAM

RESULT Audio_Sample_Load(
    const int32_t sample_id, const char *const data, const size_t size)
{
    FAIL_IF(
        sample_id < 0 || sample_id >= AUDIO_MAX_SAMPLES,
        "sample %d is out of range", sample_id);
    FAIL_IF(
        data == nullptr || size == 0, "sample %d carries no data", sample_id);
    MUST(Audio_Init());
    M_SAMPLE *const sample = &m_Samples[sample_id];
    FAIL_IF(sample->addr != 0, "sample %d is already loaded", sample_id);

    int32_t count;
    uint32_t rate;
    int16_t *const pcm =
        M_DecodeWAV((const uint8_t *)data, size, &count, &rate);
    if (pcm == nullptr) {
        M_WAV wav = {};
        M_ParseWAV((const uint8_t *)data, size, &wav);
        return FAIL(
            "sample %d is not a PCM WAV (%.4s, format %d, %d bits)",
            sample_id, data, wav.format, wav.bits);
    }
    // Longer than the AICA can count: halved in rate until it fits.
    while (count > M_MAX_LENGTH) {
        count /= 2;
        for (int32_t i = 0; i < count; i++) {
            pcm[i] = (int16_t)((pcm[2 * i] + pcm[2 * i + 1]) / 2);
        }
        rate /= 2;
    }

    // Two samples a byte, in whole 32-byte blocks as sound RAM takes them.
    const size_t bytes = (((size_t)count + 1) / 2 + 31) & ~(size_t)31;
    uint8_t *const adpcm = Memory_Alloc(bytes);
    memset(adpcm, 0, bytes);
    int32_t predictor = 0;
    int32_t step = 127;
    for (int32_t i = 0; i < count; i++) {
        const uint8_t nibble = M_EncodeADPCM(pcm[i], &predictor, &step);
        adpcm[i / 2] |= (i & 1) ? nibble << 4 : nibble;
    }
    Memory_Free(pcm);
    const uint32_t addr = snd_mem_malloc(bytes);
    if (addr != 0) {
        spu_memload_sq(addr, adpcm, bytes);
    }
    Memory_Free(adpcm);
    FAIL_IF(
        addr == 0, "sample %d does not fit in sound RAM (%u bytes left)",
        sample_id, (unsigned)snd_mem_available());
    *sample = (M_SAMPLE) { .addr = addr, .length = count, .rate = rate };
    return OK;
}

bool Audio_Sample_IsLoaded(const int32_t sample_id)
{
    return sample_id >= 0 && sample_id < AUDIO_MAX_SAMPLES
        && m_Samples[sample_id].addr != 0;
}

RESULT Audio_Sample_Unload(const int32_t sample_id)
{
    FAIL_IF(
        !Audio_Sample_IsLoaded(sample_id), "sample %d is not loaded",
        sample_id);
    for (int32_t i = 0; i < AUDIO_MAX_ACTIVE_SAMPLES; i++) {
        if (m_Sounds[i].used && m_Sounds[i].sample == sample_id) {
            IGNORE(Audio_Sample_Close(i));
        }
    }
    snd_mem_free(m_Samples[sample_id].addr);
    m_Samples[sample_id] = (M_SAMPLE) {};
    return OK;
}

RESULT Audio_Sample_UnloadAll(void)
{
    IGNORE(Audio_Sample_CloseAll());
    for (int32_t i = 0; i < AUDIO_MAX_SAMPLES; i++) {
        if (m_Samples[i].addr != 0) {
            snd_mem_free(m_Samples[i].addr);
            m_Samples[i] = (M_SAMPLE) {};
        }
    }
    return OK;
}

// ----------------------------------------------------------------------------
// Sounds on the channels

static bool M_IsSound(const int32_t sound_id)
{
    return sound_id >= 0 && sound_id < AUDIO_MAX_ACTIVE_SAMPLES
        && m_Sounds[sound_id].used;
}

// audio_sample.c's M_DecibelToMultiplier.
static float M_Gain(const int32_t value)
{
    return g_TRVersion < 3 ? powf(2.0f, value / 600.0f)
                           : powf(10.0f, value / 2000.0f);
}

// The AICA's volume (0-255), pan (0 left, 255 right) and frequency for a
// sound.
static void M_Params(
    const M_SOUND *const sound, int32_t *const vol, int32_t *const pan,
    int32_t *const freq)
{
    const float l = M_Gain(sound->volume - (sound->pan > 0 ? sound->pan : 0));
    const float r = M_Gain(sound->volume + (sound->pan < 0 ? sound->pan : 0));
    const float loud = l > r ? l : r;
    const int32_t v = m_Muted ? 0 : (int32_t)(loud * 255.0f + 0.5f);
    *vol = v > 255 ? 255 : v;
    *pan = l + r > 0.0f ? 128 + (int32_t)(127.0f * (r - l) / (r + l)) : 128;
    const int32_t f =
        (int32_t)(m_Samples[sound->sample].rate * sound->pitch + 0.5f);
    *freq = f < M_MIN_FREQ ? M_MIN_FREQ : (f > M_MAX_FREQ ? M_MAX_FREQ : f);
}

// Starts a sound's channel at a place in its sample (even, so on a byte; the
// ADPCM state starts afresh there). A looped sound always starts at the top,
// so that the loop takes the whole sample.
static void M_Start(const int32_t sound_id, int32_t from)
{
    M_SOUND *const sound = &m_Sounds[sound_id];
    const M_SAMPLE *const sample = &m_Samples[sound->sample];
    if (sound->looped || from < 0) {
        from = 0;
    }
    from &= ~1;
    sound->from = from;
    int32_t vol;
    int32_t pan;
    int32_t freq;
    M_Params(sound, &vol, &pan, &freq);
    const uint32_t length = (uint32_t)(sample->length - from);
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    memset(tmp, 0, sizeof(tmp));
    cmd->cmd = AICA_CMD_CHAN;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = m_Channels[sound_id];
    chan->cmd = AICA_CH_CMD_START;
    chan->base = sample->addr + from / 2;
    chan->type = AICA_SM_ADPCM;
    chan->length = length;
    chan->loop = sound->looped;
    chan->loopstart = 0;
    chan->loopend = length;
    chan->freq = freq;
    chan->vol = vol;
    chan->pan = pan;
    snd_sh4_to_aica(tmp, cmd->size);
    sound->sent_vol = vol;
    sound->sent_pan = pan;
    sound->sent_freq = freq;
    sound->ends_at = sound->looped
        ? 0
        : timer_us_gettime64() + (uint64_t)length * 1000000u / (uint32_t)freq;
}

// Whether a sound still sounds (or waits, paused, to go on).
static bool M_IsAlive(const int32_t sound_id)
{
    const M_SOUND *const sound = &m_Sounds[sound_id];
    return sound->used
        && (sound->paused || sound->looped
            || timer_us_gettime64() < sound->ends_at);
}

// Volume, pan and frequency of a sound already playing, sent only when one
// changed: the game sets them every frame for every sound, and the AICA's
// command queue backs up and holds new sounds back.
static void M_Update(const int32_t sound_id)
{
    M_SOUND *const sound = &m_Sounds[sound_id];
    int32_t vol;
    int32_t pan;
    int32_t freq;
    M_Params(sound, &vol, &pan, &freq);
    if (vol == sound->sent_vol && pan == sound->sent_pan
        && freq == sound->sent_freq) {
        return;
    }
    if (freq != sound->sent_freq && !sound->looped) {
        // What is left of it, at the new rate.
        const int32_t done = snd_get_pos(m_Channels[sound_id]);
        const int32_t left =
            m_Samples[sound->sample].length - sound->from - done;
        sound->ends_at = timer_us_gettime64()
            + (uint64_t)(left > 0 ? left : 0) * 1000000u / (uint32_t)freq;
    }
    sound->sent_vol = vol;
    sound->sent_pan = pan;
    sound->sent_freq = freq;
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    memset(tmp, 0, sizeof(tmp));
    cmd->cmd = AICA_CMD_CHAN;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = m_Channels[sound_id];
    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_VOL
        | AICA_CH_UPDATE_SET_PAN | AICA_CH_UPDATE_SET_FREQ;
    chan->vol = vol;
    chan->pan = pan;
    chan->freq = freq;
    snd_sh4_to_aica(tmp, cmd->size);
}

int32_t Audio_Sample_Play(
    const int32_t sample_id, const int32_t volume, const float pitch,
    const int32_t pan, const bool is_looped)
{
    if (!m_Ready || !Audio_Sample_IsLoaded(sample_id)) {
        return AUDIO_NO_SOUND;
    }
    for (int32_t i = 0; i < AUDIO_MAX_ACTIVE_SAMPLES; i++) {
        M_SOUND *const sound = &m_Sounds[i];
        // A sound that has run out frees its place.
        if (m_Channels[i] < 0 || M_IsAlive(i)) {
            continue;
        }
        *sound = (M_SOUND) {
            .used = true,
            .looped = is_looped,
            .sample = sample_id,
            .volume = volume,
            .pan = pan,
            .pitch = pitch,
        };
        M_Start(i, 0);
        return i;
    }
    return AUDIO_NO_SOUND;
}

bool Audio_Sample_IsPlaying(const int32_t sound_id)
{
    // A paused sound is still there to go on with.
    return M_IsSound(sound_id) && M_IsAlive(sound_id);
}

RESULT Audio_Sample_Pause(const int32_t sound_id)
{
    FAIL_IF(!M_IsSound(sound_id), "sound %d is not playing", sound_id);
    M_SOUND *const sound = &m_Sounds[sound_id];
    if (sound->paused) {
        return OK;
    }
    const int32_t chn = m_Channels[sound_id];
    if (!M_IsAlive(sound_id)) {
        sound->used = false; // it had run out
        return OK;
    }
    // Where it got to, from where it was started.
    sound->from += snd_get_pos(chn);
    snd_sfx_stop(chn);
    sound->paused = true;
    return OK;
}

RESULT Audio_Sample_PauseAll(void)
{
    for (int32_t i = 0; i < AUDIO_MAX_ACTIVE_SAMPLES; i++) {
        if (m_Sounds[i].used) {
            IGNORE(Audio_Sample_Pause(i));
        }
    }
    return OK;
}

RESULT Audio_Sample_Unpause(const int32_t sound_id)
{
    FAIL_IF(!M_IsSound(sound_id), "sound %d is not playing", sound_id);
    M_SOUND *const sound = &m_Sounds[sound_id];
    if (!sound->paused) {
        return OK;
    }
    sound->paused = false;
    if (!sound->looped
        && sound->from >= m_Samples[sound->sample].length - 2) {
        sound->used = false;
        return OK;
    }
    M_Start(sound_id, sound->from);
    return OK;
}

RESULT Audio_Sample_UnpauseAll(void)
{
    for (int32_t i = 0; i < AUDIO_MAX_ACTIVE_SAMPLES; i++) {
        if (m_Sounds[i].used) {
            IGNORE(Audio_Sample_Unpause(i));
        }
    }
    return OK;
}

RESULT Audio_Sample_Close(const int32_t sound_id)
{
    FAIL_IF(!M_IsSound(sound_id), "sound %d is not playing", sound_id);
    snd_sfx_stop(m_Channels[sound_id]);
    m_Sounds[sound_id].used = false;
    return OK;
}

RESULT Audio_Sample_CloseAll(void)
{
    for (int32_t i = 0; i < AUDIO_MAX_ACTIVE_SAMPLES; i++) {
        if (m_Sounds[i].used) {
            IGNORE(Audio_Sample_Close(i));
        }
    }
    return OK;
}

RESULT Audio_Sample_SetPan(const int32_t sound_id, const int32_t pan)
{
    FAIL_IF(!M_IsSound(sound_id), "sound %d is not playing", sound_id);
    m_Sounds[sound_id].pan = pan;
    if (!m_Sounds[sound_id].paused) {
        M_Update(sound_id);
    }
    return OK;
}

RESULT Audio_Sample_SetVolume(const int32_t sound_id, const int32_t volume)
{
    FAIL_IF(!M_IsSound(sound_id), "sound %d is not playing", sound_id);
    m_Sounds[sound_id].volume = volume;
    if (!m_Sounds[sound_id].paused) {
        M_Update(sound_id);
    }
    return OK;
}

RESULT Audio_Sample_SetPitch(const int32_t sound_id, const float pitch)
{
    FAIL_IF(!M_IsSound(sound_id), "sound %d is not playing", sound_id);
    m_Sounds[sound_id].pitch = pitch;
    if (!m_Sounds[sound_id].paused) {
        M_Update(sound_id);
    }
    return OK;
}

// ----------------------------------------------------------------------------
// Music streams

static bool M_IsStream(const int32_t sound_id)
{
    return sound_id >= 0 && sound_id < M_MAX_STREAMS
        && m_Streams[sound_id].used;
}

// Bytes of data for a time, on a whole byte of every channel.
static uint32_t M_StreamBytes(const M_STREAM *const s, const double seconds)
{
    if (seconds <= 0.0) {
        return 0;
    }
    // Two samples a byte in each channel.
    const uint32_t bytes =
        (uint32_t)(seconds * s->rate / 2.0) * (uint32_t)s->channels;
    return bytes < s->data_size ? bytes : s->data_size;
}

static void M_StreamApplyVolume(const M_STREAM *const s)
{
    const float v = m_Muted ? 0.0f : s->volume * s->fade * 255.0f;
    snd_stream_volume(s->hnd, v <= 0.0f ? 0 : (v >= 255.0f ? 255 : (int)v));
}

// What the stream asks for, read from the disc; at the end, back to the
// loop's start or nothing more. Runs in the poll thread, under the lock.
static void *M_StreamFeed(
    const snd_stream_hnd_t hnd, const int bytes_req, int *const bytes_recv)
{
    M_STREAM *const s = snd_stream_get_userdata(hnd);
    *bytes_recv = 0;
    if (s == nullptr || !s->used) {
        return nullptr;
    }
    int32_t want = bytes_req < (int)sizeof(m_StreamBuffer)
        ? bytes_req
        : (int)sizeof(m_StreamBuffer);
    int32_t got = 0;
    while (got < want) {
        if (s->pos >= s->end) {
            if (!s->looped) {
                break;
            }
            s->pos = s->loop_from;
        }
        uint32_t n = s->end - s->pos;
        if (n > (uint32_t)(want - got)) {
            n = want - got;
        }
        ssize_t read;
        if (s->film) {
            bool finished;
            read = (ssize_t)DC_Video_ReadAudio(
                m_StreamBuffer + got, n & ~63u, &finished);
            if (read == 0) {
                if (finished) {
                    s->end = s->pos;
                    continue;
                }
                // The player is behind: a moment of silence, which keeps
                // the clock going, and what comes next on the next call.
                read = (n & ~63u) < 1472 ? (n & ~63u) : 1472;
                if (read == 0) {
                    break;
                }
                memset(m_StreamBuffer + got, 0, read);
            }
            got += read;
            s->pos += read;
            // Whatever the player had: it hands over the rest later.
            break;
        } else {
            fs_seek(s->fd, s->data_start + s->pos, SEEK_SET);
            read = fs_read(s->fd, m_StreamBuffer + got, n);
        }
        if (read <= 0) {
            s->end = s->pos; // the disc gave out: the track ends here
            continue;
        }
        got += read;
        s->pos += read;
    }
    if (got < want && s->ends_at == 0 && (!s->film || s->pos >= s->end)) {
        s->ends_at = timer_ms_gettime64() + M_TAIL_MS;
    }
    *bytes_recv = got;
    return got > 0 ? m_StreamBuffer : nullptr;
}

// Lets a stream go, and returns its finish callback to be called outside the
// lock.
static void M_StreamRelease(
    M_STREAM *const s, void (**const finish)(int32_t, void *),
    void **const finish_data)
{
    if (finish != nullptr) {
        *finish = s->finish;
        *finish_data = s->finish_data;
    }
    if (s->playing) {
        snd_stream_stop(s->hnd);
    }
    snd_stream_destroy(s->hnd);
    if (s->fd >= 0) {
        fs_close(s->fd);
    }
    *s = (M_STREAM) { .hnd = SND_STREAM_INVALID, .fd = -1 };
}

static void *M_StreamThread(void *const arg)
{
    (void)arg;
    while (m_ThreadRun) {
        void (*finished[M_MAX_STREAMS])(int32_t, void *) = {};
        void *finished_data[M_MAX_STREAMS] = {};
        mutex_lock(&m_StreamLock);
        const uint64_t now = timer_ms_gettime64();
        for (int32_t i = 0; i < M_MAX_STREAMS; i++) {
            M_STREAM *const s = &m_Streams[i];
            if (!s->used || !s->playing) {
                continue;
            }
            snd_stream_poll(s->hnd);
            if (s->fade_step != 0.0f) {
                s->fade += s->fade_step;
                if (s->fade >= 1.0f) {
                    s->fade = 1.0f;
                    s->fade_step = 0.0f;
                } else if (s->fade <= 0.0f) {
                    s->fade = 0.0f;
                    s->fade_step = 0.0f;
                }
                M_StreamApplyVolume(s);
            }
            const bool faded_out = s->fading_out && s->fade <= 0.0f;
            if (faded_out || (s->ends_at != 0 && now >= s->ends_at)) {
                M_StreamRelease(s, &finished[i], &finished_data[i]);
            }
        }
        mutex_unlock(&m_StreamLock);
        for (int32_t i = 0; i < M_MAX_STREAMS; i++) {
            if (finished[i] != nullptr) {
                finished[i](i, finished_data[i]);
            }
        }
        thd_sleep(M_POLL_MS);
    }
    return nullptr;
}

RESULT Audio_Stream_CreateFromFile(
    const char *const path, int32_t *const out_sound_id)
{
    MUST(Audio_Init());
    // A film's sound comes from the film player's queue (video.c), not from
    // the disc: the film's file is read in one pass, picture and sound.
    const size_t path_len = strlen(path);
    if (path_len > 4 && strcasecmp(path + path_len - 4, ".fmv") == 0) {
        uint32_t rate;
        int32_t channels;
        uint32_t bytes;
        FAIL_IF(
            !DC_Video_GetAudio(&rate, &channels, &bytes),
            "%s has no sound", path);
        mutex_lock(&m_StreamLock);
        int32_t id = -1;
        for (int32_t i = 0; i < M_MAX_STREAMS; i++) {
            if (!m_Streams[i].used) {
                id = i;
                break;
            }
        }
        const snd_stream_hnd_t hnd = id < 0
            ? SND_STREAM_INVALID
            : snd_stream_alloc(M_StreamFeed, M_STREAM_BUFFER);
        if (hnd == SND_STREAM_INVALID) {
            mutex_unlock(&m_StreamLock);
            return FAIL("every stream is in use");
        }
        M_STREAM *const s = &m_Streams[id];
        *s = (M_STREAM) {
            .used = true,
            .hnd = hnd,
            .fd = -1,
            .film = true,
            .channels = channels,
            .rate = rate,
            .data_size = bytes,
            .end = bytes,
            .volume = 1.0f,
            .fade = 1.0f,
        };
        snd_stream_set_userdata(hnd, s);
        mutex_unlock(&m_StreamLock);
        *out_sound_id = id;
        return OK;
    }
    const file_t fd = fs_open(path, O_RDONLY);
    FAIL_IF(fd < 0, "cannot open %s", path);
    uint8_t head[256];
    const ssize_t head_size = fs_read(fd, head, sizeof(head));
    M_WAV wav;
    if (head_size <= 0 || !M_ParseWAV(head, (size_t)head_size, &wav)
        || (wav.format != 0x20 && wav.format != 0x14) || wav.bits != 4
        || wav.channels > 2) {
        fs_close(fd);
        return FAIL("%s is not a Yamaha ADPCM WAV", path);
    }

    mutex_lock(&m_StreamLock);
    int32_t id = -1;
    for (int32_t i = 0; i < M_MAX_STREAMS; i++) {
        if (!m_Streams[i].used) {
            id = i;
            break;
        }
    }
    const snd_stream_hnd_t hnd = id < 0
        ? SND_STREAM_INVALID
        : snd_stream_alloc(M_StreamFeed, M_STREAM_BUFFER);
    if (hnd == SND_STREAM_INVALID) {
        mutex_unlock(&m_StreamLock);
        fs_close(fd);
        return FAIL("every stream is in use");
    }
    const uint32_t size = (uint32_t)fs_total(fd);
    const uint32_t data_size = wav.data_at + wav.data_size <= size
        ? wav.data_size
        : size - wav.data_at;
    M_STREAM *const s = &m_Streams[id];
    *s = (M_STREAM) {
        .used = true,
        .hnd = hnd,
        .fd = fd,
        .channels = wav.channels,
        .rate = wav.rate,
        .data_start = wav.data_at,
        .data_size = data_size,
        .end = data_size,
        .volume = 1.0f,
        .fade = 1.0f,
    };
    snd_stream_set_userdata(hnd, s);
    mutex_unlock(&m_StreamLock);
    *out_sound_id = id;
    return OK;
}

RESULT Audio_Stream_CreateFromMemory(
    uint8_t *const data, const size_t size, int32_t *const out_sound_id)
{
    return FAIL("streams from memory are not available on the Dreamcast");
}

RESULT Audio_Stream_Close(const int32_t sound_id)
{
    mutex_lock(&m_StreamLock);
    const bool ok = M_IsStream(sound_id);
    if (ok) {
        M_StreamRelease(&m_Streams[sound_id], nullptr, nullptr);
    }
    mutex_unlock(&m_StreamLock);
    FAIL_IF(!ok, "stream %d is not open", sound_id);
    return OK;
}

RESULT Audio_Stream_SetPaused(const int32_t sound_id, const bool is_paused)
{
    mutex_lock(&m_StreamLock);
    const bool ok = M_IsStream(sound_id);
    if (ok) {
        M_STREAM *const s = &m_Streams[sound_id];
        if (is_paused && s->playing) {
            snd_stream_stop(s->hnd);
            s->playing = false;
        } else if (!is_paused && !s->playing) {
            // The stream fills its buffer from where the file stands.
            snd_stream_start_adpcm(s->hnd, s->rate, s->channels == 2);
            M_StreamApplyVolume(s);
            s->playing = true;
        }
    }
    mutex_unlock(&m_StreamLock);
    FAIL_IF(!ok, "stream %d is not open", sound_id);
    return OK;
}

RESULT Audio_Stream_Pause(const int32_t sound_id)
{
    return Audio_Stream_SetPaused(sound_id, true);
}

RESULT Audio_Stream_Unpause(const int32_t sound_id)
{
    return Audio_Stream_SetPaused(sound_id, false);
}

bool Audio_Stream_IsLooped(const int32_t sound_id)
{
    return M_IsStream(sound_id) && m_Streams[sound_id].looped;
}

RESULT Audio_Stream_SetIsLooped(const int32_t sound_id, const bool is_looped)
{
    FAIL_IF(!M_IsStream(sound_id), "stream %d is not open", sound_id);
    m_Streams[sound_id].looped = is_looped;
    return OK;
}

RESULT Audio_Stream_SetVolume(const int32_t sound_id, const float volume)
{
    mutex_lock(&m_StreamLock);
    const bool ok = M_IsStream(sound_id);
    if (ok) {
        m_Streams[sound_id].volume = volume;
        M_StreamApplyVolume(&m_Streams[sound_id]);
    }
    mutex_unlock(&m_StreamLock);
    FAIL_IF(!ok, "stream %d is not open", sound_id);
    return OK;
}

static RESULT M_Fade(
    const int32_t sound_id, const float from, const float to,
    const double seconds)
{
    mutex_lock(&m_StreamLock);
    const bool ok = M_IsStream(sound_id);
    if (ok) {
        M_STREAM *const s = &m_Streams[sound_id];
        const float polls = (float)(seconds * 1000.0 / M_POLL_MS);
        s->fade = from;
        s->fading_out = to <= 0.0f;
        s->fade_step = polls >= 1.0f ? (to - from) / polls : to - from;
        M_StreamApplyVolume(s);
    }
    mutex_unlock(&m_StreamLock);
    FAIL_IF(!ok, "stream %d is not open", sound_id);
    return OK;
}

RESULT Audio_Stream_FadeIn(const int32_t sound_id, const double seconds)
{
    return M_Fade(sound_id, 0.0f, 1.0f, seconds);
}

RESULT Audio_Stream_FadeOut(const int32_t sound_id, const double seconds)
{
    float from = 1.0f;
    if (M_IsStream(sound_id)) {
        from = m_Streams[sound_id].fade;
    }
    return M_Fade(sound_id, from, 0.0f, seconds);
}

// shortcut: no low pass on the AICA path (underwater music stays clear);
// the AICA's own filter (FLV, per channel) could do it.
RESULT Audio_Stream_SetLowPass(const int32_t sound_id, const float cutoff)
{
    FAIL_IF(!M_IsStream(sound_id), "stream %d is not open", sound_id);
    return OK;
}

// shortcut: played at its own speed; the stream's frequency could follow.
RESULT Audio_Stream_SetSpeed(const int32_t sound_id, const double speed)
{
    FAIL_IF(!M_IsStream(sound_id), "stream %d is not open", sound_id);
    return OK;
}

RESULT Audio_Stream_SetFinishCallback(
    const int32_t sound_id, void (*const callback)(int32_t, void *),
    void *const user_data)
{
    mutex_lock(&m_StreamLock);
    const bool ok = M_IsStream(sound_id);
    if (ok) {
        m_Streams[sound_id].finish = callback;
        m_Streams[sound_id].finish_data = user_data;
    }
    mutex_unlock(&m_StreamLock);
    FAIL_IF(!ok, "stream %d is not open", sound_id);
    return OK;
}

double Audio_Stream_GetTimestamp(const int32_t sound_id)
{
    if (!M_IsStream(sound_id)) {
        return -1.0;
    }
    const M_STREAM *const s = &m_Streams[sound_id];
    return (double)s->pos * 2.0 / s->channels / s->rate;
}

double Audio_Stream_GetDuration(const int32_t sound_id)
{
    if (!M_IsStream(sound_id)) {
        return -1.0;
    }
    const M_STREAM *const s = &m_Streams[sound_id];
    return (double)s->data_size * 2.0 / s->channels / s->rate;
}

RESULT Audio_Stream_SeekTimestamp(const int32_t sound_id, const double timestamp)
{
    mutex_lock(&m_StreamLock);
    const bool ok = M_IsStream(sound_id);
    if (ok) {
        M_STREAM *const s = &m_Streams[sound_id];
        s->pos = M_StreamBytes(s, timestamp);
        s->ends_at = 0;
    }
    mutex_unlock(&m_StreamLock);
    FAIL_IF(!ok, "stream %d is not open", sound_id);
    return OK;
}

RESULT Audio_Stream_SetStartTimestamp(
    const int32_t sound_id, const double timestamp)
{
    FAIL_IF(!M_IsStream(sound_id), "stream %d is not open", sound_id);
    M_STREAM *const s = &m_Streams[sound_id];
    s->loop_from = M_StreamBytes(s, timestamp);
    return OK;
}

RESULT Audio_Stream_SetStopTimestamp(
    const int32_t sound_id, const double timestamp)
{
    FAIL_IF(!M_IsStream(sound_id), "stream %d is not open", sound_id);
    M_STREAM *const s = &m_Streams[sound_id];
    s->end = timestamp > 0.0 ? M_StreamBytes(s, timestamp) : s->data_size;
    return OK;
}

RESULT Audio_Stream_SyncTimestamp(const int32_t sound_id, const double timestamp)
{
    FAIL_IF(!M_IsStream(sound_id), "stream %d is not open", sound_id);
    const double drift = Audio_Stream_GetTimestamp(sound_id) - timestamp;
    if (drift > AUDIO_DRIFT_THRESHOLD || drift < -AUDIO_DRIFT_THRESHOLD) {
        return Audio_Stream_SeekTimestamp(sound_id, timestamp);
    }
    return OK;
}

// ----------------------------------------------------------------------------
// The device

RESULT Audio_Init(void)
{
    if (m_Ready) {
        return OK;
    }
    FAIL_IF(snd_init() < 0, "the AICA did not start");
    FAIL_IF(snd_stream_init() < 0, "the AICA streams did not start");
    // Channels of their own; the lowest stay free for the music: KallistiOS
    // starts a stream's two channels together by a 32-bit mask of them, so a
    // stream on channel 32 or above never starts.
    int32_t music[M_MAX_STREAMS * 2];
    for (int32_t i = 0; i < M_MAX_STREAMS * 2; i++) {
        music[i] = snd_sfx_chn_alloc();
    }
    for (int32_t i = 0; i < AUDIO_MAX_ACTIVE_SAMPLES; i++) {
        m_Channels[i] = snd_sfx_chn_alloc();
    }
    for (int32_t i = 0; i < M_MAX_STREAMS * 2; i++) {
        if (music[i] >= 0) {
            snd_sfx_chn_free(music[i]);
        }
    }
    for (int32_t i = 0; i < M_MAX_STREAMS; i++) {
        m_Streams[i] = (M_STREAM) { .hnd = SND_STREAM_INVALID, .fd = -1 };
    }
    const kthread_attr_t attr = {
        // The disc reads under snd_stream_poll go deep: 8 K was too little.
        .stack_size = 32 * 1024,
        .label = "trx-music",
    };
    m_ThreadRun = true;
    m_Thread = thd_create_ex(&attr, M_StreamThread, nullptr);
    FAIL_IF(m_Thread == nullptr, "the music thread did not start");
    m_Ready = true;
    return OK;
}

RESULT Audio_Shutdown(void)
{
    if (!m_Ready) {
        return OK;
    }
    // The thread stops before anything it touches does: KallistiOS shuts
    // the AICA down as the program ends, and a poll after that faulted
    // (Flycast: "SH4 exception when blocked" on leaving the game).
    m_ThreadRun = false;
    if (m_Thread != nullptr) {
        thd_join(m_Thread, nullptr);
        m_Thread = nullptr;
    }
    m_Ready = false;
    IGNORE(Audio_Sample_UnloadAll());
    mutex_lock(&m_StreamLock);
    for (int32_t i = 0; i < M_MAX_STREAMS; i++) {
        if (m_Streams[i].used) {
            M_StreamRelease(&m_Streams[i], nullptr, nullptr);
        }
    }
    mutex_unlock(&m_StreamLock);
    return OK;
}

bool Audio_ShouldSkipSDLQuitAudio(void)
{
    return true;
}

static void M_ApplyMute(void)
{
    for (int32_t i = 0; i < AUDIO_MAX_ACTIVE_SAMPLES; i++) {
        if (m_Sounds[i].used && !m_Sounds[i].paused) {
            M_Update(i);
        }
    }
    mutex_lock(&m_StreamLock);
    for (int32_t i = 0; i < M_MAX_STREAMS; i++) {
        if (m_Streams[i].used) {
            M_StreamApplyVolume(&m_Streams[i]);
        }
    }
    mutex_unlock(&m_StreamLock);
}

void Audio_Mute(void)
{
    m_Muted = true;
    M_ApplyMute();
}

void Audio_Unmute(void)
{
    m_Muted = false;
    M_ApplyMute();
}

bool Audio_IsMuted(void)
{
    return m_Muted;
}

void Audio_SetReverbType(const uint8_t reverb_type)
{
    m_Reverb = reverb_type;
}

uint8_t Audio_GetReverbType(void)
{
    return m_Reverb;
}
