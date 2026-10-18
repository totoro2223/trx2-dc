#pragma once
// Dreamcast: FFmpeg is not available.
#define AV_LOG_QUIET -8
#define AV_LOG_ERROR 16
static inline void av_log_set_level(int level) { (void)level; }
