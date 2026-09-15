#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kit {

// Fixed small number of threads scheduling many periodic tasks by deadline
// (min-heap). A task returns the next delay in microseconds; <= 0 removes it.
// ffmpeg_push: ReaderPool uses it for frame-rate pacing of hundreds of mp4
// readers. GBMedia_Push: playback pacing (speed control), while live streams
// stay "push on arrival" and never occupy the scheduler.
// 定时任务调度池，用于调度许多个按截止时间执行的任务
class SchedulePool {
public:
    using Task = std::function<uint64_t()>;  // next delay in us, <=0 removes

    explicit SchedulePool(int threads = 4);  // 默认构造函数，指定线程数
    ~SchedulePool();

    SchedulePool(const SchedulePool&) = delete;
    SchedulePool& operator=(const SchedulePool&) = delete;

    // First run after delay_us; returns a task id (>0).
    uint64_t addTask(uint64_t delay_us, Task task);

    // Cancel by id (no-op if already removed/finished).
    void removeTask(uint64_t id);

    void stop();
    size_t taskCount() const;

private:
    struct Job {
        uint64_t id;
        std::chrono::steady_clock::time_point deadline;
        Task task;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };
    using JobPtr = std::shared_ptr<Job>;
    /*
    当 a->deadline > b->deadline（a 更晚到期）时返回 true，即认为 a 优先级更低、排后面。
    于是 deadline 最早的任务排到最前面，top() 拿到的是最早到期的任务。
    这就是一个最小堆（min-heap）。
    */
    struct Earlier {
        bool operator()(const JobPtr& a, const JobPtr& b) const {
            if (a->deadline != b->deadline) return a->deadline > b->deadline;
            return a->id > b->id;
        }
    };

    void run();

    //C++ 标准库的优先队列（本质是二叉堆），这里用来实现一个按截止时间排序的最小堆，作为定时任务调度器的核心数据结构
    /*
    JobPtr,                    // 1. 存储的元素类型
    std::vector<JobPtr>,       // 2. 底层容器
    Earlier                    // 3. 比较器（决定"优先级"）
    就是一颗完全二叉树，只是用vector存储的，所以也叫vector实现的优先队列
    */
    std::priority_queue<JobPtr, std::vector<JobPtr>, Earlier> heap_;
    // 用于快速取消任务，key是任务id，value是一个原子布尔指针，用于标志任务是否被取消
    std::unordered_map<uint64_t, std::shared_ptr<std::atomic<bool>>> cancel_index_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::thread> threads_;
    std::atomic<uint64_t> next_id_{1};
    std::atomic<bool> exiting_{false};
};

} // namespace kit
