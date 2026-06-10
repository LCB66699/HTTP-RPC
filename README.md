# HTTP-RPC 微服务分布式系统

基于 gRPC + Envoy + gRPC-Gateway 的微服务数据表格系统，Docker Compose 单机多容器部署，演示灰度发布、事件驱动、全文搜索和 Cache-Aside 缓存。

## 架构

```
浏览器 (Web UI)
   │  HTTPS
   ▼
nginx :443                        ← TLS 终结 + 令牌桶限流 + 静态文件
   │  upstream → envoy_pool
   ▼
Envoy Proxy :8080                 ← JWT Cookie 鉴权 (HS256) + 反向代理
   │
   ▼
gRPC-Gateway (Go) :8080          ← HTTP/JSON ↔ gRPC 转译 + JWT 中间件
   │  gRPC round_robin + keepalive
   ├──→ Auth   (rpc-auth:50051)    副本 ×2   DB: rpc_auth           Redis
   ├──→ Sheet  (rpc-sheet:50051)   副本 ×2   DB: rpc_spreadsheet     Redis + L1 缓存 + Canal
   ├──→ File   (rpc-file:50051)    副本 ×2   DB: rpc_file            Redis + L1 缓存
   └──→ Search (rpc-search:50051)  副本 ×1   Elasticsearch

Notify Service (Go)               ← RabbitMQ 消费 → MongoDB + ES 索引

基础设施:
  MySQL ×9  (auth×1 + spreadsheet×4 + file×4, 每片1主1从)
  Redis Cluster ×6 (3M+3S)
  Elasticsearch ×1 (IK 中文分词)
  MongoDB ×1 (文档存储)
  RabbitMQ ×1 (事件总线 + 死信队列)
  Consul ×1  (服务注册与健康检查)
  Canal ×1   (binlog 订阅 → L1 缓存失效)
```

### 容器清单 (~31个)

| 层 | 容器 | 数量 |
|----|------|------|
| 入口 | nginx | 1 |
| 网关 | envoy, grpc-gateway | 2 |
| C++ 服务 | auth, sheet, file, search | 7 |
| Go 服务 | notify-service | 1 |
| 存储 | MySQL, Redis, ES, MongoDB | 17 |
| 消息 | RabbitMQ | 1 |
| 治理 | Consul, Canal | 2 |

## 项目结构

```
├── gateway-grpc/              Go gRPC-Gateway (HTTP→gRPC 转译)
│   ├── main.go                   路由 + JWT 鉴权 + Cookie 管理
│   └── Dockerfile
├── envoy/                     Envoy API 网关
│   ├── envoy.yaml                JWT 鉴权 + 反向代理
│   ├── entrypoint.sh             JWT_SECRET 注入
│   ├── Dockerfile
│   └── protos/                   Go proto 定义
├── server/                    C++ gRPC 后端服务
│   ├── include/
│   │   ├── database.h            MySQL 读写分离 + ShardedDatabase 分片
│   │   ├── redis_client.h        Redis Cluster (redis-plus-plus)
│   │   ├── jwt.h                 JWT HS256 签名/验签 (OpenSSL HMAC)
│   │   ├── auth_interceptor.h    gRPC JWT 拦截器
│   │   ├── l1_cache.h            L1 LRU 本地缓存 (10K容量/30min TTL)
│   │   ├── l1_invalidator.h      Canal binlog → L1 缓存失效
│   │   ├── rabbit_publisher.h    RabbitMQ 事件发布
│   │   ├── snowflake.h           Snowflake 分布式 ID
│   │   ├── call_logger.h         调用日志 + Redis 异步 flush
│   │   └── *_service_impl.h      各服务实现头文件
│   └── src/
│       ├── main_auth.cpp         Auth 独立入口
│       ├── main_sheet.cpp        Sheet 独立入口
│       ├── main_file.cpp         File 独立入口
│       ├── main_search.cpp       Search 独立入口
│       └── ...
├── services/                  各服务独立 Dockerfile
│   ├── notify-service/           Go RabbitMQ 消费者
│   ├── auth-service/             (预留)
│   └── ...
├── proto/                     Protobuf 定义 (C++ 用)
├── consul/                    Consul 注册脚本
├── es/                        Elasticsearch Dockerfile + IK 分词
├── mongo/                     MongoDB 初始化脚本
├── redis/cluster/             Redis Cluster 配置
├── mysql/                     MySQL 主从配置
├── Dockerfile                 多阶段构建 (ubuntu → runtime)
├── docker-compose.yml         全栈部署
├── Makefile                   独立编译目标 (auth/sheet/file/search)
└── nginx.conf                 TLS + 限流 + DNS 自动刷新
```

## 独立编译 & 灰度更新

每个 C++ 服务只编译自己的代码：

```bash
make auth     # → rpc_auth   (5 cpp)
make sheet    # → rpc_sheet  (9 cpp)
make file     # → rpc_file   (8 cpp)
make search   # → rpc_search (4 cpp)
```

Docker 构建同样独立：

```bash
docker compose build auth-1     # 只编 auth, 20s
docker compose build sheet-1    # 只编 sheet
docker compose up -d auth-1     # 滚动重启, 其他服务无影响
```

## API 接口

### 认证

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/register` | 否 | 注册，返回 JWT Cookie |
| POST | `/api/login` | 否 | 登录，返回 `rpc_at` (15min) + `rpc_rt` (7d) Cookie |
| POST | `/api/refresh` | 否 | 刷新令牌（`rpc_rt` Cookie 驱动，无需 body） |
| GET | `/api/me` | 否 | 当前用户信息（username + user_id） |
| GET | `/api/health` | 否 | 健康检查 |

### 数据表格

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| GET | `/api/sheets` | 是 | 列表 (Redis 缓存, 支持分页) |
| POST | `/api/sheets` | 是 | 创建 |
| GET | `/api/sheets/{id}` | 是 | 获取单表 |
| PUT | `/api/sheets/{id}` | 是 | 更新 |
| DELETE | `/api/sheets/{id}` | 是 | 删除 |

### 文件管理

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/files/upload` | 是 | 上传 (multipart) |
| GET | `/api/files` | 是 | 列表 |
| GET | `/api/files/{id}` | 是 | 下载 |
| DELETE | `/api/files/{id}` | 是 | 删除 |

### 搜索

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| POST | `/api/search` | 是 | 全文搜索 (ES IK 分词 + user_id 过滤) |
| GET | `/api/history` | 是 | 调用日志 (Redis) |

## 核心机制

| 机制 | 实现 |
|------|------|
| JWT 鉴权 | Envoy Cookie 提取 + gRPC-Gateway HS256 验签 → gRPC metadata 注入 |
| L1 缓存 | 进程内 LRU (10K, 30min), Canal binlog 驱动失效 |
| L2 缓存 | Redis Cluster Cache-Aside, JitteredTTL 防雪崩 |
| 事件驱动 | C++ RabbitMQ 发布 → Go Notify 消费 → MongoDB + ES |
| 全文搜索 | Elasticsearch IK 分词, scope 过滤, 分页, 高亮 |
| 分布式 ID | Snowflake (worker_id = hash(host:port) & 0x1F) |
| 服务发现 | Docker DNS + Consul 注册/心跳 |
| 2PC 事务 | TM.Begin → Prepare → Commit/Rollback + undo_log |

## 测试

```bash
bash test/functional_test.sh      # 功能测试 (21项)
bash test/performance_test.sh     # 性能测试 (延迟分位 + QPS)
bash test/stress_test.sh          # 逐层压测 (L1-L4)
bash test/docker_health.sh        # 容器健康检查
```

## CI/CD

push main/PR → GitHub Actions 自动构建 + 测试。

## 启动

```bash
docker compose build
docker compose up -d
bash test/docker_health.sh
```

## 配置

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `JWT_SECRET` | `default-secret-32bytes-here!!!!!` | JWT 签名密钥 (生产环境必须覆盖) |
| `MYSQL_ROOT_PASSWORD` | `123456` | MySQL root 密码 |
| `REDIS_PASSWORD` | `rpc-redis-123456` | Redis 密码 |
