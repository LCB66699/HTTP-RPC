# HTTP-RPC 微服务分布式系统

基于 gRPC + gRPC-Gateway 的在线协作表格系统，支持文件存储、工作空间、实时协作、积分激励、多层缓存、服务发现、弹性容错和可观测性。

## 架构

```
浏览器 (Web UI) <- -> WebSocket (实时协作)
   |  HTTPS
   v
nginx :443                              <- TLS 终结 + 令牌桶限流
   |  least_conn -> gateway_pool
   v
Envoy x2 :10000                         <- JWT 边缘认证 + CORS + 熔断 + 重试
   |  STRICT_DNS -> gateway_cluster
   v
gRPC-Gateway (Go) x2 :8080             <- HTTP/JSON -> gRPC + JWT + 熔断器 + WebSocket Hub
   |  consul:/// (Consul 服务发现)
   +---> Auth/Sharing/Workspace (rpc-auth:50051)  x2   MySQL rpc_auth
   +---> Sheet  (rpc-sheet:50051)                 x2   MySQL rpc_spreadsheet (2 shards) + L1/L2 缓存
   +---> File   (rpc-file:50051)                  x2   MySQL rpc_file (2 shards) + MinIO 对象存储
   +---> Search (rpc-search:50051)                x1   Elasticsearch
   +---> Points (rpc-points:50052)                x1   Redis 积分引擎

C++ 服务 --RabbitMQ--> Notify Service (Go) --> MongoDB + ES 索引

积分事件:
  Gateway handler -> Redis PUBLISH pts:earn -> Points 服务 consumeEvents -> Redis 积分账户

实时广播:
  Gateway handler -> Redis PUBLISH ws:broadcast -> WS Hub -> WebSocket -> 浏览器

可观测性:
  Prometheus <- /api/v1/metrics   <- HTTP 计数 + gRPC 延迟 + 熔断器状态
  Grafana    <- Prometheus        <- 可视化面板
  Jaeger     <- OTLP gRPC         <- 分布式追踪

基础设施:
  Consul x1 (服务注册 + 健康检查)
  MySQL x9 (authx1 + spreadsheetx4 + filex4，每片1主1从)
  Redis Cluster x6 (3M+3S)
  Elasticsearch x1
  MongoDB x1
  RabbitMQ x1 (事件总线 + DLQ)
  MinIO x1 (文件/表格内容对象存储)
```

## 容器清单 (~36个)

| 层 | 容器 | 数量 |
|----|------|------|
| 入口 | nginx, envoy | 3 |
| 网关 | grpc-gateway | 2 |
| C++ 服务 | auth, sheet, file, search | 7 |
| Go 服务 | notify-service, points-server | 2 |
| 注册中心 | consul | 1 |
| 存储 | MySQL, Redis, ES, MongoDB, MinIO | 18 |
| 消息 | RabbitMQ | 1 |
| 监控 | Prometheus, Grafana, Jaeger | 3 |

## API

### 认证

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/v1/register` | 否 | 注册 |
| POST | `/api/v1/login` | 否 | 登录，速率限制 5次/分钟 |
| POST | `/api/v1/refresh` | 否 | 刷新令牌 |
| PUT | `/api/v1/me/password` | 是 | 修改密码 |
| POST | `/api/v1/auth/otp/send` | 否 | 发送验证码 |
| POST | `/api/v1/auth/phone/login` | 否 | 手机号登录 |
| GET | `/api/v1/me` | 是 | 当前用户信息 |

### 表格

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| GET | `/api/v1/sheets` | 是 | 列表，支持 ?workspace_id= 过滤 |
| POST | `/api/v1/sheets` | 是 | 创建，支持 workspace_id |
| GET | `/api/v1/sheets/:id` | 是 | 获取 (owner -> share -> workspace 三层权限) |
| PUT | `/api/v1/sheets/:id` | 是 | 更新 (乐观锁 + workspace editor 可编辑) |
| DELETE | `/api/v1/sheets/:id` | 是 | 删除 (仅 owner) |

### 文件

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/v1/files/upload` | 是 | 上传 (MinIO 优先) |
| GET | `/api/v1/files` | 是 | 列表，支持 workspace_id |
| GET | `/api/v1/files/:id` | 是 | 下载 (302 预签名) |
| DELETE | `/api/v1/files/:id` | 是 | 删除 |
| POST | `/api/v1/files/folder` | 是 | 创建文件夹 |
| PUT | `/api/v1/files/:id/move` | 是 | 移动 |

### 工作空间

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/v1/workspaces` | 是 | 创建工作空间 |
| GET | `/api/v1/workspaces` | 是 | 列表 |
| GET | `/api/v1/workspaces/:id` | 是 | 详情 |
| PUT | `/api/v1/workspaces/:id` | 是 | 更新名称 |
| DELETE | `/api/v1/workspaces/:id` | 是 | 删除 |
| POST | `/api/v1/workspaces/:id/members` | 是 | 添加成员 (admin/editor/viewer) |
| DELETE | `/api/v1/workspaces/:id/members/:uid` | 是 | 移除成员 |

### 分享

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/v1/sheets/:id/share` | 是 | 分享给指定用户 (view/edit) |
| GET | `/api/v1/sheets/:id/share` | 是 | 查看分享列表 |
| DELETE | `/api/v1/sheets/:id/share/:username` | 是 | 撤销分享 |
| POST | `/api/v1/sheets/:id/share-link` | 是 | 创建分享链接 |
| GET | `/api/v1/s/:token` | 否 | 通过分享 token 访问 |

### 积分

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| GET | `/api/v1/points/balance` | 是 | 查询积分余额 |
| GET | `/api/v1/points/transactions` | 是 | 积分流水记录 |
| GET | `/api/v1/points/leaderboard` | 是 | 积分排行榜 |

**积分获取规则**（事件驱动，points 服务独立消费）：

| 行为 | 积分 | 上限/天 | 特殊 |
|------|------|---------|------|
| 每日登录 | +10 | 1 次 | 连续 7 天额外 +50 |
| 创建表格 | +5 | 5 次 | |
| 上传文件 | +3 | 10 次 | |

### 搜索 & 健康

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/v1/search` | 是 | 全文搜索 (ES) |
| GET | `/api/v1/health` | 否 | 网关 + 后端熔断器状态 |
| GET | `/api/v1/health/ready` | 否 | 就绪探针，断路器开时返回 503 |
| GET | `/api/v1/metrics` | 否 | Prometheus 指标 |
| GET | `/api/v1/ws` | 是 | WebSocket 实时推送 |

## 核心机制

| 机制 | 实现 |
|------|------|
| 服务发现 | Consul 注册 + `consul:///` gRPC Resolver，只连 passing 实例 |
| 健康检查 | gRPC Health Check (C++) + grpc_health_probe (Docker) + 客户端 subchannel 摘除 |
| 负载均衡 | Consul DNS round_robin -> gRPC client round_robin + health-aware |
| 熔断器 | gobreaker (Closed->Open->Half-Open)，慢调用检测 + 失败率阈值，3 次连续失败触发 |
| 重试 | 幂等读 3 次，指数退避 50/100ms，只重试 Unavailable/DeadlineExceeded/ResourceExhausted/Aborted |
| JWT | Cookie->Gateway HS256->C++ AuthInterceptor->Auth.ValidateUser，双令牌 (access 15min + refresh 7d) |
| 权限 | 三层：owner -> 资源级分享 (P0) -> 工作空间成员 (P1) |
| 积分引擎 | 事件驱动：网关 Redis PUBLISH pts:earn -> points 服务 consumeEvents -> Redis HINCRBY |
| 连续签到 | Redis streak counter，每日登录检测昨天是否登录，连续 7 天触发 bonus |
| 实时广播 | Redis Pub/Sub ws:broadcast -> WS Hub -> WebSocket -> 浏览器 |
| SQL 注入防护 | 编译期 `make_sql` + `sql_param`，禁止裸字符串拼接 |
| 乐观锁 | CAS UPDATE WHERE version=N + affected_rows，最多 7 次重试 |
| L1 缓存 | 进程内 LRU (10K/30min)，Redis Pub/Sub 失效 |
| L2 缓存 | Redis Cluster Cache-Aside，JitteredTTL 防雪崩，null marker 防穿透 |
| 对象存储 | MinIO 优先，MySQL LONGBLOB 回退，302 预签名下载 |
| 事件驱动 | C++ RabbitMQ + MySQL outbox (CAS 状态机) -> Notify Service -> ES + MongoDB |
| 搜索 | Elasticsearch multi_match + fuzzy + highlighting，user_id 隔离 |
| 分布式 ID | Snowflake (worker_id = hash(host:port) & 0x1F) |
| 优雅关闭 | SIGTERM -> Consul deregister -> 30s grace period -> srv.Shutdown |
| CORS | `*` 默认，OPTIONS 预检直接返回 204 |
| 限流 | Nginx 令牌桶：login 120r/m，heavy 200r/s，light 10000r/s |

## 配置

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `JWT_SECRET` | - | JWT 签名密钥 (必设) |
| `MYSQL_ROOT_PASSWORD` | `123456` | MySQL root 密码 |
| `REDIS_PASSWORD` | `rpc-redis-123456` | Redis 密码 |
| `MINIO_ROOT_PASSWORD` | `rpc-minio-123456` | MinIO 密码 |
| `AUTH_ADDR` | `rpc-auth` | Auth 服务 Consul 名 |
| `SHEET_ADDR` | `rpc-sheet` | Sheet 服务 Consul 名 |
| `FILE_ADDR` | `rpc-file` | File 服务 Consul 名 |
| `SEARCH_ADDR` | `rpc-search` | Search 服务 Consul 名 |
| `POINTS_ADDR` | `rpc-points` | Points 服务 Consul 名 |

## CI/CD

- **diff 驱动**：7 个 workflow，按改动范围触发
  - `ci-lint`：每次 PR 必跑 (gofmt + clang-format)
  - `ci-cpp-{service}`：C++ 文件变更，builder 编译 + cpp_test 单测
  - `ci-go-gateway`：Go 文件变更，go test + go build
  - `ci-integration`：main 分支 + 共享依赖变更，全量 E2E + push 镜像
- **分支保护**：main 必须 PR + CI 绿 + approve，禁止直推
- **Pre-commit**：gofmt + clang-format + 密钥检测

## 测试

```bash
bash dev.sh up                   # 启动全栈
bash dev.sh gateway-test         # Go 网关单测
bash dev.sh svc-test             # Go 服务单测 (notify + points)
bash dev.sh cpp-test             # C++ 单测
bash dev.sh test                 # E2E 功能测试
```

## 启动

```bash
docker compose build
docker compose up -d
bash test/e2e/functional_test.sh
```

访问: Prometheus `:9090` | Grafana `:3000` (admin/admin) | Jaeger `:16686` | Consul `:8500`
