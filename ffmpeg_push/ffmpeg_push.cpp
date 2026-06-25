extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <iostream>
using namespace std::chrono;

#include "logger.h"
#include "rtp_packer.h"
#include "rtsp_pusher.h"
#include "aac_rtp_packer.h"
#include "common.h"


/*
第一层（无任何依赖）：
  Logger    ← 最简单，先从这个开始
  Buffer    ← 理解零拷贝设计

第二层（只依赖第一层）：
  ThreadPool / TaskExecutor
  Timer

第三层（依赖前两层）：
  EventPoller

第四层（依赖前三层）：
  RingBuffer  ← 你想要的，到这里就水到渠成了
  */

  /*
  原始YUV
	↓ [编码层] H264压缩
  H264 NALU
	↓ [封装层] RTP打包（加时间戳/序列号/分片）
  RTP包
	↓ [协议层] RTSP握手建立通道，然后传RTP包
  ZLMediaKit


  H 264打包有三种方式
  单NALU模式：  一个RTP包 = 一个NALU（小包，<MTU 1500字节） 实际行业都用1400
  STAP-A模式：  一个RTP包 = 多个NALU（聚合小包）
  FU-A模式：    一个NALU = 多个RTP包（大包分片）

  */


  /*

  问题：多个NALU连在一起，接收方怎么知道每个NALU从哪里开始到哪里结束？

  方案A（Annex B）：用特殊起始码分隔，接收方看到起始码就知道新NALU开始了
  方案B（AVCC）：  在每个NALU前面写长度，接收方读长度然后跳过对应字节数

  Annex B（网络传输/RTP用）：
  ┌──────────────┬─────────────┬──────────────┬─────────────┐
  │ 00 00 00 01  │  NALU数据   │ 00 00 00 01  │  NALU数据   │
  │   起始码      │  (SPS)      │   起始码      │  (PPS)      │
  └──────────────┴─────────────┴──────────────┴─────────────┘
  接收方：看到 00 00 00 01 就知道一个新NALU开始了

  AVCC（MP4文件存储用）：
  ┌──────────────┬─────────────┬──────────────┬─────────────┐
  │ 00 00 00 1A  │  NALU数据   │ 00 00 00 08  │  NALU数据   │
  │ 长度=26字节   │  (26字节)   │  长度=8字节   │  (8字节)    │
  └──────────────┴─────────────┴──────────────┴─────────────┘
  接收方：读前4字节得到长度，跳过对应字节，再读下一个长度

  MP4文件：
	  需要随机访问（拖进度条直接跳到某一帧）
	  用长度字段可以快速跳转，不用扫描起始码
	  存储效率更高

  网络传输/RTP：
	  是流式的，不知道总长度
	  Annex B的起始码天然适合流式解析
	  x264编码器默认输出也是Annex B
	  所有解码器看到起始码就能工作

  AVCC:    [00 00 00 1A] [NALU数据26字节] [00 00 00 08] [NALU数据8字节]
				  ↓ 转换
  AnnexB:  [00 00 00 01] [NALU数据26字节] [00 00 00 01] [NALU数据8字节]


  RTP  包结构
  0                   1                   2                   3
   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  ├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
  │V=2│P│X│  CC   │M│    PT     │         序列号 (16bit)           │
  ├───────────────────────────────────────────────────────────────────┤
  │                        时间戳 (32bit)                             │
  ├───────────────────────────────────────────────────────────────────┤
  │                        SSRC (32bit)                               │
  ├───────────────────────────────────────────────────────────────────┤
  │                        Payload (H264数据)                         │
  └───────────────────────────────────────────────────────────────────┘

  V=2    版本号，固定填2
  P      填充位，填0
  X      扩展位，填0
  CC     CSRC数量，填0
  M      marker位，一帧的最后一个RTP包填1，其余填0
  PT     payload类型，H264一般填96
  序列号  每发一个包+1，从随机值开始
  时间戳  单位是1/90000秒，从AVPacket.pts换算
  SSRC   同步源标识，随机值，整个会话保持不变

  SPS/PPS 是描述信息，没有独立时间戳
	→ 跟着关键帧（IDR）一起发
	→ 用IDR帧的时间戳

  一帧画面可能被FU-A拆成多个RTP包
	→ 这些RTP包也共用同一个时间戳
	→ 靠 marker=1 告诉接收方"这帧结束了"
  */
  /*
  ZLMediaKit 收到推流后会缓存最近一个完整的 GOP：
  ZLMediaKit内部缓存：
  [SPS/PPS + IDR][P][P][P][P][P][P]  ← 当前GOP全部缓存

  VLC连上来时：
  → ZLMediaKit 立刻把缓存的 [SPS/PPS + IDR + 所有P帧] 先发给VLC
  → VLC 从IDR开始解码，没有花屏
  */

  /*
  其中遇到的问题
  1、时间戳不连续
  2、帧率通过时间精确控制  不然花屏卡顿
  */



int main() {
	// 日志写到 ./log/ 目录，每个文件最大 64MB
	Logger::instance().addChannel(
		std::make_shared<FileChannel>(exeDir() + "log", 64, "pushcli"));

	// 需要改级别
	Logger::instance().setLevel(LogLevel::Debug);

	// 需要改成同步写入
	Logger::instance().setWriter(
		std::make_shared<SyncLogWriter>());
	
	const char* filename = "F:\\test1.mp4"; // 换成你的mp4路径

	// 1. 打开文件
	AVFormatContext* fmt_ctx = nullptr;
	if (avformat_open_input(&fmt_ctx, filename, nullptr, nullptr) < 0) {
		printf("无法打开文件\n");
		return -1;
	}

	// 2. 读取流信息
	if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
		printf("无法获取流信息\n");
		return -1;
	}

	// 3. ── 找视频/音频流 ────────────────────────────────────
	int video_stream_idx = -1;
	int audio_stream_idx = -1;
	AVCodecParameters* codecpar = nullptr;
	AVCodecParameters* apar = nullptr;
	for (int i = 0; i < (int)fmt_ctx->nb_streams; i++) {
		auto type = fmt_ctx->streams[i]->codecpar->codec_type;
		if (type == AVMEDIA_TYPE_VIDEO && video_stream_idx == -1)
		{
			video_stream_idx = i;
			codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
			InfoL << "视频编码: " << avcodec_get_name(codecpar->codec_id);
			InfoL << "分辨率: " << codecpar->width << " x " << codecpar->height;
		}
			
		else if (type == AVMEDIA_TYPE_AUDIO && audio_stream_idx == -1)
		{
			audio_stream_idx = i;
			apar = fmt_ctx->streams[audio_stream_idx]->codecpar;
			InfoL << "音频编码: " << avcodec_get_name(apar->codec_id);
			InfoL << "采样率: " << apar->sample_rate;
			InfoL << "声道数: " << apar->ch_layout.nb_channels;
			InfoL << "extradata size: " << apar->extradata_size;

		}
	}
	if (video_stream_idx == -1) { ErrorL << "找不到视频流"; return -1; }
	if (audio_stream_idx == -1) { ErrorL << "找不到音频流"; return -1; }

	AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
	AVStream* audio_stream = fmt_ctx->streams[audio_stream_idx];

	InfoL << "---  video index " << video_stream_idx << " audio index " << audio_stream_idx 
		  << "extradata size: " << codecpar->extradata_size;


	// 解析SPS/PPS extradata里就是SPS/PPS，后面打SDP要用
	SpsPps sp = parseExtradata(codecpar->extradata, codecpar->extradata_size);
	if (sp.sps.empty() || sp.pps.empty()) {
		ErrorL << "解析SPS/PPS失败";
		return -1;
	}

	std::vector<uint8_t> aac_extra(
		apar->extradata,
		apar->extradata + apar->extradata_size);


	// 创建推流器并握手
	RtspPusher pusher;
	bool ok = pusher.connect(
		"rtsp://192.168.2.128:10101/live/test",
		sp.sps, sp.pps,
		codecpar->width, codecpar->height,
		aac_extra,
		apar->sample_rate,
		apar->ch_layout.nb_channels);
	if (!ok) {
		printf("RTSP握手失败\n");
		return -1;
	}

	// 打包器回调改成发送
	RtpPacker packer([&pusher](const uint8_t* data, int size) {
		pusher.sendVideoRtp(data, size);
		});


	AacRtpPacker audio_packer([&pusher](const uint8_t* data, int size) {
		pusher.sendAudioRtp(data, size);
		});

	// ── 读包循环 ─────────────────────────────────────────
	uint32_t rtp_ts_offset = 0;
	uint32_t last_rtp_ts = 0;
	uint32_t last_audio_ts = 0;

	auto     stream_start = steady_clock::now();
	uint32_t frame_base_ts = 0;
	bool     first_frame = true;

	while (true) {  // 外层无限循环
		// 重置到文件开头
		av_seek_frame(fmt_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
		AVPacket* pkt = av_packet_alloc();
		int frame_count = 0;
		while (av_read_frame(fmt_ctx, pkt) >= 0) {
			if (pkt->stream_index == video_stream_idx) {


				// 1. AVCC转AnnexB：把前4字节长度替换成起始码
				uint8_t* data = pkt->data;
				int size = pkt->size;

				//因为一个 AVPacket 里可能有多个 NALU，需要循环处理每一个
				while (size > 4) {
					uint32_t nalu_size = (data[0] << 24) | (data[1] << 16)
						| (data[2] << 8) | data[3];
					data[0] = 0x00;
					data[1] = 0x00;
					data[2] = 0x00;
					data[3] = 0x01;
					data += 4 + nalu_size;
					size -= 4 + nalu_size;
				}

				// 2. 计算RTP时间戳
				// pts单位是time_base，换算成90000Hz
				//pkt->pts 的单位是 time_base

				
				/* 符号	含义	作用
				pts	显示时间戳	告诉解码器何时显示该帧
				time_base	时间基	pts 的单位（秒/单位）
				av_q2d	有理数转浮点	用于乘法计算
				90000	RTP 时钟频率	将秒转为 RTP 时间戳单位

				time_base 是一个分数，比如 1 / 12800
					意思是：pts 的每一个单位 = 1 / 12800 秒

					av_q2d(time_base) 把分数转成小数：
					1 / 12800 = 0.000078125 秒 / 单位

					换算成秒：
					pkt->pts * av_q2d(time_base) = 实际秒数

					RTP时间戳单位是 1 / 90000 秒（H264固定）
					所以再乘以 90000：

					实际秒数 × 90000 = RTP时间戳

					rtp_timestamp = pts × av_q2d(time_base) × 90000
					 ↑           ↑                ↑
				  FFmpeg值    转成秒          转成RTP单位

				
				//一个 packet 里的所有 NALU 都属于同一帧，显示时间相同
				//timestamp 表达的是这帧画面应该在什么时刻显示，不是包的编号，所以必须随时间递增，接收方才能正确还原播放节奏

			
				* pts 是 FFmpeg 内部的时间单位
				  timestamp 是 RTP 协议的时间单位

				pts 的单位由 time_base 决定，每个文件可能不同：
				  文件A：time_base=1/12800，25fps时 pts步进512
				  文件B：time_base=1/90000，25fps时 pts步进3600
				  文件C：time_base=1/1000， 25fps时 pts步进40

				RTP 协议规定 H264 时间戳单位固定是 1/90000
				所以必须统一换算，接收方才能用同一套逻辑处理所有流*/
	
				uint32_t rtp_ts = (uint32_t)(pkt->pts
					* 90000
					* av_q2d(video_stream->time_base)) + rtp_ts_offset;
				// 记录这轮循环第一帧的时间戳
				if (first_frame) {
					frame_base_ts = rtp_ts;
					stream_start = steady_clock::now();
					first_frame = false;
				}
				last_rtp_ts = rtp_ts;

				
				// 帧率控制
				// 计算这帧应该在什么时刻发出
				// rtp_ts单位是1/90000秒
				uint32_t elapsed = rtp_ts - frame_base_ts;
				auto target = stream_start
					+ microseconds((int64_t)elapsed * 1000000 / 90000);

				// 等到目标时间再发
				auto now = steady_clock::now();
				if (target > now) {
					std::this_thread::sleep_until(target);
				}

				// 送进打包器
				packer.packFrame(pkt->data, pkt->size, rtp_ts);

				// 判断是否关键帧（AnnexB转换后第5字节是NALU header）
				bool is_key = (pkt->size > 4) &&
					((pkt->data[4] & 0x1F) == 5);

				DebugL << "视频帧 #" << frame_count++
					<< " 大小:" << pkt->size
					<< " rtp_ts:" << rtp_ts
					<< (is_key ? " [IDR]" : "");
			}


			// 读包循环里加音频处理
			else if (pkt->stream_index == audio_stream_idx) {
				// MP4里的AAC是带ADTS头或者裸AAC
				// FFmpeg读出来的AAC packet是裸AAC数据，直接打包
				uint32_t audio_ts = (uint32_t)(pkt->pts
					* (double)apar->sample_rate
					* av_q2d(audio_stream->time_base));
				last_audio_ts = audio_ts;
				audio_packer.packFrame(pkt->data, pkt->size, audio_ts);
			}

			av_packet_unref(pkt);
		}

		// 文件读完，计算下一轮的偏移
		// 加一帧的时间，避免两轮之间时间戳跳变
		//fps.num / fps.den 表示帧率（fps）
		
		/*
		* 一帧的时间（秒） = 1 / 帧率 = fps.den / fps.num
		RTP 时间戳单位 = 实际秒数 × 90000
		合并：one_frame (RTP单位) = (fps.den / fps.num) × 90000

		*/

		
		// ── 文件循环：更新时间戳偏移 ─────────────────────
		// 文件读完，计算下一轮的偏移
		// 加一帧的时间，避免两轮之间时间戳跳变
		//fps.num / fps.den 表示帧率（fps）
		/*
		一帧的时间（秒） = 1 / 帧率 = fps.den / fps.num
		RTP 时间戳单位 = 实际秒数 × 90000
		合并：one_frame (RTP单位) = (fps.den / fps.num) × 90000
		*/
		AVRational fps = video_stream->avg_frame_rate;
		uint32_t one_frm = (uint32_t)(90000.0 * fps.den / fps.num);
		rtp_ts_offset = last_rtp_ts + one_frm;
		first_frame = true;  // 下一轮重新记录起始时间

		InfoL << "文件播放完毕，重新开始循环 (已推" << frame_count << "帧)";
		av_packet_free(&pkt);
		
	}

	avformat_close_input(&fmt_ctx);
	return 0;

	
}