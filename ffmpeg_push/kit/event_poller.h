#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <map>
#include <unordered_map>
#include <atomic>
#include <vector>
#include "semaphore.h"
#include "logger.h"

namespace kit {

// 获取当前毫秒时间戳
static inline uint64_t currentMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// 可取消的任务
class CancelableTask {
public:
    using Ptr = std::shared_ptr<CancelableTask>;
    using Func = std::function<void()>;

    explicit CancelableTask(Func f) : func_(std::move(f)) {}

    void operator()() {
        if (!cancelled_ && func_) func_();
    }

    void cancel() { cancelled_ = true; }
    bool isCancelled() const { return cancelled_; }

private:
    Func func_;
    std::atomic<bool> cancelled_{ false };
};

// 可取消的延迟任务（返回值是下次延迟ms，0表示不重复）
class DelayTask {
public:
    using Ptr = std::shared_ptr<DelayTask>;
    using Func = std::function<uint64_t()>;

    explicit DelayTask(Func f) : func_(std::move(f)) {}

    uint64_t operator()() {
        if (cancelled_ || !func_) return 0;
        return func_();
    }

    void cancel() { cancelled_ = true; }

private:
    Func func_;
    std::atomic<bool> cancelled_{ false };
};

// 事件类型
// 让每个枚举值只占一个独立的二进制位，这样就能用按位或 | 组合多个事件
enum PollEvent {
    Event_Read = 1 << 0,
    Event_Write = 1 << 1,
    Event_Error = 1 << 2,
};

using PollEventCB = std::function<void(int event)>;

class EventPoller : public std::enable_shared_from_this<EventPoller> {
public:
    using Ptr = std::shared_ptr<EventPoller>;

    // 添加一个静态工厂方法
    static std::shared_ptr<EventPoller> create(const std::string& name);
    ~EventPoller();
    // 禁止拷贝
    EventPoller(const EventPoller&) = delete;
    EventPoller& operator=(const EventPoller&) = delete;

private:
    EventPoller(const std::string& name = "poller");

public:

    void startThread();

    // 监听fd事件
    int addEvent(int fd, int event, PollEventCB cb);
    int delEvent(int fd);
    int modifyEvent(int fd, int event);

    // 在poller线程里执行任务
    CancelableTask::Ptr async(std::function<void()> task,
        bool may_sync = true);

    // 延迟执行（返回0不重复，返回N则N ms后再次执行）
    DelayTask::Ptr doDelayTask(uint64_t delay_ms,
        std::function<uint64_t()> task);

    // 判断当前线程是否是poller线程
    bool isCurrentThread() const;

    // 获取当前线程绑定的poller（thread_local）
    static Ptr getCurrentPoller();

    const std::string& name() const { return name_; }

private:
    void runLoop();
    void wakeup();          // 写pipe唤醒epoll
    void onPipeEvent();     // 处理pipe事件（执行任务队列）
    uint64_t getMinDelay(); // 最近定时器剩余时间（ms），0=无定时器
    uint64_t flushDelayTask(uint64_t now);

    void initPipe();

private:
    std::string  name_;
    std::thread  thread_;
    std::atomic<bool> exit_flag_{ false };
    semaphore    sem_started_;  // 等待线程启动

    // pipe：用于跨线程唤醒
    int pipe_fd_[2] = { -1, -1 };  // [0]=读端, [1]=写端

    // 任务队列（其他线程投递）
    std::mutex              task_mtx_;
    std::vector<CancelableTask::Ptr> task_list_;

    // 定时器（最小堆，用multimap模拟）
    std::multimap<uint64_t, DelayTask::Ptr> delay_tasks_;

#if defined(__linux__) || defined(__linux)
    int epoll_fd_ = -1;
    std::unordered_map<int, PollEventCB> event_map_;
#else
    // Windows/Mac 用 select 兜底
    struct PollRecord {
        int fd;
        int event;
        PollEventCB cb;
    };
    std::unordered_map<int, PollRecord> event_map_;
#endif
};

// EventPoller 线程池
class EventPollerPool {
public:
    static EventPollerPool& instance();

    // size=0 则使用CPU核心数
    explicit EventPollerPool(size_t size = 0);
    ~EventPollerPool() = default;

    // 获取负载最轻的poller
    EventPoller::Ptr getPoller();

    // 在所有poller上执行任务
    void foreach(std::function<void(EventPoller::Ptr)> func);

    size_t size() const { return pollers_.size(); }

private:
    std::vector<EventPoller::Ptr> pollers_;
    std::atomic<size_t> robin_{ 0 };  // 轮询分配
};
} // namespace kit
