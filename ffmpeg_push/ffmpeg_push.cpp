// Multi-stream RTSP push demo built on the media framework:
//
//   MediaManager (facade, mirrors libmk_api)
//     - MediaSource  : one stream, source agnostic (mp4 reader / camera cb)
//     - ReaderPool   : N mp4 readers paced on a few SchedulePool threads
//     - EventPollerPool : non-blocking RTP sending, a few threads serve all
//     - ThreadPool   : blocking RTSP handshakes / reconnects
//
// Default: loop one mp4 to 3 RTSP addresses. A camera source example is in
// the comments below: the capture callback just calls inputH264/inputAAC
// with wall-clock pts - exactly the same path as the mp4 reader.

#include <iostream>
#include <string>

#include "kit/logger.h"
#include "media_manager.h"

static const char* kDefaultFile = "F:\\test1.mp4";
static const char* kDefaultHost = "192.168.2.128";
static const int   kDefaultPort = 10101;
static const int   kDefaultStreams = 1;

int main(int argc, char* argv[]) {
    // logs to ./log/ (rotated at 64MB), info level, sync writer
    Logger::instance().addChannel(
        std::make_shared<FileChannel>(exeDir() + "log", 64, "pushcli"));
    Logger::instance().setLevel(LogLevel::Info);
    Logger::instance().setWriter(std::make_shared<SyncLogWriter>());

    std::string file = argc > 1 ? argv[1] : kDefaultFile;
    std::string host = argc > 2 ? argv[2] : kDefaultHost;
    int port = argc > 3 ? atoi(argv[3]) : kDefaultPort;
    int stream_count = argc > 4 ? atoi(argv[4]) : kDefaultStreams;
    if (stream_count < 1) stream_count = 1;

    MediaManager& mgr = MediaManager::instance();
    mgr.setThreads(4, 4);  // 4 mp4 reader threads + 4 poller threads

    for (int i = 0; i < stream_count; i++) {
        std::string id = "stream_" + std::to_string(i);
        auto m = mgr.createMedia("live", id);
        if (!m) {
            ErrorL << "createMedia failed: " << id;
            continue;
        }
        if (!mgr.bindFile(m, file)) {
            ErrorL << "bindFile failed: " << file;
            continue;
        }
        std::string url = "rtsp://" + host + ":" + std::to_string(port)
            + "/live/" + id;
        if (!mgr.startSendRtsp(m, url)) {
            ErrorL << "startSendRtsp failed: " << url;
        }
    }

    // Camera source example - same processing chain, no reader involved:
    //
    //   auto cam = mgr.createMedia("live", "camera_1");
    //   cam->setMediaInfo(...);  // sps/pps/width/height/aac_extra/...
    //   mgr.startSendRtsp(cam, "rtsp://.../live/camera_1");
    //   // in the capture callback thread:
    //   auto t0 = std::chrono::steady_clock::now();
    //   cam->inputH264(data, len, PtsUtil::wallClockTo90k(t0), is_key);
    //   cam->inputAAC(aac, len, PtsUtil::wallClockTo90k(t0));

    InfoL << stream_count << " streams pushing, press Enter to quit...";
    std::cin.get();

    mgr.shutdown();
    return 0;
}
