#pragma once





#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kit/event_poller.h"
#include "kit/thread_pool.h"
#include "media_source.h"

class ReaderPool;

// Facade mirroring the libmk_api entry points:
//   createMedia -> mk_media_create
//   inputH264   -> mk_media_input_h264 (on MediaSource)
//   bindFile    -> mp4 source helper (camera sources push directly)
//   startSendRtsp -> mk_media_start_send_rtp
//
// Fixed thread counts, independent of the stream count:
//   reader threads (SchedulePool) pace mp4 reading,
//   poller threads serve non-blocking RTP sending,
//   reconnect workers run the blocking RTSP handshakes.

// ~10 threads push hundreds of streams.
class MediaManager {
public:
    static MediaManager& instance();

    // Must be called before createMedia; defaults are (4, 4).
    void setThreads(int reader_threads, int poller_threads);

    MediaSource::Ptr createMedia(const std::string& app,
                                 const std::string& stream);
    bool bindFile(const MediaSource::Ptr& m, const std::string& file);
    bool startSendRtsp(const MediaSource::Ptr& m, const std::string& url);

    // Stops every live MediaSource first (and waits for their pollers), so
    // pending reconnect tasks leave their retry loop before the reconnect
    // pool joins in the destructor.
    void shutdown();

private:
    MediaManager() = default;
    MediaManager(const MediaManager&) = delete;
    MediaManager& operator=(const MediaManager&) = delete;

    std::unique_ptr<kit::EventPollerPool> poller_pool_;
    std::unique_ptr<ReaderPool> reader_pool_;
    // 4 workers so several streams can handshake in parallel; a slow or
    // silent server must never starve the remaining streams' connections.
    kit::ThreadPool reconnect_pool_{4, kit::ThreadPool::PRIORITY_NORMAL,
                                    "rtsp_reconnect"};

    // Live streams, held weakly so a MediaSource is freed as soon as the
    // caller drops it. shutdown() stops each surviving one synchronously.
    std::mutex sources_mtx_;
    std::vector<std::weak_ptr<MediaSource>> sources_;
};
