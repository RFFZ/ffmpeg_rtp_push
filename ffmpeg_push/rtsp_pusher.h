#pragma once
#define NOMINMAX
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

#include "kit/event_poller.h"

// Parse AVCC extradata into SPS/PPS (for the SDP).
struct SpsPps {
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
};
SpsPps parseExtradata(const uint8_t* extradata, int size);
std::string aacConfig(const uint8_t* data, int size);
std::string aacConfig(const std::vector<uint8_t>& data);

// Non-blocking RTSP/TCP pusher driven by one kit::EventPoller thread.
//
// The blocking handshake (OPTIONS/ANNOUNCE/SETUP/RECORD) runs inside start(),
// callable from any thread (typically a reconnect worker). Once established
// the socket switches to non-blocking and all IO events are handled on the
// bound poller thread: send queue flushed on writable events, reads drain
// server responses/RTCP, recv()==0 means the peer closed -> disconnect
// callback (the owner schedules a reconnect outside the poller thread).
class RtspPusher : public std::enable_shared_from_this<RtspPusher> {
public:
    using Ptr = std::shared_ptr<RtspPusher>;
    using DisconnectCB = std::function<void()>;

    struct Param {
        std::string url;  // rtsp://ip:port/path
        std::vector<uint8_t> sps, pps;
        int width = 0, height = 0;
        std::vector<uint8_t> aac_extradata;
        int sample_rate = 0;
        int channels = 0;
    };

    static Ptr create(const Param& param, DisconnectCB cb);
    ~RtspPusher();

    void setPoller(const kit::EventPoller::Ptr& poller);

    // Blocking RTSP handshake. On success the socket is registered to the
    // poller and sendVideoRtp/sendAudioRtp may be called.
    bool start();

    // Poller thread only. Interleaved framing ($ + channel + len) is added
    // here; on WSAEWOULDBLOCK the packet is queued and flushed later.
    void sendVideoRtp(const uint8_t* data, int size);
    void sendAudioRtp(const uint8_t* data, int size);
    bool connected() const { return connected_; }

    // Any thread: close, unregister and stop the disconnect callback.
    void shutdown();

private:
    RtspPusher(const Param& param, DisconnectCB cb);

    void onEvent(int event);       // poller thread
    void flushSendQueue();         // poller thread
    void onDisconnected(const std::string& reason);  // poller thread: close + notify
    void registerToPoller();       // poller thread: addEvent
    bool handshake(SOCKET fd);     // blocking handshake on its own socket
    void sendRequest(SOCKET fd, const std::string& req);
    std::string recvResponse(SOCKET fd);
    std::string buildSdp() const;
    void enqueueInterleaved(uint8_t channel, const uint8_t* data, int size);

    struct OutPacket {
        std::vector<uint8_t> data;  // $ + channel + length + RTP payload
        size_t offset = 0;          // partially sent bytes
    };

    Param        param_;
    DisconnectCB disconnect_cb_;
    kit::EventPoller::Ptr poller_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<SOCKET> fd_{INVALID_SOCKET};
    std::mutex start_mtx_;          // serialize concurrent start() handshakes
    std::deque<OutPacket> send_queue_;  // poller thread only
    std::string session_;
    int cseq_ = 1;
};
