#include "rtsp_pusher.h"
#include <sstream>
#include <cstring>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

// Parse "rtsp://host:port/path" into its parts (defaults: port 554, path "/").
static bool parseUrl(const std::string& url, std::string& host, int& port,
                     std::string& path) {
    auto pos = url.find("://");
    if (pos == std::string::npos) return false;
    auto host_start = pos + 3;
    auto host_end = url.find_first_of(":/", host_start);
    if (host_end == std::string::npos) {
        host = url.substr(host_start);
        port = 554;
        path = "/";
        return true;
    }
    host = url.substr(host_start, host_end - host_start);
    if (url[host_end] == ':') {
        auto port_start = host_end + 1;
        auto port_end = url.find_first_of("/", port_start);
        std::string port_str = url.substr(port_start,
            port_end == std::string::npos ? std::string::npos : port_end - port_start);
        port = atoi(port_str.c_str());
        path = port_end == std::string::npos ? "/" : url.substr(port_end);
    } else {
        port = 554;
        path = url.substr(host_end);
    }
    return true;
}

SpsPps parseExtradata(const uint8_t* extradata, int size) {
    SpsPps result;
    // AVCC extradata:
    // [01][profile][compat][level][ff][e1]
    // [sps_len hi][sps_len lo][sps data]
    // [01][pps_len hi][pps_len lo][pps data]
    if (size < 8) return result;

    int sps_len = (extradata[6] << 8) | extradata[7];
    if (size < 8 + sps_len) return result;

    result.sps.assign(extradata + 8, extradata + 8 + sps_len);

    int pps_offset = 8 + sps_len;
    if (size < pps_offset + 3) return result;

    // [pps_offset]=numOfPPS, [pps_offset+1..+2]=pps_len, then pps data
    int pps_len = (extradata[pps_offset + 1] << 8) | extradata[pps_offset + 2];
    if (size < pps_offset + 3 + pps_len) return result;
    result.pps.assign(extradata + pps_offset + 3,
        extradata + pps_offset + 3 + pps_len);

    return result;
}

std::string aacConfig(const std::vector<uint8_t>& data) {
    // SDP config= expects the AudioSpecificConfig. For AAC-LC that is the
    // first 2 bytes; trailing bytes (a stale SBR extension here) make strict
    // parsers on the server reject the config, leaving the audio track
    // permanently unready.
    std::string result;
    char buf[4];
    for (size_t i = 0; i < data.size() && i < 2; i++) {
        snprintf(buf, sizeof(buf), "%02X", data[i]);
        result += buf;
    }
    return result;
}

std::string aacConfig(const uint8_t* data, int size) {
    std::string result;
    char buf[4];
    for (int i = 0; i < size && i < 2; i++) {
        snprintf(buf, sizeof(buf), "%02X", data[i]);
        result += buf;
    }
    return result;
}

static std::string base64Encode(const uint8_t* data, int len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((size_t)((len + 2) / 3) * 4);
    int i = 0;
    while (i + 3 <= len) {
        uint32_t b = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        result += table[(b >> 18) & 0x3F];
        result += table[(b >> 12) & 0x3F];
        result += table[(b >> 6) & 0x3F];
        result += table[b & 0x3F];
        i += 3;
    }
    if (len - i == 1) {
        uint32_t b = data[i] << 16;
        result += table[(b >> 18) & 0x3F];
        result += table[(b >> 12) & 0x3F];
        result += "==";
    } else if (len - i == 2) {
        uint32_t b = (data[i] << 16) | (data[i + 1] << 8);
        result += table[(b >> 18) & 0x3F];
        result += table[(b >> 12) & 0x3F];
        result += table[(b >> 6) & 0x3F];
        result += '=';
    }
    return result;
}

RtspPusher::Ptr RtspPusher::create(const Param& param, DisconnectCB cb) {
    return Ptr(new RtspPusher(param, std::move(cb)));
}

RtspPusher::RtspPusher(const Param& param, DisconnectCB cb)
    : param_(param), disconnect_cb_(std::move(cb)) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

RtspPusher::~RtspPusher() {
    shutdown();
    WSACleanup();
}

void RtspPusher::setPoller(const kit::EventPoller::Ptr& poller) {
    poller_ = poller;
}

void RtspPusher::shutdown() {
    stopping_ = true;
    auto self = shared_from_this();
    if (poller_) {
        poller_->async([self]() { self->onDisconnected("shutdown"); }, false);
    } else {
        onDisconnected("shutdown");
    }
}

// Blocking handshake: OPTIONS -> ANNOUNCE(SDP) -> SETUP video ->
// SETUP audio -> RECORD. Uses the passed socket, which is still blocking.
bool RtspPusher::handshake(SOCKET fd) {
    cseq_ = 1;
    session_.clear();

    // 1. OPTIONS
    sendRequest(fd, "OPTIONS " + param_.url + " RTSP/1.0\r\n"
        "CSeq: " + std::to_string(cseq_++) + "\r\n"
        "User-Agent: ffmpeg_push\r\n"
        "\r\n");
    auto resp = recvResponse(fd);
    if (resp.find("200 OK") == std::string::npos) return false;

    // 2. ANNOUNCE with SDP
    std::string sdp = buildSdp();
    sendRequest(fd, "ANNOUNCE " + param_.url + " RTSP/1.0\r\n"
        "CSeq: " + std::to_string(cseq_++) + "\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(sdp.size()) + "\r\n"
        "\r\n" + sdp);
    resp = recvResponse(fd);
    if (resp.find("200 OK") == std::string::npos) return false;

    // 3. SETUP video (interleaved channel 0-1)
    sendRequest(fd, "SETUP " + param_.url + "/trackID=0 RTSP/1.0\r\n"
        "CSeq: " + std::to_string(cseq_++) + "\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        "\r\n");
    resp = recvResponse(fd);
    if (resp.find("200 OK") == std::string::npos) return false;

    // take the session id from the video SETUP response
    auto pos = resp.find("Session: ");
    if (pos != std::string::npos) {
        auto end = resp.find("\r\n", pos);
        session_ = resp.substr(pos + 9, end - pos - 9);
        auto semi = session_.find(';');
        if (semi != std::string::npos) session_ = session_.substr(0, semi);
    }
    if (session_.empty()) return false;

    // 4. SETUP audio (interleaved channel 2-3), reusing the session id
    sendRequest(fd, "SETUP " + param_.url + "/trackID=1 RTSP/1.0\r\n"
        "CSeq: " + std::to_string(cseq_++) + "\r\n"
        "Session: " + session_ + "\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
        "\r\n");
    resp = recvResponse(fd);
    if (resp.find("200 OK") == std::string::npos) return false;

    // 5. RECORD
    sendRequest(fd, "RECORD " + param_.url + " RTSP/1.0\r\n"
        "CSeq: " + std::to_string(cseq_++) + "\r\n"
        "Session: " + session_ + "\r\n"
        "\r\n");
    resp = recvResponse(fd);
    return resp.find("200 OK") != std::string::npos;
}

bool RtspPusher::start() {
    // One handshake at a time: several reconnect tasks may race each other
    // and would otherwise overwrite fd_/connected_ with different sockets.
    std::lock_guard<std::mutex> lk(start_mtx_);
    if (stopping_) return false;
    if (connected_) return true;

    std::string host;
    int port = 0;
    std::string path;
    if (!parseUrl(param_.url, host, port, path)) return false;

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return false;

    // Bound the blocking handshake: a silent peer must fail in 5s, not pin
    // the reconnect worker forever.
    DWORD timeout_ms = 5000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
        (const char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
        (const char*)&timeout_ms, sizeof(timeout_ms));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(fd);
        WarnL << "[rtsp] connect failed: " << param_.url
              << " err=" << WSAGetLastError();
        return false;
    }

    if (!handshake(fd)) {
        closesocket(fd);
        WarnL << "[rtsp] handshake failed: " << param_.url;
        return false;
    }

    // shutdown() may have raced us during the blocking handshake.
    if (stopping_) {
        closesocket(fd);
        return false;
    }

    // switch to non-blocking, then hand the socket over to the poller
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
    fd_ = fd;
    connected_ = true;

    InfoL << "[rtsp] push connected: " << param_.url;

    auto self = shared_from_this();
    poller_->async([self]() { self->registerToPoller(); }, false);
    return true;
}

void RtspPusher::registerToPoller() {
    SOCKET fd = fd_.load();
    if (!connected_ || fd == INVALID_SOCKET) return;
    auto self = shared_from_this();
    poller_->addEvent((int)fd, kit::Event_Read,
        [self](int event) { self->onEvent(event); });
}

void RtspPusher::sendRequest(SOCKET fd, const std::string& req) {
    ::send(fd, req.c_str(), (int)req.size(), 0);
}

std::string RtspPusher::recvResponse(SOCKET fd) {
    // Read until the header terminator (our requests have no bodies, so the
    // response is complete at CRLFCRLF). SO_RCVTIMEO guards against a peer
    // that never answers: without it a stuck recv() would pin a reconnect
    // worker forever and starve every other stream's handshake.
    std::string result;
    char buf[4096];
    for (;;) {
        int n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            result.append(buf, n);
            if (result.size() >= 4 &&
                result.compare(result.size() - 4, 4, "\r\n\r\n") == 0) {
                break;
            }
            continue;
        }
        break;  // SOCKET_ERROR (incl. WSAETIMEDOUT) or peer closed
    }
    return result;
}

std::string RtspPusher::buildSdp() const {
    std::string sps_b64 = base64Encode(param_.sps.data(), (int)param_.sps.size());
    std::string pps_b64 = base64Encode(param_.pps.data(), (int)param_.pps.size());

    // profile-level-id = first 3 bytes of the SPS
    char profile[7];
    snprintf(profile, sizeof(profile), "%02X%02X%02X",
        param_.sps[1], param_.sps[2], param_.sps[3]);

    std::ostringstream sdp;
    sdp << "v=0\r\n"
        << "o=- 0 0 IN IP4 127.0.0.1\r\n"
        << "s=live\r\n"
        << "c=IN IP4 127.0.0.1\r\n"
        << "t=0 0\r\n"
        << "m=video 0 RTP/AVP 96\r\n"
        << "a=rtpmap:96 H264/90000\r\n"
        << "a=fmtp:96 packetization-mode=1;"
        << "profile-level-id=" << profile << ";"
        << "sprop-parameter-sets=" << sps_b64 << "," << pps_b64 << "\r\n"
        << "a=control:trackID=0\r\n";

    if (!param_.aac_extradata.empty()) {
        sdp << "m=audio 0 RTP/AVP 97\r\n"
            << "a=rtpmap:97 mpeg4-generic/" << param_.sample_rate
            << "/" << param_.channels << "\r\n"
            << "a=fmtp:97 streamtype=5;profile-level-id=1;"
            << "mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;"
            << "config=" << aacConfig(param_.aac_extradata) << "\r\n"
            << "a=control:trackID=1\r\n";
    }

    return sdp.str();
}

// Interleaved framing: $ channel(1) length(2) payload...
void RtspPusher::enqueueInterleaved(uint8_t channel, const uint8_t* data,
                                    int size) {
    if (!connected_ || stopping_) return;
    OutPacket pkt;
    pkt.data.resize(4 + size);
    pkt.data[0] = '$';
    pkt.data[1] = channel;
    pkt.data[2] = (size >> 8) & 0xFF;
    pkt.data[3] = size & 0xFF;
    memcpy(pkt.data.data() + 4, data, size);
    send_queue_.push_back(std::move(pkt));
    flushSendQueue();
}

void RtspPusher::sendVideoRtp(const uint8_t* data, int size) {
    enqueueInterleaved(0, data, size);  // channel 0 = video RTP
}

void RtspPusher::sendAudioRtp(const uint8_t* data, int size) {
    enqueueInterleaved(2, data, size);  // channel 2 = audio RTP
}

void RtspPusher::flushSendQueue() {
    SOCKET fd = fd_.load();
    if (fd == INVALID_SOCKET) return;
    while (!send_queue_.empty()) {
        auto& pkt = send_queue_.front();
        int n = ::send(fd, (const char*)pkt.data.data() + pkt.offset,
            (int)(pkt.data.size() - pkt.offset), 0);
        if (n > 0) {
            pkt.offset += n;
            if (pkt.offset == pkt.data.size()) send_queue_.pop_front();
            continue;
        }
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            // wait for the next writable event
            poller_->modifyEvent((int)fd, kit::Event_Read | kit::Event_Write);
            return;
        }
        onDisconnected("send err " + std::to_string(WSAGetLastError()));
        return;
    }
    // drained: stop watching for writable
    poller_->modifyEvent((int)fd, kit::Event_Read);
}

void RtspPusher::onEvent(int event) {
    if (event & kit::Event_Write) {
        flushSendQueue();
        if (!connected_) return;
    }
    if (event & kit::Event_Read) {
        char buf[1024];
        for (;;) {
            SOCKET fd = fd_.load();
            int n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) continue;  // drain RTCP / late responses
            if (n == 0) {         // peer closed
                onDisconnected("peer closed");
                return;
            }
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) break;
            onDisconnected("recv err " + std::to_string(err));
            return;
        }
    }
}

void RtspPusher::onDisconnected(const std::string& reason) {
    if (!connected_.exchange(false)) return;
    std::string msg =
        "[rtsp] push disconnected: " + param_.url + " (" + reason + ")";
    InfoL << msg;
    SOCKET fd = fd_.exchange(INVALID_SOCKET);
    if (fd != INVALID_SOCKET) {
        poller_->delEvent((int)fd);
        closesocket(fd);
    }
    send_queue_.clear();
    if (!stopping_ && disconnect_cb_) disconnect_cb_();
}
