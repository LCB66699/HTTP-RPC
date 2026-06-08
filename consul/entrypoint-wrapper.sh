#!/bin/sh
# 服务启动包装器 — 后台启动服务 + 注册到 Consul
# 用法: entrypoint-wrapper.sh <service-name> <docker-service-name> <port> <binary> [args...]

SERVICE="$1"       # e.g. auth-service
SVC_HOST="$2"      # e.g. auth-1 (Docker Compose service name, DNS resolvable)
PORT="$3"
shift 3

# 后台启动实际服务
"$@" &
PID=$!

# 等待服务端口就绪
sleep 2

# 注册到 Consul (使用 Docker 服务名作为地址, Consul 容器可解析)
curl -s -X PUT "http://consul:8500/v1/agent/service/register" \
  -H 'Content-Type: application/json' \
  -d "{
    \"ID\": \"${SVC_HOST}\",
    \"Name\": \"${SERVICE}\",
    \"Address\": \"${SVC_HOST}\",
    \"Port\": ${PORT},
    \"Check\": {
      \"Name\": \"gRPC ${SVC_HOST}\",
      \"GRPC\": \"${SVC_HOST}:${PORT}\",
      \"GRPCUseTLS\": false,
      \"Interval\": \"10s\",
      \"Timeout\": \"3s\",
      \"DeregisterCriticalServiceAfter\": \"60s\"
    }
  }" > /dev/null 2>&1

echo "[consul] Registered ${SERVICE} → ${SVC_HOST}:${PORT}"

# 等待主进程
wait $PID
