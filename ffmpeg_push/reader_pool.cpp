#include "reader_pool.h"

#include "file_reader.h"

ReaderPool::ReaderPool(int threads) : schedule_(threads) {}

ReaderPool::~ReaderPool() {
    stop();
}

bool ReaderPool::addSource(const MediaSource::Ptr& media,
                           const std::string& file) {
    auto job = std::make_shared<Job>();
    job->media = media;
    job->reader = std::make_shared<FileReader>();
    if (!job->reader->open(file)) return false;

    // Stream description for the SDP (before the RTSP handshake).
    media->setMediaInfo(job->reader->info());

    // One periodic task per stream; the returned delay paces the frame rate.
    job->task_id = schedule_.addTask(0, [job]() -> uint64_t {
        VideoFrame v;
        std::vector<AudioFrame> audios;
        if (!job->reader->readNextVideoFrame(v, audios)) return 0;  // broken
        //InfoL << "input h264 pts " << v.pts << " is key " << v.is_key;
        job->media->inputH264(v.data.data(), (int)v.data.size(), v.pts,
                              v.is_key);
        for (auto& a : audios) {
           // InfoL << "input AAC pts " << v.pts;
            job->media->inputAAC(a.data.data(), (int)a.data.size(), a.pts);
        }
        return job->reader->frameIntervalUs();
    });
    return job->task_id != 0;
}

void ReaderPool::stop() {
    schedule_.stop();
}
