#!/bin/sh
# Envoy entrypoint — generates JWKS from JWT_SECRET env var, then starts Envoy.
#
# The JWT_SECRET environment variable must contain the HMAC-SHA256 key used
# to sign JWT tokens. This script converts it to JWKS format so Envoy's
# jwt_authn filter can validate tokens at the edge.

set -e

# If JWKS already exists (e.g. mounted as a volume), skip generation.
if [ -f /etc/envoy/jwt/jwks.json ]; then
    echo "[entrypoint] JWKS already exists at /etc/envoy/jwt/jwks.json, skipping generation"
else
    JWT_SECRET="${JWT_SECRET:-default-secret-32bytes-here!!!!!}"
    if [ "$JWT_SECRET" = "default-secret-32bytes-here!!!!!" ]; then
        echo "WARNING: JWT_SECRET not set, using default (insecure). Set JWT_SECRET for production." >&2
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
fi

# Hand off to the upstream envoy entrypoint
exec /usr/local/bin/envoy "$@"
