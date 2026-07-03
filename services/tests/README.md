# 服务集成测试

此目录存放基于 mock 的服务级别集成测试。
测试依赖通过 mock/stub 注入，不依赖实际基础设施。

## 目录结构

```
tests/
└── README.md
```

## 现有对应单测

| 包 | 文件 | 覆盖内容 |
|------|------|----------|
| notify-service | `outbox_test.go` | CAS 状态机、分片解析、回滚、trace 传播 |
| notify-service | `redis_test.go` | RESP AUTH + PUBLISH 协议 |
