#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "aac_rtp_packer.h"
#include "frame_source.h"
#include "kit/event_poller.h"
#include "kit/stream_source.h"
#include "kit/thread_pool.h"
#include "rtp_packer.h"
#include "rtsp_pusher.h"

// One media source = one live stream (video + audio), source agnostic.
// Mirrors libmk_api: mk_media_create / mk_media_input_h264 / ... /
// mk_media_start_send_rtsp.
//
// Any thread may push frames via inputH264/inputAAC (mp4 reader thread,
// camera capture callback, ...). Packing and non-blocking sending run on
// the single poller thread bound at creation, so one poller thread can
// serve hundreds of streams. A MediaSource can push to several RTSP
// addresses at once (1:N broadcast).
class MediaSource : public std::enable_shared_from_this<MediaSource> {
public:
    using Ptr = std::shared_ptr<MediaSource>;

    static Ptr create(const std::string& app, const std::string& stream_id,
                      const kit::EventPoller::Ptr& poller);

    void setReconnectPool(kit::ThreadPool* pool);

    // Stream description for the SDP; must be set before the first
    // startSendRtsp handshake (bindFile does it internally).
    void setMediaInfo(const MediaInfo& info);

    // The single input entry points (any thread). AnnexB + 90kHz pts.
    bool inputH264(const uint8_t* data, int len, uint32_t pts,
                   bool is_key = false);
    bool inputAAC(const uint8_t* data, int len, uint32_t pts);

    // Add one RTSP push target (1:N broadcast supported).
    bool startSendRtsp(const std::string& url);

    // Stops all sessions on the poller thread. If done is non-null it is
    // posted once the poller finished stopping, letting callers (e.g.
    // MediaManager::shutdown) wait until reconnect tasks can exit.
    void stop(kit::semaphore* done = nullptr);

    const std::string& streamId() const { return stream_id_; }

private:
    struct Session;
    using SessionPtr = std::shared_ptr<Session>;

    MediaSource(const std::string& app, const std::string& stream_id,
                const kit::EventPoller::Ptr& poller);
    void init();

    // poller thread only:
    void onVideoFrames(std::vector<VideoFrame>& frames);
    void onAudioFrames(std::vector<AudioFrame>& frames);
    void addSession(const std::string& url);
    void createSession(const std::string& url);
    void flushPendingSessions();
    void onSessionConnected(const SessionPtr& s);
    // poller thread only: replay the cached partial GOP into one session.
    void dumpCachedGop(const SessionPtr& s);
    // poller thread only: video 90kHz pts -> audio sample-rate pts.
    uint32_t videoPtsToAudio(uint32_t video_pts);

    kit::EventPoller::Ptr poller_;
    std::string app_;
    std::string stream_id_;
    kit::ThreadPool* reconnect_pool_ = nullptr;

    // Held via shared_ptr because StreamSource::wake() uses
    // shared_from_this() to keep the source alive until queued drain tasks
    // have run on the poller thread.
    std::shared_ptr<kit::StreamSource<VideoFrame>> video_src_;
    std::shared_ptr<kit::StreamSource<AudioFrame>> audio_src_;

    std::mutex info_mtx_;
    MediaInfo info_;
    std::vector<SessionPtr> sessions_;       // poller thread only
    std::vector<std::string> pending_urls_;  // poller thread only, wait info_

    // Rolling partial-GOP cache shared by all sessions (poller thread
    // only): gop_video_ always starts with the latest IDR, gop_audio_ holds
    // audio aligned to that IDR (frames older than the IDR's audio-time
    // equivalent are dropped on each new IDR). Replayed on connect so the
    // server cache starts with a complete reference chain.
    std::deque<VideoFrame> gop_video_;
    std::deque<AudioFrame> gop_audio_;
    uint64_t gop_video_bytes_ = 0;    // byte cap guard
    uint32_t last_gop_video_pts_ = 0; // pts gap detection (queue overflow)
};
