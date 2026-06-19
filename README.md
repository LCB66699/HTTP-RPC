# HTTP-RPC 微服务分布式系统

基于 gRPC + gRPC-Gateway 的微服务数据表格系统，支持文件存储、全文搜索、多层缓存、弹性容错和可观测性。

## 架构

```
浏览器 (Web UI)
   │  HTTPS
   ▼
nginx :443                           ← TLS 终结 + 令牌桶限流 (3层)
   │  least_conn → gateway_pool
   ▼
gRPC-Gateway (Go) ×2 :8080          ← HTTP/JSON ↔ gRPC + JWT + 熔断器 + 重试 + readiness + Prometheus 指标
   │  dns:/// + round_robin
   ├──→ Auth   (rpc-auth:50051)     ×2   MySQL rpc_auth (1 shard) + Redis 调用日志
   ├──→ Sheet  (rpc-sheet:50051)    ×2   MySQL rpc_spreadsheet (2 shards) + L1/L2 缓存
   ├──→ File   (rpc-file:50051)     ×2   MySQL rpc_file (2 shards) + MinIO 对象存储
   └──→ Search (rpc-search:50051)   ×1   Elasticsearch

C++ 服务 ──RabbitMQ──→ Notify Service (Go) ──→ MongoDB + ES 索引

可观测性:
  Prometheus ← 抓取 /api/v1/metrics    ← HTTP 计数 + gRPC 延迟 + 熔断器状态
  Grafana    ← Prometheus 数据源        ← 可视化面板
  Jaeger     ← OTLP gRPC                ← 分布式追踪

基础设施:
  MySQL ×9 (auth×1 + spreadsheet×4 + file×4，每片1主1从)
  Redis Cluster ×6 (3M+3S)
  Elasticsearch ×1
  MongoDB ×1
  RabbitMQ ×1 (事件总线 + DLQ 死信队列)
  MinIO ×1 (文件/表格内容对象存储)
```

## 容器清单 (~34个)

| 层 | 容器 | 数量 |
|----|------|------|
| 入口 | nginx | 1 |
| 网关 | grpc-gateway | 2 |
| C++ 服务 | auth, sheet, file, search | 7 |
| Go 服务 | notify-service | 1 |
| 存储 | MySQL, Redis, ES, MongoDB, MinIO | 18 |
| 消息 | RabbitMQ | 1 |
| 治理 | canal-server | 1 |
| 监控 | Prometheus, Grafana | 2 |
| 追踪 | Jaeger | 1 |

## 项目结构

```
├── cmd/
│   └── gateway-grpc/           Go gRPC-Gateway (HTTP→gRPC 转译)
│       ├── main.go                 路由 + JWT + 熔断器(gobreaker) + 重试 + readiness + 优雅关闭
│       ├── metrics.go              Prometheus 指标 (HTTP计数/gRPC延迟/熔断器状态)
│       ├── validate.go            请求校验 (login/register/sheet/search/file/password)
│       ├── handler_test.go        集成测试
│       ├── main_test.go           JWT 单元测试
│       └── Dockerfile
├── server/                     C++ gRPC 后端服务
│   ├── include/
│   │   ├── database.h             MySQL 读写分离 + ShardedDatabase 分片 + sql_param 编译期防SQL注入
│   │   ├── redis_client.h         Redis Cluster (redis-plus-plus) + 批量 Pipeline
│   │   ├── jwt.h                  JWT HS256 签名/验签
│   │   ├── cache_helpers.h        通用缓存 key 生成 (sheet/file 共用)
│   │   ├── sheet_helpers.h        sheet 缓存 key (委托 cache_helpers.h)
│   │   ├── file_helpers.h         file 缓存 key (委托 cache_helpers.h)
│   │   ├── auth_interceptor.h     gRPC JWT 拦截器
│   │   ├── l1_cache.h             L1 LRU 本地缓存 (10K/30min)
│   │   ├── l1_invalidator.h       Redis Pub/Sub → L1 缓存失效
│   │   ├── rabbit_publisher.h     RabbitMQ 事件发布
│   │   ├── minio_client.h         MinIO 客户端 (AWS SigV4)
│   │   ├── snowflake.h            Snowflake 分布式 ID
│   │   ├── call_logger.h          调用日志 + Redis 异步 flush + 批量 Pipeline
│   │   └── *_service_impl.h      各服务实现
│   └── src/
│       ├── main_auth.cpp          Auth 独立入口
│       ├── main_sheet.cpp         Sheet 独立入口 (含 MinIO)
│       ├── main_file.cpp          File 独立入口 (含 MinIO)
│       ├── main_search.cpp        Search 独立入口
│       ├── shared/
│       │   ├── database.cpp       所有 SQL 使用 make_sql 编译期参数化
│       │   ├── redis_client.cpp   BatchPushCallEntries Pipeline 批量写入
│       │   └── call_logger.cpp    FlushLoop 批量推送 Redis，一次 RTT
│       └── ...
├── services/
│   └── notify-service/            Go RabbitMQ 消费者 → MongoDB + ES + MinIO
├── proto/                      Protobuf 定义
├── deploy/                     部署 & 基础设施配置
│   ├── es/                      Elasticsearch Dockerfile
│   ├── mongo/                   MongoDB 初始化
│   ├── redis/cluster/           Redis Cluster 配置 + 智能 init
│   ├── mysql/                   MySQL 主从配置 + init.sql
│   ├── nginx/                   nginx.conf (TLS + 三层令牌桶限流)
│   ├── prometheus/              prometheus.yml 抓取配置
│   ├── envoy/                   Envoy Dockerfile (历史遗留)
│   ├── kanal/                   Canal binlog 订阅 (预留)
│   ├── k8s/                     Kubernetes 部署文件
│   └── lvs/                     LVS + Keepalived 4层负载均衡
├── vendor/                     第三方依赖 (redis-plus-plus)
├── docs/                       设计文档 + 调试指南
├── test/                       功能/性能/压力测试
├── Dockerfile                  多阶段构建 (ubuntu，一次编译产出全部 C++ 服务)
├── docker-compose.yml          全栈部署
├── docker-compose.override.yml 本地调试 (GDB 权限 + 源码挂载)
└── dev.sh                      本地开发快捷命令
```

## 本地开发

```bash
bash dev.sh up          # 启动全部服务
bash dev.sh gateway     # 改 Go 代码后重编 (30s)
bash dev.sh auth        # 改 C++ Auth 后重编 (3min)
bash dev.sh sheet       # 改 C++ Sheet 后重编
bash dev.sh file        # 改 C++ File 后重编
bash dev.sh test        # 跑功能测试
bash dev.sh debug sheet # GDB 断点调试 (详见 docs/DEBUG_GUIDE.md)
```

## API 接口

### 认证

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/register` | 否 | 注册，校验 username 3-20 字符、password >= 6 字符 |
| POST | `/api/login` | 否 | 登录，速率限制 5次/分钟，超限锁定5分钟 |
| POST | `/api/refresh` | 否 | 刷新令牌 (Cookie 或 Body) |
| PUT | `/api/me/password` | 是 | 修改密码 |
| POST | `/api/auth/otp/send` | 否 | 发送短信验证码 (Gateway 直接生成 OTP 存 Redis) |
| POST | `/api/auth/phone/login` | 否 | 手机号+验证码登录 |
| GET | `/api/me` | 否 | 当前用户信息 |
| GET | `/api/services` | 否 | 服务列表 |
| GET | `/api/health` | 否 | 健康检查 |
| GET | `/api/health/ready` | 否 | 就绪探针 (gRPC 连接状态 + 后端可达性) |

### 表格

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| GET | `/api/sheets` | 是 | 列表 (callWithRetry + 熔断器) |
| POST | `/api/sheets` | 是 | 创建 (数据存 MinIO) |
| GET | `/api/sheets/{id}` | 是 | 获取 (MinIO + Redis + L1 三级缓存), 含权限校验 |
| PUT | `/api/sheets/{id}` | 是 | 更新 (乐观锁 version) |
| DELETE | `/api/sheets/{id}` | 是 | 删除 |

### 文件

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/files/upload` | 是 | 上传 (multipart, 内容→MinIO, 元数据→MySQL) |
| GET | `/api/files` | 是 | 列表 (callWithRetry + 熔断器) |
| GET | `/api/files/{id}` | 是 | 下载 (MinIO 直连或 302 预签名) |
| DELETE | `/api/files/{id}` | 是 | 删除 |
| POST | `/api/files/folder` | 是 | 创建文件夹 |
| PUT | `/api/files/{id}/move` | 是 | 移动文件到目标文件夹 |

### 分享

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/sheets/{id}/share` | 是 | 分享表格给指定用户 |
| DELETE | `/api/sheets/{id}/share` | 是 | 撤销分享 |
| GET | `/api/sheets/{id}/share` | 是 | 查看共享列表 |
| POST | `/api/sheets/{id}/share-link` | 是 | 创建分享链接 |
| GET | `/api/s/{token}` | 否 | 通过分享 token 访问资源 |

### 其他

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/search` | 是 | 全文搜索 (gRPC → Search Service → ES) |
| GET | `/api/history` | 是 | 调用日志 (Redis, 最近20条) |
| GET | `/api/photos` | 是 | 图片列表 (mime_type LIKE 'image/%') |
| GET | `/api/v1/metrics` | 否 | Prometheus 指标 |

## 可观测性

### Prometheus 指标 (`GET /api/v1/metrics`)

| 指标 | 类型 | 标签 | 说明 |
|------|------|------|------|
| `http_requests_total` | Counter | `method`, `path`, `status` | HTTP 请求计数 (用 `r.Pattern` 避免参数化路由基数爆炸) |
| `grpc_request_duration_seconds` | Histogram | `service`, `method` | gRPC 调用延迟 |
| `circuit_breaker_state` | Gauge | `name` | 熔断器状态: 0=Closed, 1=HalfOpen, 2=Open |

### Grafana

接入后可用查询：
```
# 每秒请求数
rate(http_requests_total[5m])

# gRPC 延迟 P99
histogram_quantile(0.99, rate(grpc_request_duration_seconds_bucket[5m]))

# 熔断器告警 (=2 时触发)
circuit_breaker_state{name="sheet"} == 2

# 错误率
rate(http_requests_total{status=~"5.."}[5m]) / rate(http_requests_total[5m])
```

### 分布式追踪

Jaeger 以 OTLP gRPC 接收链路数据 (jaeger:4317)，可在 `http://<host>:16686` 查看。

## 核心机制

| 机制 | 实现 |
|------|------|
| 负载均衡 | Nginx least_conn → Gateway dns:/// + round_robin → C++ dns:/// + round_robin (3层) |
| 熔断器 | Gateway gobreaker (Closed→Open→Half-Open), 慢调用检测 + 失败率阈值, 每服务独立 |
| gRPC 重试 | 幂等读最多3次, 指数退避 50/100/200ms, 只重试 Unavailable/DeadlineExceeded/ResourceExhausted/Aborted |
| 就绪探针 | `/api/health/ready` 检查 auth/sheet/file gRPC 连接状态, CI 轮询替代固定 sleep |
| JWT 鉴权 | Cookie→Gateway HS256 验签→C++ AuthInterceptor→Auth.ValidateUser 三层, 双令牌(access 15min + refresh 7d) |
| 登录速率限制 | Redis INCR 每分钟5次, 超限 SET blocked 5分钟, Gateway 拦截 |
| 请求校验 | Gateway validate.go 在调用 gRPC 前校验必填字段和格式 (username/password/sheet/file/search) |
| SQL 注入防护 | 编译期 `sql_param` + `make_sql`: `static_assert` 禁止裸字符串拼接 raw SQL, 强制 `mysql_real_escape_string` 转义 |
| 乐观锁 | spreadsheet/files 表 version 列 + UPDATE WHERE version=N + affected_rows 检测, 消除 TOCTOU 竞态 |
| L1 缓存 | 进程内 LRU (10K/30min), Redis Pub/Sub 失效 |
| L2 缓存 | Redis Cluster Cache-Aside, JitteredTTL 防雪崩, 逻辑过期 + 异步刷新, null marker 防穿透 |
| 对象存储 | MinIO: 文件内容 + 表格数据, 预签名URL 302下载 |
| 事件驱动 | C++ RabbitMQ 发布 → Go Notify 消费 → MongoDB + ES |
| 全文搜索 | Elasticsearch multi_match + fuzzy + highlighting, user_id 过滤 |
| 分布式 ID | Snowflake (worker_id = hash(host:port) & 0x1F) |
| 调用日志 | C++ CallLogger 记录每条 gRPC 调用耗时/参数/结果, 后台线程批量 Pipeline 写入 Redis (一次 RTT) |
| 优雅关闭 | Go signal.Notify(SIGTERM/SIGINT) → 30s grace period → srv.Shutdown |
| 错误处理集中化 | writeGRPCResponse 统一收拢 12 个 handler 的 `if err/!Success\{writeError;return\};writeJSON` 模式 |
| 服务发现 | Docker DNS (本地) / K8s Service DNS (生产) |
| 死信队列 | RabbitMQ DLX + DLQ, 5min TTL 自动转移 |
| 限流 | Nginx 令牌桶 3层 (login 120r/m 防爆破, heavy 200r/s, light 10000r/s) |
| CORS | 环境变量 `CORS_ORIGIN` 控制, 默认 `*`, 支持 `credentials` |

## 配置

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `JWT_SECRET` | - | JWT 签名密钥 (必设) |
| `MYSQL_ROOT_PASSWORD` | `123456` | MySQL root 密码 |
| `REDIS_PASSWORD` | `rpc-redis-123456` | Redis 密码 |
| `MINIO_ROOT_PASSWORD` | `rpc-minio-123456` | MinIO 密码 |
| `DOCKER_USER` | `http-rpc` | Docker 镜像前缀 |
| `CORS_ORIGIN` | `*` | 跨域允许的 origin |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | `jaeger:4317` | OpenTelemetry OTLP 端点 |

## CI/CD

push main/PR → GitHub Actions: 格式检查 → 构建 → 就绪探针轮询 → 功能测试 → 推送 ghcr.io

## 启动

```bash
docker compose build
docker compose up -d
bash test/e2e/functional_test.sh

# 监控服务
docker compose up -d prometheus grafana jaeger
```

## 监控接入

Prometheus 自动抓取 `http://grpc-gateway:8080/api/v1/metrics`，Grafana 数据源指向 `http://prometheus:9090`。

访问地址:
- Prometheus: `http://<host>:9090`
- Grafana: `http://<host>:3000` (admin / admin)
- Jaeger: `http://<host>:16686`
