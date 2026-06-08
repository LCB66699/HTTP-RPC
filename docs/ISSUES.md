# 已知问题与架构不足

## 一、SQL 分库路由

### 1.1 取模路由无法在线扩缩容

**现状**：`user_id % shard_count` 决定数据落在哪个分片。当前 shard_count=2。

**问题**：增加分片（如 2→3）会导致几乎所有数据的路由结果变化，必须全量迁移。没有用一致性哈希或虚拟桶。

**影响**：业务增长后无法在不中断服务的情况下扩容。

### 1.2 Auth 表不分片，单点瓶颈

**现状**：`ShardedDatabase` 对 auth 操作强制走 `shards_[0]`，用户认证不可水平扩展。

**影响**：用户量增长后 auth 库先成为瓶颈。

### 1.3 广播查询 O(n)

**现状**：`GetFileStoragePath`、`GetUndoLog` 等按 `id` 查询的方法，因缺少 `user_id` 无法定位分片，只能遍历所有分片逐个查询，第一个命中即返回。

```cpp
// database.cpp:922
for (auto& db : shards_)
    if (db->GetFileStoragePath(id, storage_path)) return true;
```

**影响**：2 分片时影响不大，分片数增加后每次查询的延迟线性增长。

---

## 二、Redis Cluster

### 2.1 单 seed 初始化

**现状**：`RedisClient::Connect()` 只取 `cluster_seeds_[0]` 建立初始连接，其余 seed 被忽略。

```cpp
// redis_client.cpp:18
opts.host = cluster_seeds_[0].substr(0, colon);
```

**问题**：若第一个 seed 恰好不可达，构造函数抛异常，整个 Redis 功能降级关闭，即使其他 2 个 seed 正常。

**缓解**：redis-plus-plus 连接成功后会自动发现完整拓扑，后续节点故障不影响。仅影响初始连接那一刻。

### 2.2 未启用 READONLY 从节点读

**现状**：所有 Redis 操作路由到 master 节点。

**影响**：读压力集中在 3 个 master，3 个 slave 只做冗余不承担读流量。

### 2.3 容器 IP 漂移导致集群分裂

**现状**：`docker compose up -d --force-recreate` 重建 Redis 容器时 Docker 可能分配新 IP，`nodes.conf` 存的是旧 IP，节点互相找不到。

**已修复**：`redis/cluster/init.sh` 智能初始化脚本自动检测 IP 漂移并重建集群。详见 commit。

---

## 三、gRPC 负载均衡与故障隔离

### 3.1 File upload/download 重复实现重试逻辑

**现状**：Sheet CRUD 走 `RepRetry` 模板函数处理重试+副本隔离，File upload/download 手写了一份相同的 for 循环逻辑。

```cpp
// gateway.cpp:954 — 手写版（file upload）
for (int attempt = 0; attempt < 2; ++attempt) {
    st = file_stub_->CreateFile(&ctx, freq, &fresp);
    // ...
}
```

**影响**：两套代码维护，逻辑可能不一致，File 路径缺少对 `RecordResult` 时机的精细控制。

### 3.2 ShouldTripService 未接入熔断器

**现状**：`PerReplicaTracker::ShouldTripService()` 已实现（超过半数副本隔离时返回 true），但全代码库无人调用。

```cpp
// circuit_breaker.h — PerReplicaTracker
bool ShouldTripService() const {
    // 超过一半副本被隔离 → 应触发服务级熔断
}
```

**影响**：副本级隔离和服务级熔断各自独立决策，大面积故障时熔断响应不及时。

### 3.3 gRPC keepalive 触发 ENHANCE_YOUR_CALM

**现状**：Gateway 每 60s 发 keepalive ping（`GRPC_ARG_KEEPALIVE_TIME_MS=60000`），后端服务端认为太频繁，定期发送 GOAWAY 掐断连接。

**影响**：gRPC 连接被周期性重置，期间请求可能短暂失败（自动重试可恢复）。

---

## 四、全链路保护隔离（已实现但联动不足）

```
请求 → ① cb.AllowRequest()        熔断器（服务级）
     → ② sem_->try_acquire()       并发控制（Gateway 级）
     → ③ inner() → RepRetry()     副本隔离（IP:port 级）
     → ④ cb.RecordResult()         熔断反馈
```

| 层级 | 组件 | 粒度 | 触发条件 | 动作 |
|------|------|------|---------|------|
| L1 | `sem_` 信号量 | 全 Gateway | 并发 > max_concurrent | 排队超时 → 503 |
| L2 | `CircuitBreaker` | 服务级 | 滑动窗口错误率/慢调用率超阈值 | 快速失败 503 |
| L3 | `PerReplicaTracker` | 副本 IP:port | 连续失败 5 次 | 隔离 30s，RepRetry 换副本 |

**不足**：三层各自独立决策，L3 大面积故障时不会通知 L2 提前熔断。`ShouldTripService()` 已实现但未接入。

---

## 五、前端认证（已修复）

### 5.1 checkAuth 盲信 localStorage

**问题**：`checkAuth()` 只用 `localStorage.rpc_user` 判断登录状态，但 JWT Cookie（HttpOnly, 15min）过期后 localStorage 仍在，导致"看到主界面但 API 全 401"。

**修复**：新增 `GET /api/me` 端点，`checkAuth()` 改为先调此接口验证 Cookie 有效性。

---

## 六、部署配置（已修复）

### 6.1 nginx 未映射端口

**修复**：`nginx-1` 添加 `ports: "80:80"` 和 `ports: "443:443"`。

### 6.2 MySQL 容器缺网络别名

**修复**：`mysql-spreadsheet-0/1` 添加 `aliases: [mysql-spreadsheet]`，`mysql-file-0/1` 添加 `aliases: [mysql-file]`。

### 6.3 Redis 容器缺端口专用别名

**修复**：6 个 Redis 节点各添加端口专用别名（`redis-cluster-7000` ~ `redis-cluster-7005`），所有服务改用专用别名连接。

### 6.4 init.sql 数据库名不匹配分片命名

**修复**：`init.sql` 改为创建 `rpc_spreadsheet_0`、`rpc_spreadsheet_1`、`rpc_file_0`、`rpc_file_1`。

### 6.5 Gateway 透传空 token 到后端

**修复**：`VerifyAccessToken` 新增 `raw_token` 输出参数，`VerifyAuth` 透传真实 JWT 给后端 gRPC 认证。

### 6.6 日志缓冲不可见

**修复**：`server/src/main.cpp` 添加 `setbuf(stdout, NULL); setbuf(stderr, NULL)`。

---

## 七、LVS

### 7.1 VIP 为内网地址

**现状**：VIP 默认 `192.168.1.100`，云服务器外部不可达，LVS 实际未生效。

### 7.2 仅 TCP_CHECK

**现状**：LVS 健康检查只验证端口可达，不感知 nginx 应用层死活（如 worker 全卡死但端口仍 listening）。

---

## 八、Elasticsearch 全文搜索集成

### 8.1 proto 注解导致 C++ 编译失败

**问题**：为 proto 添加 `google.api.http` 注解和 `import "google/api/annotations.proto"` 后，C++ 编译时报 `google/api/annotations.pb.h: No such file or directory`。

**原因**：HTTP 注解仅供 Envoy 的 gRPC-JSON 转码使用，C++ 代码不需要。protoc 编译 proto 时会尝试生成 annotations 的头文件，但 google/api 目录不在 C++ 构建的 include path 中。

**解决**：从 C++ 项目引用的 proto 中移除 `import "google/api/annotations.proto"` 及所有 `option (google.api.http)` 注解，清理为纯 gRPC 定义。Envoy 需要的带注解版本在 `envoy/Dockerfile` 中单独构建 descriptor。

### 8.2 ES 容器数据目录权限导致无法启动

**问题**：ES 容器反复报 `failed to obtain node locks, tried [/usr/share/elasticsearch/data]`，`NoSuchFileException: node.lock`。

**原因**：`docker-compose.yml` 使用 bind mount `./data/elasticsearch:/usr/share/elasticsearch/data`，ES 进程以 uid 1000 运行。首次启动后目录 owner 变为 1000，后续宿主机 `rm -rf` 权限不足（Permission denied），残留的锁文件导致 ES 拒绝启动。

**解决**：改用 Docker named volume `es_data:/usr/share/elasticsearch/data`，由 Docker 管理生命周期。`sudo rm -rf` 可清理 bind mount 残留。

### 8.3 ES 内存不足反复崩溃

**问题**：ES 启动后 `java.lang.IllegalStateException: failed to obtain node locks` 反复重启。

**原因**：默认 `-Xms1g -Xmx1g` 在 3.6GB 内存服务器上加上 MySQL ×5、Redis ×6、MinIO、MongoDB 等多个容器后不足。`bootstrap.memory_lock=true` 也要求锁定内存。

**解决**：降为 `-Xms512m -Xmx512m`，设置 `bootstrap.memory_lock=false`。开发环境可接受。

### 8.4 Gateway 挂载 doc-parser 导致搜索数据不更新

**问题**：前端创建表格/上传文件后搜索不到。

**原因**：Gateway 通过 `NotifyDocParser()` 异步通知 doc-parser（`http://doc-parser:9002`）做文档解析和 ES 索引，但 doc-parser 因构建失败未运行，所有索引请求丢失。

**解决**：新增 `IndexToES()` 方法，Gateway 直接调用 ES REST API（`PUT /{index}/_doc/{id}`）写入索引，不经过 doc-parser。doc-parser 降级为可选的内容解析增强组件。

### 8.5 Snowflake 64位 ID 在 JSON 响应中被截断

**问题**：搜索返回的 `id` 字段显示为负数（如 `-1769984000`），而实际 ID 为 `55976517452181504`。

**原因**：nlohmann::json 的 `value("id", 0)` 默认将数字解析为 `int`（32位），Snowflake 生成的 64 位 ID 溢出。

**解决**：改为 `std::to_string(src.value("id", 0LL))` 输出为字符串，避免 JSON 数字精度丢失。

### 8.6 ES 索引中 type 字段缺失

**问题**：搜索结果的 `type` 字段为空字符串，前端无法区分表格和文件。

**原因**：`HandleSheetCreate`/`HandleSheetUpdate` 中构造 ES 文档时漏写 `idx_req["type"] = "sheet"`。

**解决**：补上 `type` 字段。

### 8.7 前端搜索 JS DOM 空指针

**问题**：第二次搜索时报 `Cannot read properties of null (reading 'classList')`。

**原因**：`search-empty` 元素嵌套在 `search-results-container` 内，第一次搜索后 `innerHTML = ''` 将其删除，第二次搜索时 `document.getElementById('search-empty')` 返回 null。

**解决**：所有 DOM 操作添加 null check（`if (el) el.classList...`）。

---

## 九、Envoy API 网关集成

### 9.1 proto descriptor 构建失败（多轮）

**问题**：
1. `git clone googleapis` 仓库过大，在云服务器上耗时 5 分钟以上，SSH 超时断开
2. 简化后 `google/protobuf/descriptor.proto: File not found` — `protoc` 的 `-I /usr/include` 不生效
3. `match_incoming_request_uri` 字段在当前 Envoy 版本不存在

**解决**：
1. 去除 `git clone`，将所需的 `google/protobuf/descriptor.proto`、`google/api/annotations.proto`、`google/api/http.proto` 内置到 `proto/` 目录
2. protoc 命令只用 `-I proto`，不再依赖系统路径
3. 移除不支持的 `match_incoming_request_uri` 字段
4. 将 `envoy.yaml` 改为 volume 挂载（`- ./envoy/envoy.yaml:/etc/envoy/envoy.yaml:ro`），避免每次改配置都要重建镜像

### 9.2 gRPC-JSON 转码未跑通

**问题**：proto descriptor 构建成功后，Envoy 仍报 `transcoding_filter: Unable to parse proto descriptor`。反复排查未定位根因。

**临时方案**：简化 Envoy 为 HTTP 反向代理模式，去掉 gRPC-JSON transcoder filter，直接 proxy_pass 到 Gateway。当前功能等价于 nginx upstream，但增加了 Envoy 内置的 circuit_breaker 和 STRICT_DNS round_robin。

### 9.3 nginx upstream 切换导致 502

**问题**：nginx upstream 从 `gateway_pool`（指向 gateway-1:8081, gateway-2:8081）改为 `envoy_pool`（指向 envoy:8080）后，如果 Envoy 未启动则所有请求 502。

**解决**：切换前确认 Envoy 已 running，`depends_on` 配置确保启动顺序。不使用时回退为 `gateway_pool`。

---

## 十、docker-compose 配置管理

### 10.1 批量 replace_all 导致 Dockerfile 路径混乱

**问题**：多次在 docker-compose.yml 上执行 `replace_all` 后，auth/sheet/file/gateway 等所有服务的 `dockerfile` 字段被错误覆盖为 `services/sheet-service/Dockerfile` 或 `services/auth-service/Dockerfile`，导致构建失败。

**原因**：docker-compose.yml 中多个服务共享相同的 `build: .` 或 `dockerfile: Dockerfile` 字符串，`replace_all` 无法区分上下文。

**教训**：对 docker-compose.yml 的修改应使用 `replace_all: false` 并带足够的上下文来精确定位目标服务块。批量替换仅在确认所有匹配都需要同样修改时才使用。

### 10.2 Consul 镜像名错误

**问题**：`consul:1.18` 拉取失败 403。

**原因**：官方 Consul 镜像是 `hashicorp/consul:1.18`，不是 `consul:1.18`（后者在 Docker Hub 上不存在）。

**解决**：修正为 `hashicorp/consul:1.18`。

---

## 十一、doc-parser 文档解析服务

### 11.1 Pillow 版本不兼容

**问题**：`Pillow>=10.0` 在 `python:3.11-slim`（基于 Debian Trixie）上 pip 找不到匹配的 wheel。

**解决**：移除 Pillow 和 pytesseract 依赖，doc-parser 的核心功能（PDF/DOCX/XLSX 文本提取）不依赖这两个包。OCR 功能标记为可选。

### 11.2 Tesseract 安装耗时过长

**问题**：Dockerfile 中 `apt-get install tesseract-ocr tesseract-ocr-chi-sim poppler-utils` 下载 200MB+，每次构建需 3-5 分钟。

**解决**：从 Dockerfile 中移除，doc-parser 只依赖纯 Python 库（pdfplumber、python-docx、openpyxl），构建时间降至 30 秒。

---

## 十二、服务器资源

### 12.1 C++ 编译吃满内存导致 SSH 断开

**问题**：`docker compose build --no-cache gateway-1` 在不停止其他容器的情况下运行，3.6GB 内存被 20+ 个容器 + C++ 编译器（GCC 每进程可达 500MB）吃满，SSH 连接超时无法恢复，只能云控制台硬重启。

**解决**：编译前先 `docker compose stop` 停掉所有容器释放内存，编译完成后再 `docker compose up -d` 启动。或使用 `nohup sh -c '...' &` 在服务器本地后台执行避免 SSH 超时。

### 12.2 新服务器无 sudo 权限

**问题**：目标迁移服务器（10.23.15.60, carl@）carl 用户不在 sudoers，无法安装 Docker。`su - root` 密码也不正确。

**解决**：需管理员执行 `usermod -aG sudo carl` 和 Docker 安装。在无 sudo 的环境中，可考虑 rootless Docker 方案，但需要 `uidmap` 包（也需要 root 安装）。

### 12.3 多次构建产生大量 <none> 镜像

**问题**：反复 `--no-cache` 构建 gateway 产生多版 188MB 的悬空镜像，占满磁盘空间。

**预防**：构建后 `docker image prune -f` 清理无用镜像层。
