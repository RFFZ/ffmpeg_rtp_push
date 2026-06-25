#include "rtsp_pusher.h"
#include <sstream>
#include <cstring>
#include <stdio.h>


SpsPps parseExtradata(const uint8_t* extradata, int size) {
    SpsPps result;
    // extradata格式(AVCC)：
    // [01][profile][compat][level][ff][e1]
    // [sps_len高][sps_len低][sps数据]
    // [01][pps_len高][pps_len低][pps数据]

    if (size < 8) return result;

    int sps_len = (extradata[6] << 8) | extradata[7];
    if (size < 8 + sps_len) return result;

    result.sps.assign(extradata + 8, extradata + 8 + sps_len);

    int pps_offset = 8 + sps_len;
    if (size < pps_offset + 4) return result;

    int pps_len = (extradata[pps_offset + 2] << 8) | extradata[pps_offset + 3];
    result.pps.assign(extradata + pps_offset + 3 + 1,
        extradata + pps_offset + 3 + 1 + pps_len);

    return result;
}
std::string aacConfig(const std::vector<uint8_t>& data) {
    std::string result;
    char buf[4];
    for (uint8_t b : data) {
        snprintf(buf, sizeof(buf), "%02X", b);
        result += buf;
    }
    return result;
}

std::string aacConfig(const uint8_t* data, int size) {
    std::string result;
    char buf[4];
    for (int i = 0; i < size; i++) {
        snprintf(buf, sizeof(buf), "%02X", data[i]);
        result += buf;
    }
    return result;
}

// Base64（复用之前写的，这里简单起见直接内联）
static std::string base64Encode(const uint8_t* data, int len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int i = 0;
    while (i < len) {
        uint32_t b = data[i++] << 16;
        if (i < len) b |= data[i++] << 8;
        if (i < len) b |= data[i++];
        result += table[(b >> 18) & 0x3F];
        result += table[(b >> 12) & 0x3F];
        result += (i - 2 < len) ? table[(b >> 6) & 0x3F] : '=';
        result += (i - 1 < len) ? table[b & 0x3F] : '=';
    }
    return result;
}



RtspPusher::RtspPusher() : socket_(INVALID_SOCKET), cseq_(1) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

RtspPusher::~RtspPusher() {
    disconnect();
    WSACleanup();
}


//RTSP握手(OPTIONS/ANNOUNCE/SETUP视频/SETUP音频/RECORD)
bool RtspPusher::connect(const std::string& url,
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps,
    int width, int height,
    const std::vector<uint8_t>& aac_extradata,  // 加这个
    int sample_rate,                             // 加这个
    int channels) {
    // 解析url，暂时硬编码host/port/path
    // url格式: rtsp://127.0.0.1:554/live/test
    std::string host = "192.168.2.128";
    int port = 10101;
    std::string path = "/live/test";

    // 1. 建立TCP连接
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(socket_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("连接失败\n");
        return false;
    }
    printf("TCP连接成功\n");

    // 2. OPTIONS
    std::string options_req =
        "OPTIONS " + url + " RTSP/1.0\r\n"
        "CSeq: " + std::to_string(cseq_++) + "\r\n"
        "User-Agent: ffmpeg_push\r\n"
        "\r\n";
    sendRequest(options_req);
    auto resp = recvResponse();
    printf("OPTIONS响应:\n%s\n", resp.c_str());


    // 3. ANNOUNCE（带SDP）← 必须在SETUP之前
    std::string sdp = buildSdp(sps, pps, width, height, aac_extradata, sample_rate, channels);
    std::string announce_req =
        "ANNOUNCE " + url + " RTSP/1.0\r\n"
        "CSeq: " + std::to_string(cseq_++) + "\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(sdp.size()) + "\r\n"
        "\r\n" + sdp;
    sendRequest(announce_req);
    resp = recvResponse();
    printf("ANNOUNCE响应:\n%s\n", resp.c_str());

    //interleaved  视频用0-1，音频用2-3。
    //4. 视频SETUP
    std::string setup_req =
        "SETUP " + url + "/trackID=0 RTSP/1.0\r\n"
        "CSeq: " + std::to_string(cseq_++) + "\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        "\r\n";
    sendRequest(setup_req);
    resp = recvResponse();
    printf("SETUP视频响应:\n%s\n", resp.c_str());


    // 从视频SETUP响应里拿Session ID
    auto pos = resp.find("Session: ");
    if (pos != std::string::npos) {
        auto end = resp.find("\r\n", pos);
        session_ = resp.substr(pos + 9, end - pos - 9);
        auto semi = session_.find(';');
        if (semi != std::string::npos)
            session_ = session_.substr(0, semi);
        printf("Session: %s\n", session_.c_str());
    }

    // 5. SETUP音频 ← 在视频SETUP之后，Session拿到之后
    std::string setup_audio =
        "SETUP " + url + "/trackID=1 RTSP/1.0\r\n"
        "CSeq: " + std::to_string(cseq_++) + "\r\n"
        "Session: " + session_ + "\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
        "\r\n";
    sendRequest(setup_audio);
    resp = recvResponse();
    printf("SETUP音频响应:\n%s\n", resp.c_str());


    // 5. RECORD
    std::string record_req =
        "RECORD " + url + " RTSP/1.0\r\n"
        "CSeq: " + std::to_string(cseq_++) + "\r\n"
        "Session: " + session_ + "\r\n"
        "\r\n";
    sendRequest(record_req);
    resp = recvResponse();
    printf("RECORD响应:\n%s\n", resp.c_str());

    return resp.find("200 OK") != std::string::npos;
}

std::string RtspPusher::buildSdp(const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps,
    int width, int height, 
    const std::vector<uint8_t>& aac_extradata,  // 加这个
    int sample_rate,                             // 加这个
    int channels) {
    std::string sps_b64 = base64Encode(sps.data(), sps.size());
    std::string pps_b64 = base64Encode(pps.data(), pps.size());

    // profile-level-id 从SPS前3字节来
    char profile[7];
    snprintf(profile, sizeof(profile), "%02X%02X%02X",
        sps[1], sps[2], sps[3]);

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

    sdp << "m=audio 0 RTP/AVP 97\r\n"
        << "a=rtpmap:97 mpeg4-generic/" << sample_rate << "/" << channels << "\r\n"
        << "a=fmtp:97 streamtype=5;profile-level-id=1;"
        << "mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;"
        << "config=" << aacConfig(aac_extradata) << "\r\n"  // 用vector版本
        << "a=control:trackID=1\r\n";


    return sdp.str();
}
/*
+--------+--------+--------+--------+--------+--------+-----+
|   $    | channel|   length (高8位) | length (低8位) | RTP数据... |
+--------+--------+--------+--------+--------+--------+-----+
| 1字节   | 1字节   |             2字节               |   变长     |
*/
void RtspPusher::sendVideoRtp(const uint8_t* data, int size) {
    // RTP over TCP格式：$ channel(1字节) length(2字节) rtp数据
    // 打印前4字节，看RTP头是否正确
    printf("RTP头字节: %02X %02X %02X %02X\n",
        data[0], data[1], data[2], data[3]);
    uint8_t header[4];
    header[0] = '$';
    header[1] = 0;                    // channel 0 = 视频RTP
    header[2] = (size >> 8) & 0xFF;
    header[3] = size & 0xFF;
    ::send(socket_, (char*)header, 4, 0);
    ::send(socket_, (char*)data, size, 0);
}

void RtspPusher::sendAudioRtp(const uint8_t* data, int size) {
    uint8_t header[4];
    header[0] = '$';
    header[1] = 2;                    // channel 2 = 音频RTP
    header[2] = (size >> 8) & 0xFF;
    header[3] = size & 0xFF;
    ::send(socket_, (char*)header, 4, 0);
    ::send(socket_, (char*)data, size, 0);
}

bool RtspPusher::sendRequest(const std::string& req) {
    return ::send(socket_, req.c_str(), req.size(), 0) > 0;
}

std::string RtspPusher::recvResponse() {
    char buf[4096] = {};
    ::recv(socket_, buf, sizeof(buf) - 1, 0);
    return std::string(buf);
}

void RtspPusher::disconnect() {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}