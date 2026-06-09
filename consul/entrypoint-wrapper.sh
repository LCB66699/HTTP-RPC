#!/bin/sh
# 服务启动包装器 — 后台启动服务 + 注册到 Consul
# 用法: entrypoint-wrapper.sh <service-name> <docker-service-name> <port> <binary> [args...]

SERVICE="$1"
SVC_HOST="$2"
PORT="$3"
shift 3

"$@" &
PID=$!

sleep 3

# TCP 端口检查（C++ 服务不实现标准 gRPC health 协议）
curl -s -X PUT "http://consul:8500/v1/agent/service/register" \
  -H 'Content-Type: application/json' \
  -d "{
    \"ID\": \"${SVC_HOST}\",
    \"Name\": \"${SERVICE}\",
    \"Address\": \"${SVC_HOST}\",
    \"Port\": ${PORT},
    \"Check\": {
      \"Name\": \"TCP ${SVC_HOST}\",
      \"TCP\": \"${SVC_HOST}:${PORT}\",
      \"Interval\": \"10s\",
      \"Timeout\": \"3s\"
    }
  }" > /dev/null 2>&1

echo "[consul] Registered ${SERVICE} → ${SVC_HOST}:${PORT}"

wait $PID
