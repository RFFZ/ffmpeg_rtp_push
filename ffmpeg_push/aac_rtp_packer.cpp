#include "aac_rtp_packer.h"
#include <cstring>

void AacRtpPacker::packFrame(const uint8_t* aac_data, int aac_size, uint32_t timestamp) {
    // AAC RTP包结构：
    // [RTP头12字节][AU-Header-Length 2字节][AU-Header 2字节][AAC数据]
    // AU-Header-Length = 0x0010（固定，表示后面的AU-Header是16bit）
    // AU-Header = aac_size << 3（高13bit是size，低3bit是index=0）

    int pkt_size = sizeof(RtpHeader) + 2 + 2 + aac_size;
    std::vector<uint8_t> pkt(pkt_size, 0);

    // 填RTP头
    RtpHeader* hdr = (RtpHeader*)pkt.data();
    hdr->byte0 = 0x80;
    hdr->byte1 = 0x80 | (payload_type_ & 0x7F);  // marker=1，AAC每帧都置1
    hdr->seq = htons(seq_++);
    hdr->timestamp = htonl(timestamp);
    hdr->ssrc = htonl(ssrc_);

    uint8_t* p = pkt.data() + sizeof(RtpHeader);

    // AU-Header-Length：固定0x0010，表示16bit
    p[0] = 0x00;
    p[1] = 0x10;

    // AU-Header：size左移3位，低3位是index=0
    uint16_t au_header = (uint16_t)(aac_size << 3);
    p[2] = (au_header >> 8) & 0xFF;
    p[3] = au_header & 0xFF;

    // AAC数据
    memcpy(p + 4, aac_data, aac_size);

    send_cb_(pkt.data(), pkt_size);
}