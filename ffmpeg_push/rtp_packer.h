#pragma once
#define NOMINMAX  // 禁止windows.h定义min/max宏

#include <cstdint>
#include <vector>
#include <functional>
#include <algorithm>  // std::min
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include "common.h"

//因为位域有一个额外的坑：bit在字节内的排列方向，C标准没有规定，由编译器自己决定。
/*
同样的代码：
uint8_t version : 2;
uint8_t padding : 1;

GCC：  version在低位，padding在高位
MSVC： version在高位，padding在低位
*/



class RtpPacker {
public:
    using SendCallback = std::function<void(const uint8_t* data, int size)>;

    RtpPacker(SendCallback cb)
        : send_cb_(cb)
        , seq_(0)
        , ssrc_(0x12345678)
        , payload_type_(96)
        , mtu_(1400)
    {}

    void packFrame(const uint8_t* data, int size, uint32_t timestamp);

private:
    void sendRtpPacket(const uint8_t* nalu, int nalu_size,
        uint32_t timestamp, bool marker);
    void sendSingleNalu(const uint8_t* nalu, int size,
        uint32_t timestamp, bool marker);
    void sendFuA(const uint8_t* nalu, int size,
        uint32_t timestamp, bool marker);

    // 统一填RTP头，避免重复代码
    void fillHeader(RtpHeader* hdr, bool marker, uint32_t timestamp);

    SendCallback send_cb_;
    uint16_t seq_;
    uint32_t ssrc_;
    uint8_t  payload_type_;
    int      mtu_;
};