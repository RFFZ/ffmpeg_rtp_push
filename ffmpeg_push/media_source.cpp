#include "media_source.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>

namespace {
// Rolling GOP cache cap: a pathological source (no IDR for minutes or huge
// frames) must not grow the cache without bound. Beyond the cap the cache
// is dropped and connects fall back to waiting for the next live IDR.
constexpr uint64_t kGopMaxBytes = 32ull * 1024 * 1024;
// A pts jump bigger than this means frames were lost between two cached
// frames (the bounded input queue drops the oldest under poller overload),
// so the cached chain has a hole and must restart. 2s at 90kHz, far above
// the ~200ms the 5-frame queue can hold.
constexpr uint32_t kGopPtsGap = 2u * 90000u;
}

// One RTSP push target of a MediaSource: a pusher + its packers, all touched
// only on the poller thread except the reconnect attempt counter, the
// stopped flag and the backoff sleep (worker thread).
struct MediaSource::Session : public std::enable_shared_from_this<Session> {
    std::string url;
    RtspPusher::Ptr pusher;
    std::shared_ptr<RtpPacker> video_packer;
    std::shared_ptr<AacRtpPacker> audio_packer;
    std::weak_ptr<MediaSource> owner;
    std::atomic<bool> stopped{false};
    // Interruptible backoff sleep: the reconnect worker sleeps here between
    // retries, and notifyStopped() wakes it so a shutdown never has to wait
    // out the full backoff (up to 10s). stopped is set under sleep_mtx_ to
    // make the notify race-free.
    std::mutex sleep_mtx_;
    std::condition_variable sleep_cv_;
    std::atomic<bool> ever_connected{false};
    // True right after a (re)connect until decodable video has been sent.
    // Both video and audio stay silent until then: starting a stream with a
    // P frame (or with audio ahead of video) makes players drop frames to
    // resync, and with a single-reference-frame source every dropped P frame
    // shows as macroblock garbage until the next IDR. Normally cleared
    // immediately by replaying the cached partial GOP (MediaSource::
    // gop_video_/gop_audio_) on connect; when no GOP is cached yet it waits
    // for the next live IDR. Replaying a lone cached IDR (without the frames
    // after it) was tried and rejected: the live P frames that follow have
    // no reference chain, so players see garbage until the next real IDR.
    std::atomic<bool> pending_key{false};
    int attempt = 0;

    // backoff for connect retries: 0, 1s, 2s, 4s ... capped at 10s
    // x≪n=x×2 

    /*
    attempt:  0     1     2     3     4     5     ...
    返回(ms): 1000  2000  4000  8000  10000 10000  ...
    即：1s → 2s → 4s → 8s → 10s（封顶，之后一直 10s）
    */
    static int backoffMs(int attempt) {
        int ms = 1000 << std::min(attempt, 4);
        return std::min(ms, 10000);
    }

    // Sets stopped and wakes the reconnect worker out of its backoff
    // sleep. Called from the poller thread (MediaSource::stop).
    void notifyStopped() {
        {
            std::lock_guard<std::mutex> lk(sleep_mtx_);
            stopped = true;
        }
        sleep_cv_.notify_all();
    }

    // Backoff sleep that ends early when stopped is set. Returns true if
    // the wait was interrupted, so the caller leaves the retry loop right
    // away instead of sleeping out the remaining backoff.
    bool sleepBackoff(int ms) {
        std::unique_lock<std::mutex> lk(sleep_mtx_);
        return sleep_cv_.wait_for(lk, std::chrono::milliseconds(ms),
                                  [this] { return stopped.load(); });
    }

    // Called from the poller thread on disconnect, and for the first
    // connection attempt. The blocking handshake runs on a reconnect worker
    // thread so the poller loop is never blocked.
    void scheduleReconnect() {
        auto self = shared_from_this();
        auto media = owner.lock();
        if (!media || !media->reconnect_pool_) return;
        media->reconnect_pool_->async([self, media]() {
            // Retry loop: a failed handshake keeps retrying on this same
            // worker with exponential backoff. Deliberately a loop, not a
            // recursion - pool tasks may run synchronously in the calling
            // worker thread, and recursion would grow the stack forever.
            while (!self->stopped.load()) {
                int backoff = 0;
                if (self->ever_connected.load()) {
                    // A working connection got kicked: always back off before
                    // retrying. Instant reconnects thrash a busy server and
                    // can make its cleanup path worse.
                    backoff = backoffMs(self->attempt > 0 ? self->attempt : 0);
                } else if (self->attempt > 0) {
                    backoff = backoffMs(self->attempt);
                }
                // Interruptible sleep: stop() wakes this so a shutdown never
                // waits out the full backoff.
                if (backoff > 0 && self->sleepBackoff(backoff)) {
                    break;
                }
                if (self->pusher->start()) {
                    self->attempt = 0;
                    self->ever_connected = true;
                    // Stay silent until the next real IDR arrives (see the
                    // pending_key comment above). Set first so no P frame
                    // slips out in between.
                    self->pending_key = true;
                    media->poller_->async([media, self]() {
                        media->onSessionConnected(self);
                    }, false);
                    return;
                }
                self->attempt++;
            }
        });
    }
};

MediaSource::Ptr MediaSource::create(const std::string& app,
                                     const std::string& stream_id,
                                     const kit::EventPoller::Ptr& poller) {
    Ptr p(new MediaSource(app, stream_id, poller));
    p->init();
    return p;
}

MediaSource::MediaSource(const std::string& app, const std::string& stream_id,
                         const kit::EventPoller::Ptr& poller)
    : poller_(poller), app_(app), stream_id_(stream_id) {}

void MediaSource::init() {
    // Consumer callbacks run on the poller thread. The queues themselves are
    // fed from any thread (mp4 reader / camera callback).
    std::weak_ptr<MediaSource> weak = weak_from_this();
    video_src_ = std::make_shared<kit::StreamSource<VideoFrame>>();
    audio_src_ = std::make_shared<kit::StreamSource<AudioFrame>>();
    video_src_->bindPoller(poller_,
        [weak](std::vector<VideoFrame>& frames) {
            if (auto self = weak.lock()) self->onVideoFrames(frames);
        });
    audio_src_->bindPoller(poller_,
        [weak](std::vector<AudioFrame>& frames) {
            if (auto self = weak.lock()) self->onAudioFrames(frames);
        });
}

void MediaSource::setReconnectPool(kit::ThreadPool* pool) {
    reconnect_pool_ = pool;
}

void MediaSource::setMediaInfo(const MediaInfo& info) {
    {
        std::lock_guard<std::mutex> lk(info_mtx_);
        info_ = info;
    }
    auto self = shared_from_this();
    poller_->async([self]() { self->flushPendingSessions(); }, false);
}

bool MediaSource::inputH264(const uint8_t* data, int len, uint32_t pts,
                            bool is_key) {
    if (!data || len <= 0) return false;
    static std::atomic<int> dbg{0};
    if (dbg.fetch_add(1) < 20) {
        std::string msg = "[dbg] inputH264 " + stream_id_ + " len="
            + std::to_string(len) + " pts=" + std::to_string(pts)
            + " key=" + std::to_string(is_key ? 1 : 0);
        InfoL << msg;
    }
    VideoFrame f;
    f.data.assign(data, data + len);
    f.pts = pts;
    f.is_key = is_key;
    return video_src_->push(std::move(f));
}

bool MediaSource::inputAAC(const uint8_t* data, int len, uint32_t pts) {
    if (!data || len <= 0) return false;
    AudioFrame f;
    f.data.assign(data, data + len);
    f.pts = pts;
    return audio_src_->push(std::move(f));
}

bool MediaSource::startSendRtsp(const std::string& url) {
    auto self = shared_from_this();
    poller_->async([self, url]() { self->addSession(url); }, false);
    return true;
}

void MediaSource::stop(kit::semaphore* done) {
    auto self = shared_from_this();
    poller_->async([self, done]() {
        for (auto& s : self->sessions_) {
            s->notifyStopped();
            s->pusher->shutdown();
        }
        self->sessions_.clear();
        self->pending_urls_.clear();
        self->video_src_->clear();
        self->audio_src_->clear();
        self->gop_video_.clear();
        self->gop_audio_.clear();
        self->gop_video_bytes_ = 0;
        if (done) done->post();
    }, false);
}

// ---- poller thread only --------------------------------------------------

void MediaSource::onVideoFrames(std::vector<VideoFrame>& frames) {
    static std::atomic<int> dbg2{0};
    if (dbg2.fetch_add(1) < 30) {
        std::string msg = "[dbg] onVideoFrames " + stream_id_ + " batch="
            + std::to_string(frames.size()) + " sessions="
            + std::to_string(sessions_.size());
        if (!frames.empty()) {
            msg += " f0_key=" + std::to_string(frames[0].is_key ? 1 : 0);
            msg += " f0_len=" + std::to_string(frames[0].data.size());
        }
        if (!sessions_.empty()) {
            msg += " conn="
                + std::to_string(sessions_[0]->pusher->connected() ? 1 : 0);
            msg += " pending="
                + std::to_string(sessions_[0]->pending_key.load() ? 1 : 0);
        }
        InfoL << msg;
    }
    // Maintain the rolling partial-GOP cache (shared by all sessions,
    // independent of their connection state): restart on every IDR, trim
    // audio older than the IDR, detect holes left by dropped queue frames,
    // and cap the memory.
    for (auto& f : frames) {
        if (f.is_key) {
            gop_video_.clear();
            gop_video_bytes_ = 0;
            uint32_t a_pts = videoPtsToAudio(f.pts);
            while (!gop_audio_.empty() && gop_audio_.front().pts < a_pts)
                gop_audio_.pop_front();
        } else if (!gop_video_.empty()
                   && f.pts - last_gop_video_pts_ > kGopPtsGap) {
            // The bounded input queue dropped frames (poller overload): the
            // cached chain has a hole, restart it empty until the next IDR.
            gop_video_.clear();
            gop_audio_.clear();
            gop_video_bytes_ = 0;
        }
        gop_video_.push_back(f);
        gop_video_bytes_ += f.data.size();
        last_gop_video_pts_ = f.pts;
        if (gop_video_bytes_ > kGopMaxBytes) {
            WarnL << "[media] " << stream_id_ << " GOP cache over "
                  << kGopMaxBytes << " bytes, dropping (connects will wait "
                  << "for a live IDR)";
            gop_video_.clear();
            gop_audio_.clear();
            gop_video_bytes_ = 0;
        }
    }

    for (auto& f : frames) {
        for (auto& s : sessions_) {
            if (!s->pusher->connected()) continue;
            // Until a key frame has been sent, skip everything else:
            // starting a stream with a P frame makes every player show
            // garbage until the next IDR (no reference to decode from).
            if (s->pending_key.load()) {
                if (!f.is_key) continue;
                s->pending_key.store(false);
                std::string msg = "[media] " + stream_id_
                    + " sent first live IDR (pts="
                    + std::to_string(f.pts) + ")";
                InfoL << msg;
            }
            s->video_packer->packFrame(f.data.data(), (int)f.data.size(),
                                       f.pts);
        }
    }
}

void MediaSource::onAudioFrames(std::vector<AudioFrame>& frames) {
    // Audio cache follows the video GOP: buffer only while a GOP is cached;
    // frames older than the next IDR are trimmed when that IDR arrives.
    for (auto& f : frames) {
        if (!gop_video_.empty()) gop_audio_.push_back(f);
    }
    for (auto& f : frames) {
        for (auto& s : sessions_) {
            // Audio waits for the video IDR too: sending audio ahead of
            // video makes the server's cache start with audio-only data, and
            // a player joining during that window gets a broken stream - the
            // server forwards the video gap poorly and the player shows
            // macroblock garbage. Both tracks must start together from the
            // same IDR, exactly like the known-good version before the
            // cached-key-frame replay feature was added.
            if (s->pusher->connected() && !s->pending_key.load()) {
                s->audio_packer->packFrame(f.data.data(), (int)f.data.size(),
                                           f.pts);
            }
        }
    }
}

void MediaSource::onSessionConnected(const SessionPtr& s) {
    // Handshake done: replay the cached partial GOP so both tracks start
    // immediately from a complete reference chain. When no GOP is cached
    // yet (stream just started) stay silent until the next live IDR - the
    // server cache stays empty until then, so a player joining before it
    // sees no backlog to catch up on.
    if (s->pending_key.load()) dumpCachedGop(s);
    {
        std::string msg = "[dbg] onSessionConnected " + stream_id_
            + " pending=" + std::to_string(s->pending_key.load() ? 1 : 0)
            + " conn=" + std::to_string(s->pusher->connected() ? 1 : 0)
            + (s->pending_key.load() ? " (waiting for next IDR)" : "");
        InfoL << msg;
    }
}

// poller thread only: replay the cached partial GOP into one session.
// Video first, then the audio aligned to the IDR; live frames continue
// right after with continuous pts. Without this, a long GOP kept both
// tracks silent after connect and ZLMediaKit timed out the audio track.
void MediaSource::dumpCachedGop(const SessionPtr& s) {
    if (gop_video_.empty() || !gop_video_.front().is_key) return;
    for (auto& v : gop_video_) {
        s->video_packer->packFrame(v.data.data(), (int)v.data.size(), v.pts);
    }
    // Audio starts at the IDR's time point: any earlier leftovers (frames
    // that slipped past the IDR trim through queue reordering) stay out.
    uint32_t a_start = videoPtsToAudio(gop_video_.front().pts);
    for (auto& a : gop_audio_) {
        if (a.pts < a_start) continue;
        s->audio_packer->packFrame(a.data.data(), (int)a.data.size(), a.pts);
    }
    s->pending_key.store(false);
    InfoL << "[media] " << stream_id_ << " replayed cached GOP: "
          << gop_video_.size() << " video, " << gop_audio_.size()
          << " audio frames (idr_pts=" << gop_video_.front().pts << ")";
}

// poller thread only: video 90kHz pts -> audio sample-rate pts, using the
// same sample-rate fallback as the audio packer.
uint32_t MediaSource::videoPtsToAudio(uint32_t video_pts) {
    int sample_rate = 44100;
    {
        std::lock_guard<std::mutex> lk(info_mtx_);
        if (info_.sample_rate > 0) sample_rate = info_.sample_rate;
    }
    return (uint32_t)((uint64_t)video_pts * sample_rate / 90000);
}

void MediaSource::addSession(const std::string& url) {
    MediaInfo info;
    {
        std::lock_guard<std::mutex> lk(info_mtx_);
        info = info_;
    }
    if (!info.ready()) {
        pending_urls_.push_back(url);  // wait for setMediaInfo
        return;
    }
    createSession(url);
}

void MediaSource::flushPendingSessions() {
    MediaInfo info;
    {
        std::lock_guard<std::mutex> lk(info_mtx_);
        info = info_;
    }
    if (!info.ready()) return;
    for (auto& url : pending_urls_) createSession(url);
    pending_urls_.clear();
}

void MediaSource::createSession(const std::string& url) {
    MediaInfo info;
    {
        std::lock_guard<std::mutex> lk(info_mtx_);
        info = info_;
    }

    auto s = std::make_shared<Session>();
    s->url = url;
    s->owner = weak_from_this();

    RtspPusher::Param p;
    p.url = url;
    p.sps = info.sps;
    p.pps = info.pps;
    p.width = info.width;
    p.height = info.height;
    p.aac_extradata = info.aac_extra;
    p.sample_rate = info.sample_rate;
    p.channels = info.channels;

    std::weak_ptr<Session> weak_s = s;
    s->pusher = RtspPusher::create(p, [weak_s]() {
        if (auto sp = weak_s.lock()) sp->scheduleReconnect();
    });
    s->pusher->setPoller(poller_);

    RtspPusher::Ptr pusher = s->pusher;
    s->video_packer = std::make_shared<RtpPacker>(
        [pusher](const uint8_t* d, int n) { pusher->sendVideoRtp(d, n); });
    s->audio_packer = std::make_shared<AacRtpPacker>(
        [pusher](const uint8_t* d, int n) { pusher->sendAudioRtp(d, n); },
        info.sample_rate > 0 ? info.sample_rate : 44100);

    sessions_.push_back(s);
    s->scheduleReconnect();  // first connect attempt (no backoff)
}
