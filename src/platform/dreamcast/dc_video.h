#pragma once

// The film player's sound, which audio.c plays as a stream (video.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The sound of the film being played: false when it has none.
bool DC_Video_GetAudio(uint32_t *rate, int32_t *channels, uint32_t *bytes);

// Up to size bytes of it, in order; fewer when the disc has not given them
// yet. finished says there is no more to come.
size_t DC_Video_ReadAudio(uint8_t *dst, size_t size, bool *finished);

// How much of the film's sound the AICA has played, read from its channel
// (audio.c): false when none is playing.
bool DC_Audio_FilmSeconds(double *seconds);

// Seconds of silence put in the film's sound for want of data (audio.c).
double DC_Audio_FilmSilenceSeconds(void);
