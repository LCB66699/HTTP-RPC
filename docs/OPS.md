# 运维命令手册

## 一、Docker

### 构建与启动

```bash
# 一键构建所有镜像并启动（21 个容器）
docker compose up -d --build

# 查看所有容器状态
docker compose ps

# 查看所有容器（含健康检查状态）
docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
```

### 日志

```bash
# 查看 gateway 日志（双实例）
docker compose logs grpc-gateway --tail=30
docker compose logs grpc-gateway-2 --tail=30

# 查看某个服务日志
docker compose logs sheet-1 --tail=20

# 查看 nginx 访问日志
docker compose logs nginx --tail=20

# 持续跟踪日志
docker compose logs -f grpc-gateway grpc-gateway-2
```

### 重启/停止

```bash
# 全部停止（保留数据卷）
docker compose down

# 如需完全重置所有数据: rm -rf ./data/

# 重启单个服务
docker compose restart nginx

# 单实例灰度更新（另一个实例继续服务，零停机）
docker compose up -d --build --no-deps grpc-gateway
sleep 5
docker compose up -d --build --no-deps grpc-gateway-2
```

### 零停机滚动更新

```bash
# 依次重建应用服务，每次一个实例，gRPC round_robin 自动切流量
docker compose up -d --build --no-deps auth-1
docker compose up -d --build --no-deps auth-2
docker compose up -d --build --no-deps sheet-1
docker compose up -d --build --no-deps sheet-2
docker compose up -d --build --no-deps sheet-3
docker compose up -d --build --no-deps file-1
docker compose up -d --build --no-deps file-2

# 需停服场景：docker-compose.yml 结构性变更、MySQL schema 破坏性变更、proto 字段编号修改
```

### Docker 磁盘清理（谨慎操作）

**安全原则：永远不要在日常清理中使用 `-a` 参数。**

| 命令 | 清理范围 | 风险 |
|------|---------|------|
| `docker image prune -f` | 仅 `<none>:<none>` 悬空镜像 | 零风险 |
| `docker system prune -f` | 悬空镜像 + 停止的容器 + 未用网络 | 零风险 |
| `docker image prune -a -f` | **所有**未被容器使用的镜像 | ⚠️ 会删预构建镜像 |
| `docker system prune -a -f --volumes` | 所有未用镜像 + 卷 + 构建缓存 | 🔴 清空一切 |

**日常清理（推荐）：**

```bash
# 查看占用
docker system df                          # 总览
docker images --format '{{.Size}}\t{{.Repository}}' | sort -rh | head -10

# 清理悬空镜像和停止容器
docker system prune -f                    # 安全，只删无用层
docker image prune -f                     # 最安全，只删 <none>:<none>

# 构建前清缓存（不影响现有镜像）
docker builder prune -f
```

**不要做：**
```bash
docker system prune -a -f --volumes      # 🔴 会删掉所有预构建镜像！
# 上一行执行后需要全部重新编译（5+ 小时）
```

**磁盘快满时的正确处理：**

```bash
# 1. 先看谁占空间
docker system df -v

# 2. 清理构建缓存（安全）
docker builder prune -af

# 3. 删停止的容器（安全）
docker container prune -f

# 4. 清理悬空镜像（安全）
docker image prune -f

# 5. 确认释放了多少
df -h /
```

### 清理重建

```bash
docker compose down && docker compose build --no-cache
docker compose build --no-cache --progress=plain  # 完整构建日志
docker network prune -f
```

### 进入容器排查

```bash
docker compose exec grpc-gateway sh
docker compose exec grpc-gateway ps aux
docker compose exec grpc-gateway getent hosts sheet-1
docker compose exec grpc-gateway bash -c 'echo > /dev/tcp/localhost/8081 && echo OK || echo FAIL'
```

### 模拟故障

```bash
docker compose stop sheet-2          # 验证 gRPC round_robin 切换
docker compose start sheet-2
docker compose stop file-1 file-2    # 验证 2PC 回滚 + 熔断触发
docker compose start file-1 file-2
docker compose stop grpc-gateway        # 验证 nginx least_conn 自动切 grpc-gateway-2
docker compose start grpc-gateway
```

---

## 二、LVS + Keepalived（4 层负载均衡）

**架构**：LVS Master × 1 + LVS Backup × 1 → nginx × 3（DR 模式）

```bash
# 查看 LVS 状态
docker exec -it lvs-master ipvsadm -Ln

# 查看 VIP 持有者
docker exec lvs-master ip addr show eth0 | grep 192.168

# 查看 real server 健康状态
docker exec lvs-master ipvsadm -Ln --stats

# 查看 Keepalived 日志
docker logs lvs-master --tail 20

# 手动摘除 nginx real server
docker exec lvs-master ipvsadm -d -t 192.168.1.100:443 -r nginx-2

# 恢复
docker exec lvs-master ipvsadm -a -t 192.168.1.100:443 -r nginx-2 -w 1

# LVS 故障转移测试 — 停 Master, VIP 应漂移到 Backup
docker stop lvs-master
docker exec lvs-backup ip addr show eth0 | grep 192.168  # 应看到 VIP
docker start lvs-master

# nginx 故障转移 — 停一个 nginx, LVS 应摘除
docker stop nginx-2
docker exec lvs-master ipvsadm -Ln  # nginx-2 应消失
docker start nginx-2
```

## 三、DNS 别名

```bash
docker compose exec grpc-gateway getent hosts rpc-sheet   # → 3 行 IP (sheet-1/2/3)
docker compose exec grpc-gateway getent hosts rpc-auth    # → 2 行 IP (auth-1/2)
docker compose exec grpc-gateway getent hosts rpc-file    # → 2 行 IP (file-1/2)
docker compose ps grpc-gateway grpc-gateway-2                  # 验证双实例
```

---

## 三、MySQL

**密码**：`123456`（docker-compose 默认值，可通过 `MYSQL_ROOT_PASSWORD` 环境变量覆盖）。

**分片部署架构**：
- `mysql-auth` — Auth 专用（不分片）
- `mysql-spreadsheet-0/1` — Sheet 双分片（user_id % 2）
- `mysql-file-0/1` — File 双分片（user_id % 2）

```bash
# ---- Auth 库 ----
docker exec -it http-rpc-mysql-auth-1 mysql -u root -p123456

# 查看所有注册用户
docker exec http-rpc-mysql-auth-1 mysql -u root -p123456 -N \
  -e "SELECT id, username, token_version, created_at FROM rpc_auth.users;"

# 使某用户 token 失效
docker exec http-rpc-mysql-auth-1 mysql -u root -p123456 \
  -e "UPDATE rpc_auth.users SET token_version = token_version + 1 WHERE username='lcb';"

# 查看某用户
docker exec http-rpc-mysql-auth-1 mysql -u root -p123456 \
  -e "SELECT id, username, token_version, password_hash FROM rpc_auth.users WHERE username='lcb';"

# ---- Sheet 分片 ----
docker exec http-rpc-mysql-spreadsheet-0-1 mysql -u root -p123456 \
  -e "SELECT id, username, name, row_count, updated_at FROM rpc_spreadsheet_0.spreadsheets;"

docker exec http-rpc-mysql-spreadsheet-1-1 mysql -u root -p123456 \
  -e "SELECT id, username, name FROM rpc_spreadsheet_1.spreadsheets;"

# ---- File 分片 ----
docker exec http-rpc-mysql-file-0-1 mysql -u root -p123456 \
  -e "SELECT id, original_name, size, mime_type FROM rpc_file_0.files;"

docker exec http-rpc-mysql-file-1-1 mysql -u root -p123456 \
  -e "SELECT id, original_name, size FROM rpc_file_1.files;"

# ---- 统计 ----
docker exec http-rpc-mysql-spreadsheet-0-1 mysql -u root -p123456 \
  -e "SELECT COUNT(*) FROM rpc_spreadsheet_0.spreadsheets;"

docker exec http-rpc-mysql-spreadsheet-1-1 mysql -u root -p123456 \
  -e "SELECT COUNT(*) FROM rpc_spreadsheet_1.spreadsheets;"

# ---- 查看索引 ----
docker exec http-rpc-mysql-spreadsheet-0-1 mysql -u root -p123456 \
  -e "SHOW INDEX FROM rpc_spreadsheet_0.spreadsheets;"

# ---- EXPLAIN ----
docker exec http-rpc-mysql-spreadsheet-0-1 mysql -u root -p123456 \
  -e "EXPLAIN SELECT * FROM rpc_spreadsheet_0.spreadsheets WHERE user_id=7;"

# ---- 回滚日志 ----
docker exec http-rpc-mysql-spreadsheet-0-1 mysql -u root -p123456 \
  -e "SELECT * FROM rpc_spreadsheet_0.undo_log ORDER BY id DESC LIMIT 10;"

# ---- 乐观锁版本号 ----
docker exec http-rpc-mysql-spreadsheet-0-1 mysql -u root -p123456 \
  -e "SELECT id, name, version FROM rpc_spreadsheet_0.spreadsheets WHERE id=1;"

# 查看乐观锁冲突日志
docker compose logs sheet-1 2>&1 | grep 'version conflict'
```

---

## 四、Redis Cluster

**密码**：`rpc-redis-123456`（docker-compose 默认值）。

**拓扑**：6 节点 (3M+3S)，gossip 协议自动故障转移。

```bash
# 连接任一 Cluster 节点（-c 开启集群模式）
docker exec -it http-rpc-redis-cluster-1 redis-cli -c -p 7000 -a rpc-redis-123456 --no-auth-warning

# 查看集群状态
docker exec http-rpc-redis-cluster-1 redis-cli -c -p 7000 -a rpc-redis-123456 --no-auth-warning CLUSTER INFO | grep cluster_state

# 查看节点列表
docker exec http-rpc-redis-cluster-1 redis-cli -c -p 7000 -a rpc-redis-123456 --no-auth-warning CLUSTER NODES

# ---- 缓存查询 ----
KEYS "sheet:*"
KEYS "file:*"
GET "token_ver:lcb"
GET "user:7:sheets:version"

# 查看 TTL
TTL "{sheet:42}"

# 手动删除缓存
DEL "{sheet:42}"

# ---- 熔断器状态 ----
KEYS "cb:*"
MGET cb:auth:state cb:sheet:state cb:file:state
# CLOSED / OPEN / HALF_OPEN

# 手动重置熔断器
DEL cb:sheet:state cb:sheet:fails cb:sheet:opened cb:sheet:probe

# ---- 调用历史 ----
LLEN call_history:global
LRANGE call_history:global 0 9

# ---- Token 版本 ----
KEYS "token_ver:*"
GET "token_ver:lcb"

# ---- 监控 ----
INFO memory | grep used_memory_human
INFO clients | grep connected_clients
INFO stats | grep instantaneous_ops_per_sec

# ---- 故障转移验证 ----
docker stop http-rpc-redis-cluster-1
sleep 8
docker exec http-rpc-redis-cluster-2 redis-cli -c -p 7001 -a rpc-redis-123456 --no-auth-warning CLUSTER NODES | grep master
# 应看到 redis-cluster-4 提升为 new master
docker start http-rpc-redis-cluster-1
```

---

## 五、gRPC

```bash
# 列出服务（需 grpcurl）
grpcurl -plaintext localhost:50051 list

# 调用方法
grpcurl -plaintext -d '{"user_id":7,"page":0,"page_size":10}' \
  localhost:50051 rpc.SpreadsheetService/ListSpreadsheets

# 健康检查
grpcurl -plaintext localhost:50051 grpc.health.v1.Health/Check

# 端口连通性
docker exec http-rpc-grpc-gateway-1 bash -c "echo > /dev/tcp/sheet-1/50051 && echo OK"
```

---

## 六、HTTPS / 证书

```bash
# 查看自签名证书信息
openssl s_client -connect localhost:443 -showcerts </dev/null 2>/dev/null | openssl x509 -text -noout | head -20

# 查看到期时间
echo | openssl s_client -connect localhost:443 2>/dev/null | openssl x509 -dates -noout

# 重新生成证书
docker compose up -d --build --no-deps nginx
```

---

## 七、API 测试（curl）

### 登录

```bash
curl -sk -X POST https://localhost/api/register \
  -c /tmp/cookie.jar -H 'Content-Type: application/json' \
  -d '{"username":"test","password":"123456"}'

curl -sk -X POST https://localhost/api/login \
  -c /tmp/cookie.jar -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' \
  -d '{"username":"test","password":"123456"}'
```

### 表格操作

```bash
# 创建
curl -sk -X POST https://localhost/api/sheets -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' \
  -d '{"name":"测试","description":"","headers_json":"[\"A\"]","data_json":"[[\"x\"]]"}'

# 列表
curl -sk https://localhost/api/sheets -b /tmp/cookie.jar

# 获取（POST body 传 id）
curl -sk -X POST https://localhost/api/sheets/get -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' -d '{"id":1}'

# 更新
curl -sk -X PUT https://localhost/api/sheets -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' \
  -d '{"id":1,"name":"改名","description":"","headers_json":"[\"A\"]","data_json":"[[\"x\"]]"}'

# 删除
curl -sk -X POST https://localhost/api/sheets/delete -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' -d '{"id":1}'
```

### 文件操作

```bash
curl -sk -X POST https://localhost/api/files/upload -b /tmp/cookie.jar -F "file=@test.txt"
curl -sk "https://localhost/api/files/download?id=1" -b /tmp/cookie.jar -o dl.txt
curl -sk -X POST https://localhost/api/files/delete -b /tmp/cookie.jar \
  -H 'Content-Type: application/json' -d '{"id":1}'
```

### 健康检查

```bash
curl -sk https://localhost/api/health | python3 -m json.tool
```

```json
{
  "auth":  {"channel": "READY", "breaker": "CLOSED"},
  "sheet": {"channel": "READY", "breaker": "CLOSED"},
  "file":  {"channel": "READY", "breaker": "CLOSED"},
  "gateway": "READY"
}
```

### 系统状态（含 P99 / 错误计数）

```bash
curl -sk -b /tmp/cookie.jar https://localhost/api/system/status | python3 -m json.tool
```

---

## 八、宿主机环境

### Ubuntu 依赖安装

```bash
# 编译依赖
sudo apt install -y g++ make cmake git \
  protobuf-compiler-grpc libgrpc++-dev \
  libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev \
  libnghttp2-dev

# redis-plus-plus（从源码编译，v1.3.15）
git clone --depth 1 https://github.com/sewenew/redis-plus-plus.git /tmp/rpp
cd /tmp/rpp && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DREDIS_PLUS_PLUS_CXX_STANDARD=20 \
      -DREDIS_PLUS_PLUS_BUILD_TEST=OFF ..
make -j$(nproc) && sudo make install && sudo ldconfig

# nlohmann/json（单头文件）
mkdir -p server/include/nlohmann
wget -O server/include/nlohmann/json.hpp \
  https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp

# Docker
sudo apt install -y docker.io docker-compose-v2
sudo usermod -aG docker $USER && newgrp docker
```

### JWT_SECRET 配置

```bash
JWT_SECRET=$(openssl rand -hex 64)
echo "export JWT_SECRET=$JWT_SECRET" >> ~/.bashrc
```

### 防火墙

```bash
# 安全组放行 80/443 端口（腾讯云 / AWS）
```

---

## 九、测试

```bash
# 功能测试（24 项）
bash test/functional_test.sh

# 性能测试（7 阶段 + wrk2）
bash test/performance_test.sh

# 压力测试（7 层递进）
bash test/stress_test.sh
bash test/stress_test.sh https://192.168.1.100 2000 20   # 自定义参数

# 长时间稳定性测试（5 分钟恒定负载）
STRESS_LONG=1 bash test/stress_test.sh
```

---

## 十、Docker 磁盘清理与构建优化

### 为什么 Docker 吃磁盘这么快

```
每次 docker build（尤其 --no-cache）都会产生:
  1. 中间层镜像（apt install, make 等每步一个 layer）
  2. 旧的镜像 tag 被覆盖后原镜像变成 <none>:<none>
  3. 编译产物 (117MB build context + obj 文件)
  
反复调试 proto/CMake/Dockerfile → 10+ 次重建 → 很快占满 50GB
```

### 清理命令（从安全到危险）

```bash
# 1. 查看占用
docker system df                    # 总览
docker images --format '{{.Size}}\t{{.Repository}}' | sort -rh | head -10
docker system df -v                 # 详细到每个镜像/容器/卷

# 2. 清理悬空镜像 (<none>:<none>) — 安全
docker image prune -f               # 只删无 tag 的旧镜像
docker image prune -a -f            # 删所有未被容器使用的镜像

# 3. 清理构建缓存 — 安全
docker builder prune -f             # 清理 BuildKit 缓存
docker buildx prune -f

# 4. 清理停止的容器 + 网络 — 安全
docker container prune -f
docker network prune -f

# 5. 一键全清（含数据卷） — ⚠️ 危险
docker system prune -a -f --volumes  # 删所有未用镜像+容器+卷+网络

# 6. 彻底重置（项目重建）
docker compose down -v              # 停止+删容器+删卷
docker system prune -a -f --volumes
rm -rf ./data/                      # 删持久化数据（MySQL/Redis/MinIO/ES）
```

### 构建前必做

```bash
# 编译前先清理（释放空间 + 避免旧镜像残留）
docker system prune -a -f           # 清所有无用镜像
df -h /                             # 确认 >10GB 空闲

# 然后再构建
docker compose build <service>
```

### 限制编译内存

```makefile
# Makefile 中默认 make -j$(nproc) 会并行编译
# 3.6GB 内存下必须改为单线程，否则 OOM
make -j1        # 单线程编译，内存占用 ~800MB
make -j2        # 双线程，内存占用 ~1.5GB（需先停容器）
```

### 镜像大小对比

| 镜像 | 构建方式 | 大小 |
|------|----------|------|
| C++ 服务 (auth/sheet/file/search) | ubuntu:24.04 + gRPC + MySQL + Redis | ~176MB |
| Gateway | 同上 + httplib | ~189MB |
| Go Notify | alpine + 静态编译 | ~47MB |
| Envoy | envoyproxy 官方 + 自定义配置 | ~400MB |
| ES + IK | elasticsearch 官方 + 插件 | ~2GB |

### 构建技巧

```bash
# 避免服务器 OOM: 先停所有容器再编译
docker compose stop
docker compose build <service>

# 避免 SSH 超时: 服务器后台编译
nohup docker build -t img -f Dockerfile . > /tmp/build.log 2>&1 &
# 检查进度: tail -f /tmp/build.log

# 利用缓存加速（只重建变更的服务）
docker compose build grpc-gateway    # 只编 Gateway
docker compose build auth-1       # 只编 Auth
# 而不是 docker compose build     # 全编
```

## 十一、Kubernetes 部署（可选）

```bash
# 一键部署（需要 K8s 集群 + kubectl + kustomize）
kubectl apply -k deploy/k8s/

# 查看状态
kubectl get pods -n http-rpc
kubectl get svc -n http-rpc
kubectl get ingress -n http-rpc

# 扩容
kubectl scale deployment gateway -n http-rpc --replicas=5

# 滚动更新
kubectl set image deployment/gateway gateway=http-rpc/gateway:v2.0 -n http-rpc

# 查看 HPA
kubectl get hpa gateway -n http-rpc

# 日志
kubectl logs -f deployment/gateway -n http-rpc --tail=100
```
