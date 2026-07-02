#!/bin/sh
# Envoy entrypoint — generates JWKS from JWT_SECRET env var, then starts Envoy.
#
# The JWT_SECRET environment variable must contain the HMAC-SHA256 key used
# to sign JWT tokens. This script converts it to JWKS format so Envoy's
# jwt_authn filter can validate tokens at the edge.

set -e

JWT_SECRET="${JWT_SECRET:-}"
if [ -z "$JWT_SECRET" ]; then
    echo "FATAL: JWT_SECRET environment variable is required" >&2
    exit 1
fi

mkdir -p /etc/envoy/jwt

# Base64-URL encode the secret (no padding) for JWK format
JWK_K=$(echo -n "$JWT_SECRET" | base64 -w0 | tr '+/' '-_' | tr -d '=')

cat > /etc/envoy/jwt/jwks.json <<JWKSEOF
{
  "keys": [
    {
      "kty": "oct",
      "alg": "HS256",
      "k": "${JWK_K}"
    }
  ]
}
JWKSEOF

echo "[entrypoint] JWKS generated at /etc/envoy/jwt/jwks.json"

# Hand off to the upstream envoy entrypoint
exec /usr/local/bin/envoy "$@"
