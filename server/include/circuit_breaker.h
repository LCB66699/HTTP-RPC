// 熔断器：CLOSED → OPEN (多维触发) → HALF_OPEN (多探测) → CLOSED/OPEN
//
// 多维熔断触发条件（任一满足即 OPEN）:
//   a) 连续失败 ≥ failure_threshold             — 快速熔断
//   b) 错误率 ≥ error_ratio (窗口内)             — 统计熔断
//   c) 慢调用率 ≥ slow_call_ratio (窗口内)       — 延迟熔断
//   d) P99 延迟 ≥ latency_threshold_ms (窗口内)  — 尾延迟熔断
//
// 滑动窗口: N 个时间桶组成环形缓冲区，每桶记录 total/failed/slow + 延迟直方图
//
// 半开增强: 多探测 → 聚合成功率 → 整体判决（防网络抖动单探测误判）
//
// Redis 共享状态:
//   cb:{name}:state / :fails / :opened / :probe  (原有)
//   cb:{name}:probe:count / :probe:success / :probe:phase  (半开)
//   cb:{name}:window:{bucket_ts}:total / :fail / :slow / :lat:{n}  (滑动窗口)
//
//   无 Redis → 进程内全功能降级
#pragma once
#include <string>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <array>
#include <cmath>
#include <unordered_map>
#include <ctime>
#include <cstdio>
#include "server/include/redis_client.h"

// ============================================================
// CircuitBreakerConfig — 所有可配置参数
// ============================================================
struct CircuitBreakerConfig {
    int     failure_threshold       = 5;     // 连续失败 → 快速熔断
    int     timeout_sec             = 30;    // OPEN → HALF_OPEN
    int     window_sec              = 60;    // 滑动窗口时长
    int     bucket_sec              = 10;    // 单桶时长
    int     min_samples             = 10;    // 统计评估的最小样本
    int     latency_threshold_ms    = 2000;  // P99 超此值 → OPEN（也用于慢调用判定）
    double  slow_call_ratio         = 0.5;   // 慢调用占比 → OPEN
    double  error_ratio             = 0.5;   // 错误占比 → OPEN
    int     half_open_probes        = 5;     // 半开探测次数
    double  half_open_success_ratio = 0.8;   // 半开关闭所需成功率
    int64_t sync_interval_ms        = 2000;  // Redis 同步间隔
};

// ============================================================
// LatencyHistogram — 固定桶延迟直方图 + P99 估算
// ============================================================
class LatencyHistogram {
public:
    static constexpr int kNumBuckets = 9;

    // 对数刻度桶边界 (ms): 0-2, 2-5, 5-15, 15-50, 50-150, 150-500, 500-1500, 1500-5000, >5000
    static constexpr int kBucketRanges[kNumBuckets][2] = {
        {0, 2}, {2, 5}, {5, 15}, {15, 50}, {50, 150},
        {150, 500}, {500, 1500}, {1500, 5000}, {5000, INT32_MAX}
    };

    static int BucketIndex(int64_t latency_ms) {
        for (int i = 0; i < kNumBuckets; ++i) {
            if (latency_ms <= kBucketRanges[i][1]) return i;
        }
        return kNumBuckets - 1;
    }

    // 从直方图估算 P99 (ms)
    static int64_t EstimateP99(const std::array<int, kNumBuckets>& hist, int total) {
        if (total <= 0) return 0;
        int64_t target = static_cast<int64_t>(total * 0.99);
        int64_t cumulative = 0;
        for (int i = 0; i < kNumBuckets; ++i) {
            cumulative += hist[i];
            if (cumulative >= target) {
                int64_t low = kBucketRanges[i][0];
                int64_t high = (i == kNumBuckets - 1)
                    ? kBucketRanges[i - 1][1] * 2  // 末桶推定上限
                    : kBucketRanges[i][1];
                int64_t in_bucket = hist[i];
                if (in_bucket <= 0) return low;
                int64_t prev_cum = cumulative - in_bucket;
                double frac = static_cast<double>(target - prev_cum) / in_bucket;
                return low + static_cast<int64_t>((high - low) * frac);
            }
        }
        return kBucketRanges[kNumBuckets - 1][0];
    }
};

// ============================================================
// SlidingWindow — 环形缓冲区时间分桶滑动窗口
// ============================================================
class SlidingWindow {
public:
    struct Bucket {
        int64_t start_ts = 0;           // 桶开始 Unix 时间戳(s)
        int     total    = 0;
        int     failed   = 0;
        int     slow     = 0;
        std::array<int, LatencyHistogram::kNumBuckets> hist{};
        bool    dirty    = false;       // 有无新数据需要刷到 Redis
    };

    struct Snapshot {
        int total = 0, failed = 0, slow = 0;
        std::array<int, LatencyHistogram::kNumBuckets> hist{};
        int64_t p99_ms = 0;  // 在 Aggregate 时计算
    };

    SlidingWindow(int window_sec = 60, int bucket_sec = 10)
        : window_sec_(window_sec), bucket_sec_(bucket_sec) {}

    void Record(int64_t latency_ms, bool is_failure, bool is_slow) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto& b = CurrentBucket();
        b.total++;
        if (is_failure) b.failed++;
        if (is_slow) b.slow++;
        int bi = LatencyHistogram::BucketIndex(latency_ms);
        b.hist[bi]++;
        b.dirty = true;
    }

    Snapshot Aggregate() {
        std::lock_guard<std::mutex> lock(mtx_);
        Advance();
        Snapshot snap;
        int64_t now_s = NowSeconds();
        for (auto& b : buckets_) {
            if (b.start_ts <= 0) continue;
            if (now_s - b.start_ts > window_sec_) continue;
            snap.total  += b.total;
            snap.failed += b.failed;
            snap.slow   += b.slow;
            for (int i = 0; i < LatencyHistogram::kNumBuckets; ++i)
                snap.hist[i] += b.hist[i];
        }
        snap.p99_ms = LatencyHistogram::EstimateP99(snap.hist, snap.total);
        return snap;
    }

    // 返回 dirty 桶列表供 FlushToRedis 使用
    std::vector<Bucket> DirtyBuckets() {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<Bucket> result;
        for (auto& b : buckets_) {
            if (b.dirty && b.start_ts > 0) {
                result.push_back(b);
                b.dirty = false;
            }
        }
        return result;
    }

private:
    int window_sec_;
    int bucket_sec_;
    std::array<Bucket, 6> buckets_;
    mutable std::mutex mtx_;

    static int64_t NowSeconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void Advance() {
        int64_t now_s = NowSeconds();
        int64_t cur_ts = (now_s / bucket_sec_) * bucket_sec_;
        // 找当前桶或创建
        int empty_idx = -1;
        for (int i = 0; i < static_cast<int>(buckets_.size()); ++i) {
            if (buckets_[i].start_ts == cur_ts) return;  // 已存在
            if (buckets_[i].start_ts <= 0 || now_s - buckets_[i].start_ts > window_sec_) {
                empty_idx = i;
                break;
            }
        }
        if (empty_idx < 0) {
            // 全部在窗口内且无当前桶 → 复用最旧的
            int64_t oldest_ts = INT64_MAX;
            for (int i = 0; i < static_cast<int>(buckets_.size()); ++i) {
                if (buckets_[i].start_ts < oldest_ts) {
                    oldest_ts = buckets_[i].start_ts;
                    empty_idx = i;
                }
            }
        }
        if (empty_idx >= 0) {
            buckets_[empty_idx] = Bucket{};
            buckets_[empty_idx].start_ts = cur_ts;
        }
    }

    Bucket& CurrentBucket() {
        // mtx_ 必须已被持有
        Advance();
        int64_t cur_ts = (NowSeconds() / bucket_sec_) * bucket_sec_;
        for (auto& b : buckets_) {
            if (b.start_ts == cur_ts) return b;
        }
        // 回退：返回最后一个桶（不应到达这里）
        return buckets_[0];
    }
};

// ============================================================
// CircuitBreaker — 多维熔断 + 增强半开
// ============================================================
class CircuitBreaker {
public:
    enum State { CLOSED, OPEN, HALF_OPEN };

    // 新构造函数：完整配置
    CircuitBreaker(const std::string& name, const CircuitBreakerConfig& cfg = {},
                   RedisClient* redis = nullptr)
        : name_(name), cfg_(cfg), redis_(redis),
          window_(cfg.window_sec, cfg.bucket_sec) {}

    // 向后兼容构造函数
    CircuitBreaker(const std::string& name, int threshold, int timeout_sec,
                   RedisClient* redis = nullptr)
        : name_(name), redis_(redis),
          window_(cfg_.window_sec, cfg_.bucket_sec)
    {
        cfg_.failure_threshold = threshold;
        cfg_.timeout_sec = timeout_sec;
    }

    void SetRedis(RedisClient* redis) { redis_ = redis; }

    // ---- 请求入口 ----
    bool AllowRequest() {
        if (redis_ && ShouldSync()) {
            SyncFromRedis();
            SyncFromRedisMetrics();
            FlushToRedis();
        }

        State s = state_.load(std::memory_order_acquire);
        if (s == CLOSED) return true;

        if (s == OPEN) {
            std::lock_guard<std::mutex> lock(mtx_);
            if (state_.load(std::memory_order_relaxed) != OPEN) return true;
            auto now = std::chrono::steady_clock::now();
            if (now - opened_at_ > std::chrono::seconds(cfg_.timeout_sec)) {
                EnterHalfOpen();
                return TryAcquireProbe();
            }
            return false;
        }

        // HALF_OPEN
        return TryAcquireProbe();
    }

    // ---- 统一结果记录（替代 RecordSuccess / RecordFailure）----
    void RecordResult(bool success, int64_t latency_ms) {
        bool is_transport_failure = !success;
        bool is_slow = (latency_ms >= cfg_.latency_threshold_ms);

        // 记录到滑动窗口
        window_.Record(latency_ms, is_transport_failure, is_slow);

        State s = state_.load(std::memory_order_acquire);

        if (s == CLOSED) {
            if (is_transport_failure) {
                RecordFailureInternal(s);
            } else {
                RecordSuccessInternal(s);
            }
            // 评估统计维度（仅在 CLOSED 状态）
            EvaluateSlidingWindow(s);
        } else if (s == HALF_OPEN) {
            RecordProbeResult(s, success);
        }
    }

    // 向后兼容
    void RecordSuccess() { RecordResult(true, 0); }
    void RecordFailure() { RecordResult(false, 0); }

    const char* StateStr() const {
        switch (state_.load(std::memory_order_relaxed)) {
            case CLOSED:    return "CLOSED";
            case OPEN:      return "OPEN";
            case HALF_OPEN: return "HALF_OPEN";
        }
        return "UNKNOWN";
    }
    int TimeoutSec() const { return cfg_.timeout_sec; }

    // 暴露指标给系统状态端点
    struct Metrics {
        State state;
        int local_fails;
        int total_requests;
        int failed_requests;
        int slow_requests;
        int64_t p99_ms;
    };
    Metrics GetMetrics() {
        auto snap = window_.Aggregate();
        Metrics m;
        m.state = state_.load(std::memory_order_acquire);
        m.local_fails = failure_count_;
        m.total_requests = snap.total;
        m.failed_requests = snap.failed;
        m.slow_requests = snap.slow;
        m.p99_ms = snap.p99_ms;
        return m;
    }

private:
    std::string name_;
    CircuitBreakerConfig cfg_;
    RedisClient* redis_ = nullptr;
    SlidingWindow window_;

    std::atomic<State> state_{CLOSED};
    std::chrono::steady_clock::time_point opened_at_;
    mutable std::mutex mtx_;
    int failure_count_ = 0;

    // 半开状态标记
    int probe_attempts_ = 0;
    int probe_successes_ = 0;
    std::string probe_phase_;  // "" | "ACTIVE" | "DONE"

    mutable std::atomic<int64_t> last_sync_ms_{0};

    // ---- 私有: 桶结算 ----

    void EvaluateSlidingWindow(State s) {
        if (s != CLOSED) return;
        auto snap = window_.Aggregate();
        if (snap.total < cfg_.min_samples) return;

        bool trip = false;
        if (snap.total > 0) {
            double err_rate = static_cast<double>(snap.failed) / snap.total;
            double slow_rate = static_cast<double>(snap.slow) / snap.total;
            if (err_rate >= cfg_.error_ratio) {
                printf("[CB:%s] OPEN — error rate %.2f >= %.2f\n",
                       name_.c_str(), err_rate, cfg_.error_ratio);
                trip = true;
            } else if (slow_rate >= cfg_.slow_call_ratio) {
                printf("[CB:%s] OPEN — slow rate %.2f >= %.2f\n",
                       name_.c_str(), slow_rate, cfg_.slow_call_ratio);
                trip = true;
            } else if (snap.p99_ms >= cfg_.latency_threshold_ms) {
                printf("[CB:%s] OPEN — P99 %lldms >= %dms\n",
                       name_.c_str(), (long long)snap.p99_ms, cfg_.latency_threshold_ms);
                trip = true;
            }
        }
        if (trip) {
            std::lock_guard<std::mutex> lock(mtx_);
            if (state_.load(std::memory_order_relaxed) == CLOSED) {
                state_.store(OPEN, std::memory_order_release);
                opened_at_ = std::chrono::steady_clock::now();
                if (redis_) WriteOpenToRedis();
            }
        }
    }

    // ---- 私有: 失败/成功内部处理 ----

    void RecordFailureInternal(State s) {
        int64_t shared_fails = 0;
        if (redis_) shared_fails = redis_->Increment("cb:" + name_ + ":fails");

        std::lock_guard<std::mutex> lock(mtx_);
        failure_count_++;

        bool local_hit = (failure_count_ >= cfg_.failure_threshold);
        bool shared_hit = (shared_fails > 0 && shared_fails >= cfg_.failure_threshold);

        if (s == CLOSED && (local_hit || shared_hit)) {
            state_.store(OPEN, std::memory_order_release);
            opened_at_ = std::chrono::steady_clock::now();
            printf("[CB:%s] OPEN — failures local=%d shared=%lld\n",
                   name_.c_str(), failure_count_, (long long)shared_fails);
            if (redis_) WriteOpenToRedis();
        } else if (s == HALF_OPEN) {
            // 探测失败不影响立即 OPEN——由 RecordProbeResult 统一处理
        }
    }

    void RecordSuccessInternal(State s) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            failure_count_ = 0;
            State prev = state_.exchange(CLOSED, std::memory_order_release);
            if (prev != CLOSED) {
                printf("[CB:%s] CLOSED — recovered\n", name_.c_str());
            }
        }
        if (redis_) ClearRedisState();
    }

    // ---- 私有: 增强半开 ----

    void EnterHalfOpen() {
        state_.store(HALF_OPEN, std::memory_order_release);
        probe_attempts_ = 0;
        probe_successes_ = 0;
        probe_phase_ = "ACTIVE";
        printf("[CB:%s] HALF_OPEN — probing (%d probes required)\n",
               name_.c_str(), cfg_.half_open_probes);
        if (redis_) {
            int ttl = cfg_.timeout_sec * 2;
            redis_->SetJSON("cb:" + name_ + ":state", "HALF_OPEN", ttl);
            redis_->SetJSON("cb:" + name_ + ":probe:phase", "ACTIVE", ttl);
            redis_->DeleteKey("cb:" + name_ + ":probe:count");
            redis_->DeleteKey("cb:" + name_ + ":probe:success");
        }
    }

    void RecordProbeResult(State s, bool success) {
        if (s != HALF_OPEN) return;
        if (probe_phase_ != "ACTIVE") return;

        std::lock_guard<std::mutex> lock(mtx_);
        probe_attempts_++;
        if (success) probe_successes_++;

        if (redis_) {
            redis_->Increment("cb:" + name_ + ":probe:count");
            if (success) redis_->Increment("cb:" + name_ + ":probe:success");
        }

        if (probe_attempts_ >= cfg_.half_open_probes) {
            double ratio = static_cast<double>(probe_successes_) / probe_attempts_;
            printf("[CB:%s] HALF_OPEN probe done: %d/%d (%.0f%%), threshold=%.0f%%\n",
                   name_.c_str(), probe_successes_, probe_attempts_,
                   ratio * 100, cfg_.half_open_success_ratio * 100);

            if (ratio >= cfg_.half_open_success_ratio) {
                state_.store(CLOSED, std::memory_order_release);
                failure_count_ = 0;
                printf("[CB:%s] CLOSED — probes passed\n", name_.c_str());
                if (redis_) ClearRedisState();
            } else {
                state_.store(OPEN, std::memory_order_release);
                opened_at_ = std::chrono::steady_clock::now();
                printf("[CB:%s] OPEN — probes failed\n", name_.c_str());
                if (redis_) {
                    redis_->SetJSON("cb:" + name_ + ":probe:phase", "DONE",
                                    cfg_.timeout_sec * 2);
                    WriteOpenToRedis();
                }
            }
            probe_phase_ = "DONE";
        }
    }

    // ---- 私有: 探测锁 ----

    bool TryAcquireProbe() {
        if (redis_) {
            return redis_->SetNX("cb:" + name_ + ":probe", "1", cfg_.timeout_sec);
        }
        std::lock_guard<std::mutex> lock(mtx_);
        if (probe_attempts_ >= cfg_.half_open_probes) return false;
        return true;
    }

    // ---- 私有: Redis 同步 ----

    bool ShouldSync() const {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t last = last_sync_ms_.load(std::memory_order_relaxed);
        if (now_ms - last >= cfg_.sync_interval_ms) {
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
        if (remote == local) return;

        std::lock_guard<std::mutex> lock(mtx_);
        if (remote == OPEN && local == CLOSED) {
            std::string ts_str;
            if (redis_->GetJSON("cb:" + name_ + ":opened", ts_str) && !ts_str.empty()) {
                long long ts = std::stoll(ts_str);
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
        } else if (remote == HALF_OPEN && local == OPEN) {
            // 另一实例已进入半开
            std::string phase;
            if (redis_->GetJSON("cb:" + name_ + ":probe:phase", phase) && phase == "DONE") {
                printf("[CB:%s] HALF_OPEN — probe phase DONE (another instance)\n",
                       name_.c_str());
                return;
            }
            EnterHalfOpen();
        }
    }

    void SyncFromRedisMetrics() {
        // 滑动窗口指标通过 INCR 在 Redis 侧聚合，本地仅负责 Flush
        // 这里可以做跨实例状态合并，但当前采用保守策略：本地窗口已足够
    }

    void FlushToRedis() {
        if (!redis_) return;
        auto dirty = window_.DirtyBuckets();
        int ttl = cfg_.window_sec * 2;
        for (auto& b : dirty) {
            std::string prefix = "cb:" + name_ + ":window:" + std::to_string(b.start_ts);
            if (b.total > 0)
                redis_->IncrementWithTTL(prefix + ":total", ttl);
            if (b.failed > 0)
                redis_->IncrementWithTTL(prefix + ":fail", ttl);
            if (b.slow > 0)
                redis_->IncrementWithTTL(prefix + ":slow", ttl);
            for (int i = 0; i < LatencyHistogram::kNumBuckets; ++i) {
                if (b.hist[i] > 0)
                    redis_->IncrementWithTTL(
                        prefix + ":lat:" + std::to_string(i), ttl);
            }
        }
    }

    void ClearRedisState() {
        int ttl = cfg_.timeout_sec * 2;
        redis_->SetJSON("cb:" + name_ + ":state", "CLOSED", ttl);
        redis_->DeleteKey("cb:" + name_ + ":fails");
        redis_->DeleteKey("cb:" + name_ + ":opened");
        redis_->DeleteKey("cb:" + name_ + ":probe");
        redis_->DeleteKey("cb:" + name_ + ":probe:count");
        redis_->DeleteKey("cb:" + name_ + ":probe:success");
        redis_->DeleteKey("cb:" + name_ + ":probe:phase");
    }

    void WriteOpenToRedis() {
        int ttl = cfg_.timeout_sec * 2;
        redis_->SetJSON("cb:" + name_ + ":state", "OPEN", ttl);
        long long now_ts = static_cast<long long>(std::time(nullptr));
        redis_->SetJSON("cb:" + name_ + ":opened", std::to_string(now_ts), ttl);
    }
};

// ============================================================
// PerReplicaTracker — 副本级故障追踪 (unchanged)
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

    bool AllowReplica(const std::string& addr) {
        std::shared_lock lock(mtx_);
        auto it = replicas_.find(addr);
        if (it == replicas_.end()) return true;
        if (!it->second.quarantined) return true;

        auto now = std::chrono::steady_clock::now();
        if (now - it->second.quarantined_at > std::chrono::seconds(quarantine_timeout_sec_)) {
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
        return false;
    }

    void RecordSuccess(const std::string& addr) {
        std::lock_guard lock(mtx_);
        auto& s = replicas_[addr];
        s.failure_count = 0;
        if (s.quarantined) {
            s.quarantined = false;
            if (redis_) {
                redis_->DeleteKey(QKey(addr));
                redis_->DeleteKey(FailsKey(addr));
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
