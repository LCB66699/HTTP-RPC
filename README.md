# HTTP-RPC 微服务分布式系统

基于 gRPC + Envoy + gRPC-Gateway 的微服务数据表格系统，支持文件存储、全文搜索、多层缓存和弹性容错。

## 架构

```
浏览器 (Web UI)
   │  HTTPS
   ▼
nginx :443                           ← TLS 终结 + 令牌桶限流 (3层)
   │  least_conn → envoy_pool
   ▼
Envoy ×2 :8080                       ← JWT Cookie 鉴权 + ROUND_ROBIN + outlier_detection
   │
   ▼
gRPC-Gateway (Go) ×2 :8080          ← HTTP/JSON ↔ gRPC + 熔断器 + 重试 + readiness
   │  dns:/// + round_robin
   ├──→ Auth   (rpc-auth:50051)     ×2   MySQL rpc_auth (1 shard)
   ├──→ Sheet  (rpc-sheet:50051)    ×2   MySQL rpc_spreadsheet (2 shards) + L1/L2 缓存
   ├──→ File   (rpc-file:50051)     ×2   MySQL rpc_file (2 shards) + MinIO 对象存储
   └──→ Search (rpc-search:50051)   ×1   Elasticsearch

C++ 服务 ──RabbitMQ──→ Notify Service (Go) ──→ MongoDB + ES 索引

基础设施:
  MySQL ×9 (auth×1 + spreadsheet×4 + file×4，每片1主1从)
  Redis Cluster ×6 (3M+3S)
  Elasticsearch ×1
  MongoDB ×1
  RabbitMQ ×1 (事件总线 + DLQ 死信队列)
  MinIO ×1 (文件/表格内容对象存储)
```

## 容器清单 (~33个)

| 层 | 容器 | 数量 |
|----|------|------|
| 入口 | nginx | 1 |
| 网关 | envoy, grpc-gateway | 4 (各×2) |
| C++ 服务 | auth, sheet, file, search | 7 |
| Go 服务 | notify-service | 1 |
| 存储 | MySQL, Redis, ES, MongoDB, MinIO | 18 |
| 消息 | RabbitMQ | 1 |
| 治理 | canal-server | 1 |

## 项目结构

```
├── gateway-grpc/              Go gRPC-Gateway (HTTP→gRPC 转译)
│   ├── main.go                   路由 + JWT + 熔断器(gobreaker) + 重试 + readiness
│   ├── main_test.go              JWT 单元测试
│   └── Dockerfile
├── envoy/                     Envoy API 网关
│   ├── envoy.yaml                JWT 鉴权 + circuit_breaker + outlier_detection
│   ├── entrypoint.sh
│   ├── Dockerfile
│   └── protos/                   Go proto 副本
├── server/                    C++ gRPC 后端服务
│   ├── include/
│   │   ├── database.h            MySQL 读写分离 + ShardedDatabase 分片
│   │   ├── redis_client.h        Redis Cluster (redis-plus-plus)
│   │   ├── jwt.h                 JWT HS256 签名/验签
│   │   ├── auth_interceptor.h    gRPC JWT 拦截器
│   │   ├── l1_cache.h            L1 LRU 本地缓存 (10K/30min)
│   │   ├── l1_invalidator.h      Redis Pub/Sub → L1 缓存失效
│   │   ├── rabbit_publisher.h    RabbitMQ 事件发布
│   │   ├── minio_client.h        MinIO 客户端 (AWS SigV4)
│   │   ├── snowflake.h           Snowflake 分布式 ID
│   │   ├── call_logger.h         调用日志 + Redis 异步 flush
│   │   └── *_service_impl.h     各服务实现
│   └── src/
│       ├── main_auth.cpp         Auth 独立入口
│       ├── main_sheet.cpp        Sheet 独立入口 (含 MinIO)
│       ├── main_file.cpp         File 独立入口 (含 MinIO)
│       ├── main_search.cpp       Search 独立入口
│       └── ...
├── services/
│   └── notify-service/           Go RabbitMQ 消费者 → MongoDB + ES + MinIO
├── proto/                     Protobuf 定义
├── es/                        Elasticsearch Dockerfile
├── mongo/                     MongoDB 初始化
├── redis/cluster/             Redis Cluster 配置 + 智能 init
├── mysql/                     MySQL 主从配置
├── canal/                     Canal binlog 订阅 (预留)
├── docs/                      设计文档 + 调试指南
├── test/                      功能/性能/压力测试
├── Dockerfile                 多阶段构建 (ubuntu，支持 DEBUG)
├── docker-compose.yml         全栈部署
├── docker-compose.override.yml 本地调试 (GDB 权限 + 源码挂载)
├── dev.sh                     本地开发快捷命令
├── Makefile                   独立编译目标 (auth/sheet/file/search)
└── nginx.conf                 TLS + 三层令牌桶限流
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
| POST | `/api/register` | 否 | 注册 |
| POST | `/api/login` | 否 | 登录，返回 `rpc_at` (15min) + `rpc_rt` (7d) Cookie |
| POST | `/api/refresh` | 否 | 刷新令牌 |
| GET | `/api/me` | 否 | 当前用户信息 |
| GET | `/api/health` | 否 | 健康检查 |
| GET | `/api/health/ready` | 否 | 就绪探针 (gRPC 连接状态 + 后端可达性) |

### 表格

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| GET | `/api/sheets` | 是 | 列表 (Redis 缓存 + 分页) |
| POST | `/api/sheets` | 是 | 创建 (数据存 MinIO) |
| GET | `/api/sheets/{id}` | 是 | 获取 (MinIO + Redis + L1 三级缓存) |
| PUT | `/api/sheets/{id}` | 是 | 更新 |
| DELETE | `/api/sheets/{id}` | 是 | 删除 |

### 文件

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/files/upload` | 是 | 上传 (内容→MinIO, 元数据→MySQL) |
| GET | `/api/files` | 是 | 列表 |
| GET | `/api/files/{id}` | 是 | 下载 (302 重定向到 MinIO 预签名 URL) |
| DELETE | `/api/files/{id}` | 是 | 删除 |

### 搜索

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/search` | 是 | 全文搜索 (gRPC → Search Service → ES) |
| GET | `/api/history` | 是 | 调用日志 (Redis) |

## 核心机制

| 机制 | 实现 |
|------|------|
| 负载均衡 | Nginx least_conn → Envoy ROUND_ROBIN → Gateway dns:/// + round_robin → C++ dns:/// + round_robin (4层) |
| 熔断器 | Gateway gobreaker (Closed→Open→Half-Open), 慢调用检测 + 失败率阈值, 每服务独立 |
| gRPC 重试 | 幂等读最多3次, 指数退避 50/100/200ms, 只重试 Unavailable/DeadlineExceeded/ResourceExhausted/Aborted |
| 就绪探针 | `/api/health/ready` 检查 auth/sheet/file gRPC 连接状态, CI 轮询替代固定 sleep |
| Envoy 容错 | outlier_detection: 连续5次5xx→踢出30s |
| JWT 鉴权 | Envoy Cookie→Gateway HS256 验签→C++ AuthInterceptor→Auth.ValidateUser 四层 |
| L1 缓存 | 进程内 LRU (10K/30min), Redis Pub/Sub 失效 |
| L2 缓存 | Redis Cluster Cache-Aside, JitteredTTL 防雪崩, 逻辑过期 + 异步刷新, null marker 防穿透 |
| 对象存储 | MinIO: 文件内容 + 表格数据, 预签名URL 302下载 |
| 事件驱动 | C++ RabbitMQ 发布 → Go Notify 消费 → MongoDB + ES |
| 全文搜索 | Elasticsearch multi_match + fuzzy + highlighting, user_id 过滤 |
| 分布式 ID | Snowflake (worker_id = hash(host:port) & 0x1F) |
| 服务发现 | Docker DNS (本地) / K8s Service DNS (生产) |
| 死信队列 | RabbitMQ DLX + DLQ, 5min TTL 自动转移 |
| 限流 | Nginx 令牌桶 3层 (login 120r/m 防爆破, heavy 200r/s, light 10000r/s) |

## 配置

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `JWT_SECRET` | - | JWT 签名密钥 (必设) |
| `MYSQL_ROOT_PASSWORD` | `123456` | MySQL root 密码 |
| `REDIS_PASSWORD` | `rpc-redis-123456` | Redis 密码 |
| `MINIO_ROOT_PASSWORD` | `rpc-minio-123456` | MinIO 密码 |
| `DOCKER_USER` | `http-rpc` | Docker 镜像前缀 |

## CI/CD

push main/PR → GitHub Actions: 格式检查 → 构建 → 就绪探针轮询 → 功能测试 → 推送 ghcr.io

## 启动

```bash
docker compose build
docker compose up -d
bash test/functional_test.sh
```
