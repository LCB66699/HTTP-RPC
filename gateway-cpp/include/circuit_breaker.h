// 熔断器：CLOSED → OPEN (连续失败≥N) → HALF_OPEN (超时后试探) → CLOSED/OPEN
//
// 共享状态设计（多 gateway 实例一致）：
//   Redis key 规范（所有操作写 master，读本地缓存）:
//     cb:{name}:state   → "CLOSED" / "OPEN" / "HALF_OPEN"  TTL = 2*timeout
//     cb:{name}:fails   → 整数计数（INCR 原子累加）         无 TTL（成功时 DEL）
//     cb:{name}:opened  → Unix 时间戳秒（字符串）            TTL = 2*timeout
//     cb:{name}:probe   → NX 锁，"1"                        TTL = timeout
//
//   本地 state_ 作为读缓存，每 kSyncIntervalMs(默认 2s) 从 Redis 同步一次，
//   避免每次请求都走网络 I/O。
//
//   无 Redis 时退化为进程内熔断，行为与原版完全一致。
#pragma once
#include <string>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <ctime>
#include <cstdio>
#include "server/include/redis_client.h"

class CircuitBreaker {
public:
    enum State { CLOSED, OPEN, HALF_OPEN };

    CircuitBreaker(const std::string& name, int threshold = 5, int timeout_sec = 30,
                   RedisClient* redis = nullptr)
        : name_(name), threshold_(threshold), timeout_(timeout_sec), redis_(redis) {}

    // 在 Redis 就绪后调用（Gateway::Start 中 redis_->Connect() 之后）
    void SetRedis(RedisClient* redis) { redis_ = redis; }

    bool AllowRequest() {
        // 周期性同步 Redis 状态（非阻塞：每 kSyncIntervalMs 只有一个线程真正执行）
        if (redis_ && ShouldSync()) SyncFromRedis();

        State s = state_.load(std::memory_order_acquire);

        if (s == CLOSED) return true;

        if (s == OPEN) {
            // 检查是否超时应转为 HALF_OPEN
            std::lock_guard<std::mutex> lock(mtx_);
            if (state_.load(std::memory_order_relaxed) != OPEN) return true;
            auto now = std::chrono::steady_clock::now();
            if (now - opened_at_ > timeout_) {
                state_.store(HALF_OPEN, std::memory_order_release);
                printf("[CB:%s] HALF_OPEN — probing\n", name_.c_str());
                return TryAcquireProbe();
            }
            return false;
        }

        // HALF_OPEN：只放一个试探请求（跨实例用 Redis NX 锁）
        return TryAcquireProbe();
    }

    void RecordSuccess() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            failure_count_ = 0;
            State prev = state_.exchange(CLOSED, std::memory_order_release);
            if (prev != CLOSED)
                printf("[CB:%s] CLOSED — recovered\n", name_.c_str());
        }
        if (redis_) {
            // 重置 Redis 共享状态
            int ttl = static_cast<int>(timeout_.count()) * 2;
            redis_->SetJSON("cb:" + name_ + ":state", "CLOSED", ttl);
            redis_->DeleteKey("cb:" + name_ + ":fails");
            redis_->DeleteKey("cb:" + name_ + ":opened");
            redis_->DeleteKey("cb:" + name_ + ":probe");
        }
    }

    void RecordFailure() {
        int64_t shared_fails = 0;
        if (redis_) {
            shared_fails = redis_->Increment("cb:" + name_ + ":fails");
        }

        std::lock_guard<std::mutex> lock(mtx_);
        failure_count_++;
        State s = state_.load(std::memory_order_relaxed);

        // 本地 OR 全局计数任一达到阈值即触发熔断
        bool threshold_hit = (failure_count_ >= threshold_) ||
                             (shared_fails > 0 && shared_fails >= threshold_);

        if (s == CLOSED && threshold_hit) {
            state_.store(OPEN, std::memory_order_release);
            opened_at_ = std::chrono::steady_clock::now();
            printf("[CB:%s] OPEN — failures local=%d shared=%lld\n",
                   name_.c_str(), failure_count_, (long long)shared_fails);
            if (redis_) WriteOpenToRedis();
            return;
        }
        if (s == HALF_OPEN) {
            // 探测失败 → 重新打开
            state_.store(OPEN, std::memory_order_release);
            opened_at_ = std::chrono::steady_clock::now();
            printf("[CB:%s] OPEN — probe failed\n", name_.c_str());
            if (redis_) {
                redis_->DeleteKey("cb:" + name_ + ":probe");
                WriteOpenToRedis();
            }
            return;
        }
    }

    const char* StateStr() const {
        switch (state_.load(std::memory_order_relaxed)) {
            case CLOSED:    return "CLOSED";
            case OPEN:      return "OPEN";
            case HALF_OPEN: return "HALF_OPEN";
        }
        return "UNKNOWN";
    }

private:
    std::string name_;
    int threshold_;
    std::chrono::seconds timeout_;
    RedisClient* redis_ = nullptr;

    std::atomic<State> state_{CLOSED};
    std::chrono::steady_clock::time_point opened_at_;
    mutable std::mutex mtx_;
    int failure_count_ = 0;

    // 上次 Redis 同步时间（毫秒级 steady_clock ticks，原子存储）
    mutable std::atomic<int64_t> last_sync_ms_{0};
    static constexpr int64_t kSyncIntervalMs = 2000;  // 每 2 秒同步一次

    // ---- 私有辅助 ----

    bool ShouldSync() const {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t last = last_sync_ms_.load(std::memory_order_relaxed);
        if (now_ms - last >= kSyncIntervalMs) {
            // CAS：只有一个线程能把 last 换成 now_ms，其余线程本次跳过
            return last_sync_ms_.compare_exchange_strong(
                last, now_ms, std::memory_order_relaxed);
        }
        return false;
    }

    void SyncFromRedis() {
        std::string state_str;
        if (!redis_->GetJSON("cb:" + name_ + ":state", state_str)) return;

        State remote = CLOSED;
        if (state_str == "OPEN")       remote = OPEN;
        else if (state_str == "HALF_OPEN") remote = HALF_OPEN;

        State local = state_.load(std::memory_order_acquire);
        if (remote == local) return;  // 无变化

        std::lock_guard<std::mutex> lock(mtx_);
        if (remote == OPEN && local == CLOSED) {
            // 另一个实例触发了熔断，同步 opened_at
            std::string ts_str;
            if (redis_->GetJSON("cb:" + name_ + ":opened", ts_str) && !ts_str.empty()) {
                long long ts = std::stoll(ts_str);
                auto epoch = std::chrono::seconds(ts);
                // 将绝对 Unix 时间戳换算为 steady_clock 时间点（近似）
                auto wall_now = std::chrono::system_clock::now();
                auto steady_now = std::chrono::steady_clock::now();
                auto age = std::chrono::seconds(
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            wall_now.time_since_epoch()).count()) - ts);
                opened_at_ = steady_now - age;
            } else {
                opened_at_ = std::chrono::steady_clock::now();
            }
            state_.store(OPEN, std::memory_order_release);
            printf("[CB:%s] OPEN — synced from Redis\n", name_.c_str());
        } else if (remote == CLOSED && local != CLOSED) {
            state_.store(CLOSED, std::memory_order_release);
            failure_count_ = 0;
            printf("[CB:%s] CLOSED — synced from Redis\n", name_.c_str());
        }
        // HALF_OPEN 不强制同步到本地：由 AllowRequest 内部计时管理
    }

    // HALF_OPEN 探测：跨实例互斥（Redis NX 锁）
    bool TryAcquireProbe() {
        if (redis_) {
            return redis_->SetNX("cb:" + name_ + ":probe", "1",
                                 static_cast<int>(timeout_.count()));
        }
        // 无 Redis：进程内 CAS（原版行为）
        // 用 failure_count_ 复用为 probe flag（0=no probe, 1=probing）
        std::lock_guard<std::mutex> lock(mtx_);
        if (failure_count_ < 0) return false;  // already probing
        failure_count_ = -1;
        return true;
    }

    void WriteOpenToRedis() {
        // 必须在持有 mtx_ 时调用
        int ttl = static_cast<int>(timeout_.count()) * 2;
        redis_->SetJSON("cb:" + name_ + ":state", "OPEN", ttl);
        long long now_ts = static_cast<long long>(std::time(nullptr));
        redis_->SetJSON("cb:" + name_ + ":opened", std::to_string(now_ts), ttl);
    }
};

// ============================================================
// PerReplicaTracker — 副本级故障追踪（Phase 1）
//
// 每个副本独立计数，单个副本隔离不影响其他副本。
// 只有半数以上副本被隔离时才触发服务级熔断。
// Redis key: cb:{service}:rep:{addr}:fails（副本失败计数）
//             cb:{service}:rep:{addr}:q（隔离标志 + TTL）
// ============================================================
class PerReplicaTracker {
public:
    struct ReplicaState {
        int  failure_count = 0;
        bool quarantined   = false;
        std::chrono::steady_clock::time_point quarantined_at;
    };

    PerReplicaTracker(const std::string& service_name,
                      int threshold = 5,
                      int quarantine_timeout_sec = 30,
                      RedisClient* redis = nullptr)
        : service_name_(service_name), threshold_(threshold),
          quarantine_timeout_sec_(quarantine_timeout_sec), redis_(redis) {}

    void SetRedis(RedisClient* redis) { redis_ = redis; }

    // 检查此副本是否已被隔离。隔离超时后自动释放（允许一次探测）。
    bool AllowReplica(const std::string& addr) {
        std::shared_lock lock(mtx_);
        auto it = replicas_.find(addr);
        if (it == replicas_.end()) return true;           // 新副本，放行
        if (!it->second.quarantined) return true;         // 未隔离，放行

        auto now = std::chrono::steady_clock::now();
        if (now - it->second.quarantined_at > std::chrono::seconds(quarantine_timeout_sec_)) {
            // 超时，释放隔离，允许一次探测请求
            lock.unlock();
            std::lock_guard write_lock(mtx_);
            it->second.quarantined = false;
            it->second.failure_count = 0;
            if (redis_) {
                redis_->DeleteKey(QKey(addr));
                redis_->DeleteKey(FailsKey(addr));
            }
            printf("[PRT:%s] %s released from quarantine (timeout)\n",
                   service_name_.c_str(), addr.c_str());
            return true;
        }
        return false;  // 仍在隔离期，拒绝
    }

    void RecordSuccess(const std::string& addr) {
        std::lock_guard lock(mtx_);
        auto& s = replicas_[addr];
        s.failure_count = 0;
        if (s.quarantined) {
            s.quarantined = false;
            if (redis_) {
                redis_->DeleteKey(QKey(addr));
                redis_->DeleteKey(FailsKey(addr));  // 防止 shared_fails 永不降导致死循环
            }
            printf("[PRT:%s] %s recovered\n", service_name_.c_str(), addr.c_str());
        }
    }

    void RecordFailure(const std::string& addr) {
        int shared_fails = 0;
        if (redis_) {
            shared_fails = (int)redis_->Increment("cb:" + service_name_ + ":rep:" + addr + ":fails");
        }

        std::lock_guard lock(mtx_);
        auto& s = replicas_[addr];
        s.failure_count++;
        bool hit = (s.failure_count >= threshold_) ||
                   (shared_fails > 0 && shared_fails >= threshold_);

        if (hit && !s.quarantined) {
            s.quarantined = true;
            s.quarantined_at = std::chrono::steady_clock::now();
            if (redis_) {
                redis_->SetJSON(QKey(addr), "1", quarantine_timeout_sec_ * 2);
            }
            printf("[PRT:%s] %s QUARANTINED (local=%d shared=%d)\n",
                   service_name_.c_str(), addr.c_str(), s.failure_count, shared_fails);
        }
    }

    // 隔离的副本数是否超过半数 → 触发服务级熔断
    bool ShouldTripService(int total_replicas) const {
        std::shared_lock lock(mtx_);
        int q = 0;
        for (auto& [_, s] : replicas_) if (s.quarantined) q++;
        return (q > total_replicas / 2);
    }

    std::unordered_map<std::string, ReplicaState> States() const {
        std::shared_lock lock(mtx_);
        return replicas_;
    }

private:
    std::string QKey(const std::string& addr) const {
        return "cb:" + service_name_ + ":rep:" + addr + ":q";
    }
    std::string FailsKey(const std::string& addr) const {
        return "cb:" + service_name_ + ":rep:" + addr + ":fails";
    }

    std::string service_name_;
    int threshold_;
    int quarantine_timeout_sec_;
    RedisClient* redis_;

    mutable std::shared_mutex mtx_;
    std::unordered_map<std::string, ReplicaState> replicas_;
};
