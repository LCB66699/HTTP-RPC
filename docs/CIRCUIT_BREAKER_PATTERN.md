# Gateway 路由层熔断器(CircuitBreaker)统一封装设计模式

## 1. 概述

本文档分析 HTTP-RPC 项目中 Gateway 路由层的熔断器设计模式。该模式通过一个泛型 lambda 表达式统一包裹所有后端 RPC 路由 Handler，实现**细粒度的服务级熔断保护**，防止下游服务故障引发级联雪崩。

### 关键术语

| 术语 | 定义 |
|------|------|
| CircuitBreaker | 熔断器实例，每个后端服务一个，独立维护状态机 |
| RpcResult | Handler 返回的三态枚举，区分业务失败与传输故障 |
| breaker lambda | `gateway.cpp:417` 定义的泛型包裹函数，统一注入熔断逻辑 |
| CLOSED/OPEN/HALF_OPEN | 熔断器三态：关闭(正常)/打开(熔断)/半开(试探) |

---

## 2. 背景与动机

### 2.1 为什么不把熔断逻辑放在每个 Handler 内？

如果每个 Handler 都独立调用 `cb.AllowRequest()` + `cb.RecordSuccess/Failure()`，会导致：

- **重复代码**：10+ 个 Handler 重复同样的样板代码
- **不一致风险**：某个 Handler 忘记调用 `RecordFailure()`，熔断器永不会打开
- **维护成本**：新增路由时需要手动复制熔断逻辑

### 2.2 为什么要区分 BUSINESS_FAILURE 和 TRANSPORT_FAILURE？

典型场景：用户请求创建表格时未传 `name` 参数，Handler 返回"name is required"。

- 这是**业务层的合法拒绝**，不代表后端服务故障
- 如果将其计入熔断计数，一次参数校验失败就可能推动熔断器进入 OPEN 状态
- 熔断器应只对**传输层故障**（gRPC 调用失败、后端超时、网络分区）做出响应

### 2.3 设计目标

1. **零侵入**：Handler 只返回业务信号，不感知熔断器存在
2. **精确触发**：仅传输故障推动状态迁移，业务拒绝不影响熔断决策
3. **服务隔离**：每个 gRPC 后端独立熔断器，Sheet 服务故障不影响 File 服务
4. **自动恢复**：超时后 HALF_OPEN 试探，恢复后自动回到 CLOSED

---

## 3. 设计方案

### 3.1 整体架构

```
                     breaker lambda (统一包裹层)
                           |
     +---------------------+---------------------+
     |                                           |
     v                                           v
cb.AllowRequest()?                    handler(args..., r)
     |                                        |
     |-- NO --> "circuit open"                 |
     |                                        r
     |                              RpcResult 枚举
     |                              +-- SUCCESS
     |                              +-- BUSINESS_FAILURE
     |                              +-- TRANSPORT_FAILURE
     |                                        |
     +---------> switch(result) <-------------+
                       |
        +--------------+--------------+
        |              |              |
        v              v              v
  RecordSuccess()    no-op     RecordFailure()
        |              |              |
        v              v              v
   熔断器CLOSED   不改变状态    可能触发OPEN
```

### 3.2 核心代码结构

#### 3.2.1 RpcResult 三态枚举 (`gateway.h:25`)

```cpp
// Handler 返回状态 — 区分业务失败和传输故障，防止业务错误误触发熔断
enum class RpcResult { SUCCESS, BUSINESS_FAILURE, TRANSPORT_FAILURE };
```

| 枚举值 | 含义 | 对熔断器的影响 |
|--------|------|---------------|
| `SUCCESS` | Handler 正常完成，包括业务成功和带数据的成功响应 | 调用 `RecordSuccess()`，重置失败计数 |
| `BUSINESS_FAILURE` | 业务层合法拒绝（参数校验失败、权限不足、资源不存在等） | **无操作**，不改变熔断器状态 |
| `TRANSPORT_FAILURE` | 传输层故障（gRPC 调用返回非 OK、后端连接超时、Channel 断开） | 调用 `RecordFailure()`，可能触发熔断 |

#### 3.2.2 三个独立熔断器实例 (`gateway.h:61-63`)

```cpp
CircuitBreaker cb_auth_{"auth", 3, 15};    // 阈值3，超时15秒
CircuitBreaker cb_sheet_{"sheet", 5, 30};  // 阈值5，超时30秒
CircuitBreaker cb_file_{"file", 5, 30};    // 阈值5，超时30秒
```

每个后端服务独立配置：

| 实例 | 后端服务 | 熔断阈值 | 恢复超时 | 说明 |
|------|---------|---------|---------|------|
| `cb_auth_` | AuthService | 3 | 15s | 认证服务，阈值低(仅登录/注册，出现连接问题应尽快熔断) |
| `cb_sheet_` | SpreadsheetService | 5 | 30s | 表格服务，阈值高(读写比例高，允许短时波动) |
| `cb_file_` | FileService | 5 | 30s | 文件服务，阈值高(含大文件传输，允许偶发超时) |

#### 3.2.3 breaker lambda 模板 (`gateway.cpp:417-429`)

```cpp
// 熔断包裹：Check → Handler → RecordSuccess/RecordFailure
// 只对 TRANSPORT_FAILURE 触发熔断计数，业务拒绝不影响熔断器
auto breaker = [&](CircuitBreaker& cb, auto handler, httplib::Response& res, auto&&... args) {
    std::string r;
    if (!cb.AllowRequest()) {
        r = "{\"success\":false,\"error\":\"circuit open\"}";
    } else {
        auto result = handler(args..., r);
        switch (result) {
            case RpcResult::SUCCESS:           cb.RecordSuccess(); break;
            case RpcResult::BUSINESS_FAILURE:  /* 不动熔断 */    break;
            case RpcResult::TRANSPORT_FAILURE: cb.RecordFailure(); break;
        }
    }
    res.set_content(r, "application/json");
};
```

#### 3.2.4 路由注册示例 (`gateway.cpp:432-451`)

```cpp
// Sheets — cb_sheet_ 统一保护
svr.Post("/api/sheets", [&](auto& req, auto& res) {
    std::string u; if (!require_auth(req, res, u)) return;
    breaker(cb_sheet_, [this,&u,&req](auto&... a){ return HandleSheetCreate(u, req.body, a...); }, res);
});
svr.Get("/api/sheets", [&](auto& req, auto& res) {
    std::string u; if (!require_auth(req, res, u)) return;
    breaker(cb_sheet_, [this,&u](auto&... a){ return HandleSheetList(u, a...); }, res);
});
```

---

## 4. CircuitBreaker 三态状态机实现

### 4.1 状态定义 (`circuit_breaker.h:11`)

```cpp
enum State { CLOSED, OPEN, HALF_OPEN };
```

### 4.2 状态转换图

```
       连续失败 ≥ threshold
   ┌──────────  ────────────┐
   │                        │
   ▼                        ▼
 CLOSED ──────────────→ OPEN
   ↑                     │
   │                     │ timeout 超时
   │                     ▼
   │                  HALF_OPEN
   │                     │
   └──── 试探成功 ───────┘
   │                     │
   │                     │ 试探失败
   │                     ▼
   └────────────────── OPEN (重新计时)
```

### 4.3 状态机详细说明

| 当前状态 | 触发事件 | 动作 | 下一状态 |
|----------|---------|------|---------|
| CLOSED | 收到请求 | 计数递增检查 | 若 `failure_count_ >= threshold_` 则 OPEN，否则 CLOSED |
| CLOSED | 收到成功 | 重置 `failure_count_ = 0` | CLOSED |
| OPEN | 收到请求 | 检查是否超时 | 未超时则拒绝请求，OPEN |
| OPEN | timeout 到期 | 尝试放行一个请求 | HALF_OPEN |
| HALF_OPEN | 收到请求 | CAS 原子竞争，仅胜出的线程放行 | 失败线程拒绝请求，HALF_OPEN |
| HALF_OPEN | 试探成功 | 重置计数器，关闭熔断 | CLOSED |
| HALF_OPEN | 试探失败 | 重新进入 OPEN | OPEN |

### 4.4 关键实现细节

#### 4.4.1 HALF_OPEN 原子放行 (`circuit_breaker.h:34-36`)

```cpp
// HALF_OPEN: 只放一个试探请求
if (s == HALF_OPEN) {
    bool expected = false;
    return half_open_probing_.compare_exchange_strong(expected, true);
}
```

使用 `std::atomic<bool>` 的 CAS (compare-and-exchange) 操作确保**并发安全**：多个线程同时进入 HALF_OPEN 状态时，只有一个能竞争到 `half_open_probing_` 的置位权，其余线程返回 `false` 继续拒绝请求。这保证了试探期的负载控制。

#### 4.4.2 超时时间检查的竞态处理 (`circuit_breaker.h:20-30`)

```cpp
if (s == OPEN) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mtx_);
    if (state_.load() != OPEN) return true;  // 状态已变，不走 HALF_OPEN 逻辑
    if (now - opened_at_ > timeout_) {
        state_.store(HALF_OPEN, std::memory_order_release);
        half_open_probing_.store(true, std::memory_order_release);
        printf("[CB:%s] HALF_OPEN — probing\n", name_.c_str());
        return true;
    }
    return false;
}
```

使用互斥锁保护临界区，避免两个线程同时判定超时并将状态置为 HALF_OPEN。进入临界区后**二次检查状态**：如果另一个线程抢先变更了状态（例如在 acquire 之后被 CAS 抢先），直接放行。

#### 4.4.3 成功恢复 (`circuit_breaker.h:41-47`)

```cpp
void RecordSuccess() {
    std::lock_guard<std::mutex> lock(mtx_);
    failure_count_ = 0;
    half_open_probing_.store(false, std::memory_order_release);
    State prev = state_.exchange(CLOSED, std::memory_order_release);
    if (prev == HALF_OPEN) printf("[CB:%s] CLOSED — recovered\n", name_.c_str());
}
```

成功时无条件将 `failure_count_` 重置为 0，状态转到 CLOSED。这意味着即使处于 CLOSED 状态但有零星失败（未达到阈值），一次成功也会清零计数器。这是一种**偏向恢复**的设计，避免偶发失败缓慢累积导致熔断。

#### 4.4.4 失败处理 (`circuit_breaker.h:49-68`)

```cpp
void RecordFailure() {
    std::lock_guard<std::mutex> lock(mtx_);
    failure_count_++;
    State s = state_.load(std::memory_order_relaxed);

    if (s == CLOSED && failure_count_ >= threshold_) {
        state_.store(OPEN, std::memory_order_release);
        opened_at_ = std::chrono::steady_clock::now();
        printf("[CB:%s] OPEN — %d consecutive failures\n", name_.c_str(), failure_count_);
        return;
    }
    if (s == HALF_OPEN) {
        state_.store(OPEN, std::memory_order_release);
        opened_at_ = std::chrono::steady_clock::now();
        half_open_probing_.store(false, std::memory_order_release);
        printf("[CB:%s] OPEN — probe failed\n", name_.c_str());
        return;
    }
}
```

两个路径触发 OPEN：
- **CLOSED 模式**：连续失败达到阈值，记录打开时间点用于后续超时判断
- **HALF_OPEN 模式**：试探请求失败，立即回到 OPEN 并重新计时，意味着下一次试探需要再等一个完整的 timeout 周期

### 4.5 Redis 跨实例共享状态

系统扩展为双 Gateway 实例后，单机本地状态机无法保证一致性——gateway-1 已熔断但 gateway-2 不知情，仍会向故障后端发出请求。为此，熔断器状态持久化到 Redis，所有实例共享一份状态机。

**Redis Key 结构**：

| Key | 类型 | TTL | 含义 |
|-----|------|-----|------|
| `cb:{svc}:state` | string (0/1/2) | 120s | 熔断状态（CLOSED/OPEN/HALF_OPEN） |
| `cb:{svc}:fails` | int | 120s | 连续失败计数（跨实例累计） |
| `cb:{svc}:opened` | string (unix timestamp) | 120s | OPEN 打开时间，用于超时判断 |
| `cb:{svc}:probe` | string | 30s | 半开探针锁（SetNX 竞争），防多实例同时探针 |

**工作机制**：

```
gateway-1 记录失败 → Redis INCR cb:sheet:fails → 达到阈值
  → SETEX cb:sheet:state "1" 120s
  → gateway-2 下次同步（每5s）读取 → 本地状态置 OPEN
  → gateway-2 也开始快速失败
```

**本地缓存 + 定期同步**：为避免每次请求都访问 Redis 引入额外延迟，采用"本地快速读 + 定期同步"策略：

```
AllowRequest():
  若本地缓存 < 5s 内有效 → 直接读本地状态（无 Redis I/O）
  否则 → 从 Redis 同步一次状态，更新本地缓存

RecordFailure():
  Redis INCR cb:{svc}:fails（原子累计跨实例失败）
  若达到阈值 → SETEX cb:{svc}:state "1"（OPEN）
```

**HALF_OPEN 探针锁（SetNX）**：

```cpp
// 仅第一个竞争到 NX 的 gateway 实例发出探针
bool acquired = redis_->SetNX("cb:" + name_ + ":probe", "1", 30);
if (!acquired) return false;  // 其他实例继续拒绝请求
```

**Redis 断连降级**：Redis 不可用时，自动降级为纯本地状态机，不影响请求处理：

```cpp
if (!redis_ || !redis_->IsConnected()) {
    // 纯本地状态，与单实例行为相同
    return local_state_ != OPEN;
}
```

---

## 5. 两种使用模式

### 5.1 标准模式：breaker lambda 包裹

适用于普通 JSON 请求-响应路由，Handler 签名统一的场景。

```cpp
svr.Post("/api/sheets", [&](auto& req, auto& res) {
    std::string u; if (!require_auth(req, res, u)) return;
    breaker(cb_sheet_, [this,&u,&req](auto&... a){ return HandleSheetCreate(u, req.body, a...); }, res);
});
```

**调用链**：
```
HTTP POST /api/sheets
  -> require_auth (JWT 验证)
  -> breaker(cb_sheet_, lambda, res)
       -> cb_sheet_.AllowRequest()
       -> HandleSheetCreate(u, req.body, r) -> RpcResult
       -> switch(result) -> RecordSuccess/RecordFailure
  -> res.set_content(r, "application/json")
```

### 5.2 手动模式：不适用 breaker 的场景

文件上传和下载由于涉及 multipart 表单和二进制流，无法统一走 `breaker` 的 JSON response 路径，手动注入熔断逻辑 (`gateway.cpp:464-511`)。

```cpp
// 文件上传 — 不能走标准 breaker（response 格式不同）
if (!cb_file_.AllowRequest()) {
    res.set_content("{\"success\":false,\"error\":\"circuit open\"}", "application/json");
    return;
}
// ... multipart 解析 + gRPC 调用 ...
if (!st.ok()) { cb_file_.RecordFailure(); ... return; }
if (!fresp.success()) { /* 业务失败，不动熔断 */ ... return; }
cb_file_.RecordSuccess();
```

这种手动模式与标准模式遵循**同样的语义**：
- 先 `AllowRequest()` 检查
- 传输故障调 `RecordFailure()`
- 业务失败不动熔断
- 成功调 `RecordSuccess()`

---

## 6. Handler 侧信号约定

每个 Handler 需根据实际语义准确返回 `RpcResult` 枚举。以下是典型模式：

### 6.1 SUCCESS 示例

```cpp
// HandleSheetGet — gRPC 调用成功且业务成功
auto st = sheet_stub_->GetSpreadsheet(&ctx, req, &resp);
if (!st.ok() || !resp.success()) { response = "..."; return RpcResult::TRANSPORT_FAILURE; }
// ... 组装 JSON response ...
return RpcResult::SUCCESS;
```

### 6.2 BUSINESS_FAILURE 示例

```cpp
// HandleSheetCreate — 参数校验失败
std::string name = JsonGet(body, "name");
if (name.empty()) {
    response = "{\"success\":false,\"error\":\"name is required\"}";
    return RpcResult::BUSINESS_FAILURE;  // 业务拒绝，不动熔断器
}
```

### 6.3 TRANSPORT_FAILURE 示例

```cpp
// gRPC 调用返回非 OK 状态 — 代表与后端的通信出问题
auto st = sheet_stub_->CreateSpreadsheet(&ctx, req, &resp);
if (!st.ok()) {
    response = "{\"success\":false,\"error\":\"Backend unavailable\"}";
    return RpcResult::TRANSPORT_FAILURE;  // 传输故障，推动熔断
}
```

### 6.4 组合判断示例

```cpp
// gRPC OK 但业务层失败 — 如登录时密码错误
auto st = auth_stub_->Login(&ctx, req, &resp);
bool ok = st.ok() && resp.success();
response = ok ? "...success..." : "...error...";
return ok ? RpcResult::SUCCESS : RpcResult::TRANSPORT_FAILURE;  // 注：这里"业务拒绝"仍视为 TRANSPORT_FAILURE
```

注意：当前代码中 Login/Register 的 `st.ok()` 失败和 `resp.success()` 为 false 统一归为 TRANSPORT_FAILURE。这是一个可优化的点——如果 gRPC 调用本身成功 (`st.ok()`) 但业务返回了 `success=false`（如密码错误），更合理的分类应是 BUSINESS_FAILURE。

---

## 7. 监控与可观测性

### 7.1 熔断器状态暴露

Health 接口 (`HandleHealth`) 和 SystemStatus 接口 (`HandleSystemStatus`) 均将熔断器状态作为 JSON 字段暴露：

```json
{
  "auth":   {"channel":"READY", "breaker":"CLOSED"},
  "sheet":  {"channel":"READY", "breaker":"CLOSED"},
  "file":   {"channel":"READY", "breaker":"CLOSED"}
}
```

可能的熔断器状态值：`CLOSED`、`OPEN`、`HALF_OPEN`、`UNKNOWN`。

### 7.2 日志追踪

熔断器状态变更时输出结构化日志：

```
[CB:auth] OPEN — 3 consecutive failures
[CB:sheet] HALF_OPEN — probing
[CB:sheet] CLOSED — recovered
[CB:file] OPEN — probe failed
```

日志格式：`[CB:<服务名>] <新状态> — <原因>`，可在监控系统中按 `[CB:` 关键字过滤。

### 7.3 错误计数关联

`HandleSystemStatus` 从 Redis 读取各服务的累积错误总数：

```json
{
  "errors": {
    "auth": 12,
    "spreadsheet": 3,
    "file": 7
  }
}
```

结合熔断器状态和错误计数，可以判断熔断是短暂抖动还是持续故障。

---

## 8. 权衡与取舍

### 8.1 方案优势

| 维度 | 优势 |
|------|------|
| **关注点分离** | Handler 只关心业务逻辑和返回值，熔断决策完全由 breaker lambda 和 CircuitBreaker 类负责 |
| **零模板代码** | 新增路由只需一行 `breaker(cb_, handler, res)`，不需要额外 try-catch 或 if-else |
| **精确计数** | BUSINESS_FAILURE 不触发熔断，避免"用户传错参数导致后端被熔断"的反直觉行为 |
| **服务级隔离** | 三个独立熔断器，Sheet 服务故障不会影响 Auth 和 File 请求 |
| **并发安全** | CAS 原子操作 + 互斥锁，无锁状态读取，高并发下性能良好 |
| **自动恢复** | HALF_OPEN 试探 + 成功自动 CLOSED，无需人工干预 |

### 8.2 局限性

1. **超时检测缺失**：当前熔断器只监控失败**次数**，不监控请求**耗时**。如果后端响应变慢但未超时，不会触发熔断。理想方案应加入延迟百分位监控。

2. **无滑动窗口**：使用单调递增计数器而非滑动时间窗口。如果在长周期内均匀分布 5 次失败，结果与 1 秒内集中 5 次失败相同。滑动窗口（如 30 秒内失败率 > 50%）能更精确地反映故障密度。

3. **半开状态单试探限制**：HALF_OPEN 状态只允许一个试探请求（Redis SetNX 探针锁）。对于多副本后端，一次成功试探不一定代表整体恢复。更复杂的策略可逐步放行（如先放 10% 流量）。

4. **无半开超时保护**：如果 HALF_OPEN 试探请求无限期挂起（如 gRPC 调用 pending），探针锁仅靠 Redis TTL（30s）自动释放。应在试探时设置独立的短 deadline。

5. **Redis 同步存在 5s 延迟**：本地缓存有效期 5s，极端情况下一个 gateway 发现故障后，另一个 gateway 最多需要 5s 才能同步熔断状态。这是延迟与性能（避免每次请求 Redis I/O）之间的权衡。

### 8.3 被否决的替代方案

| 方案 | 否决原因 |
|------|---------|
| **AOP/切面编程** | C++ 无原生 AOP，用继承或模板策略会增加代码复杂度，lambda 更轻量 |
| **每个 Handler 内嵌熔断代码** | 重复代码，维护成本高，容易遗漏导致熔断失效 |
| **全局统一熔断器** | 一个后端故障会熔断全部请求，违反故障隔离原则 |
| **第三方库(如 Hystrix)** | C++ 生态中无轻量成熟的 Hystrix 移植，自实现核心逻辑仅 88 行 |

### 8.4 未来演进方向

- **滑动窗口计数器**：用环形数组(time-window ring buffer)替代单调计数器，按时间段计算失败率
- **延迟熔断**：监控 p99/p999 延迟，超阈值时触发熔断
- **分级 HALF_OPEN**：逐步放行流量，观察一段时间内的成功/失败比例
- **熔断事件推送**：状态变更时向 Prometheus/AlertManager 推送指标

---

## 9. 运维指南

### 9.1 熔断器参数调整原则

| 参数 | 增大 | 减小 |
|------|------|------|
| `threshold` | 允许更多短暂抖动，减少误熔断 | 更快响应故障，提高敏感度 |
| `timeout` | 后端恢复时间窗口更长 | 更快尝试恢复，减少拒绝时长 |

推荐调整步骤：
1. 观察正常时段的最大连续失败数，设为该值 * 2 ~ * 3
2. timeout 设置为后端 P99 恢复时间 * 2
3. 压测中通过熔断占比调整参数

### 9.2 常见问题排查

| 现象 | 可能原因 | 排查步骤 |
|------|---------|---------|
| 熔断频繁触发但 gRPC Channel 显示 READY | 业务层超时设置过短 | 检查 gRPC deadline 设置 |
| 熔断从不触发 | `BUSINESS_FAILURE` 误用为 `TRANSPORT_FAILURE` | 检查 Handler 返回值语义 |
| HALF_OPEN 后反复 OPEN | 后端未完全恢复，需要更长 timeout | 增大超时时间或增加副本数 |
| "circuit open" 响应 | 后端持续故障 | 查看日志 `[CB:` 确认触发时间和原因 |

### 9.3 配置项清单

| 项 | 位置 | 默认值 | 说明 |
|----|------|--------|------|
| Auth 熔断阈值 | `gateway.h:61` | 3 | Auth 服务连续失败次数 |
| Auth 恢复超时 | `gateway.h:61` | 15s | Auth 熔断后等待时间 |
| Sheet 熔断阈值 | `gateway.h:62` | 5 | Spreadsheet 服务连续失败次数 |
| Sheet 恢复超时 | `gateway.h:62` | 30s | Spreadsheet 熔断后等待时间 |
| File 熔断阈值 | `gateway.h:63` | 5 | File 服务连续失败次数 |
| File 恢复超时 | `gateway.h:63` | 30s | File 熔断后等待时间 |
| Redis 同步间隔 | `circuit_breaker.h` | 5s | 本地缓存有效期，超过后从 Redis 同步状态 |
| Redis 探针锁 TTL | `circuit_breaker.h` | 30s | HALF_OPEN SetNX 锁的过期时间 |
| Redis Key TTL | `circuit_breaker.h` | 120s | 所有 `cb:*` key 的生存时间 |

**Redis 注入方式**（在 Gateway Redis 连接成功后）：

```cpp
// gateway.cpp — Start() 中 redis_->Connect() 之后
if (redis_->Connect()) {
    cb_auth_.SetRedis(redis_.get());
    cb_sheet_.SetRedis(redis_.get());
    cb_file_.SetRedis(redis_.get());
    printf("[Gateway] Circuit breakers connected to Redis (shared state enabled)\n");
}
```

---

## 10. 设计模式总结

Gateway 路由层的熔断器设计可以抽象为一种**信号-决策分离**模式：

```
                   业务信号层                  决策执行层
               ┌──────────────┐          ┌──────────────────┐
               │  Handler     │          │  breaker lambda  │
               │  返回        │          │  根据信号裁决    │
               │  RpcResult   │ ──────→  │  RecordSuccess   │
               │  三态枚举    │          │  RecordFailure   │
               └──────────────┘          │  (BUSINESS不处理) │
                                         └──────────────────┘
                                                  │
                                                  ▼
                                         ┌──────────────────┐
                                         │  CircuitBreaker  │
                                         │  状态机          │
                                         │  CLOSED/OPEN    │
                                         │  HALF_OPEN       │
                                         └──────────────────┘
```

核心设计要点：

1. **Handler 层**：只返回 `RpcResult` 三态信号，不感知熔断器存在，保持纯业务逻辑
2. **breaker lambda**：作为 AOP-like 切面，统一处理熔断检查 + 状态记录 + 响应输出，是模式的核心编排点
3. **CircuitBreaker 类**：封装状态机逻辑，提供 `AllowRequest/RecordSuccess/RecordFailure` 三个原子接口，是模式的底层基础设施
4. **隔离粒度**：按后端服务分配独立熔断器实例，实现故障域隔离

这种分层使各层职责单一、可独立测试。新增一个后端服务路由只需：定义 Handler -> 创建熔断器实例 -> 注册时用 breaker 包裹，总代码增量约 3 行。
