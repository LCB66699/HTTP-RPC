# HTTP-RPC 设计模式应用

## 目录

1. [Strategy（策略）](#1-strategy策略)
2. [Observer（观察者）](#2-observer观察者)
3. [Template Method（模板方法）](#3-template-method模板方法)
4. [Decorator（装饰器）](#4-decorator装饰器)
5. [Proxy（代理）](#5-proxy代理)
6. [Repository（仓库）](#6-repository仓库)
7. [Object Pool（对象池）](#7-object-pool对象池)
8. [Factory Method（工厂方法）](#8-factory-method工厂方法)
9. [Circuit Breaker（熔断器）](#9-circuit-breaker熔断器)
10. [Cache-Aside（缓存旁路）](#10-cache-aside缓存旁路)
11. [Write Invalidation + Versioned Cache（写失效+版本化缓存）](#11-write-invalidation--versioned-cache写失效版本化缓存)
12. [Two-Phase Commit（两阶段提交）](#12-two-phase-commit两阶段提交)
13. [Idempotency Key（幂等键）](#13-idempotency-key幂等键)
14. [Sharding（分片）](#14-sharding分片)

---

## 1. Strategy（策略）

### 意图

封装一系列可互换的算法，允许在运行时选择。

### 应用：三层负载均衡

整个系统在不同层使用不同策略，但对外暴露统一的路由能力。

**LVS 层 — wlc（加权最小连接）**

`lvs/keepalived.conf`:
```
virtual_server 192.168.1.100 443 {
    lb_algo wlc          ← 策略：选活跃连接最少的真实服务器
    lb_kind DR           ← 直接路由模式
}
```

**nginx 层 — least_conn + failover**

`nginx.conf:26-33`:
```nginx
upstream gateway_pool {
    least_conn;          ← 策略：七层可感知，选连接最少的 upstream
    server gateway-1:8081 max_fails=2 fail_timeout=10s;
    server gateway-2:8081 max_fails=2 fail_timeout=10s;
    keepalive 128;
}
```

**gRPC 层 — round_robin**

`gateway-cpp/src/gateway.cpp:61`:
```cpp
args.SetLoadBalancingPolicyName("round_robin");
// 策略字符串可替换为 "pick_first" / "grpclb"
```

**DNS 辅助 — 多 IP 解析 + 定时刷新**

`gateway-cpp/src/gateway.cpp:69`:
```cpp
args.SetInt(GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS, 5000);
// Docker DNS aliases 返回多个 IP → gRPC 感知 subchannel 变化
```

### 关键：统一接口

```cpp
// gateway-cpp/src/gateway.cpp:92-111 — 三个后端共用工厂
auto auth_ch_  = MakeChannel("rpc-auth:50051");   // 2 副本
auto sheet_ch_ = MakeChannel("rpc-sheet:50051");   // 3 副本
auto file_ch_  = MakeChannel("rpc-file:50051");    // 2 副本
```

---

## 2. Observer（观察者）

### 意图

定义一对多的依赖关系，当被观察者状态变化时，观察者异步处理。

### 应用：CallLogger 异步日志写入

**问题**：每次 API 调用都产生日志。同步写 Redis 会让业务线程的网络 I/O 翻倍。

**解决**：业务线程只写内存队列（微秒级），后台线程批量刷 Redis。

### 被观察者（业务线程）

`server/src/spreadsheet_service_impl.cpp:68,88` — 每次 API 结束后投递日志：
```cpp
if (logger_) {
    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    json p, r;
    p["name"] = json(req->name());
    r["id"] = json(static_cast<double>(id));
    logger_->Log(username, "SpreadsheetService", "Create", p, r, true, dur);
}
// ↑ Log() 内部: 序列化 → push_back 到 pending_ → notify_one() → 立即返回
```

### 观察者（后台线程）

`server/src/call_logger.cpp:82-105`:
```cpp
void CallLogger::FlushLoop() {
    while (running_) {
        std::vector<Entry> batch;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::milliseconds(100),
                [this] { return !pending_.empty() || !running_; });
            if (!running_ && pending_.empty()) break;
            batch.swap(pending_);               // ← 零拷贝交换
        }                                       // ← 解锁 — Redis I/O 在外面

        if (redis_ && redis_->IsConnected()) {
            for (auto& [json_str, username] : batch)
                redis_->PushCallEntry(json_str, username);
        }
    }
}
```

### 关键数据结构

`server/include/call_logger.h:48-53`:
```cpp
std::vector<std::pair<std::string, std::string>> pending_;  // 待 flush 队列
std::condition_variable cv_;                                 // 唤醒信号
std::thread flush_thread_;                                   // 后台线程
std::atomic<bool> running_{true};                            // 优雅退出
```

### SystemLogger 同模式

`server/include/system_logger.h:64-73` — 错误发生时异步 PushError 到 Redis：
```cpp
void PushError(const std::string& level, const std::string& msg) {
    // 异步聚合到 Redis errors:{service}:total
    redis_->PushCallEntry(entry);
    redis_->Increment("errors:" + service_ + ":total");
}
```

---

## 3. Template Method（模板方法）


### 意图

定义算法骨架，某些步骤延迟到调用方实现。RepRetry 的骨架（for 循环 + metadata + deadline + 重试 + 副本隔离）固定，唯一钩子是"调哪个 RPC"。

| | Strategy（三层 LB） | Template Method（RepRetry） |
|---|---|---|
| 替换粒度 | 整个算法 | 算法中的**一个步骤** |
| 骨架 | 框架提供，不可见 | `RepRetry` 自己写死 |
| 钩子 | 无 | `rpc_fn` — 调哪个 RPC |

> 如果用 Strategy 实现，调用方可以换"不重试"策略。但 `RepRetry` 的重试逻辑不可替换——只能填"调哪个 RPC"这一个坑。所以是模板方法。
                     F&& rpc_fn,                    // ← 变化的部分
                     std::string& out_peer) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        grpc::ClientContext ctx;
        // ① 注入 metadata（所有 RPC 共用）
        ctx.AddMetadata("username", username);
        ctx.AddMetadata("authorization", "Bearer " + raw_token);
        ctx.set_deadline(...);

        // ② 调用实际 RPC（变化的部分）
        auto [st, ok] = rpc_fn(&ctx);

        // ③ 统一结果处理（所有 RPC 共用）
        out_peer = ctx.peer();
        if (ok) { rep.RecordSuccess(out_peer); return true; }
        rep.RecordFailure(out_peer);
        if (rep.AllowReplica(out_peer)) return false;
    }
    return false;
}
```

**变化的部分** — 调用方只需定义"调哪个 RPC"：

```cpp
// Sheet Create（写操作，2s deadline）
RepRetry(rep, username, raw_token, 2,
    [&](grpc::ClientContext* ctx) {
        auto st = sheet_stub_->CreateSpreadsheet(ctx, req, &resp);
        return std::pair{st, st.ok() && resp.success()};
    }, peer);

// Sheet List（读操作，1s deadline）
RepRetry(rep, username, raw_token, 1,
    [&](grpc::ClientContext* ctx) {
        auto st = sheet_stub_->ListSpreadsheets(ctx, req, &resp);
        return std::pair{st, st.ok() && resp.success()};
    }, peer);
```

四个读、四个写操作共用一个模板，差别只在 lambda 里的一行 `stub_->Xxx()`。

---

## 4. Decorator（装饰器）

### 意图

动态给对象附加额外行为，不修改原对象。

### 应用：with_cb 包装路由处理器

纯业务 handler 不知道熔断和信号量：

```cpp
auto sh_list = [this](auto& req, std::string& r) {
    VerifyAuth(...);
    return HandleSheetList(u, uid, page, page_size, tok, rep_sheet_, r);
};
```

`with_cb` 包一层，不改 `sh_list` 一行代码：

```cpp
// gateway-cpp/src/gateway.cpp:1215-1221
svr.Get("/api/sheets",       with_cb(cb_sheet_, sh_list));
svr.Post("/api/sheets",      with_cb(cb_sheet_, sh_create));
svr.Put("/api/sheets",       with_cb(cb_sheet_, sh_update));
svr.Post("/api/sheets/get",  with_cb(cb_sheet_, sh_get));
// file、file upload 等同理
```

内部加了四个步骤（`gateway.cpp:1128-1170`）：

```
① cb.AllowRequest()        → 503 终止
② sem_->try_acquire_until() → 503 终止
③ inner(req, r)            → 真正干活
④ cb.RecordResult()        → 反馈熔断器
```

> `with_cb` 不是 Chain of Responsibility——四个步骤写死在一个 lambda 里，不可插拔。真正的"链"在跨层请求路径上（见下方）。

### 跨层请求链（Chain of Responsibility 的体现）

每个节点位于不同组件，独立判断，失败即终止：

```
nginx 令牌桶(429) → Gateway JWT 验签(401) → with_cb 熔断+排队(503) → RepRetry(副本切换)
     ↑                      ↑                       ↑                      ↑
  限流策略              认证策略               韧性策略               传输策略
```

---

## 5. Proxy（代理）

### 意图

为另一对象提供代理，控制访问。

### 应用：Gateway 作为 HTTP→gRPC 协议代理

```
浏览器 ──HTTPS──→ nginx ──HTTP/1.1──→ Gateway ──gRPC/HTTP/2──→ Service
                                          │
                                    协议转换: JSON ↔ Protobuf
                                    鉴权: JWT verify
                                    熔断/重试/负载均衡
```

`gateway-cpp/src/gateway.cpp` 中的每个 handler：

```cpp
auto sh_list = [this](auto& req, std::string& r) {
    // 1. JWT 验签（代理层鉴权，Service 不需要重复做）
    std::string u; int64_t uid = 0; std::string tok;
    if (!VerifyAuth(req.get_header_value("Cookie"), u, uid, tok))
        return AUTH_FAILURE;

    // 2. 从 HTTP query 参数提取 → 构造 gRPC 请求
    int page = stoi(req.get_param_value("page"));
    int page_size = stoi(req.get_param_value("page_size"));

    // 3. 调 gRPC（代理真正的后端）
    return HandleSheetList(u, uid, page, page_size, tok, rep_sheet_, r);
};
```

Service 完全不感知 HTTP，只处理 Protobuf → MySQL。代理层屏蔽了网络协议、超时重试、负载均衡的全部复杂度。

---

## 6. Repository（仓库）

### 意图

封装数据访问逻辑，业务层不写 SQL。

### 应用：Database 类

`server/include/database.h`:
```cpp
class Database {
public:
    bool CreateSpreadsheet(int64_t user_id, ...);
    bool GetSpreadsheet(int64_t id, int64_t user_id, SpreadsheetRow& out);
    bool ListSpreadsheets(int64_t user_id, std::vector<SpreadsheetSummary>& out, ...);
    bool UpdateSpreadsheet(int64_t id, int64_t user_id, ...);
    bool DeleteSpreadsheet(int64_t id, int64_t user_id);
    // File CRUD, Auth, etc.
};
```

`server/src/spreadsheet_service_impl.cpp:54`:
```cpp
// Service 层不写 SQL
bool ok = db_->CreateSpreadsheet(req->user_id(), username, ...);
```

`server/src/database.cpp:580-590` — Repository 内部封装 SQL：
```cpp
std::string sql = "INSERT INTO spreadsheets (...) VALUES (...)";
if (mysql_query(conn, sql.c_str()) == 0) ...;
```

---

## 7. Object Pool（对象池）

### 意图

复用开销大的对象，避免频繁创建销毁。

### MySQL 连接池

`server/src/database.cpp:55-82`:
```cpp
Database::Database(...) {
    write_conns_.reserve(pool_size_);       // ← 写池 4 连接
    for (int i = 0; i < pool_size_; i++) {
        write_conns_.push_back({ConnectMYSQL(write_host_, write_port_), ...});
    }
    read_conns_.reserve(read_hosts_.size());
    for (auto& host : read_hosts_) {
        read_conns_.push_back({ConnectMYSQL(host, read_port_), ...});
    }
}
```

30 秒健康检查 + 死连接自动重连（`database.cpp:751-779`）。

### Redis 连接池

`server/src/redis_client.cpp:27-28`:
```cpp
sw::redis::ConnectionPoolOptions pool_opts;
pool_opts.size = pool_size_;    // ← 每种子 4 连接，3 种子 = 12 连接
```

### gRPC Channel 复用

单例创建，不每次请求都建新 Channel：
```cpp
// gateway.h:53-55
std::shared_ptr<grpc::Channel> auth_ch_;   // ← 复用，不是每次 new
std::shared_ptr<grpc::Channel> sheet_ch_;
std::shared_ptr<grpc::Channel> file_ch_;
```

---

## 8. Factory Method（工厂方法）

### 意图

将对象创建封装为方法，隔离创建逻辑。

### 应用：MakeChannel — 唯一真正的工厂方法

`gateway-cpp/src/gateway.cpp:36-73`:
```cpp
static std::shared_ptr<grpc::Channel> MakeChannel(const std::string& addr) {
    grpc::ChannelArguments args;
    args.SetLoadBalancingPolicyName("round_robin");
    args.SetServiceConfigJSON(kRetryPolicy);
    // keepalive, DNS, message size, backoff 共 8 个参数...
    return grpc::CreateCustomChannel(addr,
                grpc::InsecureChannelCredentials(), args);
}
```

调用方不 new Channel（不配置 8 个参数），只传地址：
```cpp
auth_ch_  = MakeChannel("rpc-auth:50051");   // gateway.cpp:116
sheet_ch_ = MakeChannel("rpc-sheet:50051");   // gateway.cpp:120
file_ch_  = MakeChannel("rpc-file:50051");    // gateway.cpp:124
health_ch_= MakeChannel("rpc-sheet:50051");   // gateway.cpp:128
```

> 注意：`CreateAccessToken`/`CreateRefreshToken` 返回的是 `std::string`（基础类型），不是复杂对象，不属于工厂方法。

---

## 9. Circuit Breaker（熔断器）

### 意图

故障实例快速失败，不给故障后端继续压力，等待恢复。

### 状态机

`gateway-cpp/include/circuit_breaker.h`:
```
CLOSED ──(条件触发)──→ OPEN ──(超时)──→ HALF_OPEN ──(探测)──→ CLOSED/OPEN
```

**CLOSED→OPEN 多维触发**:
| 维度 | 阈值 | 代码 |
|------|------|------|
| 连续失败 | ≥5 次 | `failure_threshold = 5` |
| 错误率 | ≥50% (窗口内) | `error_ratio` |
| 慢调用率 | ≥50% (窗口内) | `slow_call_ratio` |
| P99 延迟 | ≥2000ms (窗口内) | `latency_threshold_ms` |

**HALF_OPEN 多探测** — 5 次探针，≥80% 成功才 → CLOSED:
`circuit_breaker.h:418-462`

**Redis 状态共享** — gateway-1/2 通过 Redis 同步熔断器状态:
`circuit_breaker.h:485-570` — `SyncFromRedis()` 每 2s 一次

### 使用

```cpp
// gateway.h:66-68 — 每个后端独立熔断器
CircuitBreaker cb_auth_{"auth", 5, 15};
CircuitBreaker cb_sheet_{"sheet", 5, 15};
CircuitBreaker cb_file_{"file", 5, 15};
```

---

## 10. Cache-Aside（缓存旁路）

### 意图

缓存和数据库独立，应用直接管理两者的读写。

### 变体：逻辑过期 + 物理 TTL 双重控制

```
读:
  Redis 命中 ──→ 检查逻辑过期(300s)
    ├─ 未过期 ──→ 返回缓存
    └─ 已过期 ──→ 返回旧缓存 + SetNX 异步刷新

  Redis 未命中 ──→ 查 MySQL ──→ 回写 Redis(JitteredTTL)
```

`server/src/spreadsheet_service_impl.cpp:93-264`:
```cpp
// 1) 查 Redis
if (redis_->GetJSON(cache_key, cached)) {
    if (cached == "__NULL__") return NotFound;    // 空值缓存
    resp->ParseFromString(cached);

    // 逻辑过期检测
    int64_t now_ts = std::time(nullptr);
    int64_t cached_ts = stoll(ts_str);
    if (now_ts - cached_ts > 300) need_refresh = true;

    // 只有一个线程去刷新
    if (need_refresh && redis_->SetNX(lock_key, "1", LOCK_TTL)) {
        std::thread(async_refresh, ...).detach();   // 异步
    }
    return OK;   // ← 立即返回旧数据，不等待刷新完成
}

// 2) Redis miss → MySQL
if (db_->GetSpreadsheet(id, uid, row)) {
    // 回写缓存（TTL 加 jitter 防雪崩）
    redis_->SetJSON(cache_key, serialized,
        RedisClient::JitteredTTL(PHYSICAL_TTL, 600));
}
```

### 双重 TTL

| TTL | 值 | 作用 |
|-----|-----|------|
| LOGICAL_TTL | 300s | 触发异步刷新 |
| PHYSICAL_TTL | 3600s + jitter(0~600s) | 硬上限防内存泄漏 |
| NULL_TTL | 60s + jitter(0~30s) | 穿透防护 |

---

## 11. Write Invalidation + Versioned Cache（写失效+版本化缓存）

### 实体缓存：懒删除

```cpp
// spreadsheet_service_impl.cpp:388-389 — 更新时删除单实体缓存
redis_->DeleteKey("u:" + uid + ":sheet:" + id);
redis_->DeleteKey("u:" + uid + ":sheet:" + id + ":ts");
```

### 列表缓存：版本号自增失效

```cpp
// spreadsheet_service_impl.cpp:22-28
static std::string VersionKey(int64_t uid) {
    return "u:" + std::to_string(uid) + ":sheets:version";   // 单调递增
}
static std::string ListCacheKey(int64_t uid, int64_t version, int page, int sz) {
    return "u:" + uid + ":sheets:v" + version + ":p" + page + ":ps" + sz;
}
```

写操作触发：
```cpp
redis_->Increment(VersionKey(req->user_id()));   // version+1 → 旧 key 自然找不到
```

**原理**：列表有无限可能的 page+page_size 组合，逐条删除不现实。版本号一跳，旧缓存"不存在" → 冷填充新 key。旧 key 靠 120s 短 TTL 自然过期。

---

## 12. Two-Phase Commit（两阶段提交）

### 意图

跨服务事务（Sheet + File）原子提交或回滚。

### 实现

`server/src/tx_manager.cpp` / `server/src/tx_resource.cpp`:

```
TM.Begin("tx-001")
  │
  ├─ Phase 1 Prepare:
  │   ├─ SpreadsheetService: INSERT + undo_log → YES
  │   └─ FileService: INSERT + undo_log → YES
  │
  └─ Phase 2 Decide:
       ├─ 全 YES → Commit All（清 undo_log）
       └─ 任一 NO → Rollback All（undo_log 回放 + 删数据）
```

### proto 接口

`proto/rpc_tx.proto`:
```protobuf
service TxManager  { rpc Begin(BeginRequest) returns (BeginResponse); }
service TxResource { rpc Prepare(PrepareRequest) returns (PrepareResponse);
                     rpc Commit(CommitRequest) returns (CommitResponse);
                     rpc Rollback(RollbackRequest) returns (RollbackResponse); }
```

---

## 13. Idempotency Key（幂等键）

### 意图

gRPC 重试不产生重复数据。

### 实现

`gateway-cpp/src/gateway.cpp`:
```cpp
// 客户端传 X-Idempotency-Key 头 → gRPC metadata
freq.set_idempotency_key(req.get_header_value("X-Idempotency-Key"));
```

`server/src/database.cpp`:
```sql
INSERT INTO spreadsheets (...) VALUES (...)
ON DUPLICATE KEY UPDATE id=id   -- 幂等：重复请求返回相同结果，不报错
```

CreateSheet/CreateFile 的 gRPC retry policy 被排除（`gateway.cpp:43-46`），因为幂等由应用层 key 保证，不需要传输层重试。

---

## 14. Sharding（分片）

### 意图

按 user_id 将数据分布到多个 MySQL 实例。

### 路由规则

`server/src/database.cpp ShardedDatabase`:
```cpp
Database* ShardFor(int64_t user_id) {
    return shards_[user_id % shard_count_];
}
```

| 操作 | 路由 |
|------|------|
| Create/Get/List Sheet | `user_id % N` — 单分片 |
| Delete/GetOwner Sheet | 广播所有分片（id 全局唯一，无 user_id） |
| Auth 操作 | shard 0（不分片） |

### docker-compose 配置

```yaml
sheet-1:
  command: ... --mysql-shards 2
  # 运行时: mysql-spreadsheet-0, mysql-spreadsheet-1
```

---

## 模式分布图

```
┌──────────────────────────────────────────────────────────────────┐
│ Gateway                                                          │
│  ┌─────────┐  ┌───────────┐  ┌─────────┐  ┌──────────────────┐  │
│  │ Strategy │  │ Decorator │  │ Factory │  │ Chain of Resp.   │  │
│  │ LB 策略  │  │ with_cb() │  │ Method  │  │ 限流→鉴权→熔断   │  │
│  └─────────┘  └───────────┘  └─────────┘  │ →排队→RPC→反馈   │  │
│                                             └──────────────────┘  │
│  ┌──────────┐  ┌────────────────┐                                │
│  │ Template │  │ Circuit Breaker│                                │
│  │ Method   │  │ 3 backend instances                              │
│  │ RepRetry │  └────────────────┘                                │
│  └──────────┘                                                     │
├──────────────────────────────────────────────────────────────────┤
│ Server (gRPC Service)                                             │
│  ┌──────────┐  ┌──────────┐  ┌────────────┐  ┌──────────┐       │
│  │ Repository│  │ Observer │  │ Cache-Aside │  │ Versioned│       │
│  │ Database │  │ CallLogger│  │逻辑过期双重│  │ Cache    │       │
│  └──────────┘  └──────────┘  │ TTL        │  │ INCR ver │       │
│                               └────────────┘  └──────────┘       │
│  ┌──────────┐  ┌──────────┐  ┌──────────────┐                    │
│  │ 2PC      │  │ Idempot. │  │ Object Pool  │                    │
│  │ TM + RM  │  │ Key      │  │ MySQL+Redis   │                    │
│  └──────────┘  └──────────┘  │ +gRPC Channel │                    │
│                               └──────────────┘                    │
├──────────────────────────────────────────────────────────────────┤
│ Data Layer                                                        │
│  ┌──────────┐  ┌──────────┐                                      │
│  │ Sharding │  │ Proxy    │                                      │
│  │ user%N   │  │ MinIO S3  │                                      │
│  └──────────┘  └──────────┘                                      │
└──────────────────────────────────────────────────────────────────┘
```
