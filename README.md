# HTTP-RPC 分布式事务系统

基于 gRPC + 2PC 协议的数据表格存储与查询系统，Docker 多节点部署，演示分布式事务和 Cache-Aside 缓存。

## 架构

```
浏览器 (Web UI)
   │  HTTPS
   ▼
nginx :443/80                    ← TLS 终结 + 静态文件(sendfile) + gzip + 限流(100r/s+burst50)
   │  /api/*  → least_conn 负载均衡
   ├─────────────────────────────────────┐
   ▼                                     ▼
Gateway-1 :8081                  Gateway-2 :8081   ← HttpOnly Cookie 鉴权 + TM(2PC) + 熔断器(Redis Cluster)
   │  gRPC/HTTP/2  round_robin (DNS aliases, 5s 重解析)
   │
   ├──→ Auth  (rpc-auth:50051)   副本 x2  DB: rpc_auth
   ├──→ Sheet (rpc-sheet:50051)  副本 x3  DB: rpc_spreadsheet (可 hash 分片)  Redis 缓存
   └──→ File  (rpc-file:50051)   副本 x2  DB: rpc_file (可 hash 分片)         MinIO

          ├── MySQL 分片: auth×1 + spreadsheet×2 + file×2 (每片1主1从)
          └── Redis Cluster 6节点 (3M+3S)
```

**容器总数：24 个**（nginx×1 + gateway×2 + auth×2 + sheet×3 + file×2 + MySQL×9 + Redis×7 + MinIO×1）
> MySQL 9 个 = mysql-auth ×1 + mysql-spreadsheet-0×2(master+slave) + mysql-spreadsheet-1×2 + mysql-file-0×2 + mysql-file-1×2

### 协议栈

```
浏览器 ──HTTPS──→ nginx ──HTTP/1.1 keepalive──→ Gateway(httplib 8081) ──gRPC/HTTP/2──→ 各 Service
                   least_conn 双实例均衡        HttpOnly Cookie 鉴权         Protobuf/MySQL/Redis
                   同时支持 h2c 8080（协程版）
```

### 数据流

```
写入: 客户端 → Gateway(生成XID) → [Prepare → Commit/Rollback]
      写操作: MySQL INSERT + undo_log → 失效 Redis 缓存

读取: 查 Redis → 命中返回 → 未命中查 MySQL → 回填 Redis
      逻辑过期(300s) → SetNX 加锁 → 后台线程异步刷新 → 返回旧值
```

## 项目结构

```
├── gateway-cpp/                 HTTP 网关 + TM
│   ├── include/
│   │   ├── gateway.h              路由、JWT、gRPC 代理、TM 协调
│   │   ├── circuit_breaker.h      多维熔断器（滑动窗口 + P99 + 慢调用率）
│   │   ├── coro_grpc.h            co_await gRPC (CompletionQueue)
│   │   ├── coro_task.h            C++20 Task<T> 协程
│   │   └── coro_sched.h           协程调度
│   └── src/
│       ├── main.cpp               网关入口
│       ├── gateway.cpp            路由 + 鉴权 + 熔断 + 协议转换
│       └── http2_server.cpp       nghttp2 h2c 服务器
├── server/                      gRPC 后端服务
│   ├── include/
│   │   ├── database.h             MySQL 读写分离 + ShardedDatabase 分片路由
│   │   ├── redis_client.h         Redis Cluster 客户端 (redis-plus-plus)
│   │   ├── tx_manager.h           2PC 事务管理器
│   │   ├── tx_resource.h          2PC 资源管理器基类
│   │   ├── auth_service_impl.h    认证服务
│   │   ├── spreadsheet_service_impl.h  表格 CRUD
│   │   ├── file_service_impl.h    文件管理
│   │   ├── health_service_impl.h  集群健康监控
│   │   ├── call_logger.h          调用日志
│   │   ├── system_logger.h        结构化日志 + 错误聚合
│   │   ├── auth_interceptor.h     gRPC JWT 拦截器
│   │   ├── jwt.h / sha256.h       安全工具
│   │   ├── minio_client.h         MinIO 对象存储客户端
│   │   └── snowflake.h            Snowflake 分布式 ID 生成
│   └── src/
│       ├── main.cpp               --service 参数启动不同角色
│       ├── database.cpp           MySQL 连接池 + 健康检查 + ShardedDatabase
│       ├── redis_client.cpp       RedisCluster 封装
│       ├── tx_manager.cpp         TM Begin() + 超时恢复
│       ├── tx_resource.cpp        RM Prepare/Commit/Rollback + undo_log
│       └── ...
├── proto/                       Protobuf 定义
│   ├── rpc_auth.proto             AuthService
│   ├── rpc_spreadsheet.proto      SpreadsheetService
│   ├── rpc_file.proto             FileService
│   ├── rpc_tx.proto               TxManager + TxResource (2PC)
│   └── rpc_health.proto           HealthMonitor
├── web-ui/                       Web 管理界面
├── redis/cluster/               Redis Cluster 配置
├── mysql/                       MySQL 主从配置
├── Dockerfile                   多阶段构建 (含 redis-plus-plus 源码编译)
├── docker-compose.yml           21 容器一键部署
├── init.sql                     MySQL 初始建库
└── Makefile
```

## 依赖模块

| 模块 | 协议 | 用途 | 来源 |
|------|------|------|------|
| **gRPC + Protobuf** | HTTP/2 | RPC 框架 | `apt install libgrpc++-dev` |
| **OpenSSL** | TLS/Crypto | JWT / SHA-256 / PBKDF2 | `apt install libssl-dev` |
| **libmysqlclient** | MySQL/TCP | MySQL 8.0 | `apt install libmysqlclient-dev` |
| **redis-plus-plus** | RESP/TCP | Redis Cluster 客户端 (含 slot 路由/MOVED) | 源码编译 (v1.3.15) |
| **hiredis** | RESP/TCP | redis-plus-plus 底层协议 | `apt install libhiredis-dev` |
| **nlohmann/json** | — | JSON 解析/序列化 | 单头文件 v3.11.3 |
| **nghttp2** | HTTP/2 h2c | 自定义 HTTP/2 服务器 | `apt install libnghttp2-dev` |
| **cpp-httplib** | HTTP/1.1 | 网关 HTTP/1.1 服务 | header-only |
| **SheetJS** | CDN | 前端 xlsx | cdn.sheetjs.com |

## gRPC 机制

| 机制 | 实现 | 位置 |
|------|------|------|
| **服务发现** | Docker DNS aliases（rpc-auth/rpc-sheet/rpc-file） | `docker-compose.yml` |
| **负载均衡** | gRPC `round_robin` + nginx `least_conn` | `gateway.cpp` / `nginx.conf` |
| **DNS 缓存** | 5s 重解析 | `gateway.cpp` |
| **Keepalive** | 客户端 60s，服务端 30s | 双向 |
| **超时** | `set_deadline(5s)` (upload=30s, download=600s) | 每次 RPC |
| **重试** | UNAVAILABLE 自动重试 3 次，退避 0.1s~5s；副本级重试 1 次 | `gateway.cpp` |
| **健康检查** | 内置 gRPC health check + HealthMonitor 服务 | 双检 |
| **熔断器** | 多维：连续失败 + 错误率 + 慢调用率 + P99 延迟；增强半开多探测 | `circuit_breaker.h` |
| **限流** | nginx 令牌桶 100r/s + burst 50 + 账号级封禁(Redis) | `nginx.conf` / `gateway.cpp` |
| **MySQL 重连** | CR_SERVER_LOST 自动重连 + 30s 健康检查 | `database.cpp` |

## Redis Cluster 水平扩展

已从 Redis Sentinel (1M+2S+3Sentinel) 迁移至 Redis Cluster (6节点: 3M+3S)：

| 特性 | Sentinel (改前) | Cluster (改后) |
|------|------|------|
| **客户端库** | hiredis C 直连 | redis-plus-plus C++ 封装 |
| **路由方式** | 写→master / 读→slave (手动) | CRC16 slot 自动路由 + MOVED 重定向 |
| **故障转移** | 外部 Sentinel 进程 | 内置 gossip 协议，replica 自动选举 |
| **水平扩展** | 垂直（加内存） | 水平（加节点 + reshard） |
| **代码量** | 620 行（连接池 + Sentinel + 健康检查） | 225 行（委托到 RedisCluster） |

## 熔断器增强

| 维度 | 改前 | 改后 |
|------|------|------|
| **触发条件** | 连续失败 5 次 | 连续失败(快速) + 错误率≥50% + 慢调用率≥50% + P99≥2000ms |
| **统计方式** | 简单计数器 | 滑动窗口 (6桶×10s=60s) + 9桶延迟直方图 |
| **半开探测** | 单次探针 SetNX | 5 次探针，≥80%成功率 → CLOSED |
| **状态共享** | 同已 | Redis `cb:*:state` / `cb:*:fails` / `cb:*:window:*` |
| **延迟测量** | 无 | 5 个注入点 (with_cb / login / register / file_up / file_down) |

## MySQL 分库设计

通过 `ShardedDatabase` 支持按 `user_id % N` 水平分片：

```
--mysql-shards 1   → 单 MySQL 实例 (默认，向后兼容)
--mysql-shards 4   → 4 分片: mysql-host-0~3 / rpc_db_0~3

路由规则:
  - CreateSpreadsheet(user_id, ...) → user_id % N
  - GetSpreadsheet(id, user_id)    → user_id % N
  - ListSpreadsheets(user_id, ...) → user_id % N
  - DeleteSpreadsheet(id)          → 广播 (id 全局唯一)
  - GetSpreadsheetOwner(id)        → 广播 (无可用的 user_id)
  - Auth 系列                        → shard 0 (不分片)
```

## 分布式事务 (2PC)

```
TM.Begin("tx-001")
  ├─ Prepare → SpreadsheetService (CreateSheet)
  │     ├─ INSERT + 写 undo_log({}) → YES
  │     └─ 失败 → NO
  ├─ Prepare → FileService (CreateFile)
  │     ├─ INSERT + 写 undo_log({}) → YES
  │     └─ 失败 → NO
  └─ 判定
       ├─ 全 YES → Commit 全部（清 undo_log）
       └─ 任一 NO → Rollback 全部（恢复快照 + 删数据）
```

## 集群健康监控

每个节点每 10s 心跳上报至 MySQL `health_status` 表，Gateway 通过 `/api/health` 查询所有节点在线状态，30s 无心跳判定为 OFFLINE。

## API 接口

### 认证

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/login` | 登录，响应写入 HttpOnly Cookie `rpc_token=<jwt>` |
| POST | `/api/register` | 注册，同上自动登录 |
| POST | `/api/logout` | 登出，清除 Cookie（Set-Cookie Max-Age=0） |

### 数据表格

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/sheets` | 列表（Redis 缓存；支持 `?page=0&page_size=20` 分页） |
| POST | `/api/sheets` | 创建，body: `{name, description, headers_json, data_json}` |
| POST | `/api/sheets/get` | 获取单表，body: `{id}` |
| PUT | `/api/sheets` | 更新，body: `{id, name, description, headers_json, data_json}` |
| POST | `/api/sheets/delete` | 删除，body: `{id}` |

### 文件管理

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/files/upload` | 上传（multipart，文件存 MinIO，元数据存 MySQL） |
| GET | `/api/files` | 列表（支持 `?page=0&page_size=20` 分页） |
| GET | `/api/files/download?id=1` | 下载 |
| POST | `/api/files/delete` | 删除，body: `{id}` |

### 系统

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/tx/begin` | 发起分布式事务 |
| GET | `/api/health` | 集群健康 + 熔断器状态 |
| GET | `/api/system/status` | 系统状态 (含 P99 / 错误数) |
| GET | `/api/services` | 服务列表 |
| GET | `/api/history` | 调用历史 |

## 配置参数

### Gateway

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--port` | 8080 | HTTP 端口 |
| `--grpc-auth` | rpc-auth:50051 | AuthService 地址 |
| `--grpc-sheet` | rpc-sheet:50051 | SpreadsheetService 地址 |
| `--grpc-file` | rpc-file:50051 | FileService 地址 |
| `--redis-cluster` | — | Redis Cluster 种子节点 (可重复多次) |
| `--redis-password` | — | Redis AUTH 密码 |
| `--redis-pool-size` | 4 | 每节点连接数 |

### Server

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--service` | all | 启动角色: auth / spreadsheet / file / all |
| `--port` | 50051 | gRPC 端口 |
| `--mysql-write-host` | — | MySQL 写库 |
| `--mysql-read-hosts` | — | MySQL 读库 (逗号分隔) |
| `--mysql-db` | rpc_demo | 数据库名 |
| `--mysql-shards` | 1 | 分片数 (>1 时 host 加 `-{i}` 后缀) |
| `--redis-cluster` | — | Redis Cluster 种子节点 |
| `--log-level` | info | off / error / warn / info / debug |

## Kubernetes 部署（可选）

```bash
# 一键部署至 K8s 集群
kubectl apply -k k8s/

# 查看状态
kubectl get pods -n http-rpc

# 扩缩容
kubectl scale deployment gateway -n http-rpc --replicas=5

# HPA 自动伸缩（2-10 副本，CPU 70% 触发）
kubectl get hpa gateway -n http-rpc
```

K8s manifest 文件位于 `k8s/` 目录，包含 Deployment、StatefulSet、Service、Ingress、HPA 完整部署。详见 `docs/OPS.md`。

## 线程安全

| 层级 | 并发原语 | 说明 |
|------|------|------|
| Gateway HTTP | 无共享状态 | 每请求独立线程上下文 |
| 熔断器读路径 | `atomic<State>` | 每请求调 AllowRequest，无锁 |
| 熔断器写路径 | `mutex` | 状态变迁时加锁，微秒级 |
| SlidingWindow | `mutex` | 3 个 int++，O(1) |
| 数据库写池 | `mutex × 4` | 每连接独立锁，4 路并行 |
| 数据库读池 | `mutex × N` | 每连接独立锁 |
| Redis Cluster | redis-plus-plus 内置 | 库保证线程安全 |
| PerReplicaTracker | `shared_mutex` | 读共享，写排他 |
| CallLogger | `mutex` + 后台线程 | 业务线程只操作内存（微秒），Redis 写入由后台 flush 线程异步完成 |
| 协程 CqLoop | 单线程 | 无竞争 |
