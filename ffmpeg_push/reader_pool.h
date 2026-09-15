#pragma once

#include <memory>
#include <string>

#include "file_reader.h"
#include "kit/schedule_pool.h"
#include "media_source.h"

// Schedules mp4 file reading for many streams on a fixed number of threads
// (min-heap based, sub-millisecond accuracy). One periodic task per stream:
// read one video frame (+ audio frames in between) -> input to the
// MediaSource -> return the frame interval in microseconds.
class ReaderPool {
public:
    explicit ReaderPool(int threads = 4);
    ~ReaderPool();

    bool addSource(const MediaSource::Ptr& media, const std::string& file);
    void stop();

private:
    struct Job {
        MediaSource::Ptr media;
        std::shared_ptr<FileReader> reader;
        uint64_t task_id = 0;
    };

    kit::SchedulePool schedule_;



    
};
