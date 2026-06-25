#include "rtp_packer.h"


// 统一填RTP头

void RtpPacker::fillHeader(RtpHeader* hdr, bool marker, uint32_t timestamp) {
    hdr->byte0 = 0x80;
	hdr->byte1 = (marker ? 0x80 : 0x00) | (payload_type_ & 0x7F);
	hdr->seq = htons(seq_++);
	hdr->timestamp = htonl(timestamp);
	hdr->ssrc = htonl(ssrc_);

}

void RtpPacker::packFrame(const uint8_t* data, int size, uint32_t timestamp) {
    const uint8_t* end = data + size;
    const uint8_t* nalu_start = nullptr;

    int i = 0;
    while (i < size) {
        if (i + 4 <= size &&
            data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            if (nalu_start) {
                //这是典型的滑动窗口思路，看到边界才处理上一段数据
                /*
                nalu_start 记录的是"上一个起始码结束的位置"
                也就是当前NALU数据的开头

                遇到下一个起始码时：
                  → 当前位置(data+i) 就是上一个NALU的结尾
                  → 发送 nalu_start 到 data+i 之间的数据
                  → 然后把 nalu_start 更新到新NALU的开头

                所以是"看到下一个起始码才发上一个NALU"
                最后一个NALU没有下一个起始码
                → 循环结束后单独处理
                */
                sendRtpPacket(nalu_start, (data + i) - nalu_start, timestamp, false);
            }
            nalu_start = data + i + 4;
            i += 4;
        }
        else if (i + 3 <= size &&
            data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (nalu_start) {
                sendRtpPacket(nalu_start, (data + i) - nalu_start, timestamp, false);
            }
            nalu_start = data + i + 3;
            i += 3;
        }
        else {
            i++;
        }
    }

    if (nalu_start && nalu_start < end) {
        sendRtpPacket(nalu_start, end - nalu_start, timestamp, true);
    }
}

void RtpPacker::sendRtpPacket(const uint8_t* nalu, int nalu_size,
    uint32_t timestamp, bool marker) {
    if (nalu_size <= 0) return;

    if (nalu_size <= mtu_) {
        sendSingleNalu(nalu, nalu_size, timestamp, marker);
    }
    else {
        sendFuA(nalu, nalu_size, timestamp, marker);
    }
}

void RtpPacker::sendSingleNalu(const uint8_t* nalu, int size,
    uint32_t timestamp, bool marker) {
   
    int pkt_size = sizeof(RtpHeader) + size;
    std::vector<uint8_t> pkt(pkt_size, 0);
    RtpHeader* hdr = (RtpHeader*)pkt.data();
    fillHeader(hdr, marker, timestamp);
    memcpy(pkt.data() + sizeof(RtpHeader), nalu, size);
    send_cb_(pkt.data(), pkt_size);

}

void RtpPacker::sendFuA(const uint8_t* nalu, int size,
    uint32_t timestamp, bool marker) {
    uint8_t nalu_header = nalu[0];
    uint8_t nalu_type = nalu_header & 0x1F;
    uint8_t nri = nalu_header & 0x60;

    const uint8_t* payload = nalu + 1;
    int remain = size - 1;
    bool first = true;

    while (remain > 0) {
        int frag_size = std::min(remain, mtu_ - 2);
        bool last = (remain - frag_size == 0);

        int pkt_size = sizeof(RtpHeader) + 2 + frag_size;
        std::vector<uint8_t> pkt(pkt_size, 0);
        RtpHeader* hdr = (RtpHeader*)pkt.data();
        fillHeader(hdr, last && marker, timestamp);
        uint8_t* fu = pkt.data() + sizeof(RtpHeader);
        fu[0] = nri | 28;
        fu[1] = nalu_type;
        if (first) fu[1] |= 0x80;
        if (last)  fu[1] |= 0x40;
        memcpy(fu + 2, payload, frag_size);
        send_cb_(pkt.data(), pkt_size);

        payload += frag_size;
        remain -= frag_size;
        first = false;
    }
}