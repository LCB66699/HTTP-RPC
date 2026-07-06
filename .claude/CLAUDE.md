# HTTP-RPC 项目 CLAUDE.md

## 开发工作流

### 本地容器调试

- 用户通过 dev 容器 (`/workspaces/HTTP-RPC`) 进行编译和测试
- Go 编译 (`go build`, `go test`) / proto 再生 (`protoc`) 由用户在容器内执行
- Docker Compose 全栈部署由用户在本地控制

### 任务执行边界

- **我来写代码**：所有文件编辑、新建文件
- **用户来执行**：`go build`、`go test`、`go mod tidy`、`protoc`、`docker compose up/down` 等命令
- **用户决定提交**：执行验证通过后，用户判断是否 commit/push

### 分支策略

- `main` — 受保护，PR + CI 绿 + approve 才能合并
- `feat/*` — 功能分支，TDD 开发，逐个 commit 推送到远程
- 本地测试通过后用户决定 push

## 测试纪律

- **TDD 优先**：先写测试，用户容器内跑确认 FAIL，再写实现，最终测试 PASS
- 每轮改动后用户执行 `go test ./...` 验证
- Go 网关测试：`cd cmd/server && go test ./... -v`
- Go 服务测试：`cd services/<name> && go test ./... -v` 或 `bash services/tests/run_mock_tests.sh`
- C++ 单测：`bash test/run_unit_tests.sh`
- E2E 测试：`bash test/e2e/functional_test.sh`

## 架构约定

- **新 Go 服务**放 `services/` 下，和 notify-service 保持一致
- **Proto 定义**放 `proto/`，Go 生成代码放 `cmd/server/gen/rpc/`
- **路由注册**：每个业务文件自管 `RegisterXxxRoutes`，`router.go` 只组装
- **权限**：owner → 分享(P0) → 工作空间成员(P1) 三层 fallback
- **积分**：事件驱动，网关发 Redis Pub/Sub，points 服务独立消费
- **实时协作**：WebSocket Hub 订阅 Redis `ws:broadcast`，按 room 广播

## 项目结构

```
cmd/server/          Go 网关 (Gin + gRPC clients + WS)
services/            Go 微服务 (notify-service, points-server, ...)
server/              C++ 微服务 (auth, sheet, file, search)
proto/               Protobuf 定义
deploy/              部署配置 (consul, envoy, nginx, mysql, redis, k8s)
test/                测试 (unit, e2e, wrk)
web-ui/              前端 (SPA)
```

## 回答偏好

- 使用中文回复

