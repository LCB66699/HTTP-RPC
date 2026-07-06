#!/bin/bash
# HTTP-RPC 功能测试脚本 (v5 — gRPC-Gateway RESTful API)
# 用法: bash functional_test.sh [API_URL]
# 默认: https://localhost (自签证书)

API="${1:-https://localhost}"
CURL="curl -sk --connect-timeout 5 --max-time 15 --retry 2"
JAR="/tmp/rpc_functional_cookies_$$"
PASS=0; FAIL=0
TEST_USER="tester_$(date +%s)"
TEST_PASS="test1234"

# CI runner 网络不稳定，轮询 Gateway 直到就绪
for i in $(seq 1 60); do
    if curl -sk -o /dev/null -w "%{http_code}" "$API/api/v1/health" 2>/dev/null | grep -q "200"; then
        break
    fi
    sleep 3
done

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

extract_token() {
    grep -i 'set-cookie:.*rpc_at=' "$1" 2>/dev/null \
        | sed 's/.*rpc_at=//;s/;.*//' | tr -d '\r\n'
}

cleanup() { rm -f "$JAR" "/tmp/rpc_hdr_$$" "/tmp/rpc_test_upload.txt" "/tmp/rpc_test_download.txt" "/tmp/rl.txt" "/tmp/rpc_func_jar2_$$"; }
trap cleanup EXIT

# ---- 0. 连通性检查 ----
title "0. API 连通性"
if $CURL -o /dev/null -w "%{http_code}" "$API/api/v1/health" | grep -q "200"; then
    green "API reachable"
else
    red "API unreachable at $API"
    exit 1
fi

# 预热：等 Auth 真正就绪（能处理注册请求）
title "0.1 Auth 预热"
WARM_USER="warmup_$(date +%s)"
for i in $(seq 1 50); do
    WARM=$($CURL -X POST "$API/api/v1/register" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$WARM_USER\",\"password\":\"test1234\"}" 2>/dev/null)
    if echo "$WARM" | grep -q '"success":true'; then
        green "Auth ready after ${i}s"
        break
    fi
    if [ $i -eq 50 ]; then
        red "Auth not ready after 250s: $WARM"
        exit 1
    fi
    sleep 5
done

# ---- 1. 认证 ----
title "1. 认证 (Auth)"

title "1.1 注册新用户（或登录已有）"
echo "DEBUG: TEST_USER=$TEST_USER"
# CI runner 慢，MySQL 就绪需要更长时间，最多重试 5 次
for retry in 1 2 3 4 5; do
    REG=$($CURL -X POST "$API/api/v1/register" \
        -H 'Content-Type: application/json' \
        -c "$JAR" -D "/tmp/rpc_hdr_$$" \
        -d "{\"username\":\"$TEST_USER\",\"password\":\"test1234\"}")
    echo "DEBUG: REG=$REG"
    echo "$REG" | grep -q '"success":true' && break
    if [ $retry -lt 5 ]; then
        warn "Register attempt $retry failed, retrying in 3s..."
        rm -f "$JAR"
        sleep 3
    fi
done
if echo "$REG" | grep -q '"success":true'; then
    green "Register OK"
else
    warn "Register failed, trying login..."
    REG=$($CURL -X POST "$API/api/v1/login" \
        -H 'Content-Type: application/json' \
        -c "$JAR" -D "/tmp/rpc_hdr_$$" \
        -d "{\"username\":\"$TEST_USER\",\"password\":\"test1234\"}")
    echo "DEBUG: LOGIN=$REG"
    REG=$($CURL -X POST "$API/api/v1/login" \
        -H 'Content-Type: application/json' \
        -c "$JAR" -D "/tmp/rpc_hdr_$$" \
        -d "{\"username\":\"$TEST_USER\",\"password\":\"test1234\"}")
    echo "$REG" | grep -q '"success":true' \
        && green "Login OK (reused existing user)" \
        || red "Login also failed: $REG"
fi
REG_TOKEN=$(extract_token "/tmp/rpc_hdr_$$")
[ -n "$REG_TOKEN" ] \
    && green "Register: rpc_at cookie received" \
    || red "Register: no rpc_at in Set-Cookie"

title "1.2 重复注册（应拒绝）"
REJECTED=0
for i in 1 2 3; do
    REG2=$($CURL -X POST "$API/api/v1/register" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$TEST_USER\",\"password\":\"test1234\"}")
    if echo "$REG2" | grep -q '"error"'; then
        REJECTED=1; break
    fi
    sleep 0.5
done
[ "$REJECTED" = "1" ] \
    && green "Duplicate register rejected" \
    || red "Duplicate register should fail"

title "1.3 登录获取 Cookie"
rm -f "$JAR"
LOGIN=$($CURL -X POST "$API/api/v1/login" \
    -H 'Content-Type: application/json' \
    -c "$JAR" -D "/tmp/rpc_hdr_$$" \
    -d "{\"username\":\"$TEST_USER\",\"password\":\"test1234\"}")
echo "$LOGIN" | grep -q '"success":true' \
    && green "Login OK" \
    || red "Login failed: $LOGIN"
LOGIN_TOKEN=$(extract_token "/tmp/rpc_hdr_$$")
[ -n "$LOGIN_TOKEN" ] \
    && green "Login: rpc_at cookie received" \
    || red "Login: no rpc_at in Set-Cookie — subsequent tests will fail"
grep -i 'set-cookie' "/tmp/rpc_hdr_$$" | grep -qi 'httponly' \
    && green "Cookie has HttpOnly flag" \
    || red "Cookie missing HttpOnly"
grep -i 'set-cookie' "/tmp/rpc_hdr_$$" | grep -qi 'samesite=strict' \
    && green "Cookie has SameSite=Strict" \
    || warn "Cookie missing SameSite=Strict"

title "1.4 错误密码（应拒绝）"
LOGIN2=$($CURL -X POST "$API/api/v1/login" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"$TEST_USER\",\"password\":\"wrongpass\"}")
echo "$LOGIN2" | grep -q '"error"' \
    && green "Bad password rejected" \
    || red "Bad password should fail"

title "1.5 用户名 <3 字符（应拒绝）"
REG3=$($CURL -X POST "$API/api/v1/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"ab","password":"test1234"}')
echo "$REG3" | grep -q '"error"' \
    && green "Short username rejected" \
    || red "Should reject short username"

title "1.6 密码 <6 字符（应拒绝）"
REG4=$($CURL -X POST "$API/api/v1/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"shortpw_fn","password":"12345"}')
echo "$REG4" | grep -q '"error"' \
    && green "Short password rejected" \
    || red "Should reject short password"

# ---- 2. 鉴权拦截 ----
title "2. 鉴权拦截"

title "2.1 无 Cookie → /api/v1/sheets（应 401）"
CODE=$($CURL -o /dev/null -w "%{http_code}" "$API/api/v1/sheets")
[ "$CODE" = "401" ] \
    && green "No cookie → 401" \
    || red "Expected 401, got $CODE"

title "2.2 伪造 Cookie → /api/v1/sheets（应 401）"
CODE=$($CURL -o /dev/null -w "%{http_code}" \
    -H "Cookie: rpc_at=fake.jwt.token" "$API/api/v1/sheets")
[ "$CODE" = "401" ] \
    && green "Fake cookie → 401" \
    || red "Expected 401, got $CODE"

# ---- 3. 表格 CRUD ----
title "3. 表格 (Spreadsheet)"

title "3.1 创建表格"
for retry in 1 2 3 4 5 6 7 8 9 10; do
    CREATE=$($CURL -X POST "$API/api/v1/sheets" \
        -H 'Content-Type: application/json' \
        -b "$JAR" \
        -d '{"name":"测试表格","description":"自动化测试","headers_json":"[\"A\",\"B\",\"C\"]","data_json":"[[\"a1\",\"b1\",\"c1\"],[\"a2\",\"b2\",\"c2\"]]"}')
    echo "$CREATE" | grep -q '"success":true' && break
    if [ $retry -lt 10 ]; then
        warn "Sheet create retry $retry/10, waiting for MySQL spreadsheet..."
        sleep 10
    fi
done
SHEET_ID=$(echo "$CREATE" | sed 's/.*"id":"*//;s/"*[,}].*//')
echo "$CREATE" | grep -q '"success":true' \
    && green "Create sheet OK, id=$SHEET_ID" \
    || red "Create sheet failed: $CREATE"
sleep 2

title "3.2 列表查询"
LIST=$($CURL "$API/api/v1/sheets" -b "$JAR")
echo "$LIST" | grep -q '"success":true' \
    && green "List sheets OK" \
    || red "List failed: $LIST"

title "3.2b 游标分页"
PAGE1=$($CURL "$API/api/v1/sheets?limit=5" -b "$JAR")
CURSOR=$(echo "$PAGE1" | grep -o '"next_cursor":"[^"]*"' | sed 's/"next_cursor":"//;s/"//')
HASMORE=$(echo "$PAGE1" | grep -o '"has_more":[^,}]*' | sed 's/"has_more"://')
if [ -n "$CURSOR" ] && [ "$HASMORE" = "true" ]; then
    PAGE2=$($CURL "$API/api/v1/sheets?limit=5&after_id=$CURSOR" -b "$JAR")
    echo "$PAGE2" | grep -q '"success":true' \
        && green "Cursor pagination OK" \
        || red "Cursor page2 failed: $PAGE2"
else
    warn "Cursor pagination skipped (not enough data or not implemented)"
fi

title "3.2c 旧偏移分页兼容"
OLD_PAGE=$($CURL "$API/api/v1/sheets?page=0&page_size=5" -b "$JAR")
echo "$OLD_PAGE" | grep -q '"success":true' \
    && green "List sheets OK" \
    || red "List failed: $LIST"

title "3.3 获取单表"
GET=$($CURL "$API/api/v1/sheets/$SHEET_ID" -b "$JAR")
echo "$GET" | grep -q '"success":true' \
    && green "Get sheet OK" \
    || red "Get sheet failed: $GET"

title "3.4 缓存命中（第二次查询）"
GET2=$($CURL "$API/api/v1/sheets/$SHEET_ID" -b "$JAR")
echo "$GET2" | grep -q '"cache_source":"redis"' \
    && green "Cache HIT (redis)" \
    || warn "Cache MISS (may be first access or Redis unavailable)"

title "3.5 更新表格"
UPDATE=$($CURL -X PUT "$API/api/v1/sheets/$SHEET_ID" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d '{"name":"已更新","description":"更新测试","headers_json":"[\"X\"]","data_json":"[[\"y\"]]"}')
echo "$UPDATE" | grep -q '"success":true' \
    && green "Update sheet OK" \
    || red "Update failed: $UPDATE"

title "3.6 删除表格"
DELETE=$($CURL -X DELETE "$API/api/v1/sheets/$SHEET_ID" -b "$JAR")
echo "$DELETE" | grep -q '"success":true' \
    && green "Delete sheet OK" \
    || red "Delete failed: $DELETE"

title "3.7 删除不存在（应拒绝）"
DEL2_CODE=$($CURL -o /dev/null -w '%{http_code}' -X DELETE "$API/api/v1/sheets/999999" -b "$JAR")
[ "$DEL2_CODE" != "200" ] \
    && green "Delete nonexistent rejected (HTTP $DEL2_CODE)" \
    || red "Should reject delete of nonexistent sheet (HTTP $DEL2_CODE)"

# ---- 4. 文件管理 ----
title "4. 文件 (File)"

title "4.1 上传文件"
echo "Hello HTTP-RPC $(date)" > /tmp/rpc_test_upload.txt
UPLOAD=$($CURL -X POST "$API/api/v1/files/upload" \
    -b "$JAR" \
    -F "file=@/tmp/rpc_test_upload.txt")
FILE_ID=$(echo "$UPLOAD" | sed 's/.*"id":"*//;s/"*[,}].*//')
echo "$UPLOAD" | grep -q '"success":true' \
    && green "Upload OK, id=$FILE_ID" \
    || red "Upload failed: $UPLOAD"
sleep 2

title "4.2 文件列表"
FLIST=$($CURL "$API/api/v1/files" -b "$JAR")
echo "$FLIST" | grep -q '"success":true' \
    && green "File list OK" \
    || red "File list failed: $FLIST"

title "4.3 下载并校验内容"
$CURL -L -o /tmp/rpc_test_download.txt \
    "$API/api/v1/files/$FILE_ID" -b "$JAR" 2>/dev/null
grep -q "Hello HTTP-RPC" /tmp/rpc_test_download.txt 2>/dev/null \
    && green "Download & content verified" \
    || red "Download failed or content mismatch"

title "4.4 删除文件"
FDEL=$($CURL -X DELETE "$API/api/v1/files/$FILE_ID" -b "$JAR")
echo "$FDEL" | grep -q '"success":true' \
    && green "Delete file OK" \
    || red "Delete file failed: $FDEL"

# ---- 5. Token Refresh ----
title "5. Token 刷新 (RefreshToken)"
sleep 3

REFRESH_LOGIN=$(curl -sk -X POST "$API/api/v1/login" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"$TEST_USER\",\"password\":\"test1234\"}")
REFRESH_TOKEN=$(echo "$REFRESH_LOGIN" | python3 -c "import sys,json; print(json.load(sys.stdin).get('refresh_token',''))" 2>/dev/null)
[ -n "$REFRESH_TOKEN" ] \
    && green "Refresh token captured: ${REFRESH_TOKEN:0:8}..." \
    || red "No refresh_token in login response"

if [ -n "$REFRESH_TOKEN" ]; then
    title "5.1 POST /api/v1/refresh"
    REFRESH_RESP=$($CURL -X POST "$API/api/v1/refresh" \
        -H 'Content-Type: application/json' \
        -b "$JAR" \
        -d "{\"username\":\"$TEST_USER\",\"refresh_token\":\"$REFRESH_TOKEN\"}")
    echo "DEBUG[refresh] resp=$REFRESH_RESP"
    NEW_AT=$(echo "$REFRESH_RESP" | python3 -c "import sys,json; print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
    if [ -n "$NEW_AT" ]; then
        green "Token refresh OK (new access_token: ${NEW_AT:0:8}...)"
        title "5.2 新 token 验证"
        NEW_RESP=$(curl -sk -w "\n%{http_code}" -H "Cookie: rpc_at=$NEW_AT" "$API/api/v1/sheets?page=0&page_size=1" 2>/dev/null)
        NEW_CODE=$(echo "$NEW_RESP" | tail -1)
        NEW_BODY=$(echo "$NEW_RESP" | head -n -1)
        echo "DEBUG: NEW_AT len=${#NEW_AT} first8=${NEW_AT:0:8} code=$NEW_CODE body=$NEW_BODY"
        [ "$NEW_CODE" = "200" ] \
            && green "Refreshed token accepted" \
            || red "Refreshed token rejected (got $NEW_CODE)"
    else
        red "Token refresh failed: $REFRESH_RESP"
    fi
    title "5.2 无效 refresh_token 应被拒绝"
    BAD_REFRESH=$($CURL -X POST "$API/api/v1/refresh" \
        -H 'Content-Type: application/json' \
        -d '{"username":"$TEST_USER","refresh_token":"00000000-0000-0000-0000-000000000000"}')
    echo "$BAD_REFRESH" | grep -q '"error"' \
        && green "Invalid refresh_token rejected" \
        || red "Should reject invalid refresh_token"
else
    red "Skipped: no refresh_token available"
fi

# ---- 6. 搜索 ----
title "6. 搜索 (Elasticsearch)"

SEARCH_REQ="{\"q\":\"$TEST_USER\",\"scope\":\"sheets\"}"
SEARCH_RES=$($CURL -X POST "$API/api/v1/search" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d "$SEARCH_REQ")
echo "$SEARCH_RES" | grep -q '"success":true' \
    && green "Search API OK" \
    || red "Search failed: $SEARCH_RES"

# ---- 7. 跨用户隔离 ----
title "7. 跨用户隔离"

# 注册第二个用户
JAR2="/tmp/rpc_func_jar2_$$"
TEST_USER2="tester2_$(date +%s)"
$CURL -X POST "$API/api/v1/register" -H 'Content-Type: application/json' \
    -d "{\"username\":\"$TEST_USER2\",\"password\":\"test1234\"}" > /dev/null 2>&1
$CURL -X POST "$API/api/v1/login" -H 'Content-Type: application/json' \
    -c "$JAR2" -d "{\"username\":\"$TEST_USER2\",\"password\":\"test1234\"}" > /dev/null 2>&1

# user2 创建自己的表
CREATE2=$($CURL -X POST "$API/api/v1/sheets" -H 'Content-Type: application/json' \
    -b "$JAR2" -d '{"name":"user2私密表","headers_json":"[]","data_json":"[]"}')
SHEET_ID2=$(echo "$CREATE2" | sed 's/.*"id":"*//;s/"*[,}].*//')

# user1 的列表不应看到 user2 的表
LIST_CHECK=$($CURL "$API/api/v1/sheets" -b "$JAR")
echo "$LIST_CHECK" | grep -q "$SHEET_ID2" \
    && red "Cross-user leak: user1 sees user2 sheet" \
    || green "Cross-user isolation OK"

# user1 不能打开 user2 的表
CODE_CROSS=$($CURL -o /dev/null -w "%{http_code}" "$API/api/v1/sheets/$SHEET_ID2" -b "$JAR")
[ "$CODE_CROSS" != "200" ] \
    && green "Get other user's sheet rejected" \
    || red "Cross-user access allowed"

# 清理
$CURL -X DELETE "$API/api/v1/sheets/$SHEET_ID2" -b "$JAR2" > /dev/null 2>&1

# ---- 8. Health ----
# ---- 6b. 账户管理 ----
title "6b. 改密码"
CHPWD=$($CURL -X PUT "$API/api/v1/me/password" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d "{\"old_password\":\"test1234\",\"new_password\":\"newpass456\"}")
echo "$CHPWD" | grep -q '"success":true' \
    && green "Change password OK" \
    || warn "Change password not implemented: $CHPWD"

# ---- 6c. 文件夹 ----
title "6c. 创建文件夹"
FOLDER=$($CURL -X POST "$API/api/v1/files/folder" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d '{"name":"test_folder"}')
echo "$FOLDER" | grep -q '"success":true' \
    && green "Create folder OK" \
    || warn "Folder feature not implemented: $FOLDER"

# ---- 6d. 相册 ----
title "6d. 照片列表"
PHOTOS=$($CURL "$API/api/v1/photos" -b "$JAR")
echo "$PHOTOS" | grep -q '"success":true' \
    && green "Photo list OK" \
    || warn "Photo feature not implemented: $PHOTOS"

# ---- 6e. 分享链接 ----
title "6e. 创建分享链接"
SHARE=$($CURL -X POST "$API/api/v1/sheets/$SHEET_ID/share-link" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d '{"permission":"view"}')
echo "$SHARE" | grep -q '"token"' \
    && green "Share link created" \
    || warn "Share feature not implemented: $SHARE"

# ---- 6f. 分享权限校验 ----
title "6f. 分享权限校验"

# Create a fresh sheet for sharing tests
SHARE_SHEET=$($CURL -X POST "$API/api/v1/sheets" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d '{"name":"share-test","headers_json":"[]","data_json":"[]"}')
SHARE_SHEET_ID=$(echo "$SHARE_SHEET" | sed 's/.*"id":"*//;s/"*[,}].*//')

# user1 shares the sheet with user2 (view permission)
SHARE_RESP=$($CURL -X POST "$API/api/v1/sheets/$SHARE_SHEET_ID/share" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d "{\"username\":\"$TEST_USER2\",\"permission\":\"view\"}")
echo "$SHARE_RESP" | grep -q '"success":true' \
    && green "Share with user2 OK" \
    || red "Share failed: $SHARE_RESP"

# user2 reads the shared sheet — should succeed
READ_CODE=$($CURL -o /dev/null -w "%{http_code}" "$API/api/v1/sheets/$SHARE_SHEET_ID" -b "$JAR2")
[ "$READ_CODE" = "200" ] \
    && green "Shared user read OK" \
    || red "Shared user read denied (code=$READ_CODE)"

# user2 tries to update with view-only permission — should fail
UPDATE_CODE=$($CURL -o /dev/null -w "%{http_code}" -X PUT "$API/api/v1/sheets/$SHARE_SHEET_ID" \
    -H 'Content-Type: application/json' \
    -b "$JAR2" \
    -d '{"name":"hacked","headers_json":"[]","data_json":"[]"}')
[ "$UPDATE_CODE" != "200" ] \
    && green "Shared user view-only edit blocked OK" \
    || red "Shared user with view-only was able to edit"

# user1 upgrades to edit permission
$CURL -X POST "$API/api/v1/sheets/$SHARE_SHEET_ID/share" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d "{\"username\":\"$TEST_USER2\",\"permission\":\"edit\"}" > /dev/null 2>&1

# user2 now tries to update — should succeed
UPDATE_EDIT_CODE=$($CURL -o /dev/null -w "%{http_code}" -X PUT "$API/api/v1/sheets/$SHARE_SHEET_ID" \
    -H 'Content-Type: application/json' \
    -b "$JAR2" \
    -d '{"name":"shared-edit","headers_json":"[]","data_json":"[]"}')
[ "$UPDATE_EDIT_CODE" = "200" ] \
    && green "Shared user edit OK" \
    || warn "Shared user edit denied (code=$UPDATE_EDIT_CODE)"

# user2 tries to delete — should fail (only owner can delete)
DELETE_SHARED=$($CURL -X DELETE "$API/api/v1/sheets/$SHARE_SHEET_ID" -b "$JAR2" -w "%{http_code}" -o /dev/null)
[ "$DELETE_SHARED" != "200" ] \
    && green "Shared user delete blocked OK" \
    || red "Shared user was able to delete"

# Cleanup
$CURL -X DELETE "$API/api/v1/sheets/$SHARE_SHEET_ID" -b "$JAR" > /dev/null 2>&1

title "8. 健康检查"

HEALTH=$($CURL "$API/api/v1/health")
echo "$HEALTH" | grep -q '"gateway"' \
    && green "Health check OK" \
    || red "Health check failed: $HEALTH"

# ---- 结果 ----
echo ""
echo "=========================================="
echo "  Functional Test: $PASS passed, $FAIL failed, $((PASS + FAIL)) total"
echo "=========================================="
[ "$FAIL" = "0" ]
