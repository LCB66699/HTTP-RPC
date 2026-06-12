# 本地调试指南

## 原理

C++ 编译后是**一个二进制文件**，不是多个文件。三个服务是三个不同二进制：

```
Auth 容器 /app/rpc_server = make auth → main_auth.cpp + auth_service_impl.cpp + ...
Sheet 容器 /app/rpc_server = make sheet → main_sheet.cpp + spreadsheet_service_impl.cpp + ...
File 容器 /app/rpc_server = make file → main_file.cpp + file_service_impl.cpp + ...
```

**哪个文件的代码在哪个服务里，就附着哪个容器。** Sheet 容器里没有 `auth_service_impl.cpp`，打断点会显示 `<PENDING>`。

---

## Go Gateway — 代码热重载

```bash
bash dev.sh gateway   # docker compose build + up，30s
```

---

## C++ 服务 — GDB 断点调试（完整流程）

### 步骤 1：构建 debug 版

```bash
bash dev.sh debug sheet    # 换成 auth/file/search
```

把 `-O2` 替换为 `-g -O0`，保留调试符号，镜像内安装 GDB。`docker-compose.override.yml` 自动挂载源码 + 开 `SYS_PTRACE`。

### 步骤 2：停掉另一个实例

```bash
docker compose stop sheet-2   # 调试期间只跑一个实例，避免请求被分流
```

### 步骤 3：附着 GDB

```bash
docker exec -it $(docker ps -qf name=sheet-1) gdb -p 1 /app/rpc_server
```

`-p 1` = attach 到容器主进程。典型输出：

```
Attaching to process 1
Reading symbols from /app/rpc_server...
(gdb)
```

此时服务暂停，GDB 等待输入。

### 步骤 4：设断点，放行

```gdb
(gdb) delete                    # 清掉上次残留断点
(gdb) break spreadsheet_service_impl.cpp:450
Breakpoint 1 at 0x5bc9d14e6e52: file server/src/spreadsheet_service_impl.cpp, line 450.
(gdb) info breakpoints
(gdb) continue
```

### 步骤 5：另一个终端触发请求

```bash
# 登录拿 token
curl -sk -c /tmp/cookie https://localhost/api/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"tester_fn","password":"test1234"}'

# 获取已有表格 ID
SHEET_ID=$(curl -sk -b /tmp/cookie https://localhost/api/sheets \
  | grep -o '"id":"[0-9]*"' | head -1 | grep -o '[0-9]*')
echo "SHEET_ID=$SHEET_ID"

# 触发请求
curl -sk -X PUT "https://localhost/api/sheets/$SHEET_ID" \
  -b /tmp/cookie -H 'Content-Type: application/json' \
  -d '{"name":"updated_name"}'
```

### 步骤 6：GDB 里单步调试

回到 GDB 终端，服务在断点处暂停：

```gdb
Thread 3 "grpcpp_sync_ser" hit Breakpoint 1,
    SpreadsheetServiceImpl::UpdateSpreadsheet (...)
    at server/src/spreadsheet_service_impl.cpp:450
450     if (auth_) {
```

逐个检查变量：

```gdb
(gdb) print req->id()
$3 = 12345678901234567


(gdb) print req->user_id()
$4 = 57317426869645312

(gdb) print req->name()
$5 = "updated_name"

(gdb) next                        # 走完 auth_->Authenticate
(gdb) next                        # 走完 ValidateCaller
(gdb) print vu_user               # 鉴权返回的用户名
$6 = "tester_fn"

(gdb) next                        # 走到 GetSpreadsheetOwner
(gdb) print owner_uid             # 查到的 owner
$7 = 57317426869645312
(gdb) print version               # 乐观锁版本号
$8 = 3

(gdb) next                        # 走到 UpdateSpreadsheet 调用
(gdb) step                        # 进入函数内部
(gdb) next
(gdb) finish                      # 跳出当前函数
(gdb) print ok
$9 = false                        # ← 更新失败！根因定位
```

### 步骤 7：退出

```gdb
(gdb) quit
A debugging session is active. Quit anyway? y
```

退出后服务继续运行，不受影响。

### 步骤 8：恢复

```bash
docker compose build sheet-1 && docker compose up -d   # 恢复优化版镜像
docker compose up -d sheet-2                            # 恢复第二个实例
```

---

## 断点速查表

### `server/src/spreadsheet_service_impl.cpp`

| 接口 | 行号 | 附着容器 | 关键变量 |
|---|---|---|---|
| CreateSpreadsheet | 63 | sheet-1 | `req->name()`, `id`, `ok` |
| GetSpreadsheet | 140 | sheet-1 | `req->id()`, `owner_uid`, `cache_source` |
| ListSpreadsheets | 357 | sheet-1 | `req->user_id()`, `total` |
| UpdateSpreadsheet | 450 | sheet-1 | `req->id()`, `owner_uid`, `version`, `ok` |
| DeleteSpreadsheet | 530 | sheet-1 | `req->id()`, `owner_uid` |
| ValidateCaller (gRPC → Auth) | 66 | sheet-1 | `token`, `vu_resp.valid()` |

### `server/src/file_service_impl.cpp`

| 接口 | 行号 | 附着容器 | 关键变量 |
|---|---|---|---|
| CreateFile | 56 | file-1 | `req->original_name()`, `id`, `ok` |
| GetFile | 124 | file-1 | `req->id()`, `owner_uid`, `cache_source` |
| ListFiles | 307 | file-1 | `req->user_id()`, `total` |
| DeleteFile | 387 | file-1 | `req->id()`, `owner_uid` |
| ValidateCaller (gRPC → Auth) | 22 | file-1 | `token`, `vu_resp.valid()` |

### `server/src/auth_service_impl.cpp`

| 接口 | 行号 | 附着容器 | 关键变量 |
|---|---|---|---|
| Login | 71 | auth-1 | `req->username()`, `stored_hash` |
| Register | 150 | auth-1 | `req->username()`, `req->password().size()` |
| ValidateUser | 330 | auth-1 | `req->token()`, `token_uid`, `username` |
| RefreshToken | 281 | auth-1 | `req->refresh_token()`, `stored_rt` |

### `server/src/main_sheet.cpp`（初始化问题）

| 位置 | 行号 | 附着容器 | 关键变量 |
|---|---|---|---|
| MySQL 连接 | 42 | sheet-1 | `mysql_host`, `mysql_password`, `mysql_shards` |
| Redis 连接 | 46 | sheet-1 | `redis_cluster_seeds`, `redis_password` |
| Auth channel | 85 | sheet-1 | `auth_addr`, `env_auth` |
| 端口监听 | 93 | sheet-1 | `addr` |

### `server/src/main_file.cpp`（初始化问题）

| 位置 | 行号 | 附着容器 | 关键变量 |
|---|---|---|---|
| MySQL 连接 | 41 | file-1 | `mysql_host`, `mysql_password`, `mysql_shards` |
| Redis 连接 | 46 | file-1 | `redis_cluster_seeds`, `redis_password` |
| Auth channel | 82 | file-1 | `auth_addr`, `env_auth` |
| 端口监听 | 91 | file-1 | `addr` |

---

## GDB 常用命令

| 命令 | 含义 |
|---|---|
| `break <file>:<line>` | 设断点 |
| `info breakpoints` | 查看所有断点 |
| `delete` | 删掉全部断点 |
| `delete <n>` | 删除第 n 个断点 |
| `continue` / `c` | 继续执行到下一个断点 |
| `next` / `n` | 单步执行（不进入函数内部） |
| `step` / `s` | 单步执行（进入函数内部） |
| `finish` | 跳出当前函数 |
| `print <var>` | 打印变量值 |
| `print/d <var>` | 十进制打印（protobuf int64 有时显示不对） |
| `print req->name()` | 打印 protobuf 字段 |
| `backtrace` / `bt` | 查看调用栈 |
| `list` | 显示当前位置源码 |
| `Ctrl+C` | 暂停运行中的程序 |
| `quit` | 退出 GDB（服务继续运行） |

---

## 调试请求速查

```bash
# 登录
curl -sk -c /tmp/cookie https://localhost/api/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"tester_fn","password":"test1234"}'

# 列表 Query（触发 ListSpreadsheets）
curl -sk -b /tmp/cookie https://localhost/api/sheets

# 单表 Query（触发 GetSpreadsheet）
curl -sk -b /tmp/cookie "https://localhost/api/sheets/58063834543370240"

# 创建（触发 CreateSpreadsheet）
curl -sk -X POST https://localhost/api/sheets \
  -b /tmp/cookie -H 'Content-Type: application/json' \
  -d '{"name":"debug_test","description":"test"}'

# 更新（触发 UpdateSpreadsheet）
curl -sk -X PUT "https://localhost/api/sheets/58063834543370240" \
  -b /tmp/cookie -H 'Content-Type: application/json' \
  -d '{"name":"updated_name"}'

# 删除（触发 DeleteSpreadsheet）
curl -sk -X DELETE "https://localhost/api/sheets/58063834543370240" \
  -b /tmp/cookie

# 搜索（触发 Search）
curl -sk -X POST https://localhost/api/search \
  -b /tmp/cookie -H 'Content-Type: application/json' \
  -d '{"q":"test","scope":"sheets"}'

# 上传文件（触发 CreateFile）
echo "test content" > /tmp/test.txt
curl -sk -X POST https://localhost/api/files/upload \
  -b /tmp/cookie -F "file=@/tmp/test.txt"

# 检查状态码
curl -sk -o /dev/null -w "%{http_code}\n" https://localhost/api/health
```

---

## 常见问题

### `HTTP_CODE: 000`

服务没起。检查：

```bash
docker compose ps --format "{{.Name}} {{.Status}}"
docker compose up -d
```

### 断点 `<PENDING>`

目标文件的代码不在当前容器里，附着错了。确认容器名：

```bash
docker ps --format "{{.Names}}" | grep -E "sheet|auth|file"
```

### `No such file or directory`

源码没挂载。确认 `docker-compose.override.yml` 有 `volumes: ./server:/src/server:ro`，并重启容器：

```bash
docker compose up -d sheet-1 --force-recreate
```

### 断点触发了但请求被分流到另一个实例

```bash
docker compose stop sheet-2    # 调试期间只跑一个
```

### `SHEET_ID` 为空

列表返回被 grep 匹配失败，手动指定 ID：

```bash
curl -sk -b /tmp/cookie https://localhost/api/sheets   # 先看返回
# 手动复制一个 ID 使用
curl -sk -b /tmp/cookie "https://localhost/api/sheets/58063834543370240"
```

---

## 熔断/重试日志

不需要 GDB，直接看日志：

```
[retry] sheet.list attempt 2/3 after 50ms    # 第 2 次重试
[cb] sheet: CLOSED → OPEN                     # 熔断触发
```

```bash
docker compose logs sheet-1 --tail=50 -f
```

---

## 运维调试命令

```bash
# 看服务日志
docker compose logs sheet-1 --tail=50 -f

# 进入容器
docker exec -it http-rpc-sheet-1-1 bash

# 查看 MySQL 数据
docker exec -it http-rpc-mysql-auth-1 mysql -uroot -p123456 rpc_auth \
  -e "SELECT id, username FROM users"

# 查看 Redis 缓存
docker exec -it http-rpc-redis-cluster-1-1 redis-cli \
  -p 7000 -a rpc-redis-123456 KEYS "*"

# 查看容器状态
docker compose ps --format "{{.Name}} {{.Status}}"
```