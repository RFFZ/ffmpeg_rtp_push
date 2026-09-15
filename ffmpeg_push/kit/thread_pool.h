#pragma once
#include <thread>
#include <mutex>
#include <queue>
#include <vector>
#include <functional>
#include <future>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <atomic>
#include "semaphore.h"  // ��������֮ǰд��

// �������ͣ�֧��ȡ��

namespace kit {
class Task {
public:
    using Ptr = std::shared_ptr<Task>;
    using Func = std::function<void()>;

    explicit Task(Func f) : func_(std::move(f)) {}

    void operator()() {
        if (!cancelled_) {
            func_();
        }
    }

    // ȡ�����������ûִ�еĻ���
    void cancel() { cancelled_ = true; }
    bool isCancelled() const { return cancelled_; }

private:
    Func func_;
    std::atomic<bool> cancelled_{ false };
};

// ������У�֧����ͨͶ�ݺͲ��
class TaskQueue {
public:
    void pushTask(Task::Ptr task) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push_back(std::move(task));
        }
        sem_.post();
    }

    void pushTaskFirst(Task::Ptr task) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push_front(std::move(task));  // �嵽��ǰ��
        }
        sem_.post();
    }

    // �����ȴ����񣬷���false��ʾ�˳�
    bool getTask(Task::Ptr& task) {
        sem_.wait();
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty()) {
            return false;  // �յ��˳��ź�
        }
        task = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    // ����N���˳��źţ���N���߳��˳�
    void pushExit(size_t n) {
        for (size_t i = 0; i < n; i++) {
            sem_.post();  // ����queue_��������߳��õ���������˳�
        }
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

private:
    std::mutex          mtx_;
    std::deque<Task::Ptr> queue_;  // ��deque֧��ͷ������
    semaphore           sem_;
};

/*    ʹ������
// ����4�̵߳��̳߳�
    ThreadPool pool(4, ThreadPool::PRIORITY_NORMAL, "worker");

    // 1. �첽Ͷ�ݣ������Ľ��
    pool.async([]() {
        printf("�������̳߳���ִ��\n");
        });

    // 2. Ͷ�ݽ������񣬲��
    pool.asyncFirst([]() {
        printf("����ִ��\n");
        });

    // 3. ͬ���ȴ����
    int result = pool.sync([]() {
        return 42;
        });
    printf("���: %d\n", result);

    // 4. ��ȡ��������
    auto task = pool.async([]() {
        printf("���������ܲ���ִ��\n");
        });
    task->cancel();  // �����ûִ�о�ȡ��
    */
// �̳߳�
class ThreadPool {
public:
    enum Priority {
        PRIORITY_LOW = 0,
        PRIORITY_NORMAL = 1,
        PRIORITY_HIGH = 2
    };

    // num:      �߳�������Ĭ��CPU������
    // priority: �߳����ȼ�
    // name:     �߳���������ʱ����
    explicit ThreadPool(
        int num = (int)std::thread::hardware_concurrency(),
        Priority priority = PRIORITY_NORMAL,
        const std::string& name = "thread_pool")
        : thread_num_(num)
        , priority_(priority)
        , name_(name)
    {
        start();
    }

    ~ThreadPool() {
        shutdown();
        wait();
    }

    // ��ֹ����
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Ͷ�������첽ִ�У�
    Task::Ptr async(std::function<void()> task) {
        // may_sync�Ż���������÷������̳߳��ֱ��ִ��
        if (isCurrentThreadIn()) {
            task();
            return nullptr;
        }
        auto t = std::make_shared<Task>(std::move(task));
        queue_.pushTask(t);
        return t;
    }

    // Ͷ�ݽ������񣨲嵽������ǰ�棩
    Task::Ptr asyncFirst(std::function<void()> task) {
        if (isCurrentThreadIn()) {
            task();
            return nullptr;
        }
        auto t = std::make_shared<Task>(std::move(task));
        queue_.pushTaskFirst(t);
        return t;
    }

    // Ͷ�����񲢵ȴ������ͬ����
    // �÷���auto result = pool.sync([](){ return 42; });
    template<typename F, typename R = std::invoke_result_t<F>>
    R sync(F&& task) {
        if (isCurrentThreadIn()) {
            return task();
        }
        std::promise<R> promise;
        auto future = promise.get_future();
        async([&promise, task = std::forward<F>(task)]() mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    task();
                    promise.set_value();
                }
                else {
                    promise.set_value(task());
                }
            }
            catch (...) {
                promise.set_exception(std::current_exception());
            }
        });
        return future.get();
    }

    size_t taskCount() { return queue_.size(); }
    size_t threadCount() const { return thread_num_; }

    // �жϵ�ǰ�߳��Ƿ����̳߳���
    bool isCurrentThreadIn() const {
        auto id = std::this_thread::get_id();
        std::lock_guard<std::mutex> lock(threads_mtx_);
        for (auto& t : threads_) {
            if (t.get_id() == id) return true;
        }
        return false;
    }

private:
    void start() {
        std::lock_guard<std::mutex> lock(threads_mtx_);
        for (int i = 0; i < thread_num_; i++) {
            threads_.emplace_back([this, i]() {
                // �����߳���������ʱ��IDE/GDB�￴����
                setThreadName(name_ + "_" + std::to_string(i));
                // �������ȼ�
                setThreadPriority(priority_);
                // ������ѭ��
                run();
                });
        }
    }

    void run() {
        Task::Ptr task;
        while (true) {
            if (!queue_.getTask(task)) {
                break;  // �յ��˳��ź�
            }
            try {
                (*task)();
                task = nullptr;
            }
            catch (std::exception& ex) {
                // �����쳣��Ӱ���̼߳�������
                fprintf(stderr, "[ThreadPool] exception: %s\n", ex.what());
            }
            catch (...) {
                fprintf(stderr, "[ThreadPool] unknown exception\n");
            }
        }
    }

    void shutdown() {
        queue_.pushExit(thread_num_);
    }

    void wait() {
        // Join outside the lock: tasks are still drained while join waits,
        // and a drained task calling async()/sync() would block forever in
        // isCurrentThreadIn() if this held threads_mtx_ across the join.
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(threads_mtx_);
            threads.swap(threads_);
        }
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    static void setThreadName(const std::string& name) {
#ifdef _WIN32
        // Windowsͨ���쳣��ʽ�����߳�����VS������ʶ��
        const DWORD MS_VC_EXCEPTION = 0x406D1388;
#pragma pack(push, 8)
        struct THREADNAME_INFO {
            DWORD  dwType;
            LPCSTR szName;
            DWORD  dwThreadID;
            DWORD  dwFlags;
        };
#pragma pack(pop)
        THREADNAME_INFO info{ 0x1000, name.c_str(), (DWORD)-1, 0 };
        __try {
            RaiseException(MS_VC_EXCEPTION, 0,
                sizeof(info) / sizeof(ULONG_PTR),
                (ULONG_PTR*)&info);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#elif defined(__APPLE__)
        pthread_setname_np(name.substr(0, 15).c_str());
#endif
    }

    static void setThreadPriority(Priority priority) {
#ifdef _WIN32
        static int priorities[] = {
            THREAD_PRIORITY_BELOW_NORMAL,
            THREAD_PRIORITY_NORMAL,
            THREAD_PRIORITY_ABOVE_NORMAL
        };
        SetThreadPriority(GetCurrentThread(), priorities[priority]);
#else
        // Linux����ͨ�߳����ȼ�����
        int policy;
        struct sched_param param;
        pthread_getschedparam(pthread_self(), &policy, &param);
        if (priority == PRIORITY_HIGH) {
            param.sched_priority = 1;
            pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
        }
#endif
    }

private:
    int                      thread_num_;
    Priority                 priority_;
    std::string              name_;
    TaskQueue                queue_;
    mutable std::mutex       threads_mtx_;
    std::vector<std::thread> threads_;
};
} // namespace kit
