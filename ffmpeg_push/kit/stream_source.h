#pragma once





#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "event_poller.h"

namespace kit {

// Bounded frame queue consumed by a single poller thread.
//
// Producers may push from any thread. The consumer callback runs on the
// EventPoller thread bound via bindPoller(). Wakeups are coalesced: at most
// one async() is posted per batch, no matter how many frames were pushed in
// between (matters for hundreds of streams x 25fps).
template <typename T>
class StreamSource : public std::enable_shared_from_this<StreamSource<T>> {
public:
    using ConsumeCB = std::function<void(std::vector<T>& frames)>;

    explicit StreamSource(size_t max_size = 5) : max_size_(max_size) {}

    // Must be called before push(); cb runs on the poller thread.
    void bindPoller(const EventPoller::Ptr& poller, ConsumeCB cb) {
        poller_ = poller;
        cb_ = std::move(cb);
    }

    // Push one frame (any thread). The oldest frame is dropped when the
    // queue is full. Returns false when no poller was bound yet.
    bool push(T&& frame) {
        bool need_wake = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (queue_.size() >= max_size_) queue_.pop_front();
            queue_.push_back(std::move(frame));
            need_wake = !pending_.load(std::memory_order_acquire);
        }
        if (need_wake) wake();
        return true;
    }

    bool push(const T& frame) {
        T copy = frame;
        return push(std::move(copy));
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return queue_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
    }

private:
    void wake() {
        // Coalesce: only the false -> true transition posts one async().
        // CAS = Compare-And-Swap（比较并交换），是一种原子的硬件指令，也是无锁编程的基石。
        // 如果 pending_是期望的值 expected  那么改成true  返回true
        // 否则  把pending_的值赋给expected  返回false
        bool expected = false;
        if (!pending_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel))
            return;
        auto self = this->shared_from_this();
        poller_->async([self]() { self->drain(); }, false);
    }

    void drain() {
        // Runs on the poller thread: swap out the whole batch, hand it to
        // the consumer, then re-arm the pending flag.
        std::vector<T> frames;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (queue_.empty()) break;
                frames.assign(std::make_move_iterator(queue_.begin()),
                              std::make_move_iterator(queue_.end()));
                queue_.clear();
            }
            if (cb_) {
                try {
                    cb_(frames);
                } catch (...) {
                    // never let a consumer exception kill the poller loop
                }
            }
            frames.clear();
        }
        pending_.store(false, std::memory_order_release);
        // A producer may have pushed while we were draining: re-wake.
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!queue_.empty()) wake();
        }
    }

private:
    size_t max_size_;
    std::mutex mtx_;
    std::deque<T> queue_;
    std::atomic<bool> pending_{false};
    EventPoller::Ptr poller_;
    ConsumeCB cb_;
};

} // namespace kit
