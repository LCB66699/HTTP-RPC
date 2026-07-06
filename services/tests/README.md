# 服务集成测试

此目录存放基于 mock 的服务级别集成测试。
测试依赖通过 mock/stub 注入，不依赖实际基础设施。

## 目录结构

```
tests/
└── README.md
```

## 运行

```bash
bash services/tests/run_mock_tests.sh
```

自动发现 `services/` 下所有含 `go.mod` 的目录并运行 `go test ./...`。

## 现有对应单测

| 服务 | 文件 | 覆盖内容 |
|------|------|----------|
| notify-service | `outbox_test.go` | CAS 状态机、分片解析、回滚、trace 传播 |
| notify-service | `redis_test.go` | RESP AUTH + PUBLISH 协议 |
| points-server | `points_test.go` | 积分获取、幂等去重、扣减、交易记录、创建 sheet 规则 |
