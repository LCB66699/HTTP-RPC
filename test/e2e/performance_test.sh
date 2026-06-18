#!/bin/bash
# HTTP-RPC 性能测试脚本 (v5 — 百分位统计 + 预热 + wrk2)
# curl + xargs -P 测并发延迟分布，wrk2 测恒定速率 QPS。
# 用法: bash performance_test.sh [API_URL] [CONCURRENT]
set -e

API="${1:-https://localhost}"
CONC="${2:-10}"
CURL="curl -sk --connect-timeout 10 --max-time 30"
JAR="/tmp/rpc_perf_cookies_$$"

# 自动记录日志到 test/logs/
LOG_DIR="$(cd "$(dirname "$0")" && pwd)/logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/performance_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee -a "$LOG_FILE") 2>&1
echo "Log: $LOG_FILE"
echo ""
PASS=0; FAIL=0

green() { echo -e "\033[32m[OK]\033[0m $1"; }
red()   { echo -e "\033[31m[ERR]\033[0m $1"; FAIL=$((FAIL + 1)); }
warn()  { echo -e "\033[33m[WARN]\033[0m $1"; }
title() { echo -e "\n\033[1;36m=== $1 ===\033[0m"; }
stat()  { echo -e "  \033[33m$1\033[0m $2"; }

TMPDIR="/tmp/rpc_perf_$$"
mkdir -p "$TMPDIR"
cleanup() { rm -rf "$JAR" "$TMPDIR" /tmp/perf_large.dat; }
trap cleanup EXIT

# ---- 百分位计算 ----
calc_pct() {
    local f="$1"
    [ ! -f "$f" ] && { echo -e "0\n0\n0\n0\n0\n0"; return; }
    local total=$(wc -l < "$f")
    [ "$total" -le 0 ] && { echo -e "0\n0\n0\n0\n0\n0"; return; }
    sort -n -o "$f" "$f"
    awk -v n="$total" '
        { a[NR]=$1 }
        END {
            p50 =int(n*0.50+0.999); if(p50 <1) p50 =1; if(p50 >n) p50 =n
            p90 =int(n*0.90+0.999); if(p90 <1) p90 =1; if(p90 >n) p90 =n
            p95 =int(n*0.95+0.999); if(p95 <1) p95 =1; if(p95 >n) p95 =n
            p99 =int(n*0.99+0.999); if(p99 <1) p99 =1; if(p99 >n) p99 =n
            p999=int(n*0.999+0.999);if(p999<1) p999=1;if(p999>n) p999=n
            printf "%f\n%f\n%f\n%f\n%f\n%f\n",a[p50],a[p90],a[p95],a[p99],a[p999],a[n]
        }' "$f"
}

# ---- 前置：登录获取 Cookie ----
login_and_get_cookie() {
    local user="$1" pass="$2" jar="$3"
    local body
    body=$(curl -sk -X POST "$API/api/login" \
        -H 'Content-Type: application/json' \
        -c "$jar" \
        -d "{\"username\":\"$user\",\"password\":\"$pass\"}" 2>/dev/null)
    if echo "$body" | grep -q '"success"'; then
        return 0
    fi
    curl -sk -X POST "$API/api/register" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$user\",\"password\":\"$pass\"}" > /dev/null 2>&1
    curl -sk -X POST "$API/api/login" \
        -H 'Content-Type: application/json' \
        -c "$jar" \
        -d "{\"username\":\"$user\",\"password\":\"$pass\"}" > /dev/null 2>&1
}

get_token_from_jar() {
    grep 'rpc_at' "$1" 2>/dev/null | awk '{print $NF}' | head -1
}

# ---- Phase 0: 预热 ----
title "0. 预热 (Warmup — 不计入结果)"

login_and_get_cookie "perf_tester" "perf1234" "$JAR"
RPC_TOKEN=$(get_token_from_jar "$JAR")
[ -z "$RPC_TOKEN" ] && { red "Cannot get cookie token"; exit 1; }
green "Cookie ready"

# 预热 gRPC 连接池 + Redis 连接池
echo -n "  预热 gRPC/Redis 连接池..."
for i in $(seq 1 50); do
    $CURL -o /dev/null "$API/api/health" 2>/dev/null
done
echo " done"

# 预创建一张表，并预热 sheet 查询
$CURL -o /dev/null -X POST "$API/api/sheets" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d '{"name":"perf-test","description":"","headers_json":"[]","data_json":"[]"}' 2>/dev/null || true

for i in $(seq 1 10); do
    $CURL -o /dev/null "$API/api/sheets" -b "$JAR" 2>/dev/null
done
green "Warmup complete"

# =====================================================================
# Phase 1: 单请求 RTT
# =====================================================================
title "1. 单请求延迟 (RTT)"

LOGIN_T=$($CURL -o /dev/null -w "%{time_total}" -X POST "$API/api/login" \
    -H 'Content-Type: application/json' \
    -d '{"username":"perf_tester","password":"perf1234"}')
stat "Login               " "${LOGIN_T}s"

NOAUTH_T=$($CURL -o /dev/null -w "%{time_total}" "$API/api/sheets")
stat "No Cookie (401)     " "${NOAUTH_T}s"

LIST1_T=$($CURL -o /dev/null -w "%{time_total}" "$API/api/sheets" -b "$JAR")
stat "Sheet List (cached) " "${LIST1_T}s"

LIST2_T=$($CURL -o /dev/null -w "%{time_total}" "$API/api/sheets" -b "$JAR")
stat "Sheet List (cached) " "${LIST2_T}s"

HEALTH_T=$($CURL -o /dev/null -w "%{time_total}" "$API/api/health")
stat "Health Check        " "${HEALTH_T}s"

# =====================================================================
# Phase 2: 并发 + 百分位统计
# =====================================================================
title "2. 并发 ($CONC c, xargs -P 精确同步)"

run_concurrent() {
    local name="$1" url="$2" method="${3:-GET}" body="$4" need_auth="${5:-yes}"
    local times_f="$TMPDIR/times_${name// /_}"
    local results_f="$TMPDIR/results_${name// /_}"

    > "$times_f"
    > "$results_f"

    # 使用 xargs -P 同步启动，避免 shell fork 先后导致的测量偏差
    local runner="$TMPDIR/runner_${name// /_}.sh"
    if [ "$method" = "POST" ]; then
        cat > "$runner" << RUNNER_EOF
t=\$($CURL -o /dev/null -w "%{time_total}" -X POST "$url" \
    -H 'Content-Type: application/json' -b "$JAR" -d '$body' 2>/dev/null)
echo "\$t" >> "$times_f"
RUNNER_EOF
    elif [ "$need_auth" = "yes" ]; then
        cat > "$runner" << RUNNER_EOF
t=\$($CURL -o /dev/null -w "%{time_total}" "$url" -b "$JAR" 2>/dev/null)
echo "\$t" >> "$times_f"
RUNNER_EOF
    else
        cat > "$runner" << RUNNER_EOF
t=\$($CURL -o /dev/null -w "%{time_total}" "$url" 2>/dev/null)
echo "\$t" >> "$times_f"
RUNNER_EOF
    fi
    chmod +x "$runner"

    seq 1 "$CONC" | xargs -P "$CONC" -I {} bash "$runner"

    # 计算百分位
    local pct_data
    pct_data=$(calc_pct "$times_f")
    local p50_ms p90_ms p95_ms p99_ms p999_ms max_ms
    p50_ms=$(echo "$pct_data" | sed -n '1p')
    p90_ms=$(echo "$pct_data" | sed -n '2p')
    p95_ms=$(echo "$pct_data" | sed -n '3p')
    p99_ms=$(echo "$pct_data" | sed -n '4p')
    p999_ms=$(echo "$pct_data" | sed -n '5p')
    max_ms=$(echo "$pct_data" | sed -n '6p')

    p50_ms=$(echo "scale=1; $p50_ms*1000" | bc -l 2>/dev/null || echo "0")
    p95_ms=$(echo "scale=1; $p95_ms*1000" | bc -l 2>/dev/null || echo "0")
    p99_ms=$(echo "scale=1; $p99_ms*1000" | bc -l 2>/dev/null || echo "0")
    max_ms=$(echo "scale=1; $max_ms*1000" | bc -l 2>/dev/null || echo "0")

    local ok
    ok=$(grep -c "ok" "$results_f" 2>/dev/null || echo 0)

    printf "  \033[33m%-25s\033[0m P50=%sms  P95=%sms  P99=%sms  max=%sms\n" \
        "$name" "$p50_ms" "$p95_ms" "$p99_ms" "$max_ms"
}

run_concurrent "Health (no auth)"  "$API/api/health"   "GET"  ""  "no"
run_concurrent "Sheet List"        "$API/api/sheets"   "GET"  ""  "yes"
run_concurrent "Sheet Get"         "$API/api/sheets/1" "GET" "" "yes"

# =====================================================================
# Phase 3: 缓存命中率
# =====================================================================
title "3. Cache-Aside 命中率"

FIRST_ID=$($CURL "$API/api/sheets" -b "$JAR" 2>/dev/null \
    | sed 's/.*"id"://;s/[},].*//')
[ -z "$FIRST_ID" ] && FIRST_ID=1

HIT=0; MIS=0; FIRST_WRITE=0
for i in $(seq 1 20); do
    RES=$($CURL "$API/api/sheets/$FIRST_ID" -b "$JAR" 2>/dev/null)
    if echo "$RES" | grep -q '"cache_source":"redis"'; then
        HIT=$((HIT + 1))
    elif echo "$RES" | grep -q '"cache_source":"mysql"'; then
        MIS=$((MIS + 1))
    else
        FIRST_WRITE=$((FIRST_WRITE + 1))
    fi
done
stat "Redis 命中   " "$HIT/20"
stat "MySQL 回源   " "$MIS/20"
[ "$FIRST_WRITE" -gt 0 ] && stat "首次写入     " "$FIRST_WRITE/20"

# =====================================================================
# Phase 4: 大文件上传
# =====================================================================
title "4. 1MB 文件上传"

dd if=/dev/urandom of=/tmp/perf_large.dat bs=1024 count=1024 2>/dev/null
UL_T=$($CURL -o /dev/null -w "%{time_total}" -X POST "$API/api/files/upload" \
    -b "$JAR" \
    -F "file=@/tmp/perf_large.dat")
stat "Upload 1MB          " "${UL_T}s"

# =====================================================================
# Phase 5: 负载测试 (curl 冷连接)
# =====================================================================
title "5. 负载测试 (300 req, 10c, curl cold-connect)"

START=$(date +%s%N)
seq 1 10 | xargs -P 10 -I {} bash -c "
    for j in \$(seq 1 30); do
        $CURL -o /dev/null '$API/api/health' 2>/dev/null
    done
"
END=$(date +%s%N)
ELAPSED_MS=$(( (END - START) / 1000000 ))
REQ_SEC=$(echo "scale=1; 300000 / $ELAPSED_MS" | bc 2>/dev/null || echo "N/A")
stat "总耗时          " "${ELAPSED_MS}ms"
stat "吞吐量 (curl)   " "${REQ_SEC} req/s (cold-connect; ab keep-alive 会更高)"

# =====================================================================
# Phase 6: 写入压测 (创建+获取+更新+删除 循环)
# =====================================================================
title "6. 写入压测 (10x 创建→获取→更新→删除 循环)"

> "$TMPDIR/write_times"
> "$TMPDIR/write_ops"

START=$(date +%s%N)
for i in $(seq 1 10); do
    OP_START=$(date +%s%N)

    # 创建
    CR=$($CURL -X POST "$API/api/sheets" \
        -H 'Content-Type: application/json' \
        -b "$JAR" \
        -d "{\"name\":\"perf-$i\",\"description\":\"\",\"headers_json\":\"[\\\"A\\\"]\",\"data_json\":\"[[\\\"v\\\"]]\"}")
    SID=$(echo "$CR" | sed 's/.*"id"://;s/[},].*//')

    if [ -n "$SID" ] && [ "$SID" != "0" ]; then
        # 获取
        $CURL "$API/api/sheets/$SID" -b "$JAR" > /dev/null 2>&1

        # 更新
        $CURL -X PUT "$API/api/sheets/$SID" \
            -H 'Content-Type: application/json' \
            -b "$JAR" \
            -d "{\"name\":\"perf-$i-updated\",\"description\":\"\",\"headers_json\":\"[\\\"B\\\"]\",\"data_json\":\"[[\\\"w\\\"]]\"}" > /dev/null 2>&1

        # 删除
        $CURL -X DELETE "$API/api/sheets/$SID" -b "$JAR" > /dev/null 2>&1

        echo "ok" >> "$TMPDIR/write_ops"
    else
        echo "err" >> "$TMPDIR/write_ops"
    fi

    OP_END=$(date +%s%N)
    OP_MS=$(( (OP_END - OP_START) / 1000000 ))
    echo "$OP_MS" >> "$TMPDIR/write_times"
done
END=$(date +%s%N)

WR_MS=$(( (END - START) / 1000000 ))
WR_OK=$(grep -c "ok" "$TMPDIR/write_ops" 2>/dev/null || echo 0)

# 写入百分位
WR_PCT=$(calc_pct "$TMPDIR/write_times")
WR_P50=$(echo "$WR_PCT" | sed -n '1p')
WR_P95=$(echo "$WR_PCT" | sed -n '3p')
WR_P99=$(echo "$WR_PCT" | sed -n '4p')
WR_P50_MS=$(echo "scale=1; $WR_P50*1000" | bc -l 2>/dev/null || echo "0")
WR_P95_MS=$(echo "scale=1; $WR_P95*1000" | bc -l 2>/dev/null || echo "0")
WR_P99_MS=$(echo "scale=1; $WR_P99*1000" | bc -l 2>/dev/null || echo "0")

stat "40 ops 总耗时       " "${WR_MS}ms (avg $(( WR_MS / 40 ))ms/op)"
stat "写操作成功率       " "${WR_OK}/10"
stat "写操作 P50/P95/P99 " "P50=${WR_P50_MS}ms  P95=${WR_P95_MS}ms  P99=${WR_P99_MS}ms"

# =====================================================================
# Phase 7: ab 权威 QPS (keep-alive)
# =====================================================================
title "7. ab 权威 QPS (keep-alive, 1000 req x 10c)"

if command -v ab &>/dev/null; then
    echo "  [健康检查基线 — 纯网关吞吐]"
    ab -n 1000 -c 10 -k "$API/api/health" 2>&1 \
        | grep -E "Requests per second|50%|95%|99%|100%|Failed requests" || true

    echo ""
    echo "  [Sheet List — 鉴权+gRPC+Redis 完整链路]"
    ab -n 1000 -c 10 -k \
        -C "rpc_at=$RPC_TOKEN" \
        "$API/api/sheets" 2>&1 \
        | grep -E "Requests per second|50%|95%|99%|100%|Non-2xx|Failed requests" || true
else
    warn "ab not installed — install apache2-utils for QPS benchmark"
fi

# =====================================================================
# Phase 8: wrk2 恒定速率压测 (可选)
# =====================================================================
WRK2=$(command -v wrk2 2>/dev/null || command -v wrk 2>/dev/null || echo "")

if [ -n "$WRK2" ]; then
    title "8. wrk2 恒定速率压测 (constant-rate, 30s, 10c, 200req/s)"

    HAS_WRK2=0
    $WRK2 --help 2>&1 | grep -q '\-R' && HAS_WRK2=1

    if [ "$HAS_WRK2" -eq 1 ]; then
        echo "  [健康检查基线 — wrk2 constant-rate 200r/s]"
        $WRK2 -t4 -c10 -d30s -R200 --latency "$API/api/health" 2>&1 \
            | grep -E "Requests/sec|Latency|50%|90%|99%" || true

        if [ -f "test/wrk_scripts/health.lua" ]; then
            echo ""
            echo "  [wrk2 + Lua 脚本 (详细延迟分布)]"
            $WRK2 -t4 -c10 -d30s -R200 --latency \
                -s test/wrk_scripts/health.lua \
                "$API/api/health" 2>&1
        fi
    else
        echo "  [wrk (非 constant-rate) 30s, 10c]"
        $WRK2 -t4 -c10 -d30s --latency "$API/api/health" 2>&1 \
            | grep -E "Requests/sec|Latency|50%|90%|99%" || true
    fi

    # 混合负载 (需要 token)
    if [ -f "test/wrk_scripts/mixed.lua" ] && [ -n "$RPC_TOKEN" ]; then
        echo ""
        echo "  [混合负载 70%list/20%get/10%create — 需要 wrk2]"
        if [ "$HAS_WRK2" -eq 1 ]; then
            RPC_TOKEN="$RPC_TOKEN" $WRK2 -t4 -c10 -d30s -R100 --latency \
                -s test/wrk_scripts/mixed.lua \
                "$API/api/sheets" 2>&1
        else
            RPC_TOKEN="$RPC_TOKEN" $WRK2 -t4 -c10 -d30s --latency \
                -s test/wrk_scripts/mixed.lua \
                "$API/api/sheets" 2>&1
        fi
    fi
else
    title "8. wrk2 (未安装 — 跳过)"
    warn "  wrk2 not found. Install:"
    warn "    sudo apt install -y wrk"
    warn "    # or for wrk2 (constant-rate):"
    warn "    git clone https://github.com/giltene/wrk2 && cd wrk2 && make"
fi

echo ""
echo "=========================================="
echo "  Performance Test Complete ($PASS pass, $FAIL fail)"
echo "=========================================="
[ "$FAIL" -eq 0 ] || exit 1
