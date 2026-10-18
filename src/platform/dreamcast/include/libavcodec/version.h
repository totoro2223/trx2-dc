#pragma once
// Dreamcast: FFmpeg is not available. Pretend to be a modern libavcodec so
// that no legacy registration calls are made.
#define LIBAVCODEC_VERSION_MAJOR 61
