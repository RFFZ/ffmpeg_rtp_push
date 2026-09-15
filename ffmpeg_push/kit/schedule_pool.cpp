#include "schedule_pool.h"

#include <exception>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "Winmm.lib")
#endif

namespace kit {

SchedulePool::SchedulePool(int threads) {
#ifdef _WIN32
    // Raise the system timer resolution so cv wait_until wakes up within
    // ~1ms (the default ~15.6ms is too coarse for 25/30fps pacing).
    timeBeginPeriod(1);
#endif
    for (int i = 0; i < threads; i++) {
        threads_.emplace_back([this]() { run(); });
    }
}

SchedulePool::~SchedulePool() {
    stop();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

uint64_t SchedulePool::addTask(uint64_t delay_us, Task task) {
    auto job = std::make_shared<Job>();
    job->id = next_id_.fetch_add(1, std::memory_order_relaxed);
    job->deadline = std::chrono::steady_clock::now()
        + std::chrono::microseconds(delay_us);
    job->task = std::move(task);
    job->cancelled = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        heap_.push(job);
        cancel_index_[job->id] = job->cancelled;
    }
    cv_.notify_one();
    return job->id;



    
}

void SchedulePool::removeTask(uint64_t id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = cancel_index_.find(id);
    if (it != cancel_index_.end()) {
        it->second->store(true, std::memory_order_relaxed);
        cancel_index_.erase(it);
    }
}

size_t SchedulePool::taskCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return heap_.size();
}

void SchedulePool::stop() {
    if (exiting_.exchange(true)) return;
    cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void SchedulePool::run() {
    for (;;) {
        JobPtr job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this]() { return exiting_ || !heap_.empty(); });
            if (exiting_) return;

            uint64_t top_id = heap_.top()->id;
            auto top_deadline = heap_.top()->deadline;
            // Wait until the top job's deadline, or until the heap changes
            // (another worker took the top, or an earlier job arrived).
            cv_.wait_until(lk, top_deadline, [this, top_id]() {
                return exiting_ || heap_.empty() || heap_.top()->id != top_id;
            });
            if (exiting_) return;
            if (heap_.empty() || heap_.top()->id != top_id) continue;

            job = heap_.top();
            heap_.pop();
        }

        if (job->cancelled->load(std::memory_order_relaxed)) continue;

        uint64_t next = 0;
        try {
            next = job->task();
        } catch (...) {
            next = 0;  // a failing task is dropped, workers keep running
        }

        if (next > 0 && !job->cancelled->load(std::memory_order_relaxed)) {
            job->deadline = std::chrono::steady_clock::now()
                + std::chrono::microseconds(next);
            std::lock_guard<std::mutex> lk(mtx_);
            heap_.push(job);
        } else {
            std::lock_guard<std::mutex> lk(mtx_);
            cancel_index_.erase(job->id);
        }
    }
}

} // namespace kit
