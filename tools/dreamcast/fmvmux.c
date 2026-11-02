// Puts a film in the format of the Dreamcast's MPEG-1 player (version 3,
// src/platform/dreamcast/video.c), for make_disc.sh. Built and run on the
// host, with the film's pictures as an MPEG-1 video stream from FFmpeg:
//
//   ffmpeg -i X.FMV -vf fps=30,scale=W:H -c:v mpeg1video -b:v 900k -g 15 \
//     -bf 0 -f mpeg1video X.m1v
//   fmvmux <output.fmv> <fps> <display width> <display height> X.m1v \
//     [sound.wav]
//
// The stream has no B pictures, so each picture is cut where the next starts
// (the sequence and group headers go with the picture after them). The sound
// is the WAV's Yamaha ADPCM (stereo, 44.1 kHz, as the music is), cut into as
// many bytes as a picture lasts and put after each, so that the player reads
// both in one pass over the disc.
//
// Output: "TRXV", u32 version (3), u32 frames, u32 fps, u16 display width,
// u16 display height, u32 sound bytes per frame, then frames x (u32 bytes of
// picture, the picture, the sound).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *ReadAll(const char *const path, size_t *const size)
{
    FILE *const fp = fopen(path, "rb");
    if (fp == NULL) {
        perror(path);
        exit(1);
    }
    fseek(fp, 0, SEEK_END);
    *size = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *const data = malloc(*size + 1);
    if (data == NULL || fread(data, 1, *size, fp) != *size) {
        fprintf(stderr, "%s: cannot read\n", path);
        exit(1);
    }
    fclose(fp);
    return data;
}

static FILE *m_Out;
static uint32_t m_Frames;
static uint32_t m_PerFrame;
static const uint8_t *m_Sound;
static size_t m_SoundSize;

static void Emit(const uint8_t *const data, const size_t size)
{
    const uint32_t bytes = (uint32_t)size;
    fwrite(&bytes, 4, 1, m_Out);
    fwrite(data, 1, size, m_Out);
    for (uint32_t i = 0; i < m_PerFrame; i++) {
        const size_t at = (size_t)m_Frames * m_PerFrame + i;
        fputc(at < m_SoundSize ? m_Sound[at] : 0, m_Out);
    }
    m_Frames++;
}

int main(const int argc, char **const argv)
{
    if (argc < 6) {
        fprintf(
            stderr,
            "usage: fmvmux out.fmv fps width height video.m1v [sound.wav]\n");
        return 2;
    }
    m_Out = fopen(argv[1], "wb");
    if (m_Out == NULL) {
        perror(argv[1]);
        return 1;
    }
    const uint32_t fps = (uint32_t)atoi(argv[2]);
    const uint16_t dw = (uint16_t)atoi(argv[3]);
    const uint16_t dh = (uint16_t)atoi(argv[4]);
    size_t video_size;
    const uint8_t *const video = ReadAll(argv[5], &video_size);

    // The sound's bytes: after the WAV's "data" chunk header.
    if (argc == 7) {
        size_t wav_size;
        const uint8_t *const wav = ReadAll(argv[6], &wav_size);
        size_t at = 12;
        while (at + 8 <= wav_size) {
            uint32_t size;
            memcpy(&size, wav + at + 4, 4);
            if (memcmp(wav + at, "data", 4) == 0) {
                m_Sound = wav + at + 8;
                m_SoundSize = wav_size - at - 8 < size ? wav_size - at - 8 : size;
                break;
            }
            at += 8 + ((size + 1) & ~1u);
        }
        m_PerFrame = (44100u / fps) & ~1u;
    }

    fwrite("TRXV", 1, 4, m_Out);
    const uint32_t version = 3;
    fwrite(&version, 4, 1, m_Out);
    fwrite(&(uint32_t) { 0 }, 4, 1, m_Out);
    fwrite(&fps, 4, 1, m_Out);
    fwrite(&dw, 2, 1, m_Out);
    fwrite(&dh, 2, 1, m_Out);
    fwrite(&m_PerFrame, 4, 1, m_Out);

    // Start codes: 00 picture, B3 sequence header, B8 group of pictures.
    size_t start = 0;
    int has_picture = 0;
    for (size_t i = 0; i + 3 < video_size; i++) {
        if (video[i] != 0 || video[i + 1] != 0 || video[i + 2] != 1) {
            continue;
        }
        const uint8_t code = video[i + 3];
        if (code == 0x00 || code == 0xB3 || code == 0xB8) {
            if (has_picture) {
                Emit(video + start, i - start);
                start = i;
                has_picture = 0;
            }
            if (code == 0x00) {
                has_picture = 1;
            }
        }
        i += 2;
    }
    if (has_picture) {
        Emit(video + start, video_size - start);
    }
    fseek(m_Out, 8, SEEK_SET);
    fwrite(&m_Frames, 4, 1, m_Out);
    fclose(m_Out);
    fprintf(stderr, "%u frames\n", m_Frames);
    return m_Frames > 0 ? 0 : 1;
}
