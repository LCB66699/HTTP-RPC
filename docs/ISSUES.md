# 已知问题与架构不足

## 一、SQL 分库路由

### 1.1 取模路由无法在线扩缩容

**现状**：`user_id % shard_count` 决定数据落在哪个分片。当前 shard_count=2。

**问题**：增加分片（如 2→3）会导致几乎所有数据的路由结果变化，必须全量迁移。没有用一致性哈希或虚拟桶。

**影响**：业务增长后无法在不中断服务的情况下扩容。

### 1.2 Auth 表不分片，单点瓶颈

**现状**：`ShardedDatabase` 对 auth 操作强制走 `shards_[0]`，用户认证不可水平扩展。

**影响**：用户量增长后 auth 库先成为瓶颈。

### 1.3 广播查询 O(n)

**现状**：`GetFileStoragePath`、`GetUndoLog` 等按 `id` 查询的方法，因缺少 `user_id` 无法定位分片，只能遍历所有分片逐个查询，第一个命中即返回。

```cpp
// database.cpp:922
for (auto& db : shards_)
    if (db->GetFileStoragePath(id, storage_path)) return true;
```

**影响**：2 分片时影响不大，分片数增加后每次查询的延迟线性增长。

---

## 二、Redis Cluster

### 2.1 单 seed 初始化

**现状**：`RedisClient::Connect()` 只取 `cluster_seeds_[0]` 建立初始连接，其余 seed 被忽略。

```cpp
// redis_client.cpp:18
opts.host = cluster_seeds_[0].substr(0, colon);
```

**问题**：若第一个 seed 恰好不可达，构造函数抛异常，整个 Redis 功能降级关闭，即使其他 2 个 seed 正常。

**缓解**：redis-plus-plus 连接成功后会自动发现完整拓扑，后续节点故障不影响。仅影响初始连接那一刻。

### 2.2 未启用 READONLY 从节点读

**现状**：所有 Redis 操作路由到 master 节点。

**影响**：读压力集中在 3 个 master，3 个 slave 只做冗余不承担读流量。

### 2.3 容器 IP 漂移导致集群分裂

**现状**：`docker compose up -d --force-recreate` 重建 Redis 容器时 Docker 可能分配新 IP，`nodes.conf` 存的是旧 IP，节点互相找不到。

**已修复**：`redis/cluster/init.sh` 智能初始化脚本自动检测 IP 漂移并重建集群。详见 commit。

---

## 三、gRPC 负载均衡与故障隔离

### 3.1 File upload/download 重复实现重试逻辑

**现状**：Sheet CRUD 走 `RepRetry` 模板函数处理重试+副本隔离，File upload/download 手写了一份相同的 for 循环逻辑。

```cpp
// gateway.cpp:954 — 手写版（file upload）
for (int attempt = 0; attempt < 2; ++attempt) {
    st = file_stub_->CreateFile(&ctx, freq, &fresp);
    // ...
}
```

**影响**：两套代码维护，逻辑可能不一致，File 路径缺少对 `RecordResult` 时机的精细控制。

### 3.2 ShouldTripService 未接入熔断器

**现状**：`PerReplicaTracker::ShouldTripService()` 已实现（超过半数副本隔离时返回 true），但全代码库无人调用。

```cpp
// circuit_breaker.h — PerReplicaTracker
bool ShouldTripService() const {
    // 超过一半副本被隔离 → 应触发服务级熔断
}
```

**影响**：副本级隔离和服务级熔断各自独立决策，大面积故障时熔断响应不及时。

### 3.3 gRPC keepalive 触发 ENHANCE_YOUR_CALM

**现状**：Gateway 每 60s 发 keepalive ping（`GRPC_ARG_KEEPALIVE_TIME_MS=60000`），后端服务端认为太频繁，定期发送 GOAWAY 掐断连接。

**影响**：gRPC 连接被周期性重置，期间请求可能短暂失败（自动重试可恢复）。

---

## 四、全链路保护隔离（已实现但联动不足）

```
请求 → ① cb.AllowRequest()        熔断器（服务级）
     → ② sem_->try_acquire()       并发控制（Gateway 级）
     → ③ inner() → RepRetry()     副本隔离（IP:port 级）
     → ④ cb.RecordResult()         熔断反馈
```

| 层级 | 组件 | 粒度 | 触发条件 | 动作 |
|------|------|------|---------|------|
| L1 | `sem_` 信号量 | 全 Gateway | 并发 > max_concurrent | 排队超时 → 503 |
| L2 | `CircuitBreaker` | 服务级 | 滑动窗口错误率/慢调用率超阈值 | 快速失败 503 |
| L3 | `PerReplicaTracker` | 副本 IP:port | 连续失败 5 次 | 隔离 30s，RepRetry 换副本 |

**不足**：三层各自独立决策，L3 大面积故障时不会通知 L2 提前熔断。`ShouldTripService()` 已实现但未接入。

---

## 五、前端认证（已修复）

### 5.1 checkAuth 盲信 localStorage

**问题**：`checkAuth()` 只用 `localStorage.rpc_user` 判断登录状态，但 JWT Cookie（HttpOnly, 15min）过期后 localStorage 仍在，导致"看到主界面但 API 全 401"。

**修复**：新增 `GET /api/me` 端点，`checkAuth()` 改为先调此接口验证 Cookie 有效性。

---

## 六、部署配置（已修复）

### 6.1 nginx 未映射端口

**修复**：`nginx-1` 添加 `ports: "80:80"` 和 `ports: "443:443"`。

### 6.2 MySQL 容器缺网络别名

**修复**：`mysql-spreadsheet-0/1` 添加 `aliases: [mysql-spreadsheet]`，`mysql-file-0/1` 添加 `aliases: [mysql-file]`。

### 6.3 Redis 容器缺端口专用别名

**修复**：6 个 Redis 节点各添加端口专用别名（`redis-cluster-7000` ~ `redis-cluster-7005`），所有服务改用专用别名连接。

### 6.4 init.sql 数据库名不匹配分片命名

**修复**：`init.sql` 改为创建 `rpc_spreadsheet_0`、`rpc_spreadsheet_1`、`rpc_file_0`、`rpc_file_1`。

### 6.5 Gateway 透传空 token 到后端

**修复**：`VerifyAccessToken` 新增 `raw_token` 输出参数，`VerifyAuth` 透传真实 JWT 给后端 gRPC 认证。

### 6.6 日志缓冲不可见

**修复**：`server/src/main.cpp` 添加 `setbuf(stdout, NULL); setbuf(stderr, NULL)`。

---

## 七、LVS

### 7.1 VIP 为内网地址

**现状**：VIP 默认 `192.168.1.100`，云服务器外部不可达，LVS 实际未生效。

### 7.2 仅 TCP_CHECK

**现状**：LVS 健康检查只验证端口可达，不感知 nginx 应用层死活（如 worker 全卡死但端口仍 listening）。
