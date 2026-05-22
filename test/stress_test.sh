#!/bin/bash
# HTTP-RPC 逐层压测脚本 (v5 — 阶梯加压 + 写入压测 + 稳定性测试)
# 用法: bash stress_test.sh [API_URL] [TOTAL_REQUESTS] [CONCURRENCY]
# 长时间稳定性: STRESS_LONG=1 bash stress_test.sh
set -e

API="${1:-https://localhost}"
INT_GW1="http://gateway-1:8081"   # Docker 内网直连 gateway-1
INT_GW2="http://gateway-2:8081"   # Docker 内网直连 gateway-2
EXT_API="https://$(curl -sk --connect-timeout 3 --max-time 5 https://ipinfo.io/ip 2>/dev/null || echo '127.0.0.1')"
REQ="${2:-5000}"
# 公网 IP 获取失败时跳过 L3（国内网络 ipinfo.io 可能不可达）
SKIP_L3=false
[ "$EXT_API" = "https://127.0.0.1" ] && SKIP_L3=true
CONC="${3:-20}"
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
RPC_TOKEN=$(grep 'rpc_token' "$JAR" 2>/dev/null | awk '{print $NF}' | head -1)
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
    docker exec http-rpc-nginx-1 which ab &>/dev/null 2>&1 || {
        docker exec http-rpc-nginx-1 apt-get update -qq > /dev/null 2>&1
        docker exec http-rpc-nginx-1 apt-get install -y -qq apache2-utils > /dev/null 2>&1
        green "  ab installed in nginx container"
    }

    local ab_args="-n $REQ -c $CONC -k"
    [ -n "$cookie" ] && ab_args="$ab_args -C \"rpc_token=$cookie\""

    docker exec http-rpc-nginx-1 bash -c "ab $ab_args \"$url\"" 2>&1 | tee $TMP.ab_out
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

if docker exec http-rpc-nginx-1 which ab &>/dev/null 2>&1 || { docker exec http-rpc-nginx-1 apt-get update -qq > /dev/null 2>&1 && docker exec http-rpc-nginx-1 apt-get install -y -qq apache2-utils > /dev/null 2>&1; }; then

    PREV_QPS=0
    PREV_P99=0
    KNEE_CONC="N/A"
    KNEE_QPS="N/A"

    for conc in 10 20 50 100 200; do
        local_n=$((conc * 100))
        echo ""
        echo "  --- ${conc}c × 100req/conn = ${local_n} total ---"

        AB_OUT=$(docker exec http-rpc-nginx-1 bash -c "ab -n $local_n -c $conc -k '$INT_GW1/api/health'" 2>&1)
        QPS=$(echo "$AB_OUT" | grep "Requests per second" | awk '{print $4}')
        P99=$(echo "$AB_OUT" | grep "99%" | awk '{print $2}')
        P50=$(echo "$AB_OUT" | grep "50%" | awk '{print $2}')

        printf "  QPS=%-8s  P50=%-8s  P99=%-8s" "$QPS" "${P50:-?}" "${P99:-?}"

        # 拐点检测: P99 突增 > 3x 前一档 或 QPS 不再增长 (<10% 增幅)
        if [ -n "$P99" ] && [ -n "$PREV_P99" ] && [ "$PREV_P99" != "0" ]; then
            P99_RATIO=$(echo "scale=2; $P99 / $PREV_P99" | bc -l 2>/dev/null || echo "1")
            if [ "$(echo "$P99_RATIO > 3.0" | bc -l 2>/dev/null)" = "1" ]; then
                echo -e "  \033[33m← P99 突增 ${P99_RATIO}x，拐点!\033[0m"
                [ "$KNEE_CONC" = "N/A" ] && KNEE_CONC="$conc" && KNEE_QPS="$PREV_QPS"
            else
                echo ""
            fi
        elif [ -n "$QPS" ] && [ -n "$PREV_QPS" ] && [ "$PREV_QPS" != "0" ]; then
            QPS_GROWTH=$(echo "scale=2; ($QPS - $PREV_QPS) / $PREV_QPS" | bc -l 2>/dev/null || echo "0")
            if [ "$(echo "$QPS_GROWTH < 0.05" | bc -l 2>/dev/null)" = "1" ]; then
                echo -e "  \033[33m← QPS 增幅仅 $(echo "scale=0; $QPS_GROWTH*100" | bc)% ，接近饱和\033[0m"
                [ "$KNEE_CONC" = "N/A" ] && KNEE_CONC="$conc" && KNEE_QPS="$QPS"
            else
                echo ""
            fi
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

docker exec http-rpc-nginx-1 curl -sk -o /dev/null -w "%{http_code}" \
    "$INT_GW1/api/health" 2>/dev/null | grep -q 200 \
    && green "  gateway-1 reachable" \
    || red "  gateway-1 unreachable"

run_ab_in_container "gateway-1 健康基线 (无鉴权)" "$INT_GW1/api/health" ""
run_ab_in_container "gateway-1 Sheet List (Cookie+gRPC)" "$INT_GW1/api/sheets" "$RPC_TOKEN"

# ===== L1b: 内网直连 gateway-2 =====
title "L1b: 内网直连 gateway-2:8081 (无 TLS)"

docker exec http-rpc-nginx-1 curl -sk -o /dev/null -w "%{http_code}" \
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
docker exec http-rpc-nginx-1 bash -c "
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
                -H \"Cookie: rpc_token=\$RPC_TOKEN\" \
                -d \"{\\\"name\\\":\\\"stress-write-r\${round}-\${i}\\\",\\\"description\\\":\\\"\\\",\\\"headers_json\\\":\\\"[\\\\\\\"A\\\\\\\"]\\\",\\\"data_json\\\":\\\"[[\\\\\\\"x\\\\\\\"]]\\\"}\")
            SID=\$(echo \"\$RES\" | sed 's/.*\\\"id\\\"://;s/[},].*//')
            if [ -n \"\$SID\" ] && [ \"\$SID\" != \"0\" ]; then
                curl -sk --connect-timeout 5 --max-time 30 -X POST \"\$GW/api/sheets/get\" \
                    -H 'Content-Type: application/json' \
                    -H \"Cookie: rpc_token=\$RPC_TOKEN\" \
                    -d \"{\\\"id\\\":\$SID}\" > /dev/null 2>&1
                curl -sk --connect-timeout 5 --max-time 30 -X PUT \"\$GW/api/sheets\" \
                    -H 'Content-Type: application/json' \
                    -H \"Cookie: rpc_token=\$RPC_TOKEN\" \
                    -d \"{\\\"id\\\":\$SID,\\\"name\\\":\\\"upd\\\",\\\"description\\\":\\\"\\\",\\\"headers_json\\\":\\\"[\\\\\\\"B\\\\\\\"]\\\",\\\"data_json\\\":\\\"[[\\\\\\\"y\\\\\\\"]]\\\"}\" > /dev/null 2>&1
                curl -sk --connect-timeout 5 --max-time 30 -X POST \"\$GW/api/sheets/delete\" \
                    -H 'Content-Type: application/json' \
                    -H \"Cookie: rpc_token=\$RPC_TOKEN\" \
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
        -C "rpc_token=$RPC_TOKEN" \
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

# trap 确保中断也能恢复 gateway-1
trap 'echo "  强制恢复 gateway-1..."; docker unpause http-rpc-gateway-1-1 2>/dev/null || true' EXIT

echo "  gateway-1 已暂停，发送 20 个请求（期望全部成功，走 gateway-2）"
FAIL_COUNT=0
for i in $(seq 1 20); do
    CODE=$($CURL -o /dev/null -w "%{http_code}" "$API/api/health" 2>/dev/null)
    [ "$CODE" != "200" ] && FAIL_COUNT=$((FAIL_COUNT + 1))
done
echo "  失败请求: $FAIL_COUNT / 20"
[ "$FAIL_COUNT" -le 2 ] \
    && green "  故障转移正常（≤2 次失败，容忍 nginx 探测延迟）" \
    || red "  故障转移异常 ($FAIL_COUNT 次失败)"

echo "  恢复 gateway-1..."
docker unpause http-rpc-gateway-1-1 2>/dev/null || true
sleep 2
green "  gateway-1 已恢复"

# 恢复正常清理 trap（覆盖前面的 EXIT trap）
trap cleanup EXIT

# ===== L5: 长时间稳定性测试 (环境变量 STRESS_LONG=1 启用) =====
if [ "${STRESS_LONG:-0}" = "1" ]; then
    title "L5: 长时间稳定性测试 (5min, 100 req/s constant-rate)"

    LONG_DURATION=300  # 5 分钟
    LONG_RATE=100
    LONG_SAMPLE_INTERVAL=30  # 每 30s 采样一次

    if docker exec http-rpc-nginx-1 which ab &>/dev/null 2>&1; then
        green "  目标: ${LONG_RATE} req/s × ${LONG_DURATION}s = $((LONG_RATE * LONG_DURATION)) total req"
        echo ""

        # 使用 ab 带 -t 时间限制，循环运行
        SAMPLES=$((LONG_DURATION / LONG_SAMPLE_INTERVAL))
        echo "  采样间隔: ${LONG_SAMPLE_INTERVAL}s, 共 ${SAMPLES} 次采样"
        echo ""
        printf "  %-8s %10s %10s %10s %10s\n" "Sample" "QPS" "P50(ms)" "P95(ms)" "P99(ms)"

        for s in $(seq 1 $SAMPLES); do
            AB_OUT=$(docker exec http-rpc-nginx-1 bash -c \
                "ab -n $((LONG_RATE * LONG_SAMPLE_INTERVAL)) -c 10 -k -t $LONG_SAMPLE_INTERVAL '$INT_GW1/api/health'" 2>&1)
            QPS=$(echo "$AB_OUT" | grep "Requests per second" | awk '{print $4}')
            P50=$(echo "$AB_OUT" | grep "50%" | awk '{print $2}')
            P95=$(echo "$AB_OUT" | grep "95%" | awk '{print $2}')
            P99=$(echo "$AB_OUT" | grep "99%" | awk '{print $2}')
            FAIL=$(echo "$AB_OUT" | grep "Failed requests:" | awk '{print $3}')

            printf "  %-8s %10s %10s %10s %10s" "$s/$SAMPLES" "${QPS:-?}" "${P50:-?}" "${P95:-?}" "${P99:-?}"
            if [ -n "$FAIL" ] && [ "$FAIL" -gt 0 ]; then
                echo -e "  \033[31mFailed:${FAIL}\033[0m"
            else
                echo ""
            fi

            # 延迟漂移检测: 如果 P99 比第一个采样增长 > 2x，警告可能的资源泄漏
            if [ "$s" -eq 1 ]; then
                FIRST_P99="$P99"
            elif [ -n "$P99" ] && [ -n "$FIRST_P99" ] && [ "$FIRST_P99" != "0" ]; then
                DRIFT=$(echo "scale=2; $P99 / $FIRST_P99" | bc -l 2>/dev/null || echo "1")
                if [ "$(echo "$DRIFT > 2.0" | bc -l 2>/dev/null)" = "1" ]; then
                    echo -e "  \033[33m  ↑ P99 漂移 ${DRIFT}x — 可能存在资源泄漏\033[0m"
                fi
            fi
        done

        green "  稳定性测试完成"
    else
        warn "  ab not available in container — skip stability test"
    fi
else
    title "L5: 稳定性测试 (跳过 — 设置 STRESS_LONG=1 启用)"
    warn "  启用方式: STRESS_LONG=1 bash test/stress_test.sh"
fi

# ===== 总结 =====
title "压测完成"
echo "  L0      阶梯加压        → 自动找到系统拐点"
echo "  L1a/L1b 内网直连单实例  → 网关裸性能基线"
echo "  L1d     内网写入压测    → 创建→获取→更新→删除 吞吐"
echo "  L2      本地 TLS        → 加 nginx + TLS 开销"
echo "  L3      公网全链路      → 加外网延迟"
echo "  L4      故障转移        → 双 gateway 高可用验证"
echo "  L5      稳定性测试      → 5min 恒定负载 (STRESS_LONG=1)"
echo ""
echo "  瓶颈判定:"
echo "  L0  拐点并发         → 系统最大有效并发"
echo "  L1  QPS < 500        → httplib 线程模型瓶颈"
echo "  L1d 写入 < 读取×0.3  → 数据库写瓶颈 (乐观锁/锁竞争)"
echo "  L2  QPS < L1×0.8     → nginx/TLS 开销过大"
echo "  L3  P99 > 200ms      → 外网带宽/延迟"
echo "  L4  失败 > 5         → nginx 健康探测间隔过长"
echo "  L5  P99 漂移 > 2x    → 资源泄漏 (内存/连接)"

rm -rf $TMP.*
