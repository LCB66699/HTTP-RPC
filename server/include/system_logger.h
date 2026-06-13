// 结构化日志系统 — 支持级别开关、Redis 错误聚合、压测时可关闭
// --log-level: off | error | warn | info | debug  (默认 info)
// off 时全部静默，error 时只记录错误，压测用 off 避免日志 I/O 影响 QPS
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

enum class LogLevel : int { OFF = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4 };

inline LogLevel ParseLogLevel(const std::string &s) {
    if (s == "off" || s == "OFF")
        return LogLevel::OFF;
    if (s == "error" || s == "ERROR")
        return LogLevel::ERROR;
    if (s == "warn" || s == "WARN")
        return LogLevel::WARN;
    if (s == "debug" || s == "DEBUG")
        return LogLevel::DEBUG;
    return LogLevel::INFO;
}

class RedisClient;

class SystemLogger {
   public:
    SystemLogger(const std::string &service, LogLevel max_level = LogLevel::INFO)
        : service_(service), max_level_(max_level) {}

    void SetRedis(RedisClient *redis) { redis_ = redis; }
    void SetLevel(LogLevel lvl) { max_level_.store(lvl); }
    LogLevel GetLevel() const { return max_level_.load(); }

    void Debug(const std::string &msg, const char *file, int line) {
        if (max_level_ < LogLevel::DEBUG)
            return;
        Log("DEBUG", msg, file, line);
    }
    void Info(const std::string &msg, const char *file, int line) {
        if (max_level_ < LogLevel::INFO)
            return;
        Log("INFO", msg, file, line);
    }
    void Warn(const std::string &msg, const char *file, int line) {
        if (max_level_ < LogLevel::WARN)
            return;
        Log("WARN", msg, file, line);
    }
    void Error(const std::string &msg, const char *file, int line) {
        if (max_level_ < LogLevel::ERROR)
            return;
        Log("ERROR", msg, file, line);
        PushError("ERROR", msg);
    }
    void Fatal(const std::string &msg, const char *file, int line) {
        if (max_level_ < LogLevel::ERROR)
            return;
        Log("FATAL", msg, file, line);
        PushError("FATAL", msg);
    }

   private:
    void Log(const char *lvl, const std::string &msg, const char *file, int line) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        fprintf(stderr, "[%lld][%s][%s] %s (%s:%d)\n", (long long)now, lvl, service_.c_str(), msg.c_str(), file, line);
    }

    void PushError(const std::string &level, const std::string &msg) {
        if (!redis_ || !redis_->IsConnected())
            return;
        auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        std::string entry = "{\"ts\":" + std::to_string(now) + ",\"level\":\"" + level + "\",\"service\":\"" +
                            service_ + "\",\"msg\":\"" + msg + "\"}";
        redis_->PushCallEntry(entry);
        redis_->Increment("errors:" + service_ + ":total");
    }

    std::string service_;
    std::atomic<LogLevel> max_level_{LogLevel::INFO};
    RedisClient *redis_ = nullptr;
    std::mutex mtx_;
};

// 便捷宏 — 压测时设 --log-level off 全局静默，不影响 QPS
#define LOG_DEBUG(logger, msg) (logger).Debug(msg, __FILE__, __LINE__)
#define LOG_INFO(logger, msg) (logger).Info(msg, __FILE__, __LINE__)
#define LOG_WARN(logger, msg) (logger).Warn(msg, __FILE__, __LINE__)
#define LOG_ERROR(logger, msg) (logger).Error(msg, __FILE__, __LINE__)
#define LOG_FATAL(logger, msg) (logger).Fatal(msg, __FILE__, __LINE__)
