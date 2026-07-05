# Gin Gateway

Gin 框架重构的 HTTP Gateway，替代 `cmd/gateway-grpc`（net/http + grpc-gateway）。

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
| `AUTH_ADDR` | rpc-auth:50051 | Auth gRPC 地址 |
| `SHEET_ADDR` | rpc-sheet:50051 | Sheet gRPC 地址 |
| `FILE_ADDR` | rpc-file:50051 | File gRPC 地址 |
| `SEARCH_ADDR` | rpc-search:50051 | Search gRPC 地址 |
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
| GET | /api/v1/health | 存活检查 |
| GET | /api/v1/health/ready | 就绪检查 |
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

## 目录结构

```
cmd/server/
├── main.go                     # 启动 + gRPC 连接 + 依赖注入
├── router/
│   ├── router.go               # 路由注册 + MetricsHandler
│   └── router_test.go           # 路由层测试（8 个）
├── handler/
│   ├── handler.go              # Handlers struct + 公共 helper
│   ├── auth.go                 # 认证端点（Login/Register/Refresh/OTP）
│   ├── sheet.go                # 表格 CRUD
│   ├── file.go                 # 文件 CRUD
│   ├── sharing.go              # 分享相关
│   ├── misc.go                 # 健康检查 / 搜索 / 用户信息
│   ├── pb.go                   # gRPC 错误码 → HTTP 状态码映射
│   └── handler_test.go         # handler 层测试（23 个）
├── middleware/
│   ├── auth.go                 # JWT 验证中间件
│   ├── circuit_breaker.go      # CBSlow 熔断器 + gRPC Interceptor
│   ├── metrics.go              # Prometheus 指标（变量 + GinMetrics + GrpcMetricsInterceptor）
│   └── middleware.go            # RequestID / Logger / CORS
└── gen/rpc/                    # Proto 生成的 Go 代码
```

## 中间件链

```
CORS → RequestID → Prometheus Metrics → Logger → Recovery → [Auth(认证组)]
```

## 测试

```bash
# 全部
go test ./... -v

# 路由层（8 个用例）
go test ./router/ -v -count=1

# handler 层（23 个用例）
go test ./handler/ -v -count=1
```

### 路由层覆盖

| 测试 | 说明 |
|------|------|
| TestHealthEndpoint | 健康检查返回 200 |
| TestMetricsEndpoint | Prometheus 指标可访问 |
| TestAuthRequiredWithoutCookie | 9 个受保护端点无 Cookie 返回 401 |
| TestPublicEndpointsNoAuth | 公开端点不拦截 |
| TestRequestIDInResponse | 响应头带 X-Request-ID |
| TestCORSMiddleware | OPTIONS 预检返回 204 |

### Handler 层覆盖

| 测试 | 说明 |
|------|------|
| TestLoginSuccess | 登录成功 200 + Set-Cookie |
| TestLoginWrongPassword | 密码错误 401 |
| TestLoginValidation | 空用户名/空密码 400 |
| TestLoginGrpcUnavailable | 后端不可用 503 |
| TestRegisterSuccess | 注册成功 200 |
| TestRegisterUsernameTooShort | 用户名校验 400 |
| TestChangePasswordUnauthorized | uid=0 返回 401 |
| TestChangePasswordSuccess | 修改成功 200 |
| TestSheetCreateSuccess | 创建表格 200 |
| TestSheetCreateValidation | 空名称 400 |
| TestSheetGetSuccess | 获取表格 200 |
| TestSheetGetNotFound | gRPC NotFound → 404 |
| TestSheetDeleteSuccess | 删除表格 200 |
| TestShareSheetSuccess | 分享成功 200 |
| TestRevokeShareSuccess | 撤销分享 200 |
| TestFileCreateFolderSuccess | 创建文件夹 200 |
| TestFileDeleteSuccess | 删除文件 200 |
| TestFileGetNotFound | gRPC NotFound → 404 |
| TestFileListSuccess | 文件列表 200 |
| TestSearchSuccess | 搜索成功 200 |
| TestSearchEmptyQuery | 空查询 400 |

### gRPC 错误码映射覆盖

| 测试 | gRPC Code | HTTP Status |
|------|-----------|-------------|
| TestGrpcNotFoundReturns404 | NotFound | 404 |
| TestGrpcPermissionDeniedReturns403 | PermissionDenied | 403 |
| TestGrpcUnauthenticatedReturns401 | Unauthenticated | 401 |
| TestGrpcInvalidArgumentReturns400 | InvalidArgument | 400 |
| TestGrpcDeadlineExceededReturns504 | DeadlineExceeded | 504 |

## 与旧代码对比

| | 旧 gateway-grpc | Gin 版本 |
|---|---|---|
| 框架 | net/http + grpc-gateway | Gin |
| 中间件 | 手动函数嵌套 | r.Use() 链式声明 |
| 路由 | 两套 mux + 分发器 | 路由组 r.Group() |
| JSON 解析 | json.NewDecoder 手写 | c.ShouldBindJSON() |
| JSON 响应 | response.go 150 行 | c.JSON(status, obj) |
| 错误处理 | WriteGRPCResponse 等 5 个函数 | grpcErr() 一个函数 |
| 校验 | validate.go 80 行 | 内联到 handler |
| 接口层 | interfaces.go 60 行 | 直接注入 proto Client |
| 总文件数 | 15 | 13 |
