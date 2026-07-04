# 单元测试

76 个测试用例，覆盖核心工具函数、基础组件和 gRPC 服务缓存逻辑。

## 运行

```bash
# 一键：构建 + 全部测试
bash test/run_unit_tests.sh

# 只跑某模块
bash test/run_unit_tests.sh --filter L1Cache.*
bash test/run_unit_tests.sh --filter "SheetMock.*:AuthService.*"

# 只构建不跑
bash test/run_unit_tests.sh --build-only

# 跳过构建（已 build 过）
bash test/run_unit_tests.sh --only-run
```

### 手动运行

```bash
# 构建
cmake -B build
cmake --build build --target cpp_test -j$(nproc)

# 全部
ctest --test-dir build --output-on-failure

# 按模块
./build/cpp_test --gtest_filter='L1Cache.*'
./build/cpp_test --gtest_filter='CallLoggerTest.*'
./build/cpp_test --gtest_filter='AuthService.*'
./build/cpp_test --gtest_filter='SheetMock.*'
./build/cpp_test --gtest_filter='SHA256.*'
./build/cpp_test --gtest_filter='JWT.*'
./build/cpp_test --gtest_filter='Snowflake.*'
```

## Mock 测试

Mock 类定义在 `test/unit/mocks.h`，基于 `service_interfaces.h` 的抽象接口：

| Mock | 基于接口 | 方法数 |
|---|---|---|
| `MockDB` | `IDatabase` | 18 |
| `MockRedis` | `IRedisClient` | 8 |
| `MockRabbit` | `IRabbitPublisher` | 1 |

### 用法示例

```cpp
#include "mocks.h"
#include "spreadsheet_service_impl.h"

TEST(SheetMock, Example) {
    MockDB db;
    MockRedis redis;
    SpreadsheetServiceImpl svc;

    // 注入 mock
    svc.SetDatabase(&db);    // IDatabase*
    svc.SetRedis(&redis);    // IRedisClient*

    // 设定期望
    EXPECT_CALL(redis, IsConnected()).WillRepeatedly(Return(true));
    EXPECT_CALL(db, GetSpreadsheetOwner(1, _, _))
        .WillOnce(DoAll(SetArgReferee<1>(42), Return(true)));

    // 调用真实 gRPC 方法
    grpc::ServerContext ctx;
    rpc::GetSpreadsheetRequest req;
    rpc::GetSpreadsheetResponse resp;
    req.set_id(1);
    req.set_user_id(42);
    svc.GetSpreadsheet(&ctx, &req, &resp);
}
```

### 已覆盖场景

| 用例 | 覆盖 |
|---|---|
| `SheetMock.GetSpreadsheetRedisCacheHit` | Redis 缓存命中 → DB 不调用 |
| `SheetMock.GetSpreadsheetCacheMissThenDbHit` | 缓存双 miss → DB 回源 → 写回 Redis |
| `SheetMock.GetSpreadsheetWrongOwner` | owner 不匹配 → `success=false` |

## 测试清单

### 基础设施

| 文件 | 用例 | 覆盖 |
|---|---|---|
| `test_l1_cache.cpp` | 14 | Get/Set/Delete/Clear, TTL 过期, LRU 淘汰, 并发读写, 命中率计数 |
| `test_call_logger.cpp` | 10 | 倒序查询, 分页(limit/offset), 服务+方法过滤, 队列溢出, 时间戳格式 |
| `test_sha256.cpp` | 7 | 哈希+验签, 错误密码, 空密码, 确定性, 旧格式兼容, 长密码 |
| `test_snowflake.cpp` | 4 | 唯一性, 单调递增, WorkerID 隔离, 范围校验 |

### 认证

| 文件 | 用例 | 覆盖 |
|---|---|---|
| `test_auth_service.cpp` | 11 | IsAdminUser(前缀/环境变量/空格/空值), b64enc(间接), 注册+登录, 密码校验, 重复注册, 输入校验 |
| `test_jwt.cpp` | 5 | 签发+验证, 错误密钥, 篡改检测, 空密钥, Base64URL 格式 |

### 服务 (Mock)

| 文件 | 用例 | 覆盖 |
|---|---|---|
| `test_sheet_mock.cpp` | 3 | GetSpreadsheet: 缓存命中/DB回源/权限拒绝 |

### 辅助函数

| 文件 | 用例 | 覆盖 |
|---|---|---|
| `test_data_helpers.cpp` | 7 | countRows(空/单行/多行), countCols(空/简单/引号内逗号/转义引号) |
| `test_sheet_helpers.cpp` | 5 | VersionKey, ListCacheKey(分页/不分页), CacheKey, LockKey |
| `test_file_helpers.cpp` | 5 | 同上, 文件服务缓存键 |
| `test_otel_helpers.cpp` | 5 | HexToBytes(合法/长度异常/非法字符), traceparent 格式+采样标记 |

## 待补充

| 模块 | 优先级 | 说明 |
|---|---|---|
| `spreadsheet_service` Create/Update/Delete | 高 | 接口注入已完成, 可直接 mock 扩展更多用例 |
| `file_service` 缓存策略 | 高 | FileServiceImpl 已注入 IDatabase/IRedisClient, 可复用 MockDB/MockRedis |
| `auth_interceptor` JWT payload 解析 | 中 | 需先抽取纯函数, 测试缺字段/过期/格式异常 |
| `search_service` ES DSL 构建 | 中 | 提取 DSL builder, mock ES 客户端 |
| `database` 分片路由 | 中 | 需 MySQL mock 或嵌入式 MySQL |
| `redis_client` Lua 脚本 | 低 | 需 Redis, 集成测试更合适 |
| `rabbit_publisher/health/l1_invalidator` | 低 | 薄封装, 单元测试价值低 |
