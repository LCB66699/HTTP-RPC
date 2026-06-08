#!/bin/sh
# Envoy entrypoint — 注入 JWT Secret 到配置, 生成 JWKS key
set -e

JWT_SECRET="${JWT_SECRET:-default-secret-32bytes-here!!!!!}"
# base64url encode (no padding)
JWT_B64=$(echo -n "$JWT_SECRET" | base64 -w0 | tr '/+' '_-' | tr -d '=\n')

# 替换模板中的占位符 (用 | 作分隔符避免 base64 中的 / 冲突)
sed "s|{{JWT_SECRET_B64}}|${JWT_B64}|g" /etc/envoy/envoy.yaml > /tmp/envoy-final.yaml

exec envoy -c /tmp/envoy-final.yaml --log-level info "$@"
