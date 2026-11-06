// The film player's MPEG-1 decoder: pl_mpeg.h from the Dreamcast fork of
// kos-ports' libmpeg (build_deps.sh puts it in deps/dc/include), which keeps
// the decoded pictures as macroblocks ready for the PowerVR's YUV converter.

#include "dc_mpeg.h"

// The decoder is the one hot loop of the films: fast, not small, even in the
// minsize build.
#pragma GCC optimize("O3")

#include "dc_level_heap.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

// The decoder's pictures take some 2.7 MB in one block. Once a level has been
// loaded the game's region holds nearly all of the heap (dc_level_heap.c) and
// newlib has too little left, so a film after a game or level restart showed
// no picture while its sound played. The big blocks come from the region (a
// title or a film has it free) and the heap keeps its slack for the rest of
// the film; the pointer is kept before the block so that the free finds the
// start. Before the first level there is no region, and the heap serves.
static void *M_Memalign(const size_t align, const size_t size)
{
    if (size < 64 * 1024) {
        return memalign(align, size);
    }
    uint8_t *const raw = DC_LevelHeap_Alloc(size + align + sizeof(void *));
    if (raw == NULL) {
        return memalign(align, size);
    }
    uint8_t *const aligned = (uint8_t *)(((uintptr_t)raw + sizeof(void *) + align - 1)
        & ~(uintptr_t)(align - 1));
    ((void **)aligned)[-1] = raw;
    return aligned;
}

static void M_Free(void *const ptr)
{
    if (ptr != NULL && DC_LevelHeap_Owns(ptr)) {
        DC_LevelHeap_Free(((void **)ptr)[-1]);
    } else {
        free(ptr);
    }
}

#define MPEG_MALLOC(sz) malloc(sz)
#define MPEG_FREE(p) M_Free(p)
#define MPEG_REALLOC(p, sz) realloc((p), (sz))
#define MPEG_MEMALIGN(a, sz) M_Memalign((a), (sz))
#define MPEG_MEMZERO(p, sz) memset((p), 0, (sz))

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
