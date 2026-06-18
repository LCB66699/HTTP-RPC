# 单元测试

70 个测试用例，覆盖项目核心工具函数和基础组件。

## 运行

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
./build/cpp_test --gtest_filter='SHA256.*'
./build/cpp_test --gtest_filter='JWT.*'
./build/cpp_test --gtest_filter='Snowflake.*'
```

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
| `auth_interceptor` JWT payload 解析 | 高 | 需先抽取纯函数, 测试缺字段/过期/格式异常 |
| `spreadsheet/file` 缓存策略 | 高 | `service_interfaces.h` 已有接口, 需改成依赖注入 |
| `search_service` ES DSL 构建 | 中 | 提取 DSL builder, mock ES 客户端 |
| `database` 分片路由 | 中 | 需 MySQL mock 或嵌入式 MySQL |
| `redis_client` Lua 脚本 | 低 | 需 Redis, 集成测试更合适 |
| `rabbit_publisher/health/l1_invalidator` | 低 | 薄封装, 单元测试价值低 |
