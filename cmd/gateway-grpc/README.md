# Gateway gRPC

HTTP → gRPC 网关，将 RESTful API 请求转换为 gRPC 调用，转发到 C++ 微服务（auth / sheet / file / search）。

## 目录结构

```
gateway-grpc/
├── main.go                  # 入口：路由注册、中间件、gRPC 客户端初始化
├── metrics.go               # Prometheus 指标和拦截器
├── test.sh                  # 测试脚本
├── Dockerfile               # 容器构建
├── .air.toml                # hot reload 配置
├── gen/rpc/                 # protoc 生成的 Go 代码（pb + gRPC stub）
├── internal/gateway/        # 共享库（辅助函数、校验、客户端接口）
│   ├── response.go          # WriteJSON / WriteGRPCResponse 等响应工具
│   ├── auth.go              # ExtractUID / GetUserFromCookie
│   ├── validate.go          # 输入校验函数
│   └── interfaces.go        # gRPC 客户端接口 + 可注入实例
└── test/                    # 单元测试（外部测试包）
    ├── jwt_test.go          # JWT 创建/验证/过期
    └── handler_test.go      # Handler 模式测试（mock gRPC 客户端）
```

## 运行测试

```bash
cd cmd/gateway-grpc
bash test.sh
```

或直接：

```bash
go test -v -count=1 ./...
```

## 测试覆盖

| 文件 | 测试内容 |
|------|---------|
| `test/jwt_test.go` | JWT 签名/验证、密钥不匹配、过期 token、无 padding base64 |
| `test/handler_test.go` | ChangePassword、Sheet 列表分页、CreateFolder、OTP、Photo、ShareLink |

测试使用 `package gateway_test`（外部测试包），通过 mock `internal/gateway` 包中导出的 gRPC 客户端接口（`gw.AuthClient` / `gw.SheetClient` / `gw.FileClient` / `gw.SharedClient`）来验证 handler 模式，不依赖真实后端。
