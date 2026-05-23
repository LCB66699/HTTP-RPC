# HTTP-RPC 测试套件

## 系统架构（当前版本）

```
浏览器 → nginx(TLS+keepalive)
           ├─ /api/* → proxy_pass gateway:8081 (HTTP/1.1)
           └─ /*     → serve /app/web-ui 静态文件
                         │
                    Gateway (双端口)
                      ├─ 8081: httplib HTTP/1.1 (nginx 用)
                      └─ 8080: nghttp2 h2c + ThreadPool (预留)
                         │
                      JWT 鉴权 + 多维熔断 + RESTful 路由
                         │
                      gRPC(round_robin) → Auth(x2) / Sheet(x3) / File(x2)
                         └→ MySQL(1主2从, 可hash分片) + Redis Cluster(6节点,3M+3S) + MinIO
```

| 层 | 核心技术 | 测试关注点 |
|----|---------|----------|
| 边缘代理 | nginx TLS + keepalive 128 + 静态文件 serve | 连接复用、TLS 开销 |
| 网关 8081 | httplib + 同步 gRPC + JWT + 多维熔断(滑动窗口/P99/慢调用率) | 并发能力、JWT 验证 |
| 网关 8080 | nghttp2 h2c + ThreadPool(eventfd唤醒) + poll I/O | stream 多路复用 |
| 负载均衡 | gRPC round_robin + Docker DNS 别名多 IP + PerReplicaTracker 副本隔离 | 副本故障不中断 |
| 缓存 | Cache-Aside + 逻辑过期异步刷新 + 版本号失效 + 空值防穿透 | 命中率、一致性 |
| 数据库 | MySQL 主从读写分离 + ShardedDatabase(user_id%N hash分片) | 分片路由、并发冲突 |
| Redis HA | Redis Cluster 6节点 (3M+3S, gossip协议自动故障转移) | 故障转移 RTO |
| 安全 | JWT HS256 + token_version 吊销 (Redis) + Gateway+Server 双验签 | Token 有效期、吊销 |
| 分布式事务 | 2PC TM + RM + undo_log 补偿 | Prepare/Commit/Rollback |
| 对象存储 | MinIO S3-compatible | 文件上传下载完整性 |

## 文件说明

| 文件 | 用途 | 耗时 |
|------|------|------|
| `functional_test.sh` | 功能正确性：认证、鉴权、CRUD、缓存、Token 吊销、文件完整性、熔断器/健康状态 | ~30s |
| `performance_test.sh` | 性能基准：预热 → 单请求延迟 → 并发(P50/P95/P99) → 缓存命中率 → 写入压测 → ab/wrk2 QPS | ~90s |
| `stress_test.sh` | 逐层压测：L0 阶梯加压 → L1 内网 → L2 TLS → L3 公网 → L4 故障转移 → L5 稳定性 | ~2-7min |
| `wrk_scripts/health.lua` | wrk2 GET 基准：纯网关吞吐，含自定义延迟分布报告 | — |
| `wrk_scripts/mixed.lua` | wrk2 读写混合：70%列表/20%获取/10%创建，模拟真实流量 | — |

## API 端点

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/health | 健康检查（gRPC Channel 状态 + 熔断器） |
| POST | /api/login | 登录 |
| POST | /api/register | 注册 |
| GET | /api/services | 服务列表 |
| GET | /api/history | 调用历史 |
| POST | /api/tx/begin | 分布式事务 |
| POST | /api/sheets | 创建表格 |
| GET | /api/sheets | 表格列表 |
| POST | /api/sheets/get | 获取单个表格 |
| PUT | /api/sheets | 更新表格 |
| POST | /api/sheets/delete | 删除表格 |
| GET | /api/files | 文件列表 |
| POST | /api/files/upload | 上传文件 |
| GET | /api/files/download | 下载文件 |
| POST | /api/files/delete | 删除文件 |

## 前置条件

```bash
cd ~/HTTP-RPC
docker compose up -d
docker compose ps  # 确认全部 healthy
sudo apt install -y curl bc apache2-utils python3

# 可选：wrk2 用于恒定速率压测和混合负载（推荐）
sudo apt install -y wrk
# 或手动编译 wrk2（支持 -R 恒定速率）:
# git clone https://github.com/giltene/wrk2 && cd wrk2 && make && sudo cp wrk /usr/local/bin/wrk2
```

## 功能测试

```bash
bash test/functional_test.sh
bash test/functional_test.sh https://192.168.1.100
```

### 测试覆盖（24 项）

| 模块 | 测试项 | 断言 |
|------|--------|------|
| 连通性 | /api/health 可达 | HTTP 200 |
| 认证 | 注册、重复注册、登录、错误密码、短用户名、短密码 | success + token |
| 鉴权 | 无 Token、假 Token | HTTP 401 |
| 表格 CRUD | 创建、列表、获取、缓存命中、更新、删除 | success + cache_source |
| 文件管理 | 上传、列表、下载校验、删除 | 文件内容完整性 |
| Token 吊销 | 递增 token_version → Redis → 旧 Token 被拒 | HTTP 401 |
| 监控 | /api/health + /api/services | gateway:READY + breaker |

### 设计约束

- **重复注册重试 3 次**：gRPC round_robin 可能分发到不同 auth 实例（竞态窗口）
- **默认 HTTPS**：nginx 301 HTTP→HTTPS，测试用 `-k` 跳过自签证书
- **Token 解析用 python3**：`json.load()` 精确提取，避免 sed 切 JSON 的脆弱性
- **JWT 格式校验**：Token 必须以 `eyJ` 开头，否则脚本直接报错退出

## 性能测试

```bash
bash test/performance_test.sh
bash test/performance_test.sh https://192.168.1.100 20   # 自定义地址 + 并发数
```

### 测试指标

| 阶段 | 内容 | 指标 |
|------|------|------|
| 0. 预热 | 50 次 health + 10 次 sheet list | 预热 gRPC/Redis/MySQL 连接池（不计分） |
| 1. 单请求 RTT | Login, 401, Sheet(cached×2), Health | 冷连接延迟 (s) |
| 2. 并发 | Health × c, Sheet List × c, Sheet Get × c | P50/P95/P99 + max (xargs -P 精确同步) |
| 3. 缓存命中率 | 20 次 GET /api/sheets/1 | Redis 命中 / MySQL 回源 / 首次写入 |
| 4. 大文件 | 1MB 文件上传 | 延迟 |
| 5. 负载 | 300 请求 / 10c (xargs -P) | 吞吐量 (req/s) |
| 6. 写入压测 | 10×(创建→获取→更新→删除) | P50/P95/P99 + 成功率 |
| 7. ab 验证 | 1000 请求 / 10c keep-alive | QPS + P50/P95/P99 |
| 8. wrk2 | 30s 恒定速率 200r/s + 混合负载 | QPS + P50/P90/P99/P99.9 (可选) |

### P50/P95/P99 百分位指标解释

| 指标 | 含义 | 为什么重要 |
|------|------|-----------|
| **P50** (中位数) | 50% 的请求比这个快 | 典型用户体验 |
| **P95** | 95% 的请求比这个快 | 绝大多数用户的体验上限 |
| **P99** | 99% 的请求比这个快 | 长尾延迟 — 影响用户体验的关键指标 |
| **P99.9** | 99.9% 的请求比这个快 | 极端长尾 — 通常是 GC/锁竞争/网络抖动 |
| **avg** | 算术平均 | 受极端值影响大，对性能分析价值有限 |

> **为什么 P99 比 avg 重要？** 假设 100 个用户中 99 个在 10ms 内得到响应，1 个等了 10s。avg 显示 110ms（看起来还行），但 P99 显示 10s（揭示真实问题）。P99 长尾通常是系统瓶颈的第一信号。

## 压力测试（逐层定位）

```bash
bash test/stress_test.sh
bash test/stress_test.sh https://localhost 5000 20
#                               API         请求数  并发数

# 长时间稳定性测试 (5 分钟恒定负载)
STRESS_LONG=1 bash test/stress_test.sh
```

### 七层递进

| 层 | 路径 | 测试内容 | 指标 |
|----|------|---------|------|
| L0 阶梯加压 | nginx容器→gateway:8081 | ab -k, 10→20→50→100→200c 递增 | 自动检测 QPS 饱和点 / P99 拐点 |
| L1a 内网基线 | nginx容器→gateway-1:8081 | 纯网关 health (无JWT/无gRPC) | QPS + P50/P95/P99 |
| L1b 内网全栈 | nginx容器→gateway-2:8081 | Sheet List (JWT+gRPC+Redis+MySQL) | QPS + P50/P95/P99 |
| L1d 写入压测 | nginx容器→gateway-1:8081 | 并发创建→获取→更新→删除循环 | ops/s + 成功率 |
| L2 本地TLS | localhost:443 | 加 nginx TLS 开销 | QPS + P50/P95/P99 |
| L3 公网 | 106.53.100.198 | 加外网延迟 | QPS + P50/P95/P99 |
| L4 故障转移 | localhost:443 | 停 gateway-1，验证流量切到 gateway-2 | 失败请求数 |
| L5 稳定性 | nginx容器→gateway-1:8081 | 5min 恒定 100r/s，每 30s 采样 | P99 漂移检测 (STRESS_LONG=1) |

### 瓶颈判定

| 现象 | 定位 |
|---|---|
| L0 拐点并发 < 50 | 网关线程池/连接池不足 |
| L1 QPS < 500 | httplib 线程模型瓶颈 |
| L1d 写入吞吐 < 读取 QPS × 0.3 | 数据库写瓶颈（乐观锁竞争 / MySQL 主库压力） |
| L1b QPS << L1a | gRPC/Redis/MySQL 后端瓶颈 |
| L2 QPS < L1a × 0.8 | nginx/TLS 开销过大 |
| L3 P99 > 200ms | 外网带宽/延迟 |
| L4 失败 > 5/20 | nginx 健康探测间隔过长 |
| L5 P99 漂移 > 2x | 资源泄漏（内存/连接/文件描述符） |

### 测试要点

- **L0-L1 在 nginx 容器内用 ab 直打 gateway:8081**，避免外网 TLS 干扰
- **全部用 ab -k (keep-alive)**，避免 TCP 握手占满连接数
- **压测时 nginx 限流已注释**（nginx.conf `limit_req` 行），避免 429 干扰
- **ab 首次运行自动安装**到 nginx 容器（apt-get apache2-utils），后续秒启
- **L5 默认关闭**，设置 `STRESS_LONG=1` 启用。适合通宵跑或 CI 流水线

### wrk2 手动使用

```bash
# 健康检查基线 (恒定速率 200r/s, 30s)
wrk2 -t4 -c10 -d30s -R200 --latency https://localhost/api/health

# 使用 Lua 脚本获得详细延迟分布
wrk2 -t4 -c10 -d30s -R200 --latency \
    -s test/wrk_scripts/health.lua \
    https://localhost/api/health

# 混合读写负载 (需要 token)
export RPC_TOKEN="eyJ..."  # 从 cookie jar 或 functional_test 输出获取
wrk2 -t4 -c20 -d60s -R200 --latency \
    -s test/wrk_scripts/mixed.lua \
    https://localhost/api/sheets

# 自定义混合比例
MIXED_LIST_PCT=50 MIXED_GET_PCT=30 RPC_TOKEN="$TOK" \
    wrk2 -t4 -c20 -d60s -R200 --latency \
    -s test/wrk_scripts/mixed.lua \
    https://localhost/api/sheets
```

## 故障注入测试

```bash
# 1. gRPC 副本容错
docker stop http-rpc-sheet-2-1
bash test/functional_test.sh
docker start http-rpc-sheet-2-1

# 2. Redis Cluster 故障转移（gossip 自动选举，5s 内恢复）
docker stop http-rpc-redis-cluster-1
sleep 8
docker logs http-rpc-redis-cluster-4 --tail 5
bash test/functional_test.sh
docker start http-rpc-redis-cluster-1

# 3. MySQL Slave 容错
docker stop http-rpc-mysql-slave-1-1
bash test/functional_test.sh
docker start http-rpc-mysql-slave-1-1

# 4. 熔断器验证：停全部 sheet 副本，触发 OPEN
docker stop http-rpc-sheet-1-1 http-rpc-sheet-2-1 http-rpc-sheet-3-1
for i in $(seq 1 6); do
  curl -sk https://localhost/api/sheets -H "Authorization: Bearer $TOKEN"
done
# 第 6 次起返回 circuit open，health 显示 breaker:OPEN
curl -sk https://localhost/api/health | python3 -m json.tool
docker start http-rpc-sheet-1-1 http-rpc-sheet-2-1 http-rpc-sheet-3-1
```
