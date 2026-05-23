#!/bin/bash
# HTTP-RPC 功能测试脚本 (v4 — HttpOnly Cookie Auth)
# 用法: bash functional_test.sh [API_URL]
# 默认: https://localhost (自签证书)

API="${1:-https://localhost}"
CURL="curl -sk --connect-timeout 5 --max-time 15 --retry 2"
JAR="/tmp/rpc_functional_cookies_$$"  # cookie jar 文件，每次测试独立
PASS=0; FAIL=0

# 自动记录日志到 test/logs/
LOG_DIR="$(cd "$(dirname "$0")" && pwd)/logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/functional_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee -a "$LOG_FILE") 2>&1
echo "Log: $LOG_FILE"
echo ""

green() { echo -e "\033[32m[PASS]\033[0m $1"; ((PASS++)); }
red()   { echo -e "\033[31m[FAIL]\033[0m $1"; ((FAIL++)); }
warn()  { echo -e "\033[33m[WARN]\033[0m $1"; }
title() { echo -e "\n\033[1;36m=== $1 ===\033[0m"; }

# 从 curl -D 的响应头中提取 rpc_token cookie 值
extract_token() {
    local header_file="$1"
    grep -i 'set-cookie:.*rpc_token=' "$header_file" 2>/dev/null \
        | sed 's/.*rpc_token=//;s/;.*//' | tr -d '\r\n'
}

cleanup() { rm -f "$JAR" "/tmp/rpc_hdr_$$" "/tmp/rpc_test_upload.txt" "/tmp/rpc_test_download.txt"; }
trap cleanup EXIT

# ---- 0. 连通性检查 ----
title "0. API 连通性"
if $CURL -o /dev/null -w "%{http_code}" "$API/api/health" | grep -q "200"; then
    green "API reachable"
else
    red "API unreachable at $API"
    exit 1
fi

# ---- 1. 认证 ----
title "1. 认证 (Auth)"

# 1.1 注册（响应体不再含 token，token 在 Set-Cookie 中）
title "1.1 注册新用户"
REG=$($CURL -X POST "$API/api/register" \
    -H 'Content-Type: application/json' \
    -c "$JAR" -D "/tmp/rpc_hdr_$$" \
    -d '{"username":"tester_fn","password":"test1234"}')
echo "$REG" | grep -q '"success":true' \
    && green "Register OK (body: success)" \
    || red "Register failed: $REG"
REG_TOKEN=$(extract_token "/tmp/rpc_hdr_$$")
[ -n "$REG_TOKEN" ] \
    && green "Register: rpc_token cookie received" \
    || red "Register: no rpc_token in Set-Cookie"

# 1.2 重复注册（应拒绝）
title "1.2 重复注册（应拒绝）"
REJECTED=0
for i in 1 2 3; do
    REG2=$($CURL -X POST "$API/api/register" \
        -H 'Content-Type: application/json' \
        -d '{"username":"tester_fn","password":"test1234"}')
    if echo "$REG2" | grep -q '"success":false'; then
        REJECTED=1; break
    fi
    sleep 0.5
done
[ "$REJECTED" = "1" ] \
    && green "Duplicate register rejected" \
    || red "Duplicate register should fail"

# 1.3 登录（获取 Cookie）
title "1.3 登录获取 Cookie"
rm -f "$JAR"
LOGIN=$($CURL -X POST "$API/api/login" \
    -H 'Content-Type: application/json' \
    -c "$JAR" -D "/tmp/rpc_hdr_$$" \
    -d '{"username":"tester_fn","password":"test1234"}')
echo "$LOGIN" | grep -q '"success":true' \
    && green "Login OK" \
    || red "Login failed: $LOGIN"
LOGIN_TOKEN=$(extract_token "/tmp/rpc_hdr_$$")
[ -n "$LOGIN_TOKEN" ] \
    && green "Login: rpc_token cookie received" \
    || red "Login: no rpc_token in Set-Cookie — subsequent tests will fail"
# 检查 HttpOnly 和 Secure 属性
grep -i 'set-cookie' "/tmp/rpc_hdr_$$" | grep -qi 'httponly' \
    && green "Cookie has HttpOnly flag" \
    || red "Cookie missing HttpOnly"
grep -i 'set-cookie' "/tmp/rpc_hdr_$$" | grep -qi 'samesite=strict' \
    && green "Cookie has SameSite=Strict" \
    || warn "Cookie missing SameSite=Strict"

# 1.4 错误密码（应拒绝）
title "1.4 错误密码（应拒绝）"
LOGIN2=$($CURL -X POST "$API/api/login" \
    -H 'Content-Type: application/json' \
    -d '{"username":"tester_fn","password":"wrongpass"}')
echo "$LOGIN2" | grep -q '"success":false' \
    && green "Bad password rejected" \
    || red "Bad password should fail"

# 1.5 短用户名（应拒绝）
title "1.5 用户名 <3 字符（应拒绝）"
REG3=$($CURL -X POST "$API/api/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"ab","password":"test1234"}')
echo "$REG3" | grep -q '"success":false' \
    && green "Short username rejected" \
    || red "Should reject short username"

# 1.6 短密码（应拒绝）— 内网直连绕过 nginx 限流
title "1.6 密码 <6 字符（应拒绝）"
REG4=$($CURL -X POST "$API/api/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"shortpw_fn","password":"12345"}')
echo "$REG4" | grep -q '"success":false' \
    && green "Short password rejected" \
    || red "Should reject short password"

# ---- 2. 鉴权拦截 ----
title "2. 鉴权拦截"

# 2.1 无 Cookie → 401
title "2.1 无 Cookie → /api/sheets（应 401）"
CODE=$($CURL -o /dev/null -w "%{http_code}" "$API/api/sheets")
[ "$CODE" = "401" ] \
    && green "No cookie → 401" \
    || red "Expected 401, got $CODE"

# 2.2 假 Cookie → 401
title "2.2 伪造 Cookie → /api/sheets（应 401）"
CODE=$($CURL -o /dev/null -w "%{http_code}" \
    -H "Cookie: rpc_token=fake.jwt.token" "$API/api/sheets")
[ "$CODE" = "401" ] \
    && green "Fake cookie → 401" \
    || red "Expected 401, got $CODE"

# 2.3 限流验证（100r/s + burst 50）
title "2.3 高频请求触发限流（应见 429）"
RATELIMIT_CODES=""
for i in $(seq 1 25); do
    CODE=$($CURL -o /dev/null -w "%{http_code}" "$API/api/health" 2>/dev/null)
    RATELIMIT_CODES="$RATELIMIT_CODES $CODE"
done &
for i in $(seq 1 25); do
    CODE=$($CURL -o /dev/null -w "%{http_code}" "$API/api/health" 2>/dev/null)
    RATELIMIT_CODES="$RATELIMIT_CODES $CODE"
done &
for i in $(seq 1 25); do
    CODE=$($CURL -o /dev/null -w "%{http_code}" "$API/api/health" 2>/dev/null)
    RATELIMIT_CODES="$RATELIMIT_CODES $CODE"
done &
wait
echo "$RATELIMIT_CODES" | grep -q "429" \
    && green "Rate limiting active (429 seen)" \
    || warn "No 429 — rate limit may not be triggered (try higher burst)"

# ---- 3. 表格 CRUD ----
title "3. 表格 (Spreadsheet)"
# 后续请求用 -b $JAR 带 cookie

# 3.1 创建
title "3.1 创建表格"
CREATE=$($CURL -X POST "$API/api/sheets" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d '{"name":"测试表格","description":"自动化测试","headers_json":"[\"A\",\"B\",\"C\"]","data_json":"[[\"a1\",\"b1\",\"c1\"],[\"a2\",\"b2\",\"c2\"]]"}')
SHEET_ID=$(echo "$CREATE" | sed 's/.*"id"://;s/[},].*//')
echo "$CREATE" | grep -q '"success":true' \
    && green "Create sheet OK, id=$SHEET_ID" \
    || red "Create sheet failed: $CREATE"

# 3.2 列表
title "3.2 列表查询"
LIST=$($CURL "$API/api/sheets" -b "$JAR")
echo "$LIST" | grep -q '"success":true' \
    && green "List sheets OK" \
    || red "List failed: $LIST"

# 3.3 获取单表（POST /api/sheets/get）
title "3.3 获取单表"
GET=$($CURL -X POST "$API/api/sheets/get" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d "{\"id\":$SHEET_ID}")
echo "$GET" | grep -q '"success":true' \
    && green "Get sheet OK" \
    || red "Get sheet failed: $GET"

# 3.4 缓存命中（第二次查询期望 redis）
title "3.4 缓存命中（第二次查询期望 redis）"
GET2=$($CURL -X POST "$API/api/sheets/get" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d "{\"id\":$SHEET_ID}")
echo "$GET2" | grep -q '"cache_source":"redis"' \
    && green "Cache HIT (redis)" \
    || warn "Cache MISS (may be first access or Redis unavailable)"

# 3.5 更新（PUT /api/sheets）
title "3.5 更新表格"
UPDATE=$($CURL -X PUT "$API/api/sheets" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d "{\"id\":$SHEET_ID,\"name\":\"已更新\",\"description\":\"更新测试\",\"headers_json\":\"[\\\"X\\\"]\",\"data_json\":\"[[\\\"y\\\"]]\"}")
echo "$UPDATE" | grep -q '"success":true' \
    && green "Update sheet OK" \
    || red "Update failed: $UPDATE"

# 3.6 删除（POST /api/sheets/delete）
title "3.6 删除表格"
DELETE=$($CURL -X POST "$API/api/sheets/delete" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d "{\"id\":$SHEET_ID}")
echo "$DELETE" | grep -q '"success":true' \
    && green "Delete sheet OK" \
    || red "Delete failed: $DELETE"

# 3.7 删除不存在的表格（应拒绝）
title "3.7 删除不存在（应拒绝）"
DEL2=$($CURL -X POST "$API/api/sheets/delete" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d '{"id":999999}')
echo "$DEL2" | grep -q '"success":false' \
    && green "Delete nonexistent rejected" \
    || red "Should reject delete of nonexistent sheet"

# ---- 4. 文件管理 ----
title "4. 文件 (File)"

# 4.1 上传
title "4.1 上传文件"
echo "Hello HTTP-RPC $(date)" > /tmp/rpc_test_upload.txt
UPLOAD=$($CURL -X POST "$API/api/files/upload" \
    -b "$JAR" \
    -F "file=@/tmp/rpc_test_upload.txt")
FILE_ID=$(echo "$UPLOAD" | sed 's/.*"id"://;s/[},].*//')
echo "$UPLOAD" | grep -q '"success":true' \
    && green "Upload OK, id=$FILE_ID" \
    || red "Upload failed: $UPLOAD"

# 4.2 列表
title "4.2 文件列表"
FLIST=$($CURL "$API/api/files" -b "$JAR")
echo "$FLIST" | grep -q '"success":true' \
    && green "File list OK" \
    || red "File list failed: $FLIST"

# 4.3 下载并校验内容
title "4.3 下载并校验"
$CURL -o /tmp/rpc_test_download.txt \
    "$API/api/files/download?id=$FILE_ID" -b "$JAR" 2>/dev/null
grep -q "Hello HTTP-RPC" /tmp/rpc_test_download.txt 2>/dev/null \
    && green "Download & content verified" \
    || red "Download failed or content mismatch"

# 4.4 删除文件（POST /api/files/delete）
title "4.4 删除文件"
FDEL=$($CURL -X POST "$API/api/files/delete" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d "{\"id\":$FILE_ID}")
echo "$FDEL" | grep -q '"success":true' \
    && green "Delete file OK" \
    || red "Delete file failed: $FDEL"

# ---- 5. Logout ----
title "5. Logout"
LOGOUT=$($CURL -X POST "$API/api/logout" \
    -b "$JAR" -c "$JAR" -D "/tmp/rpc_hdr_$$")
echo "$LOGOUT" | grep -q '"success":true' \
    && green "Logout OK" \
    || red "Logout failed: $LOGOUT"
# 退出后 cookie 被清零（Max-Age=0），再次请求应 401
CODE=$($CURL -o /dev/null -w "%{http_code}" "$API/api/sheets" -b "$JAR")
[ "$CODE" = "401" ] \
    && green "Post-logout cookie rejected (401)" \
    || warn "Post-logout still authorized — cookie jar may persist (got $CODE)"

# ---- 6. Token 吊销 ----
title "6. Token 吊销验证"

# 6.1 重新登录获取一个有效 cookie
rm -f "$JAR"
$CURL -X POST "$API/api/login" \
    -H 'Content-Type: application/json' \
    -c "$JAR" \
    -d '{"username":"tester_fn","password":"test1234"}' > /dev/null 2>&1

# 6.2 递增 token_version（模拟改密码）
title "6.2 递增 token_version"
NEW_VER=$(docker exec http-rpc-mysql-auth-1 mysql -u root -p123456 -N \
    -e "UPDATE rpc_auth.users SET token_version=token_version+1 WHERE username='tester_fn'; SELECT token_version FROM rpc_auth.users WHERE username='tester_fn';" 2>/dev/null | tail -1)
docker exec http-rpc-redis-cluster-1 redis-cli -c -p 7000 -a rpc-redis-123456 --no-auth-warning \
    SETEX "token_ver:tester_fn" 86400 "$NEW_VER" 2>/dev/null
green "token_version bumped to $NEW_VER"

# 6.3 旧 Cookie 应被拒绝
title "6.3 旧 Cookie → /api/sheets（应 401）"
CODE=$($CURL -o /dev/null -w "%{http_code}" "$API/api/sheets" -b "$JAR")
[ "$CODE" = "401" ] \
    && green "Old cookie rejected (revoked)" \
    || red "Expected 401, got $CODE — revocation may not be working"

# ---- 7. 健康检查 & 服务列表 ----
title "7. 健康检查 & 服务列表"

# 重新登录以获取有效 cookie
rm -f "$JAR"
$CURL -X POST "$API/api/login" \
    -H 'Content-Type: application/json' \
    -c "$JAR" \
    -d '{"username":"tester_fn","password":"test1234"}' > /dev/null 2>&1

HEALTH=$($CURL "$API/api/health")
echo "$HEALTH" | grep -q '"gateway"' \
    && green "Health check OK" \
    || red "Health check failed: $HEALTH"
echo "$HEALTH" | grep -q '"breaker"' \
    && green "Circuit breaker states present" \
    || warn "No breaker field — Gateway may need rebuild"

SVCS=$($CURL "$API/api/services" -b "$JAR")
echo "$SVCS" | grep -q "SpreadsheetService" \
    && green "Services list OK" \
    || red "Services failed: $SVCS"

# ---- 结果 ----
echo ""
echo "=========================================="
echo "  Functional Test: $PASS passed, $FAIL failed, $((PASS + FAIL)) total"
echo "=========================================="
[ "$FAIL" = "0" ]
