#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

extern "C" {
#include <libavutil/rational.h>
}

// Unified frame contract shared by every source (mp4 reader, camera capture
// callback, ...): AnnexB elementary stream + RTP 90kHz timestamps.
// This mirrors libmk_api: mk_media_input_h264 is source agnostic.
struct VideoFrame {
    std::vector<uint8_t> data;  // AnnexB (start codes), no AVCC length header
    uint32_t pts = 0;           // RTP 90kHz units
    bool is_key = false;
};

struct AudioFrame {
    std::vector<uint8_t> data;  // raw AAC frame (no ADTS)
    uint32_t pts = 0;           // RTP units in sample_rate (AAC rule)
};

// Stream description needed by the RTSP handshake (SDP). The mp4 reader
// fills it from codec extradata; a camera source fills it manually.
struct MediaInfo {
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> aac_extra;
    int width = 0;
    int height = 0;
    int sample_rate = 0;
    int channels = 0;

    bool ready() const { return !sps.empty() && !pps.empty(); }
};

// pts conversion helpers shared by mp4 and camera sources.
namespace PtsUtil {

// mp4: packet pts in stream time_base -> RTP 90kHz units.
inline uint32_t timeBaseTo90k(int64_t pts, AVRational tb) {
    return (uint32_t)(pts * 90000 * av_q2d(tb));
}

// audio: packet pts in stream time_base -> sample_rate units.
inline uint32_t timeBaseToSampleRate(int64_t pts, AVRational tb,
                                     int sample_rate) {
    return (uint32_t)(pts * (double)sample_rate * av_q2d(tb));
}

// camera without source timestamps: wall-clock elapsed since capture start.
inline uint32_t wallClockTo90k(std::chrono::steady_clock::time_point start) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    return (uint32_t)(us * 90 / 1000000.0);
}

// loop offset for file playback: continue right after the last pts.
inline uint32_t nextLoopOffset(uint32_t last_pts, uint32_t frame_interval) {
    return last_pts + frame_interval;
}

} // namespace PtsUtil
