package gateway

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"net/http"
)

const requestIDKey ctxKey = "request_id"

// RequestID is HTTP middleware that ensures every request carries an
// X-Request-ID. If the client sends one it is reused; otherwise a new
// 16-char hex ID is generated. The ID is set on the response header and
// stored in the request context for downstream use.
func RequestID(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		rid := r.Header.Get("X-Request-ID")
		if rid == "" {
			b := make([]byte, 8)
			rand.Read(b)
			rid = hex.EncodeToString(b)
		}
		w.Header().Set("X-Request-ID", rid)
		ctx := context.WithValue(r.Context(), requestIDKey, rid)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

// GetRequestID extracts the request ID from the context.
func GetRequestID(ctx context.Context) string {
	if v, ok := ctx.Value(requestIDKey).(string); ok {
		return v
	}
	return ""
}
