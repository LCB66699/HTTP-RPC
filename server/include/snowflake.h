// Snowflake 分布式唯一 ID 生成器
// 结构: [1位符号][41位毫秒时间戳][5位worker][12位序列号]
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

class Snowflake {
public:
  Snowflake(int worker_id = 0) : worker_id_(worker_id & 0x1F) {
    if (worker_id == 0) {
      const char *env = std::getenv("SNOWFLAKE_WORKER_ID");
      if (env)
        worker_id_ = std::atoi(env) & 0x1F;
    }
  }

  int64_t Next() {
    std::lock_guard<std::mutex> lock(mtx_);
    int64_t now = NowMs();

    if (now == last_ms_) {
      sequence_ = (sequence_ + 1) & 0xFFF;
      if (sequence_ == 0) {
        // 当前毫秒序列号用完，等下一毫秒
        while (now <= last_ms_)
          now = NowMs();
      }
    } else {
      sequence_ = 0;
    }
    last_ms_ = now;

    return ((now - kEpoch) << 22) | ((int64_t)worker_id_ << 12) | sequence_;
  }

  int WorkerId() const { return worker_id_; }

private:
  static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  static constexpr int64_t kEpoch = 1767225600000LL; // 2026-01-01 00:00:00 UTC
  int worker_id_;
  std::mutex mtx_;
  int64_t last_ms_ = 0;
  int sequence_ = 0;
};
