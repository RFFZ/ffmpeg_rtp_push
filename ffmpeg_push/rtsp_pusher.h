#pragma once
#define NOMINMAX
#include <string>
#include <cstdint>
#include <vector>
#include <functional>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

// 解析extradata，提取SPS和PPS
struct SpsPps {
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
};
SpsPps parseExtradata(const uint8_t* extradata, int size);
std::string aacConfig(const uint8_t* data, int size);
std::string aacConfig(const std::vector<uint8_t>& data);
class RtspPusher {
public:
    RtspPusher();
    ~RtspPusher();

    // 连接并完成RTSP握手
    bool connect(const std::string& url,         // rtsp://127.0.0.1:554/live/test
        const std::vector<uint8_t>& sps,
        const std::vector<uint8_t>& pps,
        int width, int height, 
        const std::vector<uint8_t>& aac_extradata,  // 加这个
        int sample_rate,                             // 加这个
        int channels);

    // 发送一帧RTP数据（握手完成后调用）
    void sendVideoRtp(const uint8_t* data, int size);
    void sendAudioRtp(const uint8_t* data, int size);
    void disconnect();

private:
    bool sendRequest(const std::string& req);
    std::string recvResponse();
    std::string buildSdp(const std::vector<uint8_t>& sps,
        const std::vector<uint8_t>& pps,
        int width, int height,
        const std::vector<uint8_t>& aac_extradata,  // 加这个
        int sample_rate,                             // 加这个
        int channels);

    SOCKET   socket_;
    int      cseq_;        // 每次请求递增
    std::string session_;  // SETUP之后服务器返回的session id
};