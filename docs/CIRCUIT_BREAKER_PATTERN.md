# Gateway 熔断器(CircuitBreaker) 设计模式

> 最后更新: 2026-05-25

## 1. 概述

HTTP-RPC Gateway 层实现了**多维熔断保护**：滑动窗口统计 + 4 维度触发 + 增强半开多探测。每个后端服务（auth/sheet/file）独立一个 CircuitBreaker 实例，通过 `with_cb` 泛型 lambda 统一包裹所有路由 Handler。

## 2. 架构

```
每个 API 请求:
  with_cb(cb, handler)
    │
    ├─ ① 熔断检查: cb.AllowRequest()
    │     CLOSED → 放行
    │     OPEN   → 503 Retry-After ← 立即拒绝
    │     HALF_OPEN → 5 次探针
    │
    ├─ ② 并发控制: semaphore.try_acquire_until(deadline)
    │     超时 → 503 server_overloaded
    │
    ├─ ③ 执行业务: inner(req, res) → RpcResult
    │
    └─ ④ 反馈: cb.RecordResult(success, latency_ms)
          ├─ 记录 SlidingWindow
          ├─ 评估多维触发条件
          └─ 更新熔断器状态
```

## 3. 多维熔断触发条件

| 维度 | 触发条件 | 默认阈值 | 说明 |
|------|------|------|------|
| 连续失败 | `failure_count_ >= N` | 5 次 | 快速熔断，秒级触发 |
| 错误率 | `failed/total >= ratio` | 50% | 滑动窗口 60s 内统计 |
| 慢调用率 | `slow/total >= ratio` | 50% | 延迟 ≥ 2000ms 的调用 |
| P99 延迟 | `estimated_p99 >= ms` | 2000ms | 9 桶对数直方图估算 |

## 4. SlidingWindow（滑动窗口）

- **结构**：6 个时间桶 × 10s = 60s 环形缓冲区
- **每桶记录**：total / failed / slow + 9 桶延迟直方图
- **滚动**：`(now_s / 10) * 10` 整除对齐桶边界，过期桶自动清零
- **P99 估算**：累积直方图计数 + 线性插值，O(9) 查询

## 5. 半开状态增强

| 机制 | 改前（单探针） | 改后（多探测） |
|------|------|------|
| 探测次数 | 1 次 | 5 次 |
| 判决方式 | 单次成功/失败 | 聚合成功率 ≥ 80% |
| 跨实例协调 | Redis SetNX 锁 | INCR probe:count + probe:success |
| 网络抖动 | 一次失败 → 重新 OPEN | 5 次中允许 1 次失败 |

## 6. 状态共享

熔断器状态通过 Redis Cluster 跨 Gateway 实例共享：

| Redis Key | 用途 | TTL |
|------|------|------|
| `cb:{name}:state` | OPEN/CLOSED/HALF_OPEN | `timeout_sec × 2` |
| `cb:{name}:fails` | INCR 失败计数 | 成功时 DEL |
| `cb:{name}:opened` | 打开时间戳 | `timeout_sec × 2` |
| `cb:{name}:probe` | 探针锁 (SetNX) | `timeout_sec` |
| `cb:{name}:probe:count` | 探测总数 (INCR) | 判决后 DEL |
| `cb:{name}:probe:success` | 成功数 (INCR) | 判决后 DEL |
| `cb:{name}:window:{ts}:*` | 滑动窗口指标 (INCR+TTL) | `window_sec × 2` |

同步策略：每 2s 由 CAS 选出一个线程执行 `FlushToRedis()` + `SyncFromRedis()`。

## 7. 延迟测量注入点

| 注入点 | 覆盖范围 |
|------|------|
| `with_cb` | 鉴权 + gRPC + JSON 构建 |
| `login` lambda | HandleLogin() 整体耗时 |
| `register` lambda | HandleRegister() 整体耗时 |
| `file_up` | gRPC 上传含重试 |
| `file_down` | gRPC 下载含重试 |

## 8. 配置参数

```cpp
struct CircuitBreakerConfig {
    int failure_threshold       = 5;
    int timeout_sec             = 30;
    int window_sec              = 60;
    int bucket_sec              = 10;
    int min_samples             = 10;
    int latency_threshold_ms    = 2000;
    double slow_call_ratio      = 0.5;
    double error_ratio          = 0.5;
    int half_open_probes        = 5;
    double half_open_success_ratio = 0.8;
};
```
