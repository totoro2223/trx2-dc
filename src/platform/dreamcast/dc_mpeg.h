#pragma once

// MPEG-1 video for the film player (video.c): pl_mpeg's decoder with the
// SH4 routines, behind a few calls so that its header stays in mpeg1.c.

#include <stddef.h>
#include <stdint.h>

typedef struct DC_MPEG DC_MPEG;

// A decoder for a stream with no B pictures (the films are made so).
DC_MPEG *DC_Mpeg_Create(void);
void DC_Mpeg_Destroy(DC_MPEG *m);

// Hands over the next bytes of the stream, or says there are no more.
void DC_Mpeg_Write(DC_MPEG *m, const uint8_t *data, size_t size);
void DC_Mpeg_End(DC_MPEG *m);

// Decodes the next picture, once the stream holds the start of the one after
// it (or has ended). Returns its macroblocks in the order the PowerVR's YUV
// converter takes them (DC_PVR_SetVideoYUV), valid until the next call, or
// nullptr if the stream has none ready.
const uint32_t *DC_Mpeg_Decode(DC_MPEG *m);
