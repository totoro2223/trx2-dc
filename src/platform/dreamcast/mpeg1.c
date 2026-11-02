// The film player's MPEG-1 decoder: pl_mpeg.h from the Dreamcast fork of
// kos-ports' libmpeg (build_deps.sh puts it in deps/dc/include), which keeps
// the decoded pictures as macroblocks ready for the PowerVR's YUV converter.

#include "dc_mpeg.h"

// The decoder is the one hot loop of the films: fast, not small, even in the
// minsize build.
#pragma GCC optimize("O3")

#include <stdlib.h>

#include <kos.h>
#include <mpeg.h>
#define PL_MPEG_IMPLEMENTATION
#include <pl_mpeg.h>

// The decoder holds only the few pictures video.c hands it (a picture is some
// 8 KB, a keyframe several times that).
#define M_BUFFER_BYTES (256 * 1024)

struct DC_MPEG {
    plm_buffer_t *buffer;
    plm_video_t *video;
};

DC_MPEG *DC_Mpeg_Create(void)
{
    DC_MPEG *const m = calloc(1, sizeof(DC_MPEG));
    if (m == NULL) {
        return NULL;
    }
    m->buffer = plm_buffer_create_with_capacity(M_BUFFER_BYTES);
    m->video = m->buffer != NULL
        ? plm_video_create_with_buffer(m->buffer, 1)
        : NULL;
    if (m->video == NULL) {
        DC_Mpeg_Destroy(m);
        return NULL;
    }
    // The films have no B pictures: each picture decoded is the one to show.
    plm_video_set_no_delay(m->video, 1);
    return m;
}

void DC_Mpeg_Destroy(DC_MPEG *const m)
{
    if (m == NULL) {
        return;
    }
    // The video decoder owns the buffer.
    if (m->video != NULL) {
        plm_video_destroy(m->video);
    } else if (m->buffer != NULL) {
        plm_buffer_destroy(m->buffer);
    }
    free(m);
}

void DC_Mpeg_Write(DC_MPEG *const m, const uint8_t *const data, const size_t size)
{
    plm_buffer_write(m->buffer, (uint8_t *)data, size);
}

void DC_Mpeg_End(DC_MPEG *const m)
{
    plm_buffer_signal_end(m->buffer);
}

const uint32_t *DC_Mpeg_Decode(DC_MPEG *const m)
{
    const plm_frame_t *const frame = plm_video_decode(m->video);
    return frame != NULL ? frame->display : NULL;
}
