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
Gateway-1 :8081                  Gateway-2 :8081   ← HttpOnly Cookie 鉴权 + TM(2PC) + 熔断器(Redis共享)
   │  gRPC/HTTP/2  round_robin (DNS aliases, 5s 重解析)
   │
   ├──→ Auth  (rpc-auth:50051)   副本 x2  DB: rpc_auth
   ├──→ Sheet (rpc-sheet:50051)  副本 x3  DB: rpc_spreadsheet  Redis 缓存
   └──→ File  (rpc-file:50051)   副本 x2  DB: rpc_file
          │
          ├── MySQL 1主2从 :3306  (rpc_auth / rpc_spreadsheet / rpc_file / rpc_tx_log)
          └── Redis 1主2从 + 3哨兵 :6379
```

**容器总数：19 个**（nginx×1 + gateway×2 + auth×2 + sheet×3 + file×2 + MySQL×3 + Redis×6）

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
```

## 项目结构

```
├── gateway-cpp/                 HTTP 网关 + TM
│   ├── gateway.cpp/h             路由、JWT、gRPC 代理、TM 协调
│   └── main.cpp                  网关入口
├── server/                      gRPC 后端服务
│   ├── main.cpp                  --service 参数启动不同角色
│   ├── database.cpp/h            MySQL 层（4 库 6 表 + undo_log + tx_log）
│   ├── redis_client.cpp/h        Redis 客户端（缓存 + 断线重连）
│   ├── tx_manager.cpp/h          2PC 事务管理器（Prepare/Commit/Rollback）
│   ├── tx_resource.cpp/h         2PC 资源管理器基类
│   ├── health_service_impl.cpp/h 集群健康监控（心跳上报 + 查询）
│   ├── auth_service_impl.cpp/h   登录/注册
│   ├── spreadsheet_service_impl.cpp/h  表格 CRUD
│   ├── file_service_impl.cpp/h   文件管理
│   ├── call_logger.cpp/h         调用日志
│   ├── auth_interceptor.cpp/h    gRPC JWT 拦截器（预留）
│   ├── jwt.h、sha256.h           安全工具
│   ├── httplib.h                 cpp-httplib HTTP/1.1 库
│   └── rpc_json.h/cpp            自定义 JSON 库
├── proto/                       Protobuf 定义
│   ├── rpc_auth.proto            AuthService
│   ├── rpc_spreadsheet.proto     SpreadsheetService
│   ├── rpc_file.proto            FileService
│   ├── rpc_tx.proto              TxManager + TxResource (2PC)
│   └── rpc_health.proto          HealthMonitor（集群健康）
├── web-ui/                      Web 管理界面
│   ├── index.html                四面板：服务/表格/文件/个人
│   ├── app.js                    xlsx 导入导出 + 在线编辑 + 文件管理
│   └── style.css
├── Dockerfile                   多阶段构建，单镜像多角色
├── docker-compose.yml           19 容器一键部署
├── init.sql                     MySQL 初始建库
└── Makefile
```

## 依赖模块

| 模块 | 协议 | 用途 | 来源 |
|------|------|------|------|
| **gRPC + Protobuf** | HTTP/2 | RPC 框架（含 round_robin LB + 5s DNS 重解析） | `apt install libgrpc++-dev` |
| **OpenSSL** | TLS/Crypto | JWT / SHA-256 / PBKDF2 密码哈希 | `apt install libssl-dev` |
| **libmysqlclient** | MySQL/TCP | MySQL 8.0 | `apt install libmysqlclient-dev` |
| **hiredis** | RESP/TCP | Redis | `apt install libhiredis-dev` |
| **cpp-httplib** | HTTP/1.1 | 网关 HTTP 服务 | [github.com/yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) |
| **SheetJS** | CDN | 前端 xlsx | [cdn.sheetjs.com](https://cdn.sheetjs.com) |

## gRPC 机制

| 机制 | 实现 | 位置 |
|------|------|------|
| **服务发现** | Docker DNS aliases（rpc-auth/rpc-sheet/rpc-file） | `docker-compose.yml` |
| **负载均衡** | gRPC `round_robin` + nginx `least_conn`（双 gateway） | `gateway.cpp` / `nginx.conf` |
| **DNS 缓存** | 5s 重解析（实例下线后最多 5s 感知） | `gateway.cpp` |
| **Keepalive** | 客户端 10s PING，服务端 30s PING | 双向 |
| **超时** | `set_deadline(5s)` | 每次 RPC |
| **重试** | UNAVAILABLE 自动重试 3 次，退避 0.1s~5s | `gateway.cpp` |
| **健康检查** | `EnableDefaultHealthCheckService` + HealthMonitor 服务 | 双检 |
| **熔断器** | CLOSED→OPEN→HALF_OPEN，状态共享到 Redis（跨 gateway 实例一致） | `circuit_breaker.h` |
| **限流** | nginx 令牌桶 100r/s + burst 50，超限返回 429 | `nginx.conf` |
| **MySQL 重连** | CR_SERVER_LOST 自动重连 | `database.cpp` |
| **Redis 重连** | 每次操作前 ReconnectIfNeeded | `redis_client.cpp` |

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

| 组件 | 文件 | 职责 |
|------|------|------|
| TM | `server/tx_manager.cpp` | 生成 XID、协调 2PC、超时恢复 |
| RM 基类 | `server/tx_resource.cpp` | Prepare/Commit/Rollback 模板 + undo_log |
| Handler | `server/main.cpp` | CreateSheet/DeleteSheet/CreateFile/DeleteFile 业务具体实现 |

## 集群健康监控

每个节点每 10s 心跳上报至 MySQL `health_status` 表，Gateway 通过 `/api/health` 查询所有节点在线状态，30s 无心跳判定为 OFFLINE。

## API 接口

### 认证

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/login` | 登录，响应写入 HttpOnly Cookie `rpc_token=<jwt>` |
| POST | `/api/register` | 注册，同上自动登录 |
| POST | `/api/logout` | 登出，清除 Cookie（Set-Cookie Max-Age=0） |

**Cookie 鉴权**：登录/注册成功后 Gateway 在响应头写入 `Set-Cookie: rpc_token=<jwt>; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=86400`。后续受保护接口通过浏览器自动携带的 Cookie 鉴权，无需手动设置 Header。

- `HttpOnly`：JS 无法读取，防 XSS 窃取
- `SameSite=Strict`：防 CSRF 跨站攻击
- `Secure`：仅 HTTPS 传输

**Token 结构**：JWT payload 含 `username`、`uid`（用户 ID）、`ver`（token 版本号）、`exp`（过期时间戳）。版本号来自 MySQL `users.token_version`，Redis 缓存 `token_ver:{username}`，`VerifyAuth` 校验版本号实现 token 吊销。

### 数据表格

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/sheets` | 列表（Redis 缓存；支持 `?page=0&page_size=20` 分页） |
| POST | `/api/sheets` | 创建（写 MySQL，失效 Redis），body: `{name, description, headers_json, data_json}` |
| POST | `/api/sheets/get` | 获取单表，body: `{id}` |
| PUT | `/api/sheets` | 更新，body: `{id, name, description, headers_json, data_json}` |
| POST | `/api/sheets/delete` | 删除，body: `{id}` |

> 注：Sheet 的 Get/Delete 使用 POST 而非 GET/DELETE，因前端通过 fetch body 传递 id，兼容 Cookie 鉴权模式。

### 文件管理

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/files/upload` | 上传（multipart） |
| GET | `/api/files` | 列表（支持 `?page=0&page_size=20` 分页） |
| GET | `/api/files/download?id=1` | 下载 |
| POST | `/api/files/delete` | 删除，body: `{id}` |

### 事务

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/tx/begin` | 发起分布式事务 |

### 监控

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/health` | 集群健康仪表盘 |
| GET | `/api/services` | 服务列表 |
| GET | `/api/history` | 调用历史 |

## 分页设计

列表接口统一采用 **0-based offset 分页**，`page_size=0` 时退化为返回全量（向后兼容）。

### gRPC 层（proto）

```protobuf
message ListSpreadsheetsRequest {
  string username  = 1;
  int32  page      = 2;   // 0-based，默认 0
  int32  page_size = 3;   // 0 = 全量（向后兼容）
}
// ListFilesRequest 结构相同
```

响应始终返回 `total`（数据库中该用户的总行数，独立 `COUNT(*)` 查询获得）和当页数据数组。

### 数据库层（MySQL）

```sql
-- 第 1 步：独立 COUNT，不拉取任何行数据
SELECT COUNT(*) FROM spreadsheets WHERE username = ?

-- 第 2 步：分页数据（page_size > 0 时追加）
SELECT id, name, ...
FROM spreadsheets
WHERE username = ?
ORDER BY updated_at DESC
LIMIT {page_size} OFFSET {page * page_size}
```

### Redis 缓存 key（含分页维度）

分页结果按维度独立缓存，避免不同页的结果互相污染：

```
user:{name}:sheets:v{ver}                        # page_size=0（全量，向后兼容）
user:{name}:sheets:v{ver}:p{page}:ps{page_size}  # 分页时追加后缀
```

写操作（Create / Update / Delete）通过 `INCR user:{name}:sheets:version` 使所有分页缓存自动失效。

---

## Redis Key 设计

统一命名规范：`{namespace}:{entity}:{id}[:{suffix}]`

| Key | 类型 | TTL | 说明 |
|-----|------|-----|------|
| `sheet:{id}` | protobuf binary | 300s | 单表完整数据 |
| `file:{id}` | protobuf binary | 300s | 单文件元数据 |
| `user:{name}:sheets:version` | int | 永久 | 表格列表缓存版本号（写操作 INCR） |
| `user:{name}:files:version` | int | 永久 | 文件列表缓存版本号（写操作 INCR） |
| `user:{name}:sheets:v{ver}[:p{p}:ps{ps}]` | protobuf binary | 120s | 表格列表缓存（含分页后缀） |
| `user:{name}:files:v{ver}[:p{p}:ps{ps}]` | protobuf binary | 120s | 文件列表缓存（含分页后缀） |
| `token_ver:{name}` | string (int) | 86400s | JWT token 版本号（Gateway Cookie 鉴权用） |
| `cb:{svc}:state` | string (0/1/2) | 120s | 熔断器状态（0=CLOSED,1=OPEN,2=HALF_OPEN），跨 gateway 共享 |
| `cb:{svc}:fails` | int | 120s | 熔断器连续失败计数，跨 gateway 累计 |
| `cb:{svc}:opened` | string (timestamp) | 120s | 熔断器打开时间点（Unix 秒），用于 timeout 判断 |
| `cb:{svc}:probe` | string | 30s | 半开探针锁（SetNX），防止多实例同时发出探针 |
| `call_history:global` | list | 30天滚动 | 全局调用日志（所有用户，LTRIM 10000） |
| `call_history:{name}` | list | 30天滚动 | 单用户调用日志（LTRIM 10000） |
| `errors:{svc}:total` | int | 永久 | 各服务错误计数 |

**call_history 双写机制**：每次调用同时写入 `call_history:global`（全局聚合，供监控面板使用）和 `call_history:{username}`（按用户隔离，供用户级别查询使用）。两个 list 均在每次 `LPUSH` 后执行 `EXPIRE 2592000`（30 天滚动 TTL），并通过 `LTRIM` 控制容量上限为 10000 条。

---

## 数据库索引设计

所有索引在 `Database::Initialize()` 中幂等创建（失败忽略），支持滚动升级。

### spreadsheets 表

| 索引名 | 列 | 类型 | 作用 |
|--------|----|------|------|
| PRIMARY | `id` | 主键 | 按 id 点查 |
| `idx_sheets_user_time` | `(username, updated_at DESC)` | 复合 | 覆盖 `WHERE username=? ORDER BY updated_at DESC LIMIT n`，消除 filesort |

**设计要点**：`username` 在前用于等值过滤，`updated_at DESC` 在后与 ORDER BY 方向一致，MySQL 可直接按索引顺序输出，无需额外排序。

### files 表

| 索引名 | 列 | 类型 | 作用 |
|--------|----|------|------|
| PRIMARY | `id` | 主键 | 按 id 点查 |
| `idx_files_user_time` | `(username, created_at DESC)` | 复合 | 覆盖 `WHERE username=? ORDER BY created_at DESC LIMIT n`，消除 filesort |

### undo_log 表

| 索引名 | 列 | 类型 | 作用 |
|--------|----|------|------|
| PRIMARY | `id` | 主键 | — |
| `idx_undo_xid_id` | `(xid, id DESC)` | 复合 | 覆盖 `WHERE xid=? ORDER BY id DESC LIMIT 1`，无 filesort |
| `idx_undo_created` | `(created_at)` | 单列 | 加速定期清理 `DELETE WHERE created_at < NOW() - INTERVAL N DAY` |

`undo_log` 通过 `Database::PurgeOldUndoLogs(days=7)` 定期清理，防止表无限膨胀。

### users 表

| 索引名 | 列 | 类型 | 作用 |
|--------|----|------|------|
| PRIMARY | `id` | 主键 | — |
| UNIQUE | `username` | 唯一 | 登录查询、注册去重（强一致，走主库） |

---

## 数据库表

| Database | 表 | 所属 | 关键列 |
|----------|-----|------|--------|
| `rpc_auth` | `users` | AuthService | `id, username(UNIQUE), password_hash, token_version INT DEFAULT 0, created_at` |
| `rpc_spreadsheet` | `spreadsheets` | SpreadsheetService | `id, username, name, headers_json(JSON), data_json(JSON), row_count, col_count, version, updated_at` |
| `rpc_spreadsheet` | `undo_log` | SpreadsheetService/2PC | `id, xid(VARCHAR 64), table_name, row_id, before_snapshot(JSON), created_at` |
| `rpc_spreadsheet` | `health_status` | HealthMonitor | 集群节点心跳 |
| `rpc_file` | `files` | FileService | `id, username, original_name, size, mime_type, file_content(LONGBLOB), storage_path(VARCHAR 512), created_at` |
| `rpc_file` | `undo_log` | FileService/2PC | 同上 |
| `rpc_tx_log` | `tx_log` | TM (Gateway) | 分布式事务日志 |

**`users` 表说明**：
- `password_hash`：采用 PBKDF2-HMAC-SHA256 100000 迭代 + 随机盐，格式为 `pbkdf2_sha256$100000$hex_salt$hex_hash`。旧格式（纯 SHA256 hex）登录时自动升级。
- `token_version`：JWT token 版本号。修改密码时递增该值，使所有已签发的 token 立即失效。

**`files` 表说明**：
- `file_content`：存量数据的 LONGBLOB（向后兼容）。
- `storage_path`：迁移至对象存储（MinIO/S3）后填写存储路径；非空时 `GetFile` 跳过读取 LONGBLOB，由调用方从对象存储获取内容。新上传文件应优先写对象存储并只在此列记录路径。

**`spreadsheets` 表说明**：
- `data_json`：存储整张表格的所有行，每次读写均为全量。`row_count`/`col_count` 为冗余计数，在 `UpdateSpreadsheet` 时同步更新。
- `version`：乐观锁版本号，`UpdateSpreadsheet(version>0)` 时追加 `AND version=?` 并校验 `affected_rows`，防止并发写冲突。
