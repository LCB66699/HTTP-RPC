#!/bin/sh
# Consul 自注册 — 在 Dockerfile CMD 之前执行
# 环境变量: CONSUL_SVC_NAME, CONSUL_SVC_HOST, CONSUL_SVC_PORT

if [ -n "$CONSUL_SVC_NAME" ] && [ -n "$CONSUL_SVC_HOST" ] && [ -n "$CONSUL_SVC_PORT" ]; then
  # 地址用 Docker DNS 别名 (rpc-auth/rpc-sheet/rpc-file), Consul 可解析
  ADDR=$(hostname -I 2>/dev/null | awk '{print $1}')
  [ -z "$ADDR" ] && ADDR="$CONSUL_SVC_HOST"
  (sleep 3
   curl -s -X PUT "http://consul:8500/v1/agent/service/register" \
     -H 'Content-Type: application/json' \
     -d "{\"ID\":\"${CONSUL_SVC_HOST}\",\"Name\":\"${CONSUL_SVC_NAME}\",\"Address\":\"${CONSUL_SVC_HOST}\",\"Port\":${CONSUL_SVC_PORT},\"Check\":{\"Name\":\"TCP ${CONSUL_SVC_HOST}\",\"TCP\":\"${CONSUL_SVC_HOST}:${CONSUL_SVC_PORT}\",\"Interval\":\"10s\",\"Timeout\":\"3s\"}}" \
     > /dev/null 2>&1
   echo "[consul] Registered ${CONSUL_SVC_NAME} → ${ADDR}:${CONSUL_SVC_PORT}" &
  ) &
fi

exec "$@"
