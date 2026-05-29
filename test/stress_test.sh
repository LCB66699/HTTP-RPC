#!/bin/bash
# HTTP-RPC 逐层压测脚本 (v5 — 阶梯加压 + 写入压测 + 稳定性测试)
# 用法: bash stress_test.sh [API_URL] [TOTAL_REQUESTS] [CONCURRENCY] [--long]
#   --long  全时长稳定性测试 (30min)，不传则默认 60s 快速浸泡
#   STRESS_LONG=0  完全跳过 L5
#   --debug  启用 set -x 逐行调试
set +e

# 先解析 --long 标记，再处理位置参数
LONG_MODE=0
POS_ARGS=()
for arg in "$@"; do
    if [ "$arg" = "--long" ]; then
        LONG_MODE=1
    else
        POS_ARGS+=("$arg")
    fi
done

API="${POS_ARGS[0]:-https://localhost}"
INT_GW1="http://gateway-1:8081"   # Docker 内网直连 gateway-1
INT_GW2="http://gateway-2:8081"   # Docker 内网直连 gateway-2
EXT_API="https://$(curl -sk --connect-timeout 3 --max-time 5 https://ipinfo.io/ip 2>/dev/null || echo '127.0.0.1')"
REQ="${POS_ARGS[1]:-5000}"
# 公网 IP 获取失败时跳过 L3（国内网络 ipinfo.io 可能不可达）
SKIP_L3=false
[ "$EXT_API" = "https://127.0.0.1" ] && SKIP_L3=true
CONC="${POS_ARGS[2]:-20}"
JAR="/tmp/rpc_stress_cookies_$$"

CURL="curl -sk --connect-timeout 5 --max-time 30"

# 自动记录日志到 test/logs/
LOG_DIR="$(cd "$(dirname "$0")" && pwd)/logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/stress_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee -a "$LOG_FILE") 2>&1
echo "Log: $LOG_FILE"
echo ""

green()  { echo -e "\033[32m$1\033[0m"; }
yellow() { echo -e "\033[33m$1\033[0m"; }
red()    { echo -e "\033[31m$1\033[0m"; }
warn()   { echo -e "\033[33m[WARN]\033[0m $1"; }
title()  { echo -e "\n\033[1;36m=== $1 ===\033[0m"; }
stat()   { echo -e "  \033[33m$1\033[0m $2"; }

cleanup() { rm -f "$JAR" /tmp/rpc_stress_$$.*; }
trap cleanup EXIT

# ---- 依赖检查 ----
for dep in bc curl sort awk; do
    command -v $dep &>/dev/null || { echo "Missing: $dep"; exit 1; }
done

# ---- 登录获取 Cookie ----
login_and_get_cookie() {
    local user="$1" pass="$2" jar="$3"
    local body
    body=$(curl -sk -X POST "$API/api/login" \
        -H 'Content-Type: application/json' \
        -c "$jar" \
        -d "{\"username\":\"$user\",\"password\":\"$pass\"}" 2>/dev/null)
    if ! echo "$body" | grep -q '"success":true'; then
        curl -sk -X POST "$API/api/register" -H 'Content-Type: application/json' \
            -d "{\"username\":\"$user\",\"password\":\"$pass\"}" > /dev/null 2>&1
        curl -sk -X POST "$API/api/login" -H 'Content-Type: application/json' \
            -c "$jar" \
            -d "{\"username\":\"$user\",\"password\":\"$pass\"}" > /dev/null 2>&1
    fi
}

login_and_get_cookie "stressor" "stress123" "$JAR"
RPC_TOKEN=$(grep 'rpc_at' "$JAR" 2>/dev/null | awk '{print $NF}' | head -1)
[ -z "$RPC_TOKEN" ] && { red "Cannot get cookie token"; exit 1; }
green "Cookie ready"

# 预创建测试数据
curl -sk -X POST "$API/api/sheets" \
    -H 'Content-Type: application/json' \
    -b "$JAR" \
    -d '{"name":"stress","description":"","headers_json":"[\"A\"]","data_json":"[[\"x\"]]"}' > /dev/null 2>&1 || true
green "Test data ready"

TMP="/tmp/rpc_stress_$$"

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
            p50=int(n*0.50+0.999); if(p50<1)p50=1; if(p50>n)p50=n
            p90=int(n*0.90+0.999); if(p90<1)p90=1; if(p90>n)p90=n
            p95=int(n*0.95+0.999); if(p95<1)p95=1; if(p95>n)p95=n
            p99=int(n*0.99+0.999); if(p99<1)p99=1; if(p99>n)p99=n
            p999=int(n*0.999+0.999); if(p999<1)p999=1; if(p999>n)p999=n
            printf "%f\n%f\n%f\n%f\n%f\n%f\n",a[p50],a[p90],a[p95],a[p99],a[p999],a[n]
        }' "$f"
}

print_latency() {
    local p50="$1" p90="$2" p95="$3" p99="$4" p999="$5" max="$6"
    printf "  %-8s %8s ms\n" "P50:"   "$(echo "scale=1; $p50*1000" | bc -l 2>/dev/null || echo 0)"
    printf "  %-8s %8s ms\n" "P90:"   "$(echo "scale=1; $p90*1000" | bc -l 2>/dev/null || echo 0)"
    printf "  %-8s %8s ms\n" "P95:"   "$(echo "scale=1; $p95*1000" | bc -l 2>/dev/null || echo 0)"
    printf "  %-8s %8s ms\n" "P99:"   "$(echo "scale=1; $p99*1000" | bc -l 2>/dev/null || echo 0)"
    printf "  %-8s %8s ms\n" "P99.9:" "$(echo "scale=1; $p999*1000" | bc -l 2>/dev/null || echo 0)"
    printf "  %-8s %8s ms\n" "Max:"   "$(echo "scale=1; $max*1000" | bc -l 2>/dev/null || echo 0)"
}

# ---- ab 基准（在 nginx 容器内执行，直连 gateway） ----
run_ab_in_container() {
    local name="$1" url="$2" cookie="$3"
    title "$name ($REQ req × ${CONC}c — ab keep-alive)"

    # 安装 ab（一次性）
    docker exec http-rpc-nginx-1-1 which ab &>/dev/null 2>&1 || {
        docker exec http-rpc-nginx-1-1 apt-get update -qq > /dev/null 2>&1
        docker exec http-rpc-nginx-1-1 apt-get install -y -qq apache2-utils > /dev/null 2>&1
        green "  ab installed in nginx container"
    }

    local ab_args="-n $REQ -c $CONC -k"
    [ -n "$cookie" ] && ab_args="$ab_args -C \"rpc_at=$cookie\""

    docker exec http-rpc-nginx-1-1 bash -c "ab $ab_args \"$url\"" 2>&1 | tee $TMP.ab_out
    grep "Requests per second" $TMP.ab_out || true
    grep -E "50%|95%|99%|100%" $TMP.ab_out || true
    local fail
    fail=$(grep "Failed requests:" $TMP.ab_out | awk '{print $3}' || echo "?")
    local non2xx
    non2xx=$(grep "Non-2xx responses:" $TMP.ab_out | awk '{print $3}' || echo "0")
    echo "  Failed: ${fail}  |  Non-2xx: ${non2xx}"
    rm -f $TMP.ab_out
}

# ==================================================================
echo ""
echo "=============================================="
echo "  HTTP-RPC 逐层压测 (v5)"
echo "  并发: ${CONC}c × $((REQ/CONC))req = $REQ total"
echo "  双 Gateway: gateway-1 + gateway-2"
echo "=============================================="

# ===== L0: 阶梯加压 — 找到系统拐点 =====
title "L0: 阶梯加压 (stepwise ramp-up, 找拐点)"

if docker exec http-rpc-nginx-1-1 which ab &>/dev/null 2>&1 || { docker exec http-rpc-nginx-1-1 apt-get update -qq > /dev/null 2>&1 && docker exec http-rpc-nginx-1-1 apt-get install -y -qq apache2-utils > /dev/null 2>&1; }; then

    PREV_QPS=0
    PREV_P99=0
    KNEE_CONC="N/A"
    KNEE_QPS="N/A"

    for conc in 10 20 50 100 200; do
        local_n=$((conc * 100))
        echo ""
        echo "  --- ${conc}c × 100req/conn = ${local_n} total ---"

        AB_OUT=$(docker exec http-rpc-nginx-1-1 bash -c "ab -n $local_n -c $conc -k '$INT_GW1/api/health'" 2>&1)
        QPS=$(echo "$AB_OUT" | grep "Requests per second" | awk '{print $4}')
        P99=$(echo "$AB_OUT" | grep "99%" | awk '{print $2}')
        P50=$(echo "$AB_OUT" | grep "50%" | awk '{print $2}')

        printf "  QPS=%-8s  P50=%-8s  P99=%-8s" "$QPS" "${P50:-?}" "${P99:-?}"

        # 拐点检测: P99 突增 > 2x 或 QPS 不增长 (<5%) 或 QPS 下降
        KNEE_FLAG=""
        if [ -n "$P99" ] && [ -n "$PREV_P99" ] && [ "$PREV_P99" != "0" ]; then
            P99_RATIO=$(echo "scale=2; $P99 / $PREV_P99" | bc -l 2>/dev/null || echo "1")
            if [ "$(echo "$P99_RATIO > 2.0" | bc -l 2>/dev/null)" = "1" ]; then
                KNEE_FLAG="P99突增${P99_RATIO}x"
            fi
        fi
        if [ -n "$QPS" ] && [ -n "$PREV_QPS" ] && [ "$PREV_QPS" != "0" ]; then
            QPS_GROWTH=$(echo "scale=2; ($QPS - $PREV_QPS) / $PREV_QPS" | bc -l 2>/dev/null || echo "0")
            if [ "$(echo "$QPS_GROWTH < 0" | bc -l 2>/dev/null)" = "1" ]; then
                KNEE_FLAG="${KNEE_FLAG:+${KNEE_FLAG}+}QPS下降$(echo "scale=0; $QPS_GROWTH*100" | bc)%"
            elif [ "$(echo "$QPS_GROWTH < 0.05" | bc -l 2>/dev/null)" = "1" ]; then
                KNEE_FLAG="${KNEE_FLAG:+${KNEE_FLAG}+}QPS增幅仅$(echo "scale=0; $QPS_GROWTH*100" | bc)%"
            fi
        fi
        if [ -n "$KNEE_FLAG" ]; then
            echo -e "  \033[33m← 拐点! ${KNEE_FLAG}\033[0m"
            [ "$KNEE_CONC" = "N/A" ] && KNEE_CONC="$conc" && KNEE_QPS="$PREV_QPS"
        else
            echo ""
        fi

        PREV_QPS="$QPS"
        PREV_P99="$P99"
    done

    green "  拐点: 并发=${KNEE_CONC}c, QPS≈${KNEE_QPS} req/s"
else
    warn "  ab not available in container — skip ramp-up"
fi

# ===== L1a: 内网直连 gateway-1 =====
title "L1a: 内网直连 gateway-1:8081 (无 TLS)"

docker exec http-rpc-nginx-1-1 curl -sk -o /dev/null -w "%{http_code}" \
    "$INT_GW1/api/health" 2>/dev/null | grep -q 200 \
    && green "  gateway-1 reachable" \
    || red "  gateway-1 unreachable"

run_ab_in_container "gateway-1 健康基线 (无鉴权)" "$INT_GW1/api/health" ""
run_ab_in_container "gateway-1 Sheet List (Cookie+gRPC)" "$INT_GW1/api/sheets" "$RPC_TOKEN"

# ===== L1b: 内网直连 gateway-2 =====
title "L1b: 内网直连 gateway-2:8081 (无 TLS)"

docker exec http-rpc-nginx-1-1 curl -sk -o /dev/null -w "%{http_code}" \
    "$INT_GW2/api/health" 2>/dev/null | grep -q 200 \
    && green "  gateway-2 reachable" \
    || red "  gateway-2 unreachable"

run_ab_in_container "gateway-2 健康基线 (无鉴权)" "$INT_GW2/api/health" ""
run_ab_in_container "gateway-2 Sheet List (Cookie+gRPC)" "$INT_GW2/api/sheets" "$RPC_TOKEN"

# (L1c removed: nginx upstream LB is covered by L2 via localhost:443)

# ===== L1d: 内网写入压测 (容器内 curl 批量创建→更新→删除) =====
title "L1d: 内网写入压测 (容器内 curl, ${WRITE_CONC:-10}c 并发创建→获取→更新→删除)"

WRITE_CONC=10
WRITE_ROUNDS=5

echo "  ${WRITE_CONC}c 并发 × ${WRITE_ROUNDS} 轮 = $((WRITE_CONC * WRITE_ROUNDS)) 次写入循环"
echo "  每循环: POST create → GET → PUT update → POST delete"

# 脚本注入 nginx 容器执行（内网 DNS 可解析 gateway-1:8081）
docker exec http-rpc-nginx-1-1 bash -c "
WRITE_CONC=$WRITE_CONC
WRITE_ROUNDS=$WRITE_ROUNDS
RPC_TOKEN='$RPC_TOKEN'
GW='$INT_GW1'
TOTAL_OK=0
TOTAL_ERR=0
START_TIME=\$(date +%s%N)

for round in \$(seq 1 \$WRITE_ROUNDS); do
    for i in \$(seq 1 \$WRITE_CONC); do
        (
            RES=\$(curl -sk --connect-timeout 5 --max-time 30 -X POST \"\$GW/api/sheets\" \
                -H 'Content-Type: application/json' \
                -H \"Cookie: rpc_at=\$RPC_TOKEN\" \
                -d \"{\\\"name\\\":\\\"stress-write-r\${round}-\${i}\\\",\\\"description\\\":\\\"\\\",\\\"headers_json\\\":\\\"[\\\\\\\"A\\\\\\\"]\\\",\\\"data_json\\\":\\\"[[\\\\\\\"x\\\\\\\"]]\\\"}\")
            SID=\$(echo \"\$RES\" | sed 's/.*\\\"id\\\"://;s/[},].*//')
            if [ -n \"\$SID\" ] && [ \"\$SID\" != \"0\" ]; then
                curl -sk --connect-timeout 5 --max-time 30 -X POST \"\$GW/api/sheets/get\" \
                    -H 'Content-Type: application/json' \
                    -H \"Cookie: rpc_at=\$RPC_TOKEN\" \
                    -d \"{\\\"id\\\":\$SID}\" > /dev/null 2>&1
                curl -sk --connect-timeout 5 --max-time 30 -X PUT \"\$GW/api/sheets\" \
                    -H 'Content-Type: application/json' \
                    -H \"Cookie: rpc_at=\$RPC_TOKEN\" \
                    -d \"{\\\"id\\\":\$SID,\\\"name\\\":\\\"upd\\\",\\\"description\\\":\\\"\\\",\\\"headers_json\\\":\\\"[\\\\\\\"B\\\\\\\"]\\\",\\\"data_json\\\":\\\"[[\\\\\\\"y\\\\\\\"]]\\\"}\" > /dev/null 2>&1
                curl -sk --connect-timeout 5 --max-time 30 -X POST \"\$GW/api/sheets/delete\" \
                    -H 'Content-Type: application/json' \
                    -H \"Cookie: rpc_at=\$RPC_TOKEN\" \
                    -d \"{\\\"id\\\":\$SID}\" > /dev/null 2>&1
                echo 'ok'
            else
                echo 'err'
            fi
        ) &
    done
    wait
done > /tmp/stress_write_results.txt

END_TIME=\$(date +%s%N)
ELAPSED_MS=\$(( (END_TIME - START_TIME) / 1000000 ))
TOTAL_OK=\$(grep -c 'ok' /tmp/stress_write_results.txt 2>/dev/null || echo 0)
TOTAL_ERR=\$(grep -c 'err' /tmp/stress_write_results.txt 2>/dev/null || echo 0)
WRITE_TOTAL=\$((WRITE_CONC * WRITE_ROUNDS))
WRITE_OPS_SEC=\$(awk -v total=\$WRITE_TOTAL -v ms=\$ELAPSED_MS 'BEGIN { printf \"%.1f\", total * 4000 / ms }')

echo \"OK_COUNT=\$TOTAL_OK\"
echo \"ERR_COUNT=\$TOTAL_ERR\"
echo \"ELAPSED_MS=\$ELAPSED_MS\"
echo \"OPS_SEC=\$WRITE_OPS_SEC\"
rm -f /tmp/stress_write_results.txt
" 2>&1 | tee "$TMP.write_output"

WRITE_OK=$(grep "OK_COUNT=" "$TMP.write_output" | sed 's/OK_COUNT=//' || echo "0")
WRITE_ELAPSED=$(grep "ELAPSED_MS=" "$TMP.write_output" | sed 's/ELAPSED_MS=//' || echo "0")
WRITE_OPS_SEC=$(grep "OPS_SEC=" "$TMP.write_output" | sed 's/OPS_SEC=//' || echo "N/A")

green "  写入成功率: ${WRITE_OK}/$((WRITE_CONC * WRITE_ROUNDS))"
stat "  总耗时: ${WRITE_ELAPSED}ms"
stat "  写入吞吐: ${WRITE_OPS_SEC} ops/s (每 op = 4 次 HTTP 调用)"
rm -f "$TMP.write_output"

# ===== L2: 本地 nginx + TLS =====
title "L2: 本地 nginx TLS (localhost:443 → least_conn → gateway-1/2)"
echo ""
warn "  nginx api_limit (rate=100r/s burst=50) 可能触发 429 — 压测时建议注释 limit_req"

AB=$(command -v ab || echo "")
if [ -n "$AB" ]; then
    echo "  [健康基线]"
    ab -n "$REQ" -c "$CONC" -k "$API/api/health" 2>&1 \
        | grep -E "Requests per second|50%|95%|99%|100%|Failed"

    echo ""
    echo "  [Sheet List — Cookie+gRPC+Redis 完整链路]"
    ab -n "$REQ" -c "$CONC" -k \
        -C "rpc_at=$RPC_TOKEN" \
        "$API/api/sheets" 2>&1 \
        | grep -E "Requests per second|50%|95%|99%|100%|Non-2xx|Failed"
else
    red "  host ab not available — install apache2-utils"
fi

# ===== L3: 公网全链路 =====
title "L3: 公网全链路 ($EXT_API)"
echo ""

if [ "$SKIP_L3" = "true" ]; then
    warn "  无法获取公网 IP，跳过 L3（ipinfo.io 不可达）"
elif [ -n "$AB" ]; then
    ab -n $((REQ/2)) -c "$CONC" -k "$EXT_API/api/health" 2>&1 \
        | grep -E "Requests per second|50%|95%|99%|100%" || true
else
    red "  host ab not available"
fi

# ===== L4: gateway 故障转移验证 =====
title "L4: gateway 故障转移 (停 gateway-1，流量应切到 gateway-2)"
echo ""

echo "  暂停 gateway-1..."
docker pause http-rpc-gateway-1-1 2>/dev/null || true
sleep 2

# 临时 EXIT trap：合并恢复 + cleanup，确保 Ctrl+C 不留残留
trap 'echo "  强制恢复 gateway-1..."; docker unpause http-rpc-gateway-1-1 2>/dev/null || true; cleanup' EXIT

echo "  gateway-1 已暂停，发送 20 个请求（期望全部成功，走 gateway-2）"
FAIL_COUNT=0
set +e
for i in $(seq 1 20); do
    CODE=$($CURL -o /dev/null -w "%{http_code}" "$API/api/health" 2>/dev/null)
    [ "$CODE" != "200" ] && FAIL_COUNT=$((FAIL_COUNT + 1))
done
set -e
echo "  失败请求: $FAIL_COUNT / 20"
if [ "$FAIL_COUNT" -le 2 ]; then
    green "  故障转移正常（≤2 次失败，容忍 nginx 探测延迟）"
else
    red "  故障转移异常 ($FAIL_COUNT 次失败)"
fi

echo "  恢复 gateway-1..."
docker unpause http-rpc-gateway-1-1 2>/dev/null || true
sleep 2
green "  gateway-1 已恢复"

trap cleanup EXIT

# ===== L5: 稳定性测试 (默认 60s 快速浸泡, --long 则 30min 全时长, STRESS_LONG=0 跳过) =====
#   配置:
#     --long                     全时长模式 (默认 30min)
#     STRESS_LONG_DURATION        测试时长 秒 (默认: 快速60s / --long 1800s)
#     STRESS_LONG_READERS         读并发数 (默认 8)
#     STRESS_LONG_WRITERS         写并发数 (默认 2)
#     STRESS_LONG_MONITOR_INTERVAL 采样间隔 秒 (默认 15/30)
#     STRESS_LONG_POOL_SIZE       测试数据池大小 (默认 100)
#     STRESS_LONG=0               完全跳过 L5
if [ "${STRESS_LONG:-1}" != "0" ]; then
    if [ "$LONG_MODE" = "1" ]; then
        LONG_DURATION=${STRESS_LONG_DURATION:-1800}
        MONITOR_INTERVAL=${STRESS_LONG_MONITOR_INTERVAL:-30}
    else
        LONG_DURATION=${STRESS_LONG_DURATION:-60}
        MONITOR_INTERVAL=${STRESS_LONG_MONITOR_INTERVAL:-15}
    fi
    LONG_READERS=${STRESS_LONG_READERS:-8}
    LONG_WRITERS=${STRESS_LONG_WRITERS:-2}
    POOL_SIZE=${STRESS_LONG_POOL_SIZE:-100}

    title "L5: 稳定性测试 ($((LONG_DURATION/60))min$([ "$LONG_MODE" = "1" ] && echo ' 全时长' || echo ' 快速浸泡'), ${LONG_READERS}r + ${LONG_WRITERS}w 混合负载)"

    LONG_TMP="/tmp/rpc_stress_long_$$"
    mkdir -p "$LONG_TMP"
    POOL_FILE="$LONG_TMP/pool_ids"        # 可读的 sheet ID 池
    R_LAT="$LONG_TMP/read_lat"            # 读延迟样本 (ms)
    W_LAT="$LONG_TMP/write_lat"           # 写延迟样本 (ms)
    SYS_LOG="$LONG_TMP/sys_metrics"       # 系统指标采样
    EVENT_LOG="$LONG_TMP/events"          # 异常事件
    RUN_FLAG="$LONG_TMP/.running"
    POOL_LOCK="$LONG_TMP/.pool_lock"

    touch "$RUN_FLAG"
    : > "$R_LAT"; : > "$W_LAT"; : > "$SYS_LOG"; : > "$EVENT_LOG"; : > "$POOL_FILE"

    # ---- 预创建测试数据池（并行，20 并发批次） ----
    green "  预创建 ${POOL_SIZE} 条测试数据（并行）..."
    POOL_BATCH=20
    for i in $(seq 1 $POOL_BATCH $POOL_SIZE); do
        end=$((i + POOL_BATCH - 1))
        [ $end -gt $POOL_SIZE ] && end=$POOL_SIZE
        pids=()
        for j in $(seq $i $end); do
            (
                RES=$(curl -sk --connect-timeout 5 --max-time 10 -X POST "$INT_GW1/api/sheets" \
                    -H 'Content-Type: application/json' \
                    -H "Cookie: rpc_at=$RPC_TOKEN" \
                    -d "{\"name\":\"ltest-${j}\",\"description\":\"\",\"headers_json\":\"[\\\"A\\\"]\",\"data_json\":\"[[\\\"x\\\"]]\"}" 2>/dev/null)
                SID=$(echo "$RES" | sed 's/.*"id"://;s/[},].*//' | grep -oE '[0-9]+' | head -1)
                if [ -n "$SID" ] && [ "$SID" != "0" ]; then
                    echo "$SID" >> "$POOL_FILE"
                fi
            ) &
            pids+=($!)
        done
        wait "${pids[@]}"
        printf "  ...%d/%d\n" $end $POOL_SIZE
    done
    POOL_COUNT=$(wc -l < "$POOL_FILE")
    green "  数据池就绪: ${POOL_COUNT} 条"

    # ---- 读 Worker: 80% List + 20% Get ----
    reader_worker() {
        local tid="$1"
        local lat_file="$2"
        while [ -f "$RUN_FLAG" ]; do
            local op=$(awk "BEGIN { print int(rand()*100) }")
            local t0 t1 elapsed code
            t0=$(date +%s%3N)
            if [ "$op" -lt 80 ]; then
                # List — 分页随机翻页 (/api/sheets?page=N&page_size=20)
                local pg=$(awk "BEGIN { print int(rand()*5) }")
                code=$(curl -sk --connect-timeout 5 --max-time 15 -o /dev/null -w "%{http_code}" \
                    -H "Cookie: rpc_at=$RPC_TOKEN" \
                    "$INT_GW1/api/sheets?page=${pg}&page_size=20" 2>/dev/null)
            else
                # Get — 从池中随机取 ID
                local sid=$(sed -n "$(( RANDOM % POOL_COUNT + 1 ))p" "$POOL_FILE" 2>/dev/null)
                [ -z "$sid" ] && sid=1
                code=$(curl -sk --connect-timeout 5 --max-time 15 -o /dev/null -w "%{http_code}" \
                    -H 'Content-Type: application/json' \
                    -H "Cookie: rpc_at=$RPC_TOKEN" \
                    -d "{\"id\":$sid}" "$INT_GW1/api/sheets/get" 2>/dev/null)
            fi
            t1=$(date +%s%3N)
            elapsed=$((t1 - t0))
            if [ "$code" = "200" ]; then
                echo "$elapsed" >> "$lat_file"
            else
                echo "$elapsed" >> "${lat_file}.err"
                echo "R_ERR $tid $code $elapsed" >> "$EVENT_LOG"
            fi
        done
    }

    # ---- 写 Worker: 50% Create + 40% Update + 10% Delete ----
    writer_worker() {
        local tid="$1"
        local lat_file="$2"
        while [ -f "$RUN_FLAG" ]; do
            local op=$(awk "BEGIN { print int(rand()*100) }")
            local t0 t1 elapsed code
            t0=$(date +%s%3N)
            if [ "$op" -lt 50 ]; then
                # Create
                local name="ltest-w${tid}-$(date +%s%3N)"
                RES=$(curl -sk --connect-timeout 5 --max-time 15 -X POST \
                    -H 'Content-Type: application/json' \
                    -H "Cookie: rpc_at=$RPC_TOKEN" \
                    -d "{\"name\":\"${name}\",\"description\":\"\",\"headers_json\":\"[\\\"A\\\"]\",\"data_json\":\"[[\\\"x\\\"]]\"}" \
                    "$INT_GW1/api/sheets" 2>/dev/null)
                SID=$(echo "$RES" | sed 's/.*"id"://;s/[},].*//' | grep -oE '[0-9]+' | head -1)
                if [ -n "$SID" ] && [ "$SID" != "0" ]; then
                    (
                        flock -x 200
                        echo "$SID" >> "$POOL_FILE"
                    ) 200>"$POOL_LOCK"
                    POOL_COUNT=$(wc -l < "$POOL_FILE")
                    code=200
                else
                    code=500
                fi
            elif [ "$op" -lt 90 ]; then
                # Update — 从池中取 ID
                local sid=$(sed -n "$(( RANDOM % POOL_COUNT + 1 ))p" "$POOL_FILE" 2>/dev/null)
                [ -z "$sid" ] && sid=1
                code=$(curl -sk --connect-timeout 5 --max-time 15 -o /dev/null -w "%{http_code}" \
                    -X PUT -H 'Content-Type: application/json' \
                    -H "Cookie: rpc_at=$RPC_TOKEN" \
                    -d "{\"id\":$sid,\"name\":\"lt-upd\",\"description\":\"\",\"headers_json\":\"[\\\"B\\\"]\",\"data_json\":\"[[\\\"y\\\"]]\"}" \
                    "$INT_GW1/api/sheets" 2>/dev/null)
            else
                # Delete → 立即 Create 替换，保持池大小
                local sid=$(sed -n "$(( RANDOM % POOL_COUNT + 1 ))p" "$POOL_FILE" 2>/dev/null)
                [ -z "$sid" ] && sid=1
                code=$(curl -sk --connect-timeout 5 --max-time 15 -o /dev/null -w "%{http_code}" \
                    -X POST -H 'Content-Type: application/json' \
                    -H "Cookie: rpc_at=$RPC_TOKEN" \
                    -d "{\"id\":$sid}" "$INT_GW1/api/sheets/delete" 2>/dev/null)
                # Replace: create new one
                RES=$(curl -sk --connect-timeout 5 --max-time 15 -X POST \
                    -H 'Content-Type: application/json' \
                    -H "Cookie: rpc_at=$RPC_TOKEN" \
                    -d "{\"name\":\"ltest-rpl-$(date +%s%3N)\",\"description\":\"\",\"headers_json\":\"[\\\"A\\\"]\",\"data_json\":\"[[\\\"x\\\"]]\"}" \
                    "$INT_GW1/api/sheets" 2>/dev/null)
                NSID=$(echo "$RES" | sed 's/.*"id"://;s/[},].*//' | grep -oE '[0-9]+' | head -1)
                if [ -n "$NSID" ] && [ "$NSID" != "0" ]; then
                    (
                        flock -x 200
                        sed -i "\|^${sid}$|d" "$POOL_FILE" 2>/dev/null
                        echo "$NSID" >> "$POOL_FILE"
                    ) 200>"$POOL_LOCK"
                fi
            fi
            t1=$(date +%s%3N)
            elapsed=$((t1 - t0))
            if [ "$code" = "200" ]; then
                echo "$elapsed" >> "$lat_file"
            else
                echo "$elapsed" >> "${lat_file}.err"
                echo "W_ERR $tid $code $elapsed" >> "$EVENT_LOG"
            fi
        done
    }

    # ---- 快速百分位计算 (内存排序，无临时文件) ----
    fast_pct() {
        local f="$1" pct="$2"
        [ ! -f "$f" ] || [ ! -s "$f" ] && { echo "0"; return; }
        sort -n -o "$f" "$f"
        local total=$(wc -l < "$f")
        local idx=$(( total * pct / 100 ))
        [ "$idx" -lt 1 ] && idx=1; [ "$idx" -gt "$total" ] && idx="$total"
        sed -n "${idx}p" "$f"
    }

    # ---- 系统指标采样 ----
    sample_sys() {
        local ts="$1"
        # 错误计数 + 熔断器状态
        local sys=$(curl -sk --connect-timeout 3 --max-time 5 "$INT_GW1/api/system/status" 2>/dev/null)
        local health=$(curl -sk --connect-timeout 3 --max-time 5 "$INT_GW1/api/health" 2>/dev/null)
        local err_total=$(echo "$sys" | grep -oE '"total_errors":[0-9]+' | grep -oE '[0-9]+' || echo "0")
        local breaker_open=$(echo "$health" | grep -oE '"open":true' | wc -l || echo "0")
        local svc_count=$(echo "$health" | grep -oE '"status":"ONLINE"' | wc -l || echo "0")
        # 容器 RSS (从宿主机侧采集更准，这里从容器内读取 /proc)
        local rss_kb=$(docker exec http-rpc-gateway-1-1 cat /proc/self/stat 2>/dev/null | awk '{print $24}' || echo "0")
        local fd_count=$(docker exec http-rpc-gateway-1-1 ls /proc/self/fd 2>/dev/null | wc -l || echo "0")
        echo "${ts} ${err_total} ${breaker_open} ${svc_count} ${rss_kb} ${fd_count}" >> "$SYS_LOG"
    }

    # ---- 启动 ----
    RUN_START=$(date +%s)
    green "  启动: ${LONG_READERS} 读 Worker + ${LONG_WRITERS} 写 Worker"
    green "  时长: $((LONG_DURATION/60))min  |  采样: 每 ${MONITOR_INTERVAL}s  |  数据池: ${POOL_COUNT} 条"
    echo ""

    # 启动读 Workers
    for r in $(seq 1 $LONG_READERS); do
        reader_worker "R${r}" "$R_LAT" &
    done
    # 启动写 Workers
    for w in $(seq 1 $LONG_WRITERS); do
        writer_worker "W${w}" "$W_LAT" &
    done
    ALL_PIDS=$(jobs -p)

    # 启动系统采样器 (后台)
    (
        while [ -f "$RUN_FLAG" ]; do
            NOW=$(date +%s)
            sample_sys "$NOW"
            sleep $MONITOR_INTERVAL
        done
    ) &
    SYS_PID=$!

    # ---- 监控输出 ----
    printf "  %-10s | %-20s | %-20s | %-20s\n" "Time" "Read (QPS/P50/P99)" "Write (QPS/P50/P99)" "System (Err/Open/P99Drift)"
    printf "  -----------|----------------------|----------------------|--------------------\n"

    PREV_R_TOTAL=0; PREV_W_TOTAL=0; PREV_R_ERR=0; PREV_W_ERR=0
    FIRST_R_P99=-1; FIRST_W_P99=-1

    while [ -f "$RUN_FLAG" ]; do
        ELAPSED=$(( $(date +%s) - RUN_START ))
        [ "$ELAPSED" -ge "$LONG_DURATION" ] && break

        sleep $MONITOR_INTERVAL

        # 读统计
        R_TOTAL=$(wc -l < "$R_LAT" 2>/dev/null || echo 0)
        R_ERR=$(wc -l < "${R_LAT}.err" 2>/dev/null || echo 0)
        R_DELTA=$(( R_TOTAL - PREV_R_TOTAL ))
        R_QPS=$(awk -v d="$R_DELTA" -v intv="$MONITOR_INTERVAL" 'BEGIN { printf "%.0f", d / intv }')
        R_P50=$(fast_pct "$R_LAT" 50)
        R_P99=$(fast_pct "$R_LAT" 99)
        R_P999=$(fast_pct "$R_LAT" 99.9)
        PREV_R_TOTAL=$R_TOTAL

        # 写统计
        W_TOTAL=$(wc -l < "$W_LAT" 2>/dev/null || echo 0)
        W_ERR=$(wc -l < "${W_LAT}.err" 2>/dev/null || echo 0)
        W_DELTA=$(( W_TOTAL - PREV_W_TOTAL ))
        W_QPS=$(awk -v d="$W_DELTA" -v intv="$MONITOR_INTERVAL" 'BEGIN { printf "%.0f", d / intv }')
        W_P50=$(fast_pct "$W_LAT" 50)
        W_P99=$(fast_pct "$W_LAT" 99)
        PREV_W_TOTAL=$W_TOTAL

        # 系统指标
        LAST_SYS=$(tail -1 "$SYS_LOG" 2>/dev/null)
        SYS_ERR=$(echo "$LAST_SYS" | awk '{print $2}')
        SYS_BOPEN=$(echo "$LAST_SYS" | awk '{print $3}')
        SYS_RSS=$(echo "$LAST_SYS" | awk '{printf "%.0fM", $5/1024}')
        SYS_FD=$(echo "$LAST_SYS" | awk '{print $6}')

        # P99 漂移检测
        DRIFT_FLAG=""
        if [ "$FIRST_R_P99" = "-1" ] && [ "$R_P99" != "0" ]; then
            FIRST_R_P99=$R_P99; FIRST_W_P99=$W_P99
        elif [ "$FIRST_R_P99" != "0" ] && [ "$R_P99" != "0" ]; then
            R_DRIFT=$(awk -v a="$R_P99" -v b="$FIRST_R_P99" 'BEGIN { printf "%.1f", a/b }')
            W_DRIFT=$(awk -v a="$W_P99" -v b="$FIRST_W_P99" 'BEGIN { if(b>0) printf "%.1f", a/b; else print "0" }')
            if [ "$(echo "$R_DRIFT > 2.0" | bc -l 2>/dev/null)" = "1" ] || [ "$(echo "$W_DRIFT > 2.0" | bc -l 2>/dev/null)" = "1" ]; then
                DRIFT_FLAG=" \033[33m←DRIFT R:${R_DRIFT}x W:${W_DRIFT}x\033[0m"
                echo "$(date +%s) P99_DRIFT R=${R_DRIFT}x W=${W_DRIFT}x" >> "$EVENT_LOG"
            fi
        fi

        TIME_STR=$(printf "%02d:%02d" $((ELAPSED/60)) $((ELAPSED%60)))
        printf "  %-10s | %4s qps  P50=%4sms P99=%4sms | %4s qps  P50=%4sms P99=%4sms | err=%s open=%s rss=%s fd=%s%s\n" \
            "$TIME_STR" "$R_QPS" "$R_P50" "$R_P99" "$W_QPS" "$W_P50" "$W_P99" \
            "${SYS_ERR:-0}" "${SYS_BOPEN:-0}" "${SYS_RSS:-?}" "${SYS_FD:-?}" "$DRIFT_FLAG"

        # 熔断器打开告警
        if [ "${SYS_BOPEN:-0}" -gt 0 ]; then
            echo -e "  \033[31m[ALERT] 熔断器打开! open=${SYS_BOPEN} at ${TIME_STR}\033[0m"
        fi
    done

    # ---- 停止所有 Worker ----
    RUN_END=$(date +%s)
    rm -f "$RUN_FLAG"
    kill $SYS_PID 2>/dev/null || true
    kill $ALL_PIDS 2>/dev/null || true
    wait $ALL_PIDS 2>/dev/null || true

    # ---- 最终报告 ----
    title "L5 稳定性测试报告"
    TOTAL_DUR=$((RUN_END - RUN_START))
    R_TOTAL=$(wc -l < "$R_LAT" 2>/dev/null || echo 0)
    W_TOTAL=$(wc -l < "$W_LAT" 2>/dev/null || echo 0)
    R_ERR=$(wc -l < "${R_LAT}.err" 2>/dev/null || echo 0)
    W_ERR=$(wc -l < "${W_LAT}.err" 2>/dev/null || echo 0)
    TOTAL_REQ=$((R_TOTAL + W_TOTAL))
    TOTAL_ERR=$((R_ERR + W_ERR))
    TOTAL_DEL=$((R_TOTAL + W_TOTAL + R_ERR + W_ERR))

    stat "总时长:"    "$((TOTAL_DUR/60))min $((TOTAL_DUR%60))s"
    stat "总请求:"    "${TOTAL_REQ} (成功) + ${TOTAL_ERR} (失败) = ${TOTAL_DEL} (全部)"
    stat "平均读 QPS:" "$(awk -v t="$R_TOTAL" -v d="$TOTAL_DUR" 'BEGIN { printf "%.0f", t/d }')"
    stat "平均写 QPS:" "$(awk -v t="$W_TOTAL" -v d="$TOTAL_DUR" 'BEGIN { printf "%.0f", t/d }')"
    stat "错误率:"    "$(awk -v e="$TOTAL_ERR" -v t="$TOTAL_DEL" 'BEGIN { printf "%.3f%%", e/t*100 }')"
    echo ""

    echo "  读延迟百分位:"
    print_latency \
        "$(fast_pct "$R_LAT" 50 | awk '{printf "%.6f", $1/1000}')" \
        "$(fast_pct "$R_LAT" 90 | awk '{printf "%.6f", $1/1000}')" \
        "$(fast_pct "$R_LAT" 95 | awk '{printf "%.6f", $1/1000}')" \
        "$(fast_pct "$R_LAT" 99 | awk '{printf "%.6f", $1/1000}')" \
        "$(fast_pct "$R_LAT" 99.9 | awk '{printf "%.6f", $1/1000}')" \
        "$(tail -1 "$R_LAT" | awk '{printf "%.6f", $1/1000}')"
    echo ""

    echo "  写延迟百分位:"
    print_latency \
        "$(fast_pct "$W_LAT" 50 | awk '{printf "%.6f", $1/1000}')" \
        "$(fast_pct "$W_LAT" 90 | awk '{printf "%.6f", $1/1000}')" \
        "$(fast_pct "$W_LAT" 95 | awk '{printf "%.6f", $1/1000}')" \
        "$(fast_pct "$W_LAT" 99 | awk '{printf "%.6f", $1/1000}')" \
        "$(fast_pct "$W_LAT" 99.9 | awk '{printf "%.6f", $1/1000}')" \
        "$(tail -1 "$W_LAT" | awk '{printf "%.6f", $1/1000}')"

    # 系统指标汇总
    if [ -s "$SYS_LOG" ]; then
        echo ""
        echo "  系统指标趋势:"
        SYS_LINES=$(wc -l < "$SYS_LOG")
        FIRST_RSS=$(head -1 "$SYS_LOG" | awk '{printf "%.0f", $5/1024}')
        LAST_RSS=$(tail -1 "$SYS_LOG" | awk '{printf "%.0f", $5/1024}')
        FIRST_FD=$(head -1 "$SYS_LOG" | awk '{print $6}')
        LAST_FD=$(tail -1 "$SYS_LOG" | awk '{print $6}')
        MAX_P99_D=$(grep -c "P99_DRIFT" "$EVENT_LOG" 2>/dev/null || echo 0)
        MAX_ERR=$(awk '{if($2>max)max=$2}END{print max+0}' "$SYS_LOG")
        stat "RSS 变化:"    "${FIRST_RSS}M → ${LAST_RSS}M ($(awk -v a="$FIRST_RSS" -v b="$LAST_RSS" 'BEGIN { printf "%+.0fM", b-a }'))"
        stat "FD 变化:"     "${FIRST_FD} → ${LAST_FD} ($(awk -v a="$FIRST_FD" -v b="$LAST_FD" 'BEGIN { printf "%+d", b-a }'))"
        stat "峰值错误数:"  "${MAX_ERR}"
        stat "P99 漂移次数:" "${MAX_P99_D}"
    fi

    # 异常事件汇总
    if [ -s "$EVENT_LOG" ]; then
        echo ""
        echo "  异常事件 (前 10 条):"
        head -10 "$EVENT_LOG" | while IFS= read -r line; do
            echo "    $line"
        done
        EVT_TOTAL=$(wc -l < "$EVENT_LOG")
        [ "$EVT_TOTAL" -gt 10 ] && echo "    ... 共 ${EVT_TOTAL} 条"
    fi

    # 判定
    echo ""
    if [ "$TOTAL_ERR" -eq 0 ] && [ "$MAX_P99_D" -eq 0 ]; then
        green "  ✓ 稳定性测试通过: 零错误, 零 P99 漂移"
    elif [ "$(echo "$TOTAL_ERR / $TOTAL_DEL * 100" | bc -l 2>/dev/null | cut -d. -f1)" -lt 1 ] && [ "$MAX_P99_D" -le 2 ]; then
        yellow "  ⚠ 稳定性测试基本通过: 错误率 < 1%, P99 漂移 ≤ 2 次"
    else
        red "  ✗ 稳定性测试异常: 错误率 $(awk -v e="$TOTAL_ERR" -v t="$TOTAL_DEL" 'BEGIN { printf "%.2f%%", e/t*100 }'), P99 漂移 ${MAX_P99_D} 次"
    fi

    rm -rf "$LONG_TMP"
else
    title "L5: 稳定性测试 (跳过 — STRESS_LONG=0)"
    warn "  L5 已跳过，去除 STRESS_LONG=0 即可启用"
fi

# ===== 总结 =====
title "压测完成"
echo "  L0      阶梯加压        → 自动找到系统拐点"
echo "  L1a/L1b 内网直连单实例  → 网关裸性能基线"
echo "  L1d     内网写入压测    → 创建→获取→更新→删除 吞吐"
echo "  L2      本地 TLS        → 加 nginx + TLS 开销"
echo "  L3      公网全链路      → 加外网延迟"
echo "  L4      故障转移        → 双 gateway 高可用验证"
echo "  L5      稳定性测试      → 混合读写 + 系统指标漂移检测 (默认 60s, --long 全时长)"
echo ""
echo "  瓶颈判定:"
echo "  L0  拐点并发         → 系统最大有效并发"
echo "  L1  QPS < 500        → httplib 线程模型瓶颈"
echo "  L1d 写入 < 读取×0.3  → 数据库写瓶颈 (乐观锁/锁竞争)"
echo "  L2  QPS < L1×0.8     → nginx/TLS 开销过大"
echo "  L3  P99 > 200ms      → 外网带宽/延迟"
echo "  L4  失败 > 5         → nginx 健康探测间隔过长"
echo "  L5  P99 漂移 > 2x    → 资源泄漏 (内存/FD/连接池)"

rm -rf $TMP.*
