#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

// GrpcCircuitBreaker — 保护 gRPC 服务间调用的熔断器
//
// 状态机:
//   CLOSED  → 连续 max_failures 次失败 → OPEN
//   OPEN    → open_timeout 到期          → HALF_OPEN
//   HALF_OPEN → 探测成功                → CLOSED
//   HALF_OPEN → 探测失败                → OPEN
//
// 线程安全：内部持锁，适合 ValidateCaller 等中低频调用路径。
class GrpcCircuitBreaker {
   public:
    explicit GrpcCircuitBreaker(int max_failures = 5, int open_timeout_sec = 30)
        : max_failures_(max_failures), open_timeout_(std::chrono::seconds(open_timeout_sec)) {}

    // 执行受熔断保护的调用 fn。
    // fn 返回 true = 成功，false = 失败。
    // Call 返回 true 当且仅当电路允许且 fn 成功。
    bool Call(const std::string &label, std::function<bool()> fn) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == OPEN) {
            auto elapsed = std::chrono::steady_clock::now() - open_since_;
            if (elapsed < open_timeout_) {
                return false;  // 电路断开，快速失败
            }
            // 超时到期 → HALF_OPEN，允许一次探测
            state_ = HALF_OPEN;
            half_open_used_ = false;
        }

        if (state_ == HALF_OPEN) {
            if (half_open_used_) {
                return false;  // 已有探测在途
            }
            half_open_used_ = true;
        }

        bool ok = fn();

        if (ok) {
            consecutive_failures_ = 0;
            if (state_ == HALF_OPEN) {
                state_ = CLOSED;
            }
            return true;
        }

        ++consecutive_failures_;
        if (state_ == HALF_OPEN) {
            state_ = OPEN;
            open_since_ = std::chrono::steady_clock::now();
            half_open_used_ = false;
        } else if (consecutive_failures_ >= max_failures_) {
            state_ = OPEN;
            open_since_ = std::chrono::steady_clock::now();
        }
        return false;
    }

   private:
    enum State { CLOSED, OPEN, HALF_OPEN };

    std::mutex mutex_;
    State state_ = CLOSED;
    int consecutive_failures_ = 0;
    bool half_open_used_ = false;
    std::chrono::steady_clock::time_point open_since_;

    int max_failures_;
    std::chrono::milliseconds open_timeout_;
};
