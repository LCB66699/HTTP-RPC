# HTTP-RPC 分布式事务系统 — 设计文档

> 最后更新: 2026-05-22

## 一、项目概述

基于 C++20/gRPC + 2PC 协议的分布式数据表格与文件管理系统，Docker 19 容器多节点部署，演示分布式事务、Cache-Aside 缓存、主从读写分离、熔断器高可用。

### 架构拓扑

```
浏览器 (HTTPS)
   │
nginx :443/80                    ← TLS 终结 + sendfile 静态文件 + 限流(100r/s+burst50)
   │  /api/* → least_conn 负载均衡
   ├─────────────────────────────────────┐
   ▼                                     ▼
Gateway-1 :8080 (h2c)          Gateway-2 :8080 (h2c)
   │  HttpOnly Cookie 鉴权 + 2PC TM + 熔断器(Redis 共享)
   │  gRPC round_robin + C++20 协程 + DNS 5s 重解析
   │
   ├──→ Auth  (rpc-auth:50051)   副本 x2  DB: rpc_auth
   ├──→ Sheet (rpc-sheet:50051)  副本 x3  DB: rpc_spreadsheet  Redis 缓存
   └──→ File  (rpc-file:50051)   副本 x2  DB: rpc_file         MinIO 对象存储
          │
          ├── MySQL Master x1 + Slave x2  (ROW binlog, GTID 复制)
          ├── Redis Master x1 + Slave x2 + Sentinel x3 (quorum=2, AOF 持久化)
          └── MinIO (S3 兼容对象存储)
```

**容器总数：19**（nginx+gwy2+auth2+sheet3+file2+mysql3+redis6）

### 项目结构

```
gateway-cpp/    HTTP 网关 + 2PC TM（nghttp2-asio h2c + httplib HTTP/1.1）
server/         gRPC 后端服务（单二进制 --service 参数多角色）
proto/          Protobuf 定义（auth/spreadsheet/file/tx/health）
web-ui/         原生 JS SPA（四面板：服务/表格/文件/个人）
mysql/          MySQL 主从配置（master + 2 slave）
redis/          Redis 主从+哨兵配置
test/           bash 功能/性能/压力测试 + wrk 脚本
docs/           设计文档
```

---

## 二、协议栈

```
浏览器 ──HTTPS/HTTP2──→ nginx ──h2c──→ Gateway ──gRPC/HTTP2──→ Backend
       JSON+fetch           proxy_pass      JWT+protobuf       MySQL/Redis/MinIO
```

| 层 | 组件 | 协议 |
|----|------|------|
| 边缘代理 | nginx | TLS 1.3 + HTTP/2 h2c |
| 网关主服务 | nghttp2-asio (8080) | HTTP/2 h2c, 128 并发流, C++20 协程 |
| 网关辅服务 | cpp-httplib (8081) | HTTP/1.1 keepalive, nginx 兼容过渡 |
| RPC | gRPC + Protobuf | HTTP/2, round_robin, 5s DNS 重解析 |
| 数据库 | libmysqlclient | MySQL 8.0 TCP |
| 缓存 | hiredis | Redis 7 RESP |
| 对象存储 | MinIO client | S3 REST API |

### 为什么不用 gRPC-Web？

Gateway 模式更直观——所有 API 可 curl 复现，JWT 在网关集中管理，前端无需引入 gRPC-Web JS 运行时。

### 为什么有同步/协程两套 gRPC 调用？

- **协程 gRPC**（h2c 8080）：C++20 `co_await`，不阻塞线程，数千并发
- **同步 gRPC**（HTTP/1.1 8081）：nginx proxy_pass 兼容过渡，逐步迁移中

---

## 三、项目迭代演进

| 迭代 | 内容 | 动机 |
|------|------|------|
| 1.0 | gRPC Math/String 演示 | 学习项目起点 |
| 1.1 | 替换为 SpreadsheetService | 有状态业务才能展示缓存模式 |
| 1.2 | SQLite → MySQL 8.0 | 嵌入式 DB 不适合分布式场景 |
| 1.3 | 新增 FileService | 文件管理需求 |
| 1.4 | 引入 2PC 分布式事务 | 跨服务操作一致性 |
| 1.5 | 移除 ZooKeeper | gRPC DNS 别名 + round_robin 替代 |
| 1.6 | Docker Compose 化 | 单进程→多容器分布式部署 |
| 1.7 | nginx 边缘代理 | cpp-httplib 静态文件性能瓶颈 |
| 1.8 | HTTPS 自签证书 | 传输加密 |
| 1.9 | 安全加固（6 项） | 白盒审计：JWT secret/密码哈希/Token 吊销 |
| 1.10 | MySQL/Redis 主从架构 | 消除单点故障，读写分离 |
| 1.11 | Sentinel 故障转移集成 | Redis Master 宕机自动发现新主 |
| 1.12 | Gateway 线程池 | 控制 gRPC 并发 + 背压保护 |
| 1.13 | MySQL 乐观锁 + Redis 连接池 | 跨副本并发写 + 消除锁竞争 |
| 1.14 | RESTful API 转换 | POST get/delete → GET/DELETE 资源化 URL |
| 1.15 | 熔断器 + 服务注册加固 | 后端故障快速失败，gRPC Channel 实时健康检查 |
| 1.16 | nginx→Gateway HTTP/2 升级 | nghttp2-asio 替代 httplib，消除 HTTP/1.1 队头阻塞 |
| 1.17 | 双 Gateway 横向扩展 | 消除单点，least_conn 负载均衡，零停机更新 |
| 1.18 | HttpOnly Cookie 鉴权 | 防 XSS/CSRF，替代 JS 变量存 token |
| 1.19 | 熔断器 Redis 跨实例共享 | 双 gateway 状态一致，SetNX 探针锁 |
| 1.20 | 纵深防御 + 日志系统 + 数据持久化 | AuthInterceptor 二层鉴权、fail-close、SystemLogger、bind mount |

---

## 四、核心设计决策

### 4.1 服务间通信：gRPC

| 对比 | gRPC（已选） | Dubbo | brpc |
|------|------------|-------|------|
| 协议 | HTTP/2 + Protobuf | 自定义 TCP | 自定义 TCP |
| 多语言 | 原生支持 | Java 为主 | C++ 原生 |
| 流式传输 | unary/server/client/bidi | 3.0 Triple 兼容 gRPC | 支持 |
| 生态 | CNCF, K8s 原生 | 中国 Java 生态 | 百度内部 |
| 选型理由 | Protobuf 强类型 + HTTP/2 多路复用 + C++ 一等公民 + 跨语言 | | |

### 4.2 分布式事务：2PC

| 协议 | 一致性 | 复杂度 | 选型理由 |
|------|--------|--------|---------|
| **2PC** | 强一致 | 中 | 短事务秒级完成，适合演示 |
| SAGA | 最终一致 | 高 | 需补偿机制，过重 |
| TCC | 强一致 | 高 | Try-Confirm-Cancel 三阶段过重 |

**undo_log 设计**：JSON 快照替代反向 SQL——操作前存原始数据，回滚时恢复。一种机制覆盖 INSERT/DELETE/UPDATE。

### 4.3 鉴权方案：HttpOnly Cookie + JWT

| 属性 | 效果 |
|------|------|
| `HttpOnly` | JS 不可读，防 XSS 窃取 |
| `Secure` | 仅 HTTPS 传输 |
| `SameSite=Strict` | 仅同源请求，防 CSRF |

**双层纵深防御**：Gateway 验证 JWT → 转发 `raw_token` 到 gRPC metadata → Server AuthInterceptor 二次验证。防止内网直连 gRPC 端口绕过鉴权。

### 4.4 服务发现：Docker DNS 别名

为什么不用 ZooKeeper？Docker 内嵌 DNS 将服务别名解析为多 IP → gRPC round_robin 自动轮询。消除了 ZK 部署和 libzookeeper_mt 依赖。

### 4.5 熔断器粒度：服务级 + 副本级双层级

项目已实现 `PerReplicaTracker`（`circuit_breaker.h:222-329`）：
- **副本级**：单副本连续失败→隔离该副本，健康副本继续服务
- **服务级**：半数以上副本被隔离→触发服务级熔断
- **跨实例共享**：状态持久化到 Redis `cb:{svc}:*`，SetNX 探针锁防止多实例同时试探

### 4.6 网关框架：nghttp2-asio

| 决策 | 选择 | 原因 |
|------|------|------|
| HTTP/2 模式 | h2c（cleartext） | Docker 内网无需 TLS |
| C++ 库 | nghttp2-asio v1.62.0 | gRPC 底层同栈，128 并发流 |
| 集成方式 | vendored + patch | 源码级集成 |

---

## 五、数据库与缓存设计

### 5.1 Schema

| 数据库 | 表 | 关键列 |
|--------|-----|--------|
| `rpc_auth` | `users` | `id BIGINT PK`, `username UNIQUE`, `password_hash`, `token_version` |
| `rpc_spreadsheet` | `spreadsheets` | `id BIGINT PK`, `user_id BIGINT`, `name`, `headers_json JSON`, `data_json JSON`, `version INT`, `idempotency_key CHAR(36) UNIQUE NULL` |
| `rpc_spreadsheet` | `undo_log` | `xid`, `table_name`, `row_id`, `before_snapshot JSON` |
| `rpc_file` | `files` | `id BIGINT PK`, `user_id BIGINT`, `original_name`, `size`, `mime_type`, `file_content LONGBLOB`(向后兼容), `storage_path`(MinIO) |
| `rpc_tx_log` | `tx_log` | `xid UNIQUE`, `status`, `participants_json`, `timeout_at` |

### 5.2 索引

| 表 | 索引 | 类型 | 覆盖场景 |
|----|------|------|---------|
| spreadsheets | `PRIMARY (id)` | 聚簇 | 点查 |
| spreadsheets | `idx_sheets_user_time (user_id, updated_at DESC)` | 复合 | 列表分页，消除 filesort |
| files | `idx_files_user_time (user_id, created_at DESC)` | 复合 | 同上 |
| undo_log | `idx_undo_xid_id (xid, id DESC)` | 复合 | 查最新回滚记录 |
| undo_log | `idx_undo_created (created_at)` | 单列 | 定期清理 |

### 5.3 缓存策略：Cache-Aside + 版本号命名空间

```
读: Redis → 命中返回 → 未命中查 MySQL → 回填 Redis (TTL 300s)
写: MySQL INSERT/UPDATE → DEL 单条缓存 → INCR 列表版本号
```

版本号命名空间失效机制：
```
INCR user:100:sheets:version (5→6)
  → 旧 key: ...v5:p0:ps20 无人再查
  → 新 key: ...v6:p0:ps20 cache miss → MySQL → 回填
  → 旧 key 等 120s TTL 自动过期
```
一个 INCR，所有分页缓存全部作废，无需遍历删除。

### 5.4 Redis Key 规范

```
{namespace}:{entity}:{id}[:{suffix}]
```

| Key | 类型 | TTL | 说明 |
|-----|------|-----|------|
| `sheet:{id}` | protobuf binary | 300s | 单表缓存 |
| `file:{id}` | protobuf binary | 300s | 文件元数据（<1MB） |
| `user:{uid}:sheets:v{ver}:p{p}:ps{ps}` | protobuf binary | 120s | 分页列表缓存 |
| `token_ver:{username}` | string | 86400s | Token 版本号 |
| `cb:{svc}:state` | string (CLOSED/OPEN/HALF_OPEN) | 120s | 熔断器状态 |
| `cb:{svc}:fails` | int | 120s | 熔断失败计数 |
| `cb:{svc}:opened` | timestamp | 120s | 熔断打开时间 |
| `cb:{svc}:probe` | string (NX 锁) | 30s | 半开探针锁 |
| `call_history:global` | list (LTRIM 10000) | 30d | 全局调用日志 |
| `errors:{svc}:total` | int | 永久 | 错误计数器 |

### 5.5 读写分离与一致性

**MySQL 主从**：
- Master 写入所有 CUD + 6 个强一致性读（`GetUser`、`UserExists`、`GetTokenVersion`、`GetSpreadsheetOwner`、`GetFileOwner`、`GetUndoLog`）
- Slave 处理普通查询，DNS round-robin 轮询
- binlog ROW 格式 + GTID 自动定位

**Redis 主从 + Sentinel**：
- Master 写入，Slave 轮询读取
- 3 Sentinel quorum=2 监控
- 应用端 ReconnectMaster() 三层恢复：直连 → Sentinel 查询 → 新 master 重连
- 故障转移 RTO 约 8-10s，写操作自动重试

**MySQL 乐观锁**：`UPDATE ... SET version=version+1 WHERE id=? AND version=?`，冲突时业务层重试最多 3 次。

### 5.6 分页

0-based offset 分页，`page_size=0` 退化为全量。每个分页独立缓存 key（含 `p{page}:ps{page_size}` 后缀）。Delayed Join 优化深分页。

### 5.7 Snowflake 全局 ID

```
1 位符号位 | 41 位时间戳(ms) | 5 位 worker ID | 12 位序列号
```

已替代 `AUTO_INCREMENT` 作为 spreadsheets/files 主键。worker_id 从端口号低 5 位派生。

**待修复**：所有同端口实例 worker_id 相同（如 3 个 sheet 副本端口均为 50051），极端情况下同毫秒+同序列号会碰撞。建议从 `SNOWFLAKE_WORKER_ID` 环境变量读取。

---

## 六、安全设计

### 6.1 认证链路

```
POST /api/login {username, password}
  → Gateway 解析 HTTP body
    → gRPC AuthService.Login()
      → MySQL master 查 password_hash (PBKDF2-HMAC-SHA256, 100k 迭代, 16B 随机盐)
        → 验证通过: JWT 签发 (HS256, payload={username, uid, ver, exp})
          → Redis SET token_ver:{username}
            → Set-Cookie: rpc_token=<jwt>; HttpOnly; Secure; SameSite=Strict
```

### 6.2 Token 吊销

- JWT payload 嵌入 `ver`（来自 `users.token_version`）
- Gateway 验证时查 Redis `token_ver:{username}`
- `token_ver < Redis 中的当前值` → 拒绝（已吊销）
- 修改密码 → `UPDATE users SET token_version = token_version + 1` → 所有旧 JWT 立即失效
- Redis 不可用时 **fail-closed**（拒绝请求，不静默放行）

### 6.3 已实施的安全措施

| 措施 | 实现 |
|------|------|
| 密码哈希 | PBKDF2-HMAC-SHA256 100k 迭代 + 16B 随机盐 |
| 密码内存擦除 | `OPENSSL_cleanse` 使用后立即清除 |
| JWT 常量时间比较 | XOR-based `constant_time_eq` |
| Cookie 安全属性 | HttpOnly + Secure + SameSite=Strict |
| 双层纵深防御 | Gateway JWT + Server AuthInterceptor |
| Username 转义 | JSON 注入防护 |
| 强一致读 | 关键查询强制走 Master |

### 6.4 已知安全缺陷

| 编号 | 问题 | 严重度 | 状态 |
|------|------|--------|------|
| S-1 | JWT Secret 仍静默回退到硬编码默认值 | **致命** | ❌ 代码未修 |
| S-2 | 登录无防爆破（无失败计数/锁定/延迟） | **高** | ❌ 未实现 |
| S-3 | 前端 `Math.random()` 生成 UUID（非 crypto） | 中 | ❌ 未修复 |
| S-4 | Snowflake worker_id 同端口碰撞风险 | 中 | ❌ 待修 |

---

## 七、API 接口

### 认证

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/login` | 登录，Set-Cookie 下发 JWT |
| POST | `/api/register` | 注册，自动登录 |
| POST | `/api/logout` | 登出，清除 Cookie |

### 表格

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/sheets?page=0&page_size=20` | 列表（Redis 缓存） |
| POST | `/api/sheets` | 创建 |
| GET | `/api/sheets/:id` | 获取 |
| PUT | `/api/sheets` | 更新（乐观锁） |
| DELETE | `/api/sheets/:id` | 删除 |

### 文件

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/files?page=0&page_size=20` | 列表 |
| POST | `/api/files/upload` | 上传（multipart） |
| GET | `/api/files/download?id=1` | 下载 |
| DELETE | `/api/files/:id` | 删除 |

### 监控

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/health` | gRPC Channel + 熔断器状态（免认证） |
| GET | `/api/system/status` | 错误计数 + 服务详情（需认证） |
| GET | `/api/history` | 调用历史 |

---

## 八、技术债务与改善优先级

### P0 — 安全底线（本周）

| 编号 | 问题 | 文件 | 工时 |
|------|------|------|------|
| S-1 | JWT Secret 拒绝默认值，缺失→启动失败 | `gateway-cpp/src/main.cpp:19-21`, `server/src/main.cpp:34-36` | 0.5d |
| S-2 | 登录防爆破（Redis 计数 + 锁定） | `server/src/auth_service_impl.cpp:81-151` | 1d |
| S-3 | 前端 `crypto.getRandomValues()` | `web-ui/app.js` | 0.1d |

### P1 — 高危架构问题（2-3 周）

| 编号 | 问题 | 文件 | 工时 |
|------|------|------|------|
| N-1 | ThreadPool 无界队列 + 拒绝策略 | `gateway-cpp/include/thread_pool.h:46-53` | 0.5d |
| N-2 | 文件存储强制 MinIO，禁止降级 MySQL BLOB | `server/src/file_service_impl.cpp` | 2d |
| N-3 | 版本号 key 加 TTL（Lua 脚本 INCR + EXPIRE） | `server/src/redis_client.cpp` | 0.5d |
| N-4 | 2PC RecoveryLoop 实际实现 | `server/src/tx_manager.cpp:169-184` | 3d |
| N-5 | 连接池健康检查（MySQL PING / Redis PING） | `server/src/database.cpp`, `server/src/redis_client.cpp` | 2d |
| N-6 | Snowflake worker_id 从 SNOWFLAKE_WORKER_ID 环境变量 | `server/src/main.cpp:128` | 0.5d |

### P2 — 工程优化（1-2 月）

| 编号 | 问题 | 说明 | 工时 |
|------|------|------|------|
| P-2 | 文件流式上传 | gRPC streaming 或 MinIO presigned URL | 5d |
| P-4 | http2_server accept 改为 epoll 事件驱动 | 替代 thread-per-connection | 5d |
| N-7 | 替换自制 JSON 解析器为 nlohmann/json | `gateway.cpp`, `main.cpp` 中的 `JsonGet` | 1d |
| N-8 | 请求级 trace ID（X-Request-Id 全链路传递） | nginx → Gateway → gRPC → MySQL/Redis | 1d |
| N-9 | 数据库备份/恢复机制 | crontab + mysqldump sidecar | 2d |
| O-4 | Prometheus + Grafana 监控 | metrics 端点 + 仪表盘 + 告警规则 | 5d |
| N-10 | 单元测试覆盖（GoogleTest） | 当前仅 bash 脚本功能测试 | 5d+ |

---

## 九、运维速查

详见 [OPS.md](OPS.md)，涵盖 Docker/MySQL/Redis/gRPC/HTTPS/curl/压测全部操作命令。

关键命令：
```bash
docker compose up -d --build              # 一键启动 19 容器
docker compose logs gateway-1 --tail=30   # 查看日志
docker compose restart gateway-1          # 单实例重启（不影响服务）
curl -k https://localhost/api/health      # 健康检查（免认证）
bash test/functional_test.sh             # 功能测试
```
