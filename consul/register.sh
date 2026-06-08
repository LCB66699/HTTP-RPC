#!/bin/sh
# Consul 服务注册脚本 — 各微服务启动后调用，将自己注册到 Consul

set -e

SERVICE_NAME="${1:?Usage: $0 <service-name> <port>}"
SERVICE_PORT="${2:-50051}"
SERVICE_ID="${SERVICE_NAME}-$(hostname)"

CONSUL_HOST="${CONSUL_HOST:-consul}"
CONSUL_PORT="${CONSUL_PORT:-8500}"

# 注册服务
curl -s -X PUT "http://${CONSUL_HOST}:${CONSUL_PORT}/v1/agent/service/register" \
  -H 'Content-Type: application/json' \
  -d "{
    \"ID\": \"${SERVICE_ID}\",
    \"Name\": \"${SERVICE_NAME}\",
    \"Address\": \"$(hostname -i 2>/dev/null || hostname)\",
    \"Port\": ${SERVICE_PORT},
    \"Meta\": {
      \"version\": \"1.0\"
    },
    \"Check\": {
      \"Name\": \"gRPC health check\",
      \"GRPC\": \"$(hostname):${SERVICE_PORT}\",
      \"GRPCUseTLS\": false,
      \"Interval\": \"10s\",
      \"Timeout\": \"3s\",
      \"DeregisterCriticalServiceAfter\": \"60s\"
    }
  }"

echo ""
echo "[consul-register] Registered ${SERVICE_NAME} (${SERVICE_ID}) on port ${SERVICE_PORT}"
