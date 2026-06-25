#pragma once
#include <sstream>
#include <string>
#include <memory>
#include <fstream>
#include <thread>
#include <queue>
#include <functional>
#include <vector>
#include <map>

#include "semaphore.h"

// 跨平台处理 timeval
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/time.h>
#endif

// ─── 日志级别 ────────────────────────────────────────────
enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error
};

// ─── LogContext ───────────────────────────────────────────
// 学ZLMediaKit：直接继承ostringstream，本身就是流
// 一条日志的完整信息载体
class LogContext : public std::ostringstream {
public:
    using Ptr = std::shared_ptr<LogContext>;

    LogContext(LogLevel level, const char* file, const char* func, int line);

    LogLevel    level;
    std::string file;       // 短文件名
    std::string func;       // 函数名
    int         line;
    std::string thread_name; // 打日志的线程名
    int         repeat = 0;  // 重复次数，相同日志合并
    struct timeval tv;       // 精确到微秒

    // 获取格式化后的内容字符串
    const std::string& content();

private:
    bool        got_content_ = false;
    std::string content_;
};

// ─── LogChannel ──────────────────────────────────────────
// 输出目标基类
class LogChannel {
public:
    using Ptr = std::shared_ptr<LogChannel>;

    explicit LogChannel(const std::string& name, LogLevel level = LogLevel::Trace)
        : name_(name), level_(level) {}
    virtual ~LogChannel() = default;

    virtual void write(const LogContext::Ptr& ctx) = 0;
    const std::string& name() const { return name_; }
    void setLevel(LogLevel level) { level_ = level; }
    LogLevel getLevel() const { return level_; }

protected:
    // 格式化输出到流，子类复用
    void format(std::ostream& out, const LogContext::Ptr& ctx,
        bool enable_color = false, bool enable_detail = true);

    std::string name_;
    LogLevel    level_;
};

// 控制台输出
class ConsoleChannel : public LogChannel {
public:
    ConsoleChannel(LogLevel level = LogLevel::Trace)
        : LogChannel("console", level) {}
    void write(const LogContext::Ptr& ctx) override;
};

// 文件输出
// 高级文件输出：支持目录、按时间命名、按大小切片
class FileChannel : public LogChannel {
public:
    // dir: 日志目录
    // max_size_mb: 单个文件最大MB，超过则新建
    FileChannel(const std::string& dir = "./log",
        size_t max_size_mb = 64,const std::string log_fix = "log",
        LogLevel level = LogLevel::Trace);

    void write(const LogContext::Ptr& ctx) override;

    void setMaxSizeMb(size_t mb) { max_size_ = mb * 1024 * 1024; }

private:
    void checkAndRotate();   // 检查是否需要切换文件
    void openNewFile();      // 创建新文件
    void createDir();        // 创建目录

    std::string  dir_;
    std::string fix_;
    size_t       max_size_;  // 字节
    size_t       cur_size_ = 0;
    std::ofstream file_;
    std::mutex   mtx_;
};

// ─── LogWriter ───────────────────────────────────────────
// 学ZLMediaKit：写入策略抽象层
// 解决"什么时候写"，和LogChannel解决"写到哪"分离
class Logger;
class LogWriter {
public:
    using Ptr = std::shared_ptr<LogWriter>;
    virtual ~LogWriter() = default;
    virtual void write(const LogContext::Ptr& ctx, Logger& logger) = 0;
};

// 同步写入（直接写，会阻塞调用方）
class SyncLogWriter : public LogWriter {
public:
    void write(const LogContext::Ptr& ctx, Logger& logger) override;
};

// 异步写入（投入队列，不阻塞调用方）
class AsyncLogWriter : public LogWriter {
public:
    AsyncLogWriter();
    ~AsyncLogWriter();
    void write(const LogContext::Ptr& ctx, Logger& logger) override;

private:
    void threadLoop();

    bool running_ = true;
    std::mutex mtx_;
    semaphore sem_;
    // pair: <日志条目, 目标logger>
    std::queue<std::pair<LogContext::Ptr, Logger*>> queue_;
    std::thread thread_;
};

// ─── Logger ──────────────────────────────────────────────
class Logger {
public:
    using Ptr = std::shared_ptr<Logger>;

    static Logger& instance();

    explicit Logger(const std::string& name = "default");
    ~Logger() = default;

    // 添加/删除输出目标
    void addChannel(const LogChannel::Ptr& channel);
    void delChannel(const std::string& name);

    // 设置写入策略（默认异步）
    void setWriter(const LogWriter::Ptr& writer);

    // 设置所有channel的级别
    void setLevel(LogLevel level);

    // 写入一条日志，由LogContextCapture析构时调用
    void write(const LogContext::Ptr& ctx);

    // 直接写到所有channel，供LogWriter调用
    void writeChannels(const LogContext::Ptr& ctx);

    const std::string& name() const { return name_; }

private:
    std::string name_;
    LogWriter::Ptr writer_;
    std::mutex mtx_;
    std::map<std::string, LogChannel::Ptr> channels_;
};

// ─── LogContextCapture ───────────────────────────────────
// 学ZLMediaKit：临时对象，收集流式输入，析构时提交
// InfoL << "hello" << 123  就是构造这个临时对象然后析构
class LogContextCapture {
public:
    LogContextCapture(Logger& logger, LogLevel level,
        const char* file, const char* func, int line)
        : logger_(logger)
        , ctx_(std::make_shared<LogContext>(level, file, func, line))
    {}

    // 拷贝构造：宏展开时可能需要
    LogContextCapture(const LogContextCapture& that)
        : logger_(that.logger_), ctx_(that.ctx_) {}

    ~LogContextCapture() {
        if (ctx_) {
            logger_.write(ctx_);
        }
    }

    template<typename T>
    LogContextCapture& operator<<(T&& val) {
        if (ctx_) {
            (*ctx_) << std::forward<T>(val);
        }
        return *this;
    }

    // 支持 << std::endl 立即输出
    LogContextCapture& operator<<(std::ostream& (*f)(std::ostream&)) {
        if (ctx_) {
            logger_.write(ctx_);
            ctx_ = nullptr;  // 提交后清空，避免析构时重复提交
        }
        return *this;
    }

private:
    Logger& logger_;
    LogContext::Ptr ctx_;
};

// 获取当前exe所在目录
static inline std::string exeDir() {
#ifdef _WIN32
    char buf[1024] = {};
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
    std::string path(buf);
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? "./" : path.substr(0, pos + 1);
#else
    char buf[1024] = {};
    readlink("/proc/self/exe", buf, sizeof(buf));
    std::string path(buf);
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? "./" : path.substr(0, pos + 1);
#endif
}

// ─── 全局Logger访问 ──────────────────────────────────────
Logger& getLogger();
void setLogger(Logger* logger);

// ─── 宏 ──────────────────────────────────────────────────
#define WriteL(level) LogContextCapture(getLogger(), level, __FILE__, __FUNCTION__, __LINE__)
#define TraceL WriteL(LogLevel::Trace)
#define DebugL WriteL(LogLevel::Debug)
#define InfoL  WriteL(LogLevel::Info)
#define WarnL  WriteL(LogLevel::Warn)
#define ErrorL WriteL(LogLevel::Error)