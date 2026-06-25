#pragma once
#define NOMINMAX

#include <cstdint>
#include <vector>
#include <functional>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#endif

// RTP头，所有打包器共用
#pragma pack(push, 1)
struct RtpHeader {
    uint8_t  byte0;      // V=2, P=0, X=0, CC=0
    uint8_t  byte1;      // M + PT
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
};
#pragma pack(pop)

static_assert(sizeof(RtpHeader) == 12, "RtpHeader must be 12 bytes");