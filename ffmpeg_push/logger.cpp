#include "logger.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <sstream>

// FileChannel
#ifdef _WIN32
#include <direct.h>    // _mkdir
#include <io.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

#ifdef _WIN32
#include <winsock2.h>
// Windows没有gettimeofday，自己实现
static int gettimeofday(struct timeval* tp, void*) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    tp->tv_sec = (long)std::chrono::duration_cast<std::chrono::seconds>(now).count();
    tp->tv_usec = (long)(std::chrono::duration_cast<std::chrono::microseconds>(now).count() % 1000000);
    return 0;
}
#else
#include <sys/time.h>
#endif

// ─── 工具函数 ────────────────────────────────────────────

static std::string shortFile(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

static const char* levelStr(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "T";
    case LogLevel::Debug: return "D";
    case LogLevel::Info:  return "I";
    case LogLevel::Warn:  return "W";
    case LogLevel::Error: return "E";
    default:              return "?";
    }
}

static std::string getThreadName() {
    std::ostringstream ss;
    ss << std::this_thread::get_id();
    return ss.str();
}




// ─── LogContext ───────────────────────────────────────────

LogContext::LogContext(LogLevel lv, const char* f, const char* fn, int l)
    : level(lv)
    , file(shortFile(f))
    , func(fn)
    , line(l)
    , thread_name(getThreadName())
{
    gettimeofday(&tv, nullptr);
}

const std::string& LogContext::content() {
    if (!got_content_) {
        content_ = str();
        got_content_ = true;
    }
    return content_;
}

// ─── LogChannel ──────────────────────────────────────────

void LogChannel::format(std::ostream& out, const LogContext::Ptr& ctx,
    bool enable_color, bool enable_detail) {
    // 格式化时间
    time_t sec = ctx->tv.tv_sec;
    struct tm tm_info;
#ifdef _WIN32
    localtime_s(&tm_info, &sec);
#else
    localtime_r(&sec, &tm_info);
#endif
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    out << "[" << time_buf
        << "." << std::setfill('0') << std::setw(3) << (ctx->tv.tv_usec / 1000)
        << "][" << levelStr(ctx->level) << "]";

    if (enable_detail) {
        out << "[" << ctx->thread_name << "]"
            << "[" << ctx->file << ":" << ctx->line << "]"
            << "[" << ctx->func << "]";
    }

    out << " " << ctx->content();

    // 重复日志合并显示
    if (ctx->repeat > 0) {
        out << " (repeat x" << ctx->repeat + 1 << ")";
    }
    out << "\n";
}

// ConsoleChannel
void ConsoleChannel::write(const LogContext::Ptr& ctx) {
    if (ctx->level < level_) return;

#ifndef _WIN32
    // Linux终端颜色
    const char* color = "";
    const char* reset = "\033[0m";
    switch (ctx->level) {
    case LogLevel::Trace: color = "\033[37m"; break;  // 白
    case LogLevel::Debug: color = "\033[36m"; break;  // 青
    case LogLevel::Info:  color = "\033[32m"; break;  // 绿
    case LogLevel::Warn:  color = "\033[33m"; break;  // 黄
    case LogLevel::Error: color = "\033[31m"; break;  // 红
    default: break;
    }
    std::cout << color;
    format(std::cout, ctx, true, true);
    std::cout << reset;
#else
    format(std::cout, ctx, false, true);
#endif
}



// 生成当前时间的文件名：20260515_143022_001.log
static std::string makeLogFileName(const std::string& dir, int index,const std::string& fix_) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info;
#ifdef _WIN32
    localtime_s(&tm_info, &t);
#else
    localtime_r(&t, &tm_info);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm_info);

    // 拼完整路径
    std::string path = dir + "/" + fix_+"_" + buf;
    if (index > 0) {
        path += "_" + std::to_string(index);
    }
    path += ".log";
    return path;
}

FileChannel::FileChannel(const std::string& dir,
    size_t max_size_mb, const std::string log_fix,
    LogLevel level)
    : LogChannel("file", level),
    dir_(dir),
    fix_(log_fix),
    max_size_(max_size_mb * 1024 * 1024)
{
    createDir();
    openNewFile();
}

void FileChannel::createDir() {
    // 逐级创建目录
    std::string path;
    for (char c : dir_) {
        path += c;
        if (c == '/' || c == '\\') {
            MKDIR(path.c_str());
        }
    }
    MKDIR(dir_.c_str());
}

void FileChannel::openNewFile() {
    // 同一秒内多次切换，加序号避免重名
    static int index = 0;
    std::string path = makeLogFileName(dir_, index++,fix_);

    if (file_.is_open()) {
        file_.close();
    }
    file_.open(path, std::ios::app);
    cur_size_ = 0;

    if (file_.is_open()) {
        // 在新文件开头写一行标记
        file_ << "=== Log started: " << path << " ===\n";
        file_.flush();
    }
}

void FileChannel::checkAndRotate() {
    if (cur_size_ >= max_size_) {
        openNewFile();
    }
}

void FileChannel::write(const LogContext::Ptr& ctx) {
    if (ctx->level < level_) return;
    std::lock_guard<std::mutex> lock(mtx_);

    checkAndRotate();

    if (file_.is_open()) {
        // 记录写前位置来计算写入大小
        auto before = file_.tellp();
        format(file_, ctx, false, true);
        file_.flush();
        auto after = file_.tellp();
        if (before >= 0 && after >= 0) {
            cur_size_ += (size_t)(after - before);
        }
    }
}

// ─── LogWriter ───────────────────────────────────────────

// 同步写入
void SyncLogWriter::write(const LogContext::Ptr& ctx, Logger& logger) {
    logger.writeChannels(ctx);
}

// 异步写入
AsyncLogWriter::AsyncLogWriter() {
    thread_ = std::thread(&AsyncLogWriter::threadLoop, this);
}

AsyncLogWriter::~AsyncLogWriter() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        running_ = false;
    }
    sem_.post();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void AsyncLogWriter::write(const LogContext::Ptr& ctx, Logger& logger) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push({ ctx, &logger });
    }
    sem_.post();
}

void AsyncLogWriter::threadLoop() {
    while (running_) {
        sem_.wait();
       
        std::queue<std::pair<LogContext::Ptr, Logger*>> local;
        {
            std::lock_guard<std::mutex> lock(mtx_);  // 加锁再swap
            std::swap(local, queue_);
        }


        while (!local.empty()) {
            auto& item = local.front();
            item.second->writeChannels(item.first);
            local.pop();
        }

        if (!running_) break;
    }
}

// ─── Logger ──────────────────────────────────────────────

static Logger* g_defaultLogger = nullptr;

Logger& getLogger() {
    if (g_defaultLogger) return *g_defaultLogger;
    return Logger::instance();
}

void setLogger(Logger* logger) {
    g_defaultLogger = logger;
}

Logger& Logger::instance() {
    static Logger inst("default");
    return inst;
}

Logger::Logger(const std::string& name) : name_(name) {
    // 默认异步写入
    writer_ = std::make_shared<AsyncLogWriter>();
    // 默认加控制台输出
    addChannel(std::make_shared<ConsoleChannel>());
}

void Logger::addChannel(const LogChannel::Ptr& channel) {
    std::lock_guard<std::mutex> lock(mtx_);
    channels_[channel->name()] = channel;
}

void Logger::delChannel(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);
    channels_.erase(name);
}

void Logger::setWriter(const LogWriter::Ptr& writer) {
    writer_ = writer;
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& pr : channels_) {
        pr.second->setLevel(level);
    }
}

void Logger::write(const LogContext::Ptr& ctx) {
    if (writer_) {
        writer_->write(ctx, *this);
    }
    else {
        writeChannels(ctx);
    }
}

void Logger::writeChannels(const LogContext::Ptr& ctx) {
    std::lock_guard<std::mutex> lock(mtx_);
    // repeat检测：和上一条日志内容相同则合并
    // （这里简化处理，实际ZLMediaKit有更完整的实现）
    for (auto& pr : channels_) {
        pr.second->write(ctx);
    }
}