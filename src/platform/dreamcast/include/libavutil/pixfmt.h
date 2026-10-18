#pragma once
// Dreamcast: FFmpeg is not available. Only the identifiers the engine names
// outside of trx/av are provided.
enum AVPixelFormat {
    AV_PIX_FMT_NONE = -1,
    AV_PIX_FMT_RGB24 = 2,
    AV_PIX_FMT_BGRA = 28,
    AV_PIX_FMT_RGBA = 26,
};
