#pragma once

extern "C" {
#include <libavformat/avformat.h>
}
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "frame_source.h"
#include "rtsp_pusher.h"  // parseExtradata / SpsPps

// Single mp4 file reader: AVCC -> AnnexB conversion, time_base -> RTP pts,
// automatic loop at EOF with a pts offset (PtsUtil::nextLoopOffset).
// One instance per stream; the pacing itself lives in ReaderPool.
class FileReader {
public:
    FileReader() = default;
    ~FileReader();

    bool open(const std::string& file);
    void close();

    const MediaInfo& info() const { return info_; }
    uint32_t frameIntervalUs() const { return frame_interval_us_; }

    // Reads until the next video frame (converted to AnnexB, pts in 90kHz);
    // audio frames met along the way are appended to audios. Loops at EOF.
    bool readNextVideoFrame(VideoFrame& v, std::vector<AudioFrame>& audios);

private:
    AVFormatContext* fmt_ctx_ = nullptr;
    int video_stream_idx_ = -1;
    int audio_stream_idx_ = -1;

    MediaInfo info_;
    uint32_t frame_interval_us_ = 40000;  // 25fps default
    uint32_t one_frm_90k_ = 3600;
    uint32_t rtp_ts_offset_ = 0;
    uint32_t audio_ts_offset_ = 0;
    uint32_t last_video_pts_ = 0;
    uint32_t last_audio_pts_ = 0;
};
