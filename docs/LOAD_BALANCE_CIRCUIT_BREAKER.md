# 负载均衡 & 熔断改造方案

## 现状诊断

| 机制 | 位置 | 状态 | 问题 |
|------|------|------|------|
| Envoy ROUND_ROBIN | `envoy/envoy.yaml` | 活跃 | 单端点，ROUND_ROBIN 无意义 |
| Envoy circuit_breaker | `envoy/envoy.yaml` | 活跃 | 阈值 1024，从未触发 |
| C++ CircuitBreaker | `server/include/circuit_breaker.h` | **死代码**（687 行） | 从未 include/实例化 |
| Nginx upstream | `deploy/nginx/nginx.conf` | 活跃 | least_conn，但后端只有 1 个 envoy |
| Gateway gRPC | `cmd/gateway-grpc/main.go` | 活跃 | passthrough，单 IP 固定连接 |
| C++ gRPC 调用 | `main_sheet.cpp` | 活跃 | 单 channel，2s deadline |

**核心问题：auth×2、sheet×2、file×2 各两个实例，但第二个实例从未收到流量。**

根因：gRPC 默认 `passthrough` resolver，`rpc-auth:50051` 被当作字面地址。Docker DNS 返回两个 IP，但 gRPC 只连第一个，第二个无流量。

---

## 分层方案

### Tier 1 — gRPC 客户端负载均衡（改动最小，收益最大）

**改什么：** `cmd/gateway-grpc/main.go` 加 `dns:///` 前缀 + `round_robin` 配置

```
改前：
authConn, _ := grpc.NewClient("rpc-auth:50051", creds, kp)

改后：
authConn, _ := grpc.NewClient("dns:///rpc-auth:50051", creds, kp,
    grpc.WithDefaultServiceConfig(`{"loadBalancingConfig":[{"round_robin":{}}]}`))
```

**效果：**
- `dns:///` 激活 DNS resolver → Docker DNS 返回 auth-1 + auth-2 IP
- gRPC 建两条子连接 → round_robin 分发
- auth-2、sheet-2、file-2 立即开始接收流量
- 一个实例挂掉 → keepalive 13s 检测到 → 自动从轮训中移除
- **不改 Docker、不改 C++、不改 Envoy**

---

### Tier 2 — gRPC 重试（可用性提升）

**改什么：** `cmd/gateway-grpc/main.go` 加 `callWithRetry` 泛型重试函数

```
func callWithRetry[T any](ctx context.Context, maxAttempts int, label string, 
    fn func(context.Context) (T, error)) (T, error) {
    for attempt := 0; attempt < maxAttempts; attempt++ {
        if attempt > 0 {
            time.Sleep(time.Duration(50 << (attempt-1)) * time.Millisecond)
        }
        result, err := fn(ctx)
        if err == nil { return result, nil }
        if st, ok := status.FromError(err); ok && st.Code() == codes.Unavailable { continue }
        return result, err
    }
    return *new(T), lastErr
}
```

**只加重试的端点：**
- ✅ `GET /api/sheets/{id}`、`GET /api/files/{id}` — 读操作
- ✅ `GET /api/sheets`、`GET /api/files` — 列表读
- ✅ `DELETE /api/sheets/{id}`、`DELETE /api/files/{id}` — 删除幂等
- ❌ 不重试非幂等写入（PUT/POST）

---

### Tier 3 — 熔断器（防雪崩）

**改什么：** `cmd/gateway-grpc/main.go` 引入 `sony/gobreaker`，每个 C++ 服务一个熔断器

**状态机：**
```
Closed（正常）→ 连续失败 ≥5 且失败率 ≥50% → Open
Open → 30s 后 → Half-Open（最多 3 次探针）
Half-Open → 成功则 Closed / 失败继续 Open
```

**效果：**
- sheet 服务挂了 → 10s 内熔断 → 后续请求立即返回 503，不等 2s 超时
- sheet 恢复 → Half-Open 探针通过 → 自动恢复

**组合逻辑：** 熔断器（外层）+ 重试（内层）。先判熔断，再重试。熔断 Open 时不重试。

---

### Tier 4 — Consul 健康感知路由

**改什么：** `docker-compose.yml` grpc-gateway 服务加 `dns: consul`，地址改为 Consul DNS

```yaml
dns: consul
environment:
  AUTH_ADDR: "auth-service.service.consul:50051"
  SHEET_ADDR: "sheet-service.service.consul:50051"
  FILE_ADDR: "file-service.service.consul:50051"
```

**效果：**
- Consul TCP 健康检查不通过的实例不返回在 DNS 结果里
- C++ 服务宕机 → 10-20s 后从 DNS 移除

---

### Tier 5 — Envoy 阈值调整

**改什么：** `envoy/envoy.yaml` 降低 circuit_breaker 阈值 + 加 outlier_detection

```yaml
max_connections: 100     # 原 1024
max_pending_requests: 50
max_requests: 200
outlier_detection:
  interval: 10s
  base_ejection_time: 30s
  consecutive_gateway_failure: 5
```

---

### Tier 6 — 清理死代码（可选）

删除 `server/include/circuit_breaker.h`（687 行，从未被 include）。

---

## 依赖关系

```
Tier 1 → Tier 2 → Tier 3
Tier 1 → Tier 4
Tier 5 独立
Tier 6 独立
```

每层可独立部署，互不阻塞。

---

## 涉及文件

| 文件 | Tier | 改动 |
|------|------|------|
| `cmd/gateway-grpc/main.go` | 1, 2, 3 | DNS resolver + round_robin + retry + gobreaker |
| `cmd/gateway-grpc/go.mod` | 3 | 加 `sony/gobreaker` |
| `docker-compose.yml` | 4 | dns: consul + Consul DNS 地址 |
| `envoy/envoy.yaml` | 5 | 阈值降低 + outlier_detection |
| `server/include/circuit_breaker.h` | 6 | 删除 |

---

## 验证方式

| Tier | 验证方法 |
|------|---------|
| 1 | `docker compose up -d` → `docker logs http-rpc-auth-2-1` 发现有请求。kill auth-1，请求仍成功 |
| 2 | kill 一个实例，gateway 日志显示 `[retry] auth attempt 2/3` |
| 3 | 反复请求已挂实例，日志显示 `[cb] auth: CLOSED → OPEN`，快速返回 503 |
| 4 | kill 一个实例，15s 后 `dig auth-service.service.consul` 不再返回该 IP |
| 5 | Envoy admin `:9901/clusters` 显示新阈值 |
