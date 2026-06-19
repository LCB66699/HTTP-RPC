#!/bin/bash
# HTTP-RPC 逐层压测 (v6 — gRPC-Gateway 架构)
# 用法: bash stress_test.sh [API_URL] [REQUESTS] [CONCURRENCY] [--long]
set +e

API="${1:-https://localhost}"
REQ="${2:-5000}"
CONC="${3:-20}"
LONG_MODE=0
for arg in "$@"; do [ "$arg" = "--long" ] && LONG_MODE=1; done

JAR="/tmp/rpc_stress_cookies_$$"
CURL="curl -sk --connect-timeout 5 --max-time 30"

LOG_DIR="$(cd "$(dirname "$0")" && pwd)/logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/stress_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee -a "$LOG_FILE") 2>&1
echo "Log: $LOG_FILE"
echo ""

green() { echo -e "\033[32m[OK]\033[0m $1"; }
red()   { echo -e "\033[31m[FAIL]\033[0m $1"; }
warn()  { echo -e "\033[33m[WARN]\033[0m $1"; }
title() { echo -e "\n\033[1;36m=== $1 ===\033[0m"; }
stat()  { echo -e "  \033[33m$1\033[0m $2"; }

cleanup() { rm -f "$JAR" /tmp/stress_*.tmp; }
trap cleanup EXIT

TMPDIR="/tmp/stress_$$"
mkdir -p "$TMPDIR"

# ---- 登录 ----
$CURL -X POST "$API/api/v1/login" -H 'Content-Type: application/json' \
    -c "$JAR" -d '{"username":"stress_tester","password":"stress1234"}' > /dev/null 2>&1
RPC_TOKEN=$(grep 'rpc_at' "$JAR" 2>/dev/null | awk '{print $NF}' | head -1)
[ -z "$RPC_TOKEN" ] && {
    $CURL -X POST "$API/api/v1/register" -H 'Content-Type: application/json' \
        -d '{"username":"stress_tester","password":"stress1234"}' > /dev/null 2>&1
    $CURL -X POST "$API/api/v1/login" -H 'Content-Type: application/json' \
        -c "$JAR" -d '{"username":"stress_tester","password":"stress1234"}' > /dev/null 2>&1
    RPC_TOKEN=$(grep 'rpc_at' "$JAR" 2>/dev/null | awk '{print $NF}' | head -1)
}
green "Cookie ready"

# ===== L1: 内网直连 grpc-gateway (无 TLS) =====
title "L1: 直连 grpc-gateway:8082 (无 TLS, $REQ req x $CONC c)"

ab_bench() {
    local name="$1" url="$2" cookie="$3"
    echo "  [$name]"
    if [ -n "$cookie" ]; then
        ab -n "$REQ" -c "$CONC" -k -C "rpc_at=$cookie" "$url" 2>&1 \
            | grep -E "Requests per second|Failed|Non-2xx|Time per request" | head -6
    else
        ab -n "$REQ" -c "$CONC" -k "$url" 2>&1 \
            | grep -E "Requests per second|Failed|Non-2xx|Time per request" | head -6
    fi
    echo ""
}

GW="http://localhost:8082"
ab_bench "Health (无鉴权)"        "$GW/api/v1/health" ""
ab_bench "Sheet List (鉴权+gRPC)" "$GW/api/v1/sheets" "$RPC_TOKEN"

# ===== L2: 经 nginx HTTPS =====
title "L2: nginx HTTPS ($API, $REQ req x $CONC c)"

ab_bench "Health (TLS)"           "$API/api/v1/health" ""
ab_bench "Sheet List (TLS+Auth)" "$API/api/v1/sheets" "$RPC_TOKEN"

# ===== L3: 写入压测 =====
CREATED_IDS="$TMPDIR/created_ids.txt"
:> "$CREATED_IDS"

title "L3: 写入压测 (200 次创建→获取→更新→删除循环)"

START=$(date +%s%N)
WRITE_OK=0; WRITE_FAIL=0
for i in $(seq 1 200); do
    CR=$($CURL -X POST "$API/api/v1/sheets" -H 'Content-Type: application/json' \
        -b "$JAR" -d "{\"name\":\"stress-$i\",\"headers_json\":\"[]\",\"data_json\":\"[]\"}" 2>/dev/null)
    SID=$(echo "$CR" | sed 's/.*"id"://;s/[},].*//')
    if [ -n "$SID" ] && [ "$SID" != "0" ]; then
        $CURL "$API/api/v1/sheets/$SID" -b "$JAR" -o /dev/null 2>/dev/null
        $CURL -X PUT "$API/api/v1/sheets/$SID" -H 'Content-Type: application/json' \
            -b "$JAR" -d '{"name":"stress-updated","headers_json":"[]","data_json":"[]"}' -o /dev/null 2>/dev/null
        $CURL -X DELETE "$API/api/v1/sheets/$SID" -b "$JAR" -o /dev/null 2>/dev/null
        WRITE_OK=$((WRITE_OK + 1))
    else
        WRITE_FAIL=$((WRITE_FAIL + 1))
    fi
done
END=$(date +%s%N)
WR_MS=$(( (END - START) / 1000000 ))
stat "写入成功率  " "$WRITE_OK/200 ($(( WRITE_OK * 100 / 200 ))%)"
stat "总耗时      " "${WR_MS}ms (avg $(( WR_MS / 200 ))ms/op)"

# ===== L4: 并发读取稳定性 =====
title "L4: 并发读取稳定性 (1000 req x 20c, 10s 持续)"

$CURL -X POST "$API/api/v1/sheets" -H 'Content-Type: application/json' \
    -b "$JAR" -d '{"name":"read-stress","headers_json":"[\"A\"]","data_json":"[[\"v\"]]"}' > /dev/null 2>&1
SHEET_ID=$($CURL "$API/api/v1/sheets" -b "$JAR" 2>/dev/null | sed 's/.*"id"://;s/[},].*//')

echo "  [混合负载: 70%/api/v1/sheets + 20%/api/v1/sheets/$SHEET_ID + 10%/api/v1/health]"
:> "$TMPDIR/read_codes.txt"
REQ_COUNT=1000
for i in $(seq 1 $REQ_COUNT); do
    MOD=$((i % 10))
    if [ "$MOD" -lt 7 ]; then
        curl -sk -o /dev/null -w "%{http_code}\n" "$API/api/v1/sheets" -b "$JAR" >> "$TMPDIR/read_codes.txt" 2>/dev/null &
    elif [ "$MOD" -lt 9 ]; then
        curl -sk -o /dev/null -w "%{http_code}\n" "$API/api/v1/sheets/$SHEET_ID" -b "$JAR" >> "$TMPDIR/read_codes.txt" 2>/dev/null &
    else
        curl -sk -o /dev/null -w "%{http_code}\n" "$API/api/v1/health" >> "$TMPDIR/read_codes.txt" 2>/dev/null &
    fi
    [ $((i % 50)) -eq 0 ] && wait
done
wait

TOTAL_OK=$(grep -c "200" "$TMPDIR/read_codes.txt" 2>/dev/null || echo 0)
TOTAL_401=$(grep -c "401" "$TMPDIR/read_codes.txt" 2>/dev/null || echo 0)
TOTAL_429=$(grep -c "429" "$TMPDIR/read_codes.txt" 2>/dev/null || echo 0)
stat "200 (成功)  " "$TOTAL_OK"
stat "401 (无鉴权)" "$TOTAL_401"
stat "429 (限流)  " "$TOTAL_429"

$CURL -X DELETE "$API/api/v1/sheets/$SHEET_ID" -b "$JAR" > /dev/null 2>&1

# ===== L5: 长时间浸泡 (可选) =====
if [ "$LONG_MODE" -eq 1 ]; then
    title "L5: 30min 稳定性浸泡"
    DURATION=1800  # 30min
    START_T=$(date +%s)
    COUNT=0; FAILS=0
    while [ $(($(date +%s) - START_T)) -lt $DURATION ]; do
        CODE=$(curl -sk -o /dev/null -w "%{http_code}" "$API/api/v1/health" 2>/dev/null)
        [ "$CODE" = "200" ] && COUNT=$((COUNT + 1)) || FAILS=$((FAILS + 1))
        sleep 2
        [ $((COUNT % 50)) -eq 0 ] && [ "$COUNT" -gt 0 ] && \
            echo "  $(date +%H:%M:%S)  $COUNT ok, $FAILS fail"
    done
    stat "30min 总计   " "$COUNT ok, $FAILS fail"
else
    title "L5: 跳过长时间浸泡 (使用 --long 启用)"
fi

echo ""
echo "=========================================="
echo "  Stress Test Complete (write: $WRITE_OK/$((WRITE_OK+WRITE_FAIL)), read: $TOTAL_OK/$((TOTAL_OK+TOTAL_401+TOTAL_429)))"
echo "=========================================="
[ "$WRITE_FAIL" -gt "$((WRITE_OK/2))" ] && exit 1 || exit 0
