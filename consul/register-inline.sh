#!/bin/sh
# Consul 自注册 — 在 Dockerfile CMD 之前执行
# 环境变量: CONSUL_SVC_NAME, CONSUL_SVC_HOST, CONSUL_SVC_PORT

if [ -n "$CONSUL_SVC_NAME" ] && [ -n "$CONSUL_SVC_HOST" ] && [ -n "$CONSUL_SVC_PORT" ]; then
  # 等主进程启动
  (sleep 3
   curl -s -X PUT "http://consul:8500/v1/agent/service/register" \
     -H 'Content-Type: application/json' \
     -d "{\"ID\":\"${CONSUL_SVC_HOST}\",\"Name\":\"${CONSUL_SVC_NAME}\",\"Address\":\"${CONSUL_SVC_HOST}\",\"Port\":${CONSUL_SVC_PORT},\"Check\":{\"Name\":\"gRPC ${CONSUL_SVC_HOST}\",\"GRPC\":\"${CONSUL_SVC_HOST}:${CONSUL_SVC_PORT}\",\"GRPCUseTLS\":false,\"Interval\":\"10s\",\"Timeout\":\"3s\",\"DeregisterCriticalServiceAfter\":\"60s\"}}" \
     > /dev/null 2>&1
   echo "[consul] Registered ${CONSUL_SVC_NAME} → ${CONSUL_SVC_HOST}:${CONSUL_SVC_PORT}" &
  ) &
fi

exec "$@"
