#pragma once
#define NOMINMAX
#include <cstdint>
#include <vector>
#include <functional>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#endif

#include <algorithm>

#include "common.h"

class AacRtpPacker {
public:
    using SendCallback = std::function<void(const uint8_t* data, int size)>;

    AacRtpPacker(SendCallback cb, int sample_rate = 44100)
        : send_cb_(cb)
        , seq_(0)
        , ssrc_(0x87654321)
        , payload_type_(97)
        , sample_rate_(sample_rate)
    {}

    void packFrame(const uint8_t* aac_data, int aac_size, uint32_t timestamp);

private:
    SendCallback send_cb_;
    uint16_t seq_;
    uint32_t ssrc_;
    uint8_t  payload_type_;
    int      sample_rate_;
};