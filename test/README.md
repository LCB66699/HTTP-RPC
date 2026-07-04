# HTTP-RPC 测试套件

## 测试目标

在单台 2核4GB 云服务器上，验证一个由 20+ 容器、6 个独立微服务、5 种数据存储组成的分布式系统是否**正确、稳定、可观测**。测试体系围绕四个维度设计：

| 维度 | 目标 | 对应测试 |
|------|------|---------|
| **功能正确性** | 每个 API 的行为符合预期，边界条件有防御 | functional_test |
| **系统完整性** | 所有服务能正常启动、互连、协同完成一个完整业务流程 | docker_health |
| **性能基线** | 在已知硬件条件下建立性能参考值，后续变更可对比 | performance_test |
| **稳定性边界** | 找到系统的吞吐上限和故障恢复能力，知道什么时候该扩容 | stress_test |

测试执行顺序：**先确认活着（docker_health）→ 再逐个功能验证（functional）→ 然后性能基准 → 最后压测**。

## 系统架构（当前版本 v2）

```
浏览器 → Nginx (TLS+限流)
           ├─ /api/* → Envoy:8080 (JWT Cookie 验签)
           │              └→ gRPC-Gateway:8082 (HTTP→gRPC 自动转码)
           │                   └→ gRPC → Auth(x2) / Sheet(x3) / File(x2) / Search
           └─ /*     → serve /app/web-ui 静态文件

微服务集群:
  Auth Service ×2   (C++, 认证+Token签发, 独立镜像 176MB)
  Sheet Service ×3  (C++, 表格CRUD, RabbitMQ发布)
  File Service ×2   (C++, 文件CRUD, RabbitMQ发布)
  Search Service     (C++, ES查询)
  Notify Service     (Go, RabbitMQ消费 → MongoDB + ES 索引)
  gRPC-Gateway      (Go, HTTP→gRPC 转码 + JWT校验, 42MB)

数据层:
  MySQL×5(分库主从) + Redis Cluster×6 + MongoDB + Elasticsearch + RabbitMQ + Consul
```

| 层 | 核心技术 | 测试关注点 |
|----|---------|----------|
| 边缘代理 | Nginx TLS + least_conn + 令牌桶限流 | 连接复用、TLS 开销、429 速率限制 |
| API 网关 | Envoy JWT Auth Filter (Cookie→HS256验签) | Token 有效期、401 拦截 |
| 协议转码 | gRPC-Gateway (Go, proto 注解自动转码) | JSON↔Protobuf 正确性 |
| 服务间通信 | gRPC round_robin + Consul 服务注册 | 副本故障切换、健康检查 |
| 事件驱动 | RabbitMQ Topic 交换机 + Notify 消费者 | 消息可达性、死信队列 |
| 缓存 | Cache-Aside + L1 LRU + Canal binlog 失效 | 命中率、近实时一致性 |
| 搜索 | Elasticsearch + IK 中文分词 | 索引延迟、高亮、分词 |
| 文档存储 | MongoDB (解析后文本) + ES (搜索索引) | 查询性能 |
| 安全 | JWT HS256 双 Token (Access 15min + Refresh 7d) + 盗用检测 | Token 刷新、吊销 |

## 文件说明

| 文件 | 用途 | 耗时 |
|------|------|------|
| `run_unit_tests.sh` | C++ 单元测试：GoogleTest 构建 + CTest 运行 | ~20s |
| `e2e/docker_health.sh` | 全容器健康检查，逐个验证 healthy 状态 | ~2min |
| `e2e/functional_test.sh` | 功能正确性：认证、鉴权、CRUD、缓存、Token 刷新、文件完整性 | ~45s |
| `e2e/performance_test.sh` | 性能基准：预热 → 单请求延迟 → 并发(P50/P95/P99) → 缓存命中率 → ab/wrk2 QPS | ~90s |
| `e2e/stress_test.sh` | 逐层压测：L0 阶梯加压 → L1 内网 → L2 TLS → L3 公网 → L4 故障转移 → L5 稳定性 | ~2-37min |
| `wrk/health.lua` | wrk2 GET 基准：纯网关吞吐，含自定义延迟分布报告 | — |
| `wrk/mixed.lua` | wrk2 读写混合：70%列表/20%获取/10%创建，模拟真实流量 | — |

## API 端点

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/health | 健康检查 |
| POST | /api/login | 登录 |
| POST | /api/register | 注册 |
| POST | /api/refresh | Token 刷新（ATT→新AT） |
| POST | /api/search | 全文搜索（ES） |
| GET | /api/me | 当前用户信息 |
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
| GET | /api/files/preview | 文件预览（MongoDB 读取文本） |
| GET | /metrics | Prometheus 指标 |

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

## Quick Start（推荐）

```bash
# 全量测试（按顺序执行）
make test-all

# 快速冒烟（5min内完成）
make test-smoke

# 单独测试
make test-functional      # 功能正确性
make test-performance     # 性能基准
make test-stress          # 压力测试
make test-docker-health   # 容器健康检查

# 清理测试日志
make test-clean
```

## 功能测试

```bash
bash test/e2e/functional_test.sh
bash test/e2e/functional_test.sh https://192.168.1.100
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
bash test/e2e/performance_test.sh
bash test/e2e/performance_test.sh https://192.168.1.100 20   # 自定义地址 + 并发数
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
bash test/e2e/stress_test.sh
bash test/e2e/stress_test.sh https://localhost 5000 20
#                               API         请求数  并发数

# 全时长稳定性测试 (30min)
bash test/e2e/stress_test.sh --long

# 自定义参数 + 全时长
bash test/e2e/stress_test.sh --long https://localhost 5000 20
STRESS_LONG_DURATION=3600 STRESS_LONG_READERS=20 STRESS_LONG_WRITERS=5 \
    bash test/e2e/stress_test.sh --long

# 跳过 L5
STRESS_LONG=0 bash test/e2e/stress_test.sh
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
| L5 稳定性 | nginx容器→gateway-1:8081 | 混合读写(80%+20%)，默认 60s 浸泡 / --long 30min | P99漂移/RSS/FD/熔断器/错误率 |

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
| L5 P99 漂移 > 2x 或 RSS/FD 持续增长 | 资源泄漏（内存/连接/文件描述符） |

### 测试要点

- **L0-L1 在 nginx 容器内用 ab 直打 gateway:8081**，避免外网 TLS 干扰
- **全部用 ab -k (keep-alive)**，避免 TCP 握手占满连接数
- **压测时 nginx 限流已注释**（nginx.conf `limit_req` 行），避免 429 干扰
- **ab 首次运行自动安装**到 nginx 容器（apt-get apache2-utils），后续秒启
- **L5 默认运行 60s 快速浸泡**，加 `--long` 切换到 30min 全时长 + 系统指标采集。跳过: `STRESS_LONG=0`

### wrk2 手动使用

```bash
# 健康检查基线 (恒定速率 200r/s, 30s)
wrk2 -t4 -c10 -d30s -R200 --latency https://localhost/api/health

# 使用 Lua 脚本获得详细延迟分布
wrk2 -t4 -c10 -d30s -R200 --latency \
    -s test/wrk/health.lua \
    https://localhost/api/health

# 混合读写负载 (需要 token)
export RPC_TOKEN="eyJ..."  # 从 cookie jar 或 functional_test 输出获取
wrk2 -t4 -c20 -d60s -R200 --latency \
    -s test/wrk/mixed.lua \
    https://localhost/api/sheets

# 自定义混合比例
MIXED_LIST_PCT=50 MIXED_GET_PCT=30 RPC_TOKEN="$TOK" \
    wrk2 -t4 -c20 -d60s -R200 --latency \
    -s test/wrk/mixed.lua \
    https://localhost/api/sheets
```

## 故障注入测试

```bash
# 1. gRPC 副本容错
docker stop http-rpc-sheet-2-1
bash test/e2e/functional_test.sh
docker start http-rpc-sheet-2-1

# 2. Redis Cluster 故障转移（gossip 自动选举，5s 内恢复）
docker stop http-rpc-redis-cluster-1
sleep 8
docker logs http-rpc-redis-cluster-4 --tail 5
bash test/e2e/functional_test.sh
docker start http-rpc-redis-cluster-1

# 3. MySQL 分片 Slave 容错
docker stop http-rpc-mysql-spreadsheet-0-slave-1
bash test/e2e/functional_test.sh
docker start http-rpc-mysql-spreadsheet-0-slave-1

# 4. 熔断器验证：停全部 sheet 副本，触发 OPEN
docker stop http-rpc-sheet-1-1 http-rpc-sheet-2-1 http-rpc-sheet-3-1
for i in $(seq 1 6); do
  curl -sk https://localhost/api/sheets -H "Authorization: Bearer $TOKEN"
done
# 第 6 次起返回 circuit open，health 显示 breaker:OPEN
curl -sk https://localhost/api/health | python3 -m json.tool
docker start http-rpc-sheet-1-1 http-rpc-sheet-2-1 http-rpc-sheet-3-1
```
