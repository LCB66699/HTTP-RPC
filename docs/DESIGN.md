# HTTP-RPC 分布式事务系统 — 设计文档

> 最后更新: 2026-05-25

## 一、项目概述

基于 C++20/gRPC + 2PC 协议的分布式数据表格与文件管理系统，Docker 多节点部署，演示分布式事务、Cache-Aside 缓存、分库分表、多维熔断、双 Token 鉴权、LVS 四层负载均衡。

### 架构拓扑

```
Client (HTTPS)
   │
LVS + Keepalived (DR模式, VIP浮动)    ← 4层 TCP 负载均衡
   │  wlc 加权最小连接
   ├──────────────┬──────────────┐
   ▼              ▼              ▼
nginx-1 :443     nginx-2 :443   nginx-3 :443   ← TLS 终结 + 限流 + 静态文件
   │  /api/* → upstream gateway_pool least_conn
   ├──────────────────┐
   ▼                  ▼
Gateway-1 :8081      Gateway-2 :8081   ← 双 Token 鉴权 + 协议转换 + 2PC TM + 多维熔断
   │  gRPC round_robin + 信号量限流(256并发) + PerReplicaTracker
   │
   ├──→ Auth  (2副本)  DB: rpc_auth
   ├──→ Sheet (3副本)  DB: rpc_spreadsheet (2分片)  Redis 缓存
   └──→ File  (2副本)  DB: rpc_file (2分片)         MinIO 对象存储
          │
          ├── MySQL 分片 ×5 (auth×1 + spreadsheet×2 + file×2)
          ├── Redis Cluster 6节点 (3M+3S, gossip 故障转移)
          └── MinIO (S3 兼容对象存储)
```

**容器总数：27 个**（LVS×2 + nginx×3 + gateway×2 + auth×2 + sheet×3 + file×2 + MySQL×5 + Redis×7 + MinIO×1）

## 二、分层架构

### 第一层：LVS + Keepalived（4 层 TCP）

- **协议**：TCP 直接路由 (DR)
- **调度算法**：wlc（加权最小连接）
- **高可用**：Keepalived VRRP，Master/Backup VIP 漂移
- **RTO**：< 1s（advert_int=1s，秒级切换）

### 第二层：nginx（7 层 HTTP）

- **TLS 终结**：自签证书，prod 换 Let's Encrypt
- **限流**：令牌桶 100r/s burst=50 (API)，5r/m burst=2 (登录)
- **静态文件**：sendfile 零拷贝
- **负载均衡**：least_conn → gateway_pool (keepalive 128)

### 第三层：Gateway（业务 7 层）

- **鉴权**：双 Token（AT JWT 15min + RT UUID 7d Redis）
- **协议转换**：HTTP JSON ↔ gRPC Protobuf (nlohmann/json)
- **多维熔断**：滑动窗口 + P99 延迟 + 慢调用率 + 错误率 + 增强半开多探测
- **并发控制**：C++20 counting_semaphore (256)，排队 3s 超时 503
- **登录爆破**：账号级 Redis 限流 (15次/5min → 封禁 30min)
- **2PC TM**：undo_log 补偿式分布式事务

### 第四层：后端服务

- **Auth**：用户注册/登录、Token 签发、密码哈希 (PBKDF2-SHA256)
- **Spreadsheet**：表格 CRUD、乐观锁 (version CAS)、Redis 缓存
- **File**：文件管理、MinIO 对象存储、元数据 MySQL

## 三、数据层设计

### MySQL 分库

| 服务 | 分片数 | 路由键 | 说明 |
|------|--------|--------|------|
| Auth | 1 | — | 不分片，单库百万级足够 |
| Spreadsheet | 2 | `user_id % 2` | hash 分片 |
| File | 2 | `user_id % 2` | hash 分片 |

分片路由：`ShardedDatabase` 包装 N 个 `Database` 实例。无 `user_id` 的操作广播到所有分片（id 全局唯一）。

### Redis Cluster

- 6 节点 (3M+3S)，CRC16 slot 分片
- redis-plus-plus 客户端自动处理 MOVED/ASK 重定向
- 每 Gateway 独立连接池 (3 seeds × 4 conns = 12 连接)

### 数据一致性

- **Cache-Aside**：逻辑过期 300s 异步刷新 + 空值防穿透 + 版本号批量失效
- **2PC 事务**：TM Prepare → RM INSERT + undo_log → TM Commit/Rollback
- **乐观锁**：`UPDATE WHERE version=?` + 3 次冲突重试
- **缓存失效**：写操作 → MySQL 更新 → Redis INCR version（所有列表缓存自动过期）

## 四、安全设计

### 双 Token 鉴权

| | Access Token | Refresh Token |
|------|------|------|
| 格式 | JWT (HS256) | UUID v4 |
| 有效期 | 15 min | 7 days |
| 存储 | Cookie HttpOnly `rpc_at` | Redis `rt:{username}` |
| 验签 | 本地 HS256（不查 Redis） | Redis GET 比对 |
| 吊销 | 等 15min 自然过期 | DEL `rt:{username}` 立即生效 |

### 防爆破

- nginx 层：IP 维度 100r/s burst=50
- Gateway 层：账号维度 15次/5min → 封禁 30min (Redis)
- 改密：DEL `rt:{username}` → 所有设备强制重新登录

## 五、并发控制

| 层级 | 机制 | 限制值 |
|------|------|--------|
| LVS | wlc 调度 + DR 转发 | 线速 |
| nginx | `limit_req` 令牌桶 | 100r/s burst=50 |
| Gateway | `counting_semaphore<256>` | 256 并发，排队 3s |
| 熔断器 | OPEN → 503 | 触发时 |
| MySQL 写池 | `mutex ×4` 每连接 | 4 并行写 |
| MySQL 读池 | `mutex ×N` 每连接 | N 并行读 |
| CAS 乐观锁 | `UPDATE WHERE version=?` | 3 次重试 |

## 六、可观测性

- **健康检查**：`/api/health` (gateway+breaker 状态)，gRPC health check
- **系统状态**：`/api/system/status` (P99 / 错误率 / 慢调用率)
- **调用历史**：`/api/history` (Redis call_history:global, LPUSH+LTRIM 10000)
- **日志**：结构化日志 (SystemLogger)，log-level 可配

## 七、部署方式

### Docker Compose（开发/测试）

```bash
docker compose up -d --build
```

27 个容器，单机部署。

### Kubernetes（生产）

```bash
kubectl apply -k k8s/
```

StatefulSet (MySQL/Redis/MinIO) + Deployment (Gateway/Services) + Ingress + HPA

## 八、技术栈

| 层级 | 技术 |
|------|------|
| 语言 | C++20 (g++ -fcoroutines) |
| RPC | gRPC/HTTP/2 + Protobuf |
| 网关 | httplib + nghttp2 + nlohmann/json |
| 数据库 | MySQL 8.0 (master-slave, GTID 复制, ROW binlog) |
| 缓存 | Redis 7 Cluster (redis-plus-plus) |
| 对象存储 | MinIO (S3 兼容) |
| 负载均衡 | LVS (DR) + nginx |
| 部署 | Docker Compose / Kubernetes |
