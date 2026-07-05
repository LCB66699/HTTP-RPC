#!/bin/bash
# Service registration wrapper for Consul.
# Usage: register.sh <service-name> <port> -- <binary> [args...]
# Registers the service on startup, deregisters on SIGTERM/SIGINT.

set -e

SERVICE="$1"
PORT="$2"
shift 2

BINARY=""
BINARY_ARGS=()
while [ $# -gt 0 ]; do
  if [ "$1" = "--" ]; then
    shift
    BINARY="$1"
    shift
    BINARY_ARGS=("$@")
    break
  fi
  shift
done

CONSUL="${CONSUL_HTTP_ADDR:-http://consul:8500}"
NODE_ID="${SERVICE}-${HOSTNAME:-$(hostname)}"

MY_IP=$(hostname -i 2>/dev/null | awk '{print $1}')
if [ -z "$MY_IP" ] || [ "$MY_IP" = "127.0.0.1" ]; then
  MY_IP=$(ip -o -4 addr show eth0 2>/dev/null | awk '{print $4}' | cut -d/ -f1)
fi
if [ -z "$MY_IP" ]; then
  MY_IP="127.0.0.1"
  echo "[consul] WARNING: could not determine routable IP, using ${MY_IP}"
fi

REGISTER_PAYLOAD=$(cat <<EOF
{
  "ID": "${NODE_ID}",
  "Name": "${SERVICE}",
  "Address": "${MY_IP}",
  "Port": ${PORT},
  "Check": {
    "Name": "${SERVICE} gRPC health",
    "GRPC": "${MY_IP}:${PORT}",
    "GRPCUseTLS": false,
    "Interval": "10s",
    "Timeout": "3s",
    "DeregisterCriticalServiceAfter": "90s"
  }
}
EOF
)

curl -s -X PUT "${CONSUL}/v1/agent/service/register" -d "${REGISTER_PAYLOAD}" > /dev/null
echo "[consul] registered ${NODE_ID} at ${MY_IP}:${PORT}"

cleanup() {
  echo "[consul] deregistering ${NODE_ID}"
  curl -s -X PUT "${CONSUL}/v1/agent/service/deregister/${NODE_ID}" > /dev/null 2>&1 || true
  exit 0
}
trap cleanup SIGTERM SIGINT

if [ -n "$BINARY" ]; then
  exec "$BINARY" "${BINARY_ARGS[@]}"
else
  wait
fi
