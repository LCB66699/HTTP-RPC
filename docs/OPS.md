# 运维命令手册

## 一、Docker

### 构建与启动

```bash
# 一键构建所有镜像并启动（19 个容器）
docker compose up -d --build

# 查看所有容器状态
docker compose ps

# 查看所有容器（含健康检查状态）
docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
```

### 日志

```bash
# 查看 gateway 日志（双实例）
docker compose logs gateway-1 --tail=30
docker compose logs gateway-2 --tail=30

# 查看某个服务日志
docker compose logs sheet-1 --tail=20

# 查看 nginx 访问日志（能看到 502/404/200）
docker compose logs nginx --tail=20

# 持续跟踪日志
docker compose logs -f gateway-1 gateway-2
```

### 重启/停止

```bash
# 全部停止（保留数据卷）
docker compose down

# 全部停止（数据持久化在 ./data/ 目录，不会丢失）
docker compose down
# 如需完全重置所有数据，手动删除数据目录: rm -rf ./data/

# 重启单个服务
docker compose restart nginx

# 单独重建某个 gateway（不影响另一个，实现零停机更新）
docker compose up -d --build --no-deps gateway-1
# 等 gateway-1 healthy 后再更新 gateway-2
docker compose up -d --build --no-deps gateway-2
```

### 零停机滚动更新

```bash
# 更新应用服务（auth/sheet/file），每次更新一个实例，不中断整体服务
docker compose up -d --build --no-deps auth-1
docker compose up -d --build --no-deps auth-2

# 更新前确认实例健康状态
docker compose ps gateway-1 gateway-2 auth-1 auth-2

# 停服更新场景（需停服）：
# 1. docker-compose.yml 结构性变更（如新增/删除服务、修改 volume 映射）
# 2. MySQL schema 破坏性变更（DROP COLUMN / 改数据类型）
# 3. protobuf message 字段编号修改
```

### 清理重建

```bash
# 完全清理后重新构建（解决缓存导致的编译问题）
docker compose down && docker compose build --no-cache

# 查看完整构建日志（定位编译错误）
docker compose build --no-cache --progress=plain

# 清理所有 Docker 缓存（⚠️ 含数据卷，会清掉 ./data/ 以外的一切）
docker system prune -a -f --volumes

# 清理网络残留
docker network prune -f
```

### 进入容器排查

```bash
# 进入 gateway-1 容器
docker compose exec gateway-1 sh

# 查看进程
docker compose exec gateway-1 ps aux

# 查看 DNS 解析
docker compose exec gateway-1 getent hosts sheet-1

# 验证端口监听（TCP 8081）
docker compose exec gateway-1 bash -c 'echo > /dev/tcp/localhost/8081 && echo "8081 open" || echo "8081 closed"'
```

### 模拟故障

```bash
# 宕掉一个 sheet 实例（验证 gRPC 客户端侧负载均衡切换）
docker compose stop sheet-2

# 恢复
docker compose start sheet-2

# 宕掉整个 file 服务（验证 2PC 回滚 + 熔断器触发）
docker compose stop file-1 file-2
docker compose start file-1 file-2

# 宕掉一个 gateway（验证 nginx least_conn 故障转移）
docker compose stop gateway-1
# nginx 自动将所有流量切至 gateway-2，无中断
docker compose start gateway-1

# 查看熔断器状态（Redis 共享）
docker exec http-rpc-redis-master-1 redis-cli -a rpc-redis-020421 KEYS "cb:*"
docker exec http-rpc-redis-master-1 redis-cli -a rpc-redis-020421 GET "cb:sheet:state"
# 0=CLOSED, 1=OPEN, 2=HALF_OPEN
```

---

## 二、DNS 别名（负载均衡验证）

```bash
# 在 gateway 容器内验证 DNS 别名返回多个 IP
docker compose exec gateway-1 getent hosts rpc-sheet
# 预期输出 3 行（sheet-1/2/3 的 IP）

docker compose exec gateway-1 getent hosts rpc-auth
# 预期输出 2 行（auth-1/2 的 IP）

docker compose exec gateway-1 getent hosts rpc-file
# 预期输出 2 行（file-1/2 的 IP）

# 验证 nginx 层负载均衡：两个 gateway 均在健康状态
docker compose ps gateway-1 gateway-2
```

---

## 三、MySQL

**注意：MySQL 运行在 Docker 容器里，不在宿主机上。** 宿主机 `mysql -u root -p020421` 连的是本机安装的 MySQL（与项目无关），会报 `Unknown database 'rpc_%'`。所有数据操作必须进容器执行。

```bash
# 容器名（当前 compose 项目名 http-rpc）
docker exec -it http-rpc-mysql-master-1 mysql -u root -p020421

# 一条命令查询（免交互）
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SHOW DATABASES LIKE 'rpc_%';"

# 查看所有数据库
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SHOW DATABASES LIKE 'rpc_%';"

# ---- 用户数据 (rpc_auth) ----

# 查看所有注册用户
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT id, username, token_version, created_at FROM rpc_auth.users;"

# 查看某用户的具体信息
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT id, username, token_version, password_hash, created_at FROM rpc_auth.users WHERE username='lcb';"

# 使某用户 token 失效（递增 token_version）
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "UPDATE rpc_auth.users SET token_version = token_version + 1 WHERE username='lcb';"

# 查看 token_version 变更后的值
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT username, token_version FROM rpc_auth.users WHERE username='lcb';"

# ---- 表格数据 (rpc_spreadsheet) ----

# 查看所有表格
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT id, username, name, row_count, col_count, updated_at FROM rpc_spreadsheet.spreadsheets;"

# 查看某表格的完整数据
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT id, name, headers_json, data_json FROM rpc_spreadsheet.spreadsheets WHERE id=1\G"

# 统计每个用户的表格数量
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT username, COUNT(*) AS cnt FROM rpc_spreadsheet.spreadsheets GROUP BY username;"

# 删除某张表格
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "DELETE FROM rpc_spreadsheet.spreadsheets WHERE id=1;"

# ---- 文件数据 (rpc_file) ----

# 查看文件列表
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT id, original_name, size, mime_type, created_at FROM rpc_file.files;"

# 查看某文件的内容大小（LONGBLOB 体积）
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT id, original_name, size, LENGTH(file_content) AS content_bytes FROM rpc_file.files WHERE id=1;"

# ---- 事务日志 (rpc_tx_log) ----

# 查看所有事务
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT xid, status, created_at FROM rpc_tx_log.tx_log;"

# ---- 健康状态 ----

# 查看所有节点心跳
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT node_id, service, status, last_heartbeat FROM rpc_spreadsheet.health_status;"

# ---- 回滚日志 ----

# 查看未清理的 undo_log（2PC 残留）
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT * FROM rpc_spreadsheet.undo_log;"
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT * FROM rpc_file.undo_log;"

# ---- 性能分析 ----

# EXPLAIN 看是否走索引（关注 key 列：NULL=全表扫描）
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "EXPLAIN SELECT * FROM rpc_spreadsheet.spreadsheets WHERE username='test';"

# 查看所有索引
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SHOW INDEX FROM rpc_spreadsheet.spreadsheets;"

# 查看建表语句（含索引定义）
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SHOW CREATE TABLE rpc_spreadsheet.spreadsheets\G"

# 查看表大小（MB）
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "
  SELECT table_schema, table_name,
    ROUND(data_length/1024/1024,2) AS data_mb,
    ROUND(index_length/1024/1024,2) AS index_mb
  FROM information_schema.tables
  WHERE table_schema LIKE 'rpc_%';
"

# Profiling 查看查询实际耗时
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SET profiling=1; SELECT * FROM rpc_spreadsheet.spreadsheets WHERE username='test'; SHOW PROFILES;"
```

### 主从复制状态

```bash
# 查看 master 状态（File/Position/GTID）
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SHOW MASTER STATUS\G" -h mysql-master

# 从容器内查 slave-1 复制状态
docker exec http-rpc-docker-mysql-slave-1-1 mysql -u root -p020421 -e "SHOW SLAVE STATUS\G" 2>/dev/null | grep -E "Slave_IO_Running|Slave_SQL_Running|Seconds_Behind_Master"

# 从容器内查 slave-2 复制状态
docker exec http-rpc-docker-mysql-slave-2-1 mysql -u root -p020421 -e "SHOW SLAVE STATUS\G" 2>/dev/null | grep -E "Slave_IO_Running|Slave_SQL_Running|Seconds_Behind_Master"

# 实时监控复制延迟
watch -n 1 "docker exec http-rpc-docker-mysql-slave-1-1 mysql -u root -p020421 -e 'SHOW SLAVE STATUS\G' 2>/dev/null | grep Seconds_Behind_Master"

# 如果 token_version 列缺失，手动添加
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "ALTER TABLE rpc_auth.users ADD COLUMN token_version INT NOT NULL DEFAULT 0;"

# ---- 乐观锁版本号（spreadsheets.version） ----
# 每次 UpdateSpreadsheet 版本号自动 +1，跨副本并发写冲突时自动重试（最多 3 次）

# 查看某表格的当前版本号
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "SELECT id, name, version FROM rpc_spreadsheet.spreadsheets WHERE id=1;"

# 如果 version 列缺失，手动添加（服务端 Initialize 也会自动 ALTER TABLE）
docker exec http-rpc-mysql-master-1 mysql -u root -p020421 -e "ALTER TABLE rpc_spreadsheet.spreadsheets ADD COLUMN version INT NOT NULL DEFAULT 1;"

# 观察乐观锁冲突（正常情况无输出，高并发写同一 sheet 时出现 retry 日志）
docker compose logs sheet-1 2>&1 | grep 'version conflict'
```

---

## 四、Redis

```bash
# 连接 Redis 主库（写操作入口）
docker exec -it http-rpc-docker-redis-master-1 redis-cli -a rpc-redis-020421

# 连接 Redis 从库（读操作入口）
docker exec -it http-rpc-docker-redis-slave-1-1 redis-cli -a rpc-redis-020421

# 进入交互后，以下命令通用（以 master 为例）：
# ---- 缓存查询 ----

# 查看所有表格缓存
KEYS "sheet:*"

# 查看所有文件缓存
KEYS "file:*"

# 查看某用户的列表版本号
GET "user:test:sheets:version"

# 查看所有 key
KEYS "*"

# ---- 缓存操作 ----

# 查看某个缓存的 TTL（剩余秒数）
TTL "sheet:1"

# 手动删除某表格缓存（模拟缓存失效，下次读取走 MySQL）
DEL "sheet:1"

# 手动递增列表版本号（使所有列表缓存失效）
INCR "user:test:sheets:version"

# ---- 调用历史 ----

# 最近 10 条调用记录
LRANGE call_history 0 9

# 总调用次数
LLEN call_history

# 查看单条记录详情
LINDEX call_history 0

# ---- 故障排查 ----

# 1. 确认 Redis 密码是否生效（返回空 = 未配置密码）
docker exec http-rpc-docker-redis-master-1 redis-cli -a 'rpc-redis-020421' CONFIG GET requirepass

# 2. 确认 conf 是文件而非目录（目录会导致配置未加载）
docker exec http-rpc-docker-redis-master-1 ls -la /usr/local/etc/redis/redis.conf

# 3. 如果 conf 是目录，需要在宿主机修复
#    cd ~/HTTP-RPC-docker
#    rm -rf redis/master/redis.conf
#    tee redis/master/redis.conf << 'EOF'
#    port 6379
#    bind 0.0.0.0
#    requirepass rpc-redis-020421
#    masterauth rpc-redis-020421
#    EOF
#    docker rm -f <redis容器> && docker compose up -d

# 4. 查看缓存命中日志（确认 Redis 缓存生效）
docker compose logs sheet-1 2>&1 | grep '\[Sheet:cache\]'
# HIT  = 缓存命中
# MISS = 未命中（冷启动正常，连续出现说明回填失败）
# SetJSON FAILED / NOT CONNECTED = 异常，需排查

# ---- Token 版本号（吊销校验） ----

# 查看某用户的 token 版本号
GET "token_ver:lcb"

# 手动删除 token 版本缓存（会在下次登录时重新加载）
DEL "token_ver:lcb"

# 查看所有设置了 token 版本号的用户
KEYS "token_ver:*"

# ---- 监控 ----

# 内存使用
INFO memory

# 连接数
INFO clients

# 命中率
INFO stats | grep keyspace

# ---- 主从哨兵状态 ----

# 查看主从复制信息（master 视角）
INFO replication

# 注意：SENTINEL 是 Redis 命令不是 shell 命令，必须在 redis-cli 里执行。
# 若在 shell 直接敲 `SENTINEL slaves redis-master` 会报 command not found。

# 进入 sentinel 交互模式（端口 26379）
docker exec -it http-rpc-docker-redis-sentinel-1-1 redis-cli -p 26379

# --- 以下在 redis-cli > 内执行 ---
# 查看从库列表
> SENTINEL slaves redis-master

# 查看当前 master 地址
> SENTINEL get-master-addr-by-name redis-master

# 查看哨兵集群状态
> SENTINEL masters

# 从容器外直接查询 sentinel
docker exec http-rpc-docker-redis-sentinel-1-1 redis-cli -p 26379 SENTINEL get-master-addr-by-name redis-master

# 查看 sentinel 日志
docker logs http-rpc-docker-redis-sentinel-1-1 --tail 20

# ---- 连接池监控 ----
# 每个服务副本维护 pool_size 个 master + pool_size 个 slave 连接（默认 4）
# 启动日志关键词: [Redis:pool] master conn 1/4 connected

# 查看服务端 Redis 连接池初始化情况
docker compose logs sheet-1 2>&1 | grep '\[Redis:pool\]'

# 查看当前 Redis 客户端连接数（验证连接池生效）
CLIENT LIST | grep -o 'addr=[^ ]*' | sort | uniq -c

# 总连接数
INFO clients | grep connected_clients
```

---

## 五、gRPC

### 客户端测试

```bash
# 使用 grpcurl 直接调试 gRPC 服务（需要安装 grpcurl）
# 列出服务
grpcurl -plaintext localhost:50051 list

# 调用方法
grpcurl -plaintext -d '{"username":"test"}' localhost:50051 rpc.SpreadsheetService/List

# 健康检查
grpcurl -plaintext localhost:50051 grpc.health.v1.Health/Check

# 从 Docker 内测试
docker exec http-rpc-docker-gateway-1 sh -c \
  "grpcurl -plaintext sheet-1:50051 list 2>/dev/null || echo 'grpcurl not installed'"
```

### 连接验证

```bash
# 测试 gRPC 端口是否可达
docker exec http-rpc-docker-gateway-1 bash -c "echo > /dev/tcp/sheet-1/50051 && echo OK || echo FAIL"

# 查看 gRPC 连接状态
docker exec http-rpc-docker-gateway-1 sh -c "netstat -an | grep 50051"
```

---

## 六、HTTPS / 证书

### 查看当前证书

```bash
# 查看自签名证书信息
openssl s_client -connect localhost:443 -showcerts </dev/null 2>/dev/null | openssl x509 -text -noout | head -20

# 查看证书到期时间
echo | openssl s_client -connect localhost:443 2>/dev/null | openssl x509 -dates -noout
```

### 重新生成证书

```bash
# 证书在 nginx 容器 /certs/ 目录
# 重新生成需要重建 nginx 镜像
docker compose up -d --build --no-deps nginx
```

---

## 七、API 测试（curl）

### 登录（Cookie 模式）

```bash
# 注册（响应会 Set-Cookie: rpc_token=...）
curl -s -X POST https://localhost/api/register -k \
  -c /tmp/cookie.jar \
  -H 'Content-Type: application/json' \
  -d '{"username":"test","password":"123456"}'

# 登录（-c 保存 Cookie，-b 携带 Cookie）
curl -s -X POST https://localhost/api/login -k \
  -c /tmp/cookie.jar -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' \
  -d '{"username":"test","password":"123456"}'

# 登出（清除 Cookie）
curl -s -X POST https://localhost/api/logout -k \
  -c /tmp/cookie.jar -b /tmp/cookie.jar
```

### 表格操作

```bash
# 创建表格（-b 携带 Cookie 鉴权）
curl -s -X POST https://localhost/api/sheets -k \
  -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' \
  -d '{"name":"测试","description":"","headers_json":"[\"A\",\"B\"]","data_json":"[[\"a1\",\"b1\"],[\"a2\",\"b2\"]]"}'

# 列表查询
curl -s https://localhost/api/sheets -k \
  -b /tmp/cookie.jar

# 获取单个表格（POST body 传 id）
curl -s -X POST https://localhost/api/sheets/get -k \
  -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' \
  -d '{"id":1}'

# 更新表格（PUT body 传全量）
curl -s -X PUT https://localhost/api/sheets -k \
  -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' \
  -d '{"id":1,"name":"改名","description":"","headers_json":"[\"A\"]","data_json":"[[\"x\"]]"}'

# 删除表格（POST body 传 id）
curl -s -X POST https://localhost/api/sheets/delete -k \
  -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' \
  -d '{"id":1}'
```

### 文件操作

```bash
# 列表查询
curl -s https://localhost/api/files -k \
  -b /tmp/cookie.jar

# 上传文件
curl -s -X POST https://localhost/api/files/upload -k \
  -b /tmp/cookie.jar \
  -F "file=@test.txt"

# 下载文件
curl -s "https://localhost/api/files/download?id=1" -k \
  -b /tmp/cookie.jar -o downloaded.txt

# 删除文件（POST body 传 id）
curl -s -X POST https://localhost/api/files/delete -k \
  -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' \
  -d '{"id":1}'
```

### 健康检查

```bash
# 无需认证 — 返回 gRPC Channel 实时状态 + 熔断器状态
curl -s https://localhost/api/health -k | python3 -m json.tool
```

响应格式：

```json
{
  "auth":  {"channel": "READY",  "breaker": "CLOSED"},
  "sheet": {"channel": "READY",  "breaker": "CLOSED"},
  "file":  {"channel": "READY",  "breaker": "CLOSED"},
  "gateway": "READY"
}
```

| 字段 | 可能值 | 说明 |
|------|--------|------|
| channel | READY / FAILED / CONNECTING / IDLE | gRPC 子通道实时连接状态 |
| breaker | CLOSED / OPEN / HALF_OPEN | 熔断器状态；OPEN 时该服务已熔断，请求直接拒绝 |

**熔断器触发时**：`channel: "FAILED"` + `breaker: "OPEN"`，所有到该服务的请求毫秒级返回 503 而不堆积。30s 后自动 HALF_OPEN 试探。

**熔断器状态跨 gateway 共享（Redis）**：

```bash
# 查看所有熔断器 Redis 状态
docker exec http-rpc-redis-master-1 redis-cli -a rpc-redis-020421 KEYS "cb:*"
docker exec http-rpc-redis-master-1 redis-cli -a rpc-redis-020421 MGET cb:auth:state cb:sheet:state cb:file:state
# 返回值：0=CLOSED, 1=OPEN, 2=HALF_OPEN

# 手动重置某个服务的熔断器（清除 Redis 状态）
docker exec http-rpc-redis-master-1 redis-cli -a rpc-redis-020421 DEL cb:sheet:state cb:sheet:fails cb:sheet:opened cb:sheet:probe
```

**人工触发熔断恢复**：重启两个 Gateway 容器会清除本地状态，Redis 中的状态会随 TTL 过期（最长 120s）或手动 DEL：

```bash
docker compose restart gateway-1 gateway-2
```

### 系统状态监控

```bash
# 系统状态（需认证）— 错误计数 + Channel 状态 + 熔断器状态
# 使用 Cookie jar 鉴权
curl -sk -X POST https://localhost/api/login -c /tmp/cookie.jar -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' -d '{"username":"lcb","password":"123456"}'
curl -s -k -b /tmp/cookie.jar https://localhost/api/system/status | python3 -m json.tool
```

响应格式：

```json
{
  "services": {
    "auth":  {"channel": "READY",  "breaker": "CLOSED"},
    "sheet": {"channel": "READY",  "breaker": "CLOSED"},
    "file":  {"channel": "READY",  "breaker": "CLOSED"}
  },
  "errors": {
    "auth": 0,
    "spreadsheet": 2,
    "file": 0
  },
  "log_level": "active"
}
```

| 字段 | 说明 |
|------|------|
| services | 各服务 gRPC Channel 连通性 + 熔断器状态 |
| errors | 各服务累计错误数（来自 Redis `errors:{service}:total`） |
| log_level | 日志开关状态（`active` 或 `off`） |

### 日志级别控制

```bash
# --log-level 参数控制日志输出（默认 info）
#   off   : 完全静默，压测时避免日志 I/O 影响 QPS
#   error : 只记录 ERROR/FATAL
#   warn  : 记录 WARN 及以上
#   info  : 记录 INFO 及以上（正常模式）
#   debug : 记录所有（开发调试）

# docker-compose.yml 中传入：
#   command: >
#     /app/rpc_server --service spreadsheet --log-level error ...
```

**压测时关日志**：设 `--log-level off`，所有 `fprintf(stderr)` 和 Redis 错误推送全部静默，不影响基准 QPS。

**查看错误日志**：

```bash
# 容器日志（stderr 输出）
docker compose logs sheet-1 2>&1 | grep -E 'ERROR|FATAL'

# Redis 中累计错误数
docker exec http-rpc-redis-master-1 redis-cli -a rpc-redis-020421 GET "errors:spreadsheet:total"

# 最近的错误详情
docker exec http-rpc-redis-master-1 redis-cli -a rpc-redis-020421 LRANGE call_history 0 20 | grep ERROR
```

---

## 八、压测命令

### 功能测试

```bash
# 运行功能测试脚本（18 个测试用例，覆盖所有 API）
bash test/functional_test.sh

# 带详细输出（显示每个测试的请求与响应）
bash test/functional_test.sh -v
```

### 性能测试

```bash
# 运行性能测试（7 个测试组：表格 CRUD、文件上传下载、并发登录等）
bash test/performance_test.sh

# 性能测试输出包含：QPS、P50/P90/P95/P99 延迟分布
# 运行结束后自动生成 JSON 报告
```

### 压力测试

```bash
# 运行压力测试（5 阶段递增 QPS）
bash test/stress_test.sh

# 压测分 5 个阶段，每阶段持续 60 秒：
# Phase 1:  10 QPS
# Phase 2:  50 QPS
# Phase 3: 100 QPS
# Phase 4: 200 QPS
# Phase 5: 500 QPS
# 每个阶段输出：QPS + P50/P90/P95/P99/P99.9 延迟

# 查看测试报告
cat test/reports/latest.json
```

---

## 九、宿主机环境

### Ubuntu 依赖安装

```bash
# 编译依赖
sudo apt install -y g++ protobuf-compiler-grpc libgrpc++-dev \
  libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev

# Docker
sudo apt install -y docker.io docker-compose-v2

# 用户加 docker 组（免 sudo）
sudo usermod -aG docker $USER
newgrp docker
```

### Docker 镜像加速

```bash
# 国内网络配镜像源
sudo tee /etc/docker/daemon.json << 'EOF'
{
  "registry-mirrors": [
    "https://docker.m.daocloud.io",
    "https://dockerhub.icu"
  ]
}
EOF
sudo systemctl restart docker
```

### JWT_SECRET 配置

```bash
# 生成随机密钥（64 字节 hex）
JWT_SECRET=$(openssl rand -hex 64)
echo "export JWT_SECRET=$JWT_SECRET" >> ~/.bashrc

# docker-compose.yml 会自动读取宿主机的 JWT_SECRET 环境变量
# 所有服务（gateway + auth）必须使用同一个 secret 才能校验 token
```

### 防火墙

```bash
# 腾讯云：安全组放行 80、443 端口
# 入口方向 TCP 0.0.0.0/0 → 端口 80、443
```
