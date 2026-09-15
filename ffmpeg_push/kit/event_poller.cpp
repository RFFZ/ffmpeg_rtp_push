// FD_SETSIZE must be defined before winsock2.h is included, otherwise
// Windows select() only watches 64 sockets (one poller serves ~125).
#ifdef _WIN32
#define FD_SETSIZE 2048
#endif

#include "event_poller.h"
#include <stdexcept>
#include <algorithm>

namespace kit {

#if defined(__linux__) || defined(__linux)
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#define HAS_EPOLL
#elif defined(_WIN32)
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <unistd.h>
#endif

// thread_local��ÿ���̶߳���һ��
static thread_local std::weak_ptr<EventPoller> tl_current_poller;

EventPoller::Ptr EventPoller::getCurrentPoller() {
    return tl_current_poller.lock();
}

void EventPoller::startThread() {
    thread_ = std::thread(&EventPoller::runLoop, this);
    sem_started_.wait();
}


// ��̬���������Ķ���
std::shared_ptr<EventPoller> EventPoller::create(const std::string& name) {
    auto ptr = std::shared_ptr<EventPoller>(new EventPoller(name));
    ptr->startThread();  // ������ɺ������߳�
    return ptr;
}



EventPoller::EventPoller(const std::string& name) : name_(name) {
    initPipe();
}

EventPoller::~EventPoller() {
    exit_flag_ = true;
    wakeup();
    if (thread_.joinable()) thread_.join();

#ifdef HAS_EPOLL
    if (epoll_fd_ != -1) { close(epoll_fd_); epoll_fd_ = -1; }
#endif
    if (pipe_fd_[0] != -1) {
#ifdef _WIN32
        closesocket(pipe_fd_[0]);
        closesocket(pipe_fd_[1]);
#else
        close(pipe_fd_[0]);
        close(pipe_fd_[1]);
#endif
    }
}

void EventPoller::initPipe() {
#ifdef _WIN32
    // Windows��socket pairģ��pipe
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(listener, (sockaddr*)&addr, sizeof(addr));
    listen(listener, 1);

    int addrlen = sizeof(addr);
    getsockname(listener, (sockaddr*)&addr, &addrlen);

    pipe_fd_[1] = (int)socket(AF_INET, SOCK_STREAM, 0);
    connect((SOCKET)pipe_fd_[1], (sockaddr*)&addr, sizeof(addr));
    pipe_fd_[0] = (int)accept(listener, nullptr, nullptr);
    closesocket(listener);

    // ���÷�����
    u_long mode = 1;
    ioctlsocket((SOCKET)pipe_fd_[0], FIONBIO, &mode);
    ioctlsocket((SOCKET)pipe_fd_[1], FIONBIO, &mode);
#else
    if (pipe(pipe_fd_) != 0) {
        throw std::runtime_error("pipe() failed");
    }
    // ���÷�����
    fcntl(pipe_fd_[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_fd_[1], F_SETFL, O_NONBLOCK);
#endif

#ifdef HAS_EPOLL
    epoll_fd_ = epoll_create(1024);
    if (epoll_fd_ == -1) {
        throw std::runtime_error("epoll_create failed");
    }
    // ��pipe���˼���epoll
    struct epoll_event ev {};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = pipe_fd_[0];
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, pipe_fd_[0], &ev);
#endif
}

void EventPoller::wakeup() {
    char buf = 0;
#ifdef _WIN32
    send((SOCKET)pipe_fd_[1], &buf, 1, 0);
#else
    write(pipe_fd_[1], &buf, 1);
#endif
}

void EventPoller::onPipeEvent() {
    // �ſ�pipe
    char buf[256];
    while (true) {
#ifdef _WIN32
        int n = recv((SOCKET)pipe_fd_[0], buf, sizeof(buf), 0);
#else
        int n = read(pipe_fd_[0], buf, sizeof(buf));
#endif
        if (n <= 0) break;
    }

    // ȡ�����д�ִ������
    std::vector<CancelableTask::Ptr> tasks;
    {
        std::lock_guard<std::mutex> lock(task_mtx_);
        tasks.swap(task_list_);
    }

    for (auto& t : tasks) {
        try { (*t)(); }
        catch (std::exception& e) {
            ErrorL << "[" << name_ << "] async task exception: " << e.what();
        }
    }
}

uint64_t EventPoller::flushDelayTask(uint64_t now) {
    // ִ�������ѵ��ڵĶ�ʱ��
    auto it = delay_tasks_.begin();
    while (it != delay_tasks_.end() && it->first <= now) {
        auto task = it->second;
        it = delay_tasks_.erase(it);
        try {
            uint64_t next = (*task)();
            if (next > 0) {
                // �ظ��������¼���
                delay_tasks_.emplace(now + next, std::move(task));
            }
        }
        catch (std::exception& e) {
            ErrorL << "[" << name_ << "] delay task exception: " << e.what();
        }
    }

    // ������һ����ʱ����ʣ��ʱ��
    if (delay_tasks_.empty()) return 0;
    return delay_tasks_.begin()->first - now;
}

uint64_t EventPoller::getMinDelay() {
    if (delay_tasks_.empty()) return 0;
    auto now = currentMs();
    auto first = delay_tasks_.begin()->first;
    if (first <= now) {
        return flushDelayTask(now);
    }
    return first - now;
}

void EventPoller::runLoop() {
    // �󶨵�ǰ�߳�
    tl_current_poller = shared_from_this();
    sem_started_.post();

    InfoL << "[" << name_ << "] started";

#ifdef HAS_EPOLL
    struct epoll_event events[1024];
    while (!exit_flag_) {
        uint64_t timeout = getMinDelay();  // ms��0=�޶�ʱ�����õȴ�

        int ret = epoll_wait(epoll_fd_, events, 1024,
            timeout == 0 ? -1 : (int)timeout);
        if (ret < 0) continue;

        for (int i = 0; i < ret; i++) {
            int fd = events[i].data.fd;

            if (fd == pipe_fd_[0]) {
                onPipeEvent();
                continue;
            }

            auto it = event_map_.find(fd);
            if (it == event_map_.end()) continue;

            // Copy the callback before calling it: the callback may delEvent
            // (erase from event_map_), invalidating the iterator.
            auto cb = it->second;

            int ev = 0;
            if (events[i].events & (EPOLLIN | EPOLLHUP))  ev |= Event_Read;
            if (events[i].events & EPOLLOUT)               ev |= Event_Write;
            if (events[i].events & EPOLLERR)               ev |= Event_Error;

            try { cb(ev); }
            catch (std::exception& e) {
                ErrorL << "[" << name_ << "] event cb exception: " << e.what();
            }
        }
    }
#else
    // Windows/Mac select �汾
    while (!exit_flag_) {
        uint64_t timeout_ms = getMinDelay();

        fd_set read_set, write_set, err_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_ZERO(&err_set);

        int max_fd = pipe_fd_[0];
        FD_SET((SOCKET)pipe_fd_[0], &read_set);

        for (auto& pr : event_map_) {
            if (pr.second.event & Event_Read)
                FD_SET((SOCKET)pr.first, &read_set);
            if (pr.second.event & Event_Write)
                FD_SET((SOCKET)pr.first, &write_set);
            if (pr.second.event & Event_Error)
                FD_SET((SOCKET)pr.first, &err_set);
            if (pr.first > max_fd) max_fd = pr.first;
        }

        struct timeval tv {};
        tv.tv_sec = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)(timeout_ms % 1000 * 1000);

        int ret = select(max_fd + 1, &read_set, &write_set, &err_set,
            timeout_ms == 0 ? nullptr : &tv);
        if (ret <= 0) continue;

        // pipe�¼�
        if (FD_ISSET((SOCKET)pipe_fd_[0], &read_set)) {
            onPipeEvent();
        }

        // Collect ready fds first: the callbacks may add/del/modify events
        // (delEvent erases from event_map_), so iterating the map while
        // calling them would invalidate iterators.
        std::vector<std::pair<int, PollEventCB>> ready;
        for (auto& pr : event_map_) {
            int ev = 0;
            if (FD_ISSET((SOCKET)pr.first, &read_set))  ev |= Event_Read;
            if (FD_ISSET((SOCKET)pr.first, &write_set)) ev |= Event_Write;
            if (FD_ISSET((SOCKET)pr.first, &err_set))   ev |= Event_Error;
            if (ev == 0) continue;
            ready.emplace_back(pr.first, pr.second.cb);  // copy the callback
        }
        for (auto& item : ready) {
            try { item.second(item.first); }
            catch (std::exception& e) {
                ErrorL << "[" << name_ << "] event cb exception: " << e.what();
            }
        }
    }
#endif

    InfoL << "[" << name_ << "] stopped";
}

int EventPoller::addEvent(int fd, int event, PollEventCB cb) {
    if (isCurrentThread()) {
#ifdef HAS_EPOLL
        struct epoll_event ev {};
        ev.data.fd = fd;
        ev.events = 0;
        if (event & Event_Read)  ev.events |= EPOLLIN;
        if (event & Event_Write) ev.events |= EPOLLOUT;
        if (event & Event_Error) ev.events |= EPOLLERR;
        ev.events |= EPOLLET;  // ��Ե����

        int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        if (ret == 0) event_map_[fd] = std::move(cb);
        return ret;
#else
        event_map_[fd] = { fd, event, std::move(cb) };
        return 0;
#endif
    }
    // ���̣߳�Ͷ�ݵ�poller�߳�ִ��
    async([this, fd, event, cb = std::move(cb)]() mutable {
        addEvent(fd, event, std::move(cb));
    }, false);
    return 0;
}

int EventPoller::delEvent(int fd) {
    if (isCurrentThread()) {
#ifdef HAS_EPOLL
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
        event_map_.erase(fd);
        return 0;
    }
    async([this, fd]() { delEvent(fd); }, false);
    return 0;
}

int EventPoller::modifyEvent(int fd, int event) {
    if (isCurrentThread()) {
#ifdef HAS_EPOLL
        struct epoll_event ev {};
        ev.data.fd = fd;
        ev.events = 0;
        if (event & Event_Read)  ev.events |= EPOLLIN;
        if (event & Event_Write) ev.events |= EPOLLOUT;
        if (event & Event_Error) ev.events |= EPOLLERR;
        ev.events |= EPOLLET;
        return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
#else
        auto it = event_map_.find(fd);
        if (it != event_map_.end()) it->second.event = event;
        return 0;
#endif
    }
    async([this, fd, event]() { modifyEvent(fd, event); }, false);
    return 0;
}

CancelableTask::Ptr EventPoller::async(std::function<void()> task,
    bool may_sync) {
    if (may_sync && isCurrentThread()) {
        task();
        return nullptr;
    }
    auto t = std::make_shared<CancelableTask>(std::move(task));
    {
        std::lock_guard<std::mutex> lock(task_mtx_);
        task_list_.push_back(t);
    }
    wakeup();
    return t;
}

DelayTask::Ptr EventPoller::doDelayTask(uint64_t delay_ms,
    std::function<uint64_t()> task) {
    auto dt = std::make_shared<DelayTask>(std::move(task));
    auto deadline = currentMs() + delay_ms;

    // ������poller�߳������ delay_tasks_
    async([this, deadline, dt]() {
        delay_tasks_.emplace(deadline, dt);
        }, false);

    return dt;
}

bool EventPoller::isCurrentThread() const {
    return std::this_thread::get_id() == thread_.get_id();
}

// ���� EventPollerPool ����������������������������������������������������������������������������

EventPollerPool& EventPollerPool::instance() {
    static EventPollerPool inst;
    return inst;
}

EventPollerPool::EventPollerPool(size_t size) {
    if (size == 0) {
        size = std::thread::hardware_concurrency();
    }
    pollers_.reserve(size);
    for (size_t i = 0; i < size; i++) {
        pollers_.push_back(
            EventPoller::create("poller_" + std::to_string(i)));
    }
    InfoL << "EventPollerPool created, size=" << size;
}

EventPoller::Ptr EventPollerPool::getPoller() {
    // ��ѯ���䣬���ؾ���
    size_t idx = robin_.fetch_add(1) % pollers_.size();
    return pollers_[idx];
}

void EventPollerPool::foreach(std::function<void(EventPoller::Ptr)> func) {
    for (auto& p : pollers_) func(p);
}
} // namespace kit
