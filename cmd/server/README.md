# Gin Gateway

Gin 框架的 HTTP Gateway，提供 REST API → gRPC 转译、JWT 认证、熔断器、Prometheus 指标、WebSocket 实时推送。

## 启动

```bash
cd cmd/server
go mod tidy && go build -o gin-gateway .
JWT_SECRET=xxx ./gin-gateway
```

环境变量：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `PORT` | 8080 | 监听端口 |
| `JWT_SECRET` | - | JWT HMAC 密钥（必填） |
| `AUTH_ADDR` | rpc-auth | Auth/Sharing/Workspace 服务名 (Consul) |
| `SHEET_ADDR` | rpc-sheet | Sheet 服务名 |
| `FILE_ADDR` | rpc-file | File 服务名 |
| `SEARCH_ADDR` | rpc-search | Search 服务名 |
| `POINTS_ADDR` | rpc-points | Points 服务名 |
| `REDIS_ADDR` | redis-cluster-7000:7000 | Redis 地址 |
| `REDIS_PASSWORD` | rpc-redis-123456 | Redis 密码 |

## API 端点

### 公开（无需认证）

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | /api/v1/login | 密码登录 |
| POST | /api/v1/register | 注册 |
| POST | /api/v1/refresh | 刷新 Token |
| POST | /api/v1/auth/otp/send | 发送验证码 |
| POST | /api/v1/auth/phone/login | 手机验证码登录 |
| GET | /api/v1/health | 存活检查 + 后端熔断器状态 |
| GET | /api/v1/health/ready | 就绪检查（断路器开时 503） |
| GET | /api/v1/metrics | Prometheus 指标 |
| GET | /api/v1/s/:token | 分享链接跳转 |

### 认证用户

| 方法 | 路径 | 说明 |
|------|------|------|
| PUT | /api/v1/me/password | 修改密码 |
| GET | /api/v1/me | 当前用户信息 |
| GET | /api/v1/services | 可用服务列表 |
| GET | /api/v1/history | 调用历史 |
| POST | /api/v1/sheets | 创建表格 |
| GET | /api/v1/sheets | 表格列表 |
| GET | /api/v1/sheets/:id | 获取表格 |
| PUT | /api/v1/sheets/:id | 更新表格 |
| DELETE | /api/v1/sheets/:id | 删除表格 |
| POST | /api/v1/sheets/:id/share | 分享 |
| GET | /api/v1/sheets/:id/share | 分享列表 |
| DELETE | /api/v1/sheets/:id/share/:username | 撤销分享 |
| POST | /api/v1/sheets/:id/share-link | 创建分享链接 |
| GET | /api/v1/files | 文件列表 |
| POST | /api/v1/files/upload | 上传文件 |
| GET | /api/v1/files/:id | 获取文件 |
| DELETE | /api/v1/files/:id | 删除文件 |
| PUT | /api/v1/files/:id/move | 移动文件 |
| POST | /api/v1/files/folder | 创建文件夹 |
| POST | /api/v1/search | 全文搜索 |

### 工作空间

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | /api/v1/workspaces | 创建 |
| GET | /api/v1/workspaces | 列表 |
| GET | /api/v1/workspaces/:id | 详情 |
| PUT | /api/v1/workspaces/:id | 更新 |
| DELETE | /api/v1/workspaces/:id | 删除 |
| POST | /api/v1/workspaces/:id/members | 添加成员 |
| DELETE | /api/v1/workspaces/:id/members/:uid | 移除成员 |

### 积分

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/v1/points/balance | 查询积分余额 |
| GET | /api/v1/points/transactions | 积分流水 |
| GET | /api/v1/points/leaderboard | 积分排行 |

### WebSocket

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/v1/ws | WebSocket 实时推送（JWT Cookie 认证） |

## 目录结构

```
cmd/server/
├── main.go                     # 启动 + gRPC Consul 客户端 + 依赖注入
├── router/
│   ├── router.go               # 路由注册（5 个业务模块组装）
│   └── router_test.go           # 路由层测试
├── handler/
│   ├── handler.go              # Handlers struct + broadcastRoom + JWT helpers
│   ├── auth.go                 # 登录/注册/刷新/OTP/改密码（含积分奖励）
│   ├── sheet.go                # 表格 CRUD（含积分奖励 + 广播）
│   ├── file.go                 # 文件 CRUD（含积分奖励）
│   ├── sharing.go              # 分享权限管理
│   ├── workspace.go            # 工作空间 CRUD + 成员管理
│   ├── points.go               # 积分查询 + 事件发布
│   ├── misc.go                 # 健康检查 / 搜索 / 用户信息
│   ├── pb.go                   # gRPC 错误码 → HTTP 状态码映射
│   └── handler_test.go         # handler 层测试
├── middleware/
│   ├── auth.go                 # JWT 验证中间件
│   ├── circuit_breaker.go      # CBSlow 熔断器 + gRPC Interceptor
│   ├── metrics.go              # Prometheus 指标
│   └── middleware.go            # RequestID / Logger / CORS
├── ws/
│   ├── hub.go                  # Hub 房间管理 + Redis Pub/Sub 广播
│   ├── client.go               # WebSocket 读写协程
│   └── handler.go              # HTTP → WS 升级
├── discovery/
│   └── consul.go               # Consul gRPC Resolver（consul:/// scheme）
└── gen/rpc/                    # Proto 生成的 Go 代码
```

## 中间件链

```
CORS → RequestID → Prometheus Metrics → Logger → Recovery → [Auth(认证组)]
```

## 测试

```bash
go test ./... -v                # 全部
go test ./router/ -v -count=1   # 路由层
go test ./handler/ -v -count=1  # handler 层
```

### 路由层覆盖

| 测试 | 说明 |
|------|------|
| TestHealthEndpoint | 健康检查返回 200 |
| TestMetricsEndpoint | Prometheus 指标可访问 |
| TestAuthRequiredWithoutCookie | 受保护端点无 Cookie 返回 401 |
| TestPublicEndpointsNoAuth | 公开端点不拦截 |
| TestRequestIDInResponse | 响应头带 X-Request-ID |
| TestCORSMiddleware | OPTIONS 预检返回 204 |

### Handler 层覆盖

| 测试 | 说明 |
|------|------|
| TestLogin~ | 登录成功/失败/校验/gRPC 不可用 |
| TestRegister~ | 注册成功/用户名校验 |
| TestSheetCreate/Get/Delete~ | 表格 CRUD |
| TestShare/Revoke~ | 分享管理 |
| TestFileCreate/Delete/List~ | 文件 CRUD |
| TestSearch~ | 搜索成功/空查询 |
| TestGrpcXxxReturnsYyy | 6 个 gRPC 错误码 → HTTP 状态映射 |
| TestGetBalance/Transactions/Leaderboard | 积分查询端点 |
| TestPublishPointEvent~ | 积分事件发布边界情况 |

## gRPC 错误码映射

| gRPC Code | HTTP Status |
|-----------|-------------|
| NotFound | 404 |
| PermissionDenied | 403 |
| Unauthenticated | 401 |
| InvalidArgument | 400 |
| DeadlineExceeded | 504 |
