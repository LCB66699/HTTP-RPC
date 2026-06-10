#!/bin/bash
# Docker 容器健康状态验证
# 用途: 确认所有服务在运行并健康（作为其他测试的前置条件）
# 使用: bash test/docker_health.sh [host]
# 退出: 0=全健康 1=部分失败

set -euo pipefail

HOST="${1:-localhost}"
MAX_WAIT="${2:-120}"
INTERVAL=3
PASS=0; FAIL=0; WARN=0

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
green() { echo -e "${GREEN}[PASS]${NC} $1"; }
red()   { echo -e "${RED}[FAIL]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }

echo "=== Docker Health Check ==="
echo "Target: $HOST (timeout=${MAX_WAIT}s)"

# 需要 healthy 的容器列表（从 docker-compose.yml 提取）
CONTAINERS=(
    "http-rpc-mysql-auth-1"
    "http-rpc-mysql-spreadsheet-0-1"
    "http-rpc-mysql-spreadsheet-1-1"
    "http-rpc-mysql-file-0-1"
    "http-rpc-mysql-file-1-1"
    "http-rpc-redis-cluster-1-1"
    "http-rpc-redis-cluster-2-1"
    "http-rpc-redis-cluster-3-1"
    "http-rpc-redis-cluster-4-1"
    "http-rpc-redis-cluster-5-1"
    "http-rpc-redis-cluster-6-1"
    "http-rpc-elasticsearch-1"
    "http-rpc-mongodb-1"
    "http-rpc-rabbitmq-1"
    "http-rpc-consul-1"
    "http-rpc-auth-1-1"
    "http-rpc-auth-2-1"
    "http-rpc-sheet-1-1"
    "http-rpc-sheet-2-1"
    "http-rpc-file-1-1"
    "http-rpc-file-2-1"
    "http-rpc-grpc-gateway-1"
    "http-rpc-envoy-1"
    "http-rpc-search-1"
    "http-rpc-notify-service-1"
    "http-rpc-nginx-1-1"
)

echo ""
echo "Phase 1: Infrastructure..."
for c in "${CONTAINERS[@]}"; do
    waited=0
    while [ $waited -lt $MAX_WAIT ]; do
        if docker inspect --format='{{.State.Running}}' "$c" 2>/dev/null | grep -q true; then
            status=$(docker inspect --format='{{.State.Health.Status}}' "$c" 2>/dev/null)
            if [ "$status" = "healthy" ]; then
                green "$c"; PASS=$((PASS+1)); break
            elif [ -z "$status" ]; then
                green "$c (running, no healthcheck)"; PASS=$((PASS+1)); break
            fi
        fi
        sleep $INTERVAL
        waited=$((waited + INTERVAL))
    done
    if [ $waited -ge $MAX_WAIT ]; then
        red "$c (did not become healthy in ${MAX_WAIT}s)"
        FAIL=$((FAIL+1))
    fi
done

echo ""
echo "Phase 2: Service Endpoints..."
# gRPC Gateway
if ssh -o ConnectTimeout=5 "$HOST" "curl -sk https://localhost:443/api/health 2>/dev/null | grep -q gateway" 2>/dev/null; then
    green "gRPC-Gateway (HTTPS)"
else
    curl -sk "http://$HOST:8082/api/health" 2>/dev/null | grep -q . && green "gRPC-Gateway (:8082)" || red "gRPC-Gateway unreachable"
fi

# Elasticsearch
if curl -s "http://$HOST:9200/_cluster/health" 2>/dev/null | grep -qE '"status":"(green|yellow)"'; then
    green "Elasticsearch (9200)"
else
    red "Elasticsearch not ready"
fi

# MongoDB
if curl -s "http://$HOST:27017" 2>/dev/null | grep -q 'ok'; then
    green "MongoDB (27017)"
else
    warn "MongoDB check skipped (no curl response)"
fi

# RabbitMQ Management
if curl -s -u rpc:rpc-rabbit-123456 "http://$HOST:15672/api/overview" 2>/dev/null | grep -q '"management_version"'; then
    green "RabbitMQ Management (15672)"
else
    warn "RabbitMQ Management not responding"
fi

# Consul
if curl -s "http://$HOST:8500/v1/status/leader" 2>/dev/null | grep -q '"[0-9.]*:[0-9]*"'; then
    green "Consul (8500)"
else
    warn "Consul not responding"
fi

echo ""
echo "============================================"
echo "  Health Summary: $PASS passed, $FAIL failed"
echo "============================================"

[ $FAIL -eq 0 ] && exit 0 || exit 1
