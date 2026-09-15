#include "file_reader.h"

#include <cstdio>

FileReader::~FileReader() {
    close();
}

bool FileReader::open(const std::string& file) {
    close();

    if (avformat_open_input(&fmt_ctx_, file.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        close();
        return false;
    }

    video_stream_idx_ = -1;
    audio_stream_idx_ = -1;
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; i++) {
        auto type = fmt_ctx_->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && video_stream_idx_ == -1)
            video_stream_idx_ = (int)i;
        else if (type == AVMEDIA_TYPE_AUDIO && audio_stream_idx_ == -1)
            audio_stream_idx_ = (int)i;
    }
    if (video_stream_idx_ == -1) {
        close();
        return false;
    }

    AVCodecParameters* vp = fmt_ctx_->streams[video_stream_idx_]->codecpar;
    SpsPps sp = parseExtradata(vp->extradata, vp->extradata_size);
    if (sp.sps.empty() || sp.pps.empty()) {
        close();
        return false;
    }
    info_.sps = std::move(sp.sps);
    info_.pps = std::move(sp.pps);
    info_.width = vp->width;
    info_.height = vp->height;

    if (audio_stream_idx_ != -1) {
        AVCodecParameters* ap = fmt_ctx_->streams[audio_stream_idx_]->codecpar;
        info_.aac_extra.assign(ap->extradata, ap->extradata + ap->extradata_size);
        info_.sample_rate = ap->sample_rate;
        info_.channels = ap->ch_layout.nb_channels;
    }

    // frame interval from fps (fall back to 25fps when unknown)
    AVRational fps = fmt_ctx_->streams[video_stream_idx_]->avg_frame_rate;
    if (fps.num <= 0 || fps.den <= 0)
        fps = fmt_ctx_->streams[video_stream_idx_]->r_frame_rate;
    if (fps.num <= 0 || fps.den <= 0) {
        fps.num = 25;
        fps.den = 1;
    }
    frame_interval_us_ = (uint32_t)(fps.den * 1000000.0 / fps.num);
    one_frm_90k_ = (uint32_t)(90000.0 * fps.den / fps.num);

    InfoL << "[reader] open " << file << ": " << info_.width << "x"
          << info_.height << " fps=" << fps.num << "/" << fps.den
          << " tb=" << fmt_ctx_->streams[video_stream_idx_]->time_base.num
          << "/" << fmt_ctx_->streams[video_stream_idx_]->time_base.den
          << " sps=" << info_.sps.size() << " pps=" << info_.pps.size();

    {
        std::string sps_hex;
        char hexbuf[4];
        for (uint8_t b : info_.sps) {
            snprintf(hexbuf, sizeof(hexbuf), "%02X", b);
            sps_hex += hexbuf;
        }
        std::string msg = "[reader] extradata sps=" + sps_hex;
        InfoL << msg;
    }

    if (audio_stream_idx_ != -1) {
        std::string aac_hex;
        char hexbuf[4];
        for (uint8_t b : info_.aac_extra) {
            snprintf(hexbuf, sizeof(hexbuf), "%02X", b);
            aac_hex += hexbuf;
        }
        std::string msg = "[reader] audio " + file + ": sr="
            + std::to_string(info_.sample_rate) + " ch="
            + std::to_string(info_.channels) + " aac_extra("
            + std::to_string(info_.aac_extra.size()) + "B)=" + aac_hex;
        InfoL << msg;
    } else {
        std::string msg = "[reader] audio " + file + ": none";
        InfoL << msg;
    }

    return true;
}

void FileReader::close() {
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    video_stream_idx_ = -1;
    audio_stream_idx_ = -1;
    rtp_ts_offset_ = 0;
    audio_ts_offset_ = 0;
    last_video_pts_ = 0;
    last_audio_pts_ = 0;
}

bool FileReader::readNextVideoFrame(VideoFrame& v,
                                    std::vector<AudioFrame>& audios) {
    if (!fmt_ctx_) return false;

    AVStream* vs = fmt_ctx_->streams[video_stream_idx_];
    AVStream* as = audio_stream_idx_ != -1
        ? fmt_ctx_->streams[audio_stream_idx_] : nullptr;

    AVPacket* pkt = av_packet_alloc();
    bool got = false;

    while (!got) {
        int ret = av_read_frame(fmt_ctx_, pkt);
        if (ret < 0) {
            // EOF: rewind and shift the timestamps one frame interval
            if (av_seek_frame(fmt_ctx_, video_stream_idx_, 0,
                              AVSEEK_FLAG_BACKWARD) < 0) {
                break;  // broken file, stop this source
            }
            rtp_ts_offset_ = PtsUtil::nextLoopOffset(last_video_pts_,
                                                     one_frm_90k_);
            if (as && last_audio_pts_) {
                audio_ts_offset_ = last_audio_pts_ +
                    (uint32_t)(info_.sample_rate * frame_interval_us_ / 1000000.0);
            }
            continue;
        }

        if (pkt->stream_index == video_stream_idx_) {
            // AVCC -> AnnexB in place: replace each 4-byte length with 0001.
            // While walking the NALUs, note whether the packet already
            // carries an IDR / SPS / PPS in-band: some files put the
            // parameter sets in front of the IDR inside the sample itself.
            uint8_t* d = pkt->data;
            int size = pkt->size;
            bool has_idr = false;
            bool has_sps = false;
            bool has_pps = false;
            while (size > 4) {
                uint32_t nalu_size = (d[0] << 24) | (d[1] << 16)
                    | (d[2] << 8) | d[3];
                uint8_t nalu_type = d[4] & 0x1F;
                has_idr |= (nalu_type == 5);
                has_sps |= (nalu_type == 7);
                has_pps |= (nalu_type == 8);
                d[0] = 0x00;
                d[1] = 0x00;
                d[2] = 0x00;
                d[3] = 0x01;
                d += 4 + nalu_size;
                size -= 4 + nalu_size;
            }

            uint32_t pts = PtsUtil::timeBaseTo90k(pkt->pts, vs->time_base)
                + rtp_ts_offset_;
            last_video_pts_ = pts;

            v.data.assign(pkt->data, pkt->data + pkt->size);
            v.pts = pts;
            // A key frame is any frame containing an IDR - not just one
            // whose FIRST NALU is an IDR (in-band SPS/PPS, which sit before
            // the IDR, would defeat that naive check and every frame would
            // look like a P frame).
            v.is_key = has_idr;
            if (has_idr && !(has_sps && has_pps)) {
                // AVCC packets usually carry no SPS/PPS (they live in
                // extradata only), so the pushed stream would never contain
                // them. Prepend them to key frames that lack them: the
                // server's GOP cache then always starts with a decodable
                // prefix and players never show garbage while waiting for
                // an IDR.
                std::vector<uint8_t> full;
                full.reserve(8 + info_.sps.size() + info_.pps.size()
                             + v.data.size());
                full.insert(full.end(), {0x00, 0x00, 0x00, 0x01});
                full.insert(full.end(), info_.sps.begin(), info_.sps.end());
                full.insert(full.end(), {0x00, 0x00, 0x00, 0x01});
                full.insert(full.end(), info_.pps.begin(), info_.pps.end());
                full.insert(full.end(), v.data.begin(), v.data.end());
                v.data.swap(full);
            }
            got = true;
        } else if (pkt->stream_index == audio_stream_idx_) {
            AudioFrame a;
            a.data.assign(pkt->data, pkt->data + pkt->size);
            a.pts = PtsUtil::timeBaseToSampleRate(pkt->pts, as->time_base,
                                                  info_.sample_rate)
                + audio_ts_offset_;
            last_audio_pts_ = a.pts;
            audios.push_back(std::move(a));
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return got;
}
