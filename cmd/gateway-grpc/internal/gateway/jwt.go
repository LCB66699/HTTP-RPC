package gateway

import (
	"context"
	"fmt"
	"net/http"
	"strconv"
	"strings"

	"github.com/golang-jwt/jwt/v5"
)

// AuthContext holds verified JWT claims for the current request.
type AuthContext struct {
	UserID   int64
	Username string
	Role     string
}

type ctxKey string

const authCtxKey ctxKey = "auth"

var jwtSecret []byte

// SetJWTSecret configures the HMAC key used to verify JWT tokens.
// Must be called at startup before any requests are handled.
func SetJWTSecret(secret string) {
	jwtSecret = []byte(secret)
}

// verifyJWT parses and validates an HS256 JWT token, returning the claims.
// Compatible with the C++ jwt::create / jwt::verify implementation.
func verifyJWT(tokenStr string) (*AuthContext, error) {
	token, err := jwt.Parse(tokenStr, func(token *jwt.Token) (interface{}, error) {
		if _, ok := token.Method.(*jwt.SigningMethodHMAC); !ok {
			return nil, fmt.Errorf("unexpected signing method: %v", token.Header["alg"])
		}
		return jwtSecret, nil
	})
	if err != nil {
		return nil, fmt.Errorf("jwt parse: %w", err)
	}
	claims, ok := token.Claims.(jwt.MapClaims)
	if !ok || !token.Valid {
		return nil, fmt.Errorf("invalid token claims")
	}

	ac := &AuthContext{}
	if uid, ok := claims["uid"].(float64); ok {
		ac.UserID = int64(uid)
	}
	if username, ok := claims["username"].(string); ok {
		ac.Username = username
	}
	if role, ok := claims["role"].(string); ok {
		ac.Role = role
	}
	return ac, nil
}

// publicPaths lists endpoints that do not require authentication.
var publicPaths = map[string]bool{
	"/api/v1/login":          true,
	"/api/v1/register":       true,
	"/api/v1/refresh":        true,
	"/api/v1/auth/otp/send":  true,
	"/api/v1/auth/phone/login": true,
	"/api/v1/health":         true,
	"/api/v1/health/ready":   true,
	"/api/v1/metrics":        true,
}

// isPublicPath returns true if the path does not require authentication.
func isPublicPath(path string) bool {
	if publicPaths[path] {
		return true
	}
	// prefix match for share-by-token: /api/v1/s/{token}
	if strings.HasPrefix(path, "/api/v1/s/") {
		return true
	}
	return false
}

// AuthMiddleware verifies the rpc_at JWT cookie on every request.
// Public endpoints are passed through without authentication.
// On missing or invalid token, the middleware returns 401 immediately.
//
// On successful verification, the middleware overwrites X-Rpc-Uid and
// X-Rpc-Username headers with verified values (defense-in-depth: even if
// Envoy already set them, we re-verify and overwrite).
func AuthMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if isPublicPath(r.URL.Path) {
			next.ServeHTTP(w, r)
			return
		}

		cookie, err := r.Cookie("rpc_at")
		if err != nil || cookie.Value == "" {
			WriteJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{
				"success": false, "error": "authentication required",
			})
			return
		}

		ac, err := verifyJWT(cookie.Value)
		if err != nil {
			WriteJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{
				"success": false, "error": "invalid or expired token",
			})
			return
		}

		r.Header.Set("X-Rpc-Uid", strconv.FormatInt(ac.UserID, 10))
		r.Header.Set("X-Rpc-Username", ac.Username)
		ctx := context.WithValue(r.Context(), authCtxKey, ac)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

// GetAuthContext extracts the verified AuthContext from the request context.
// Returns nil if the request was not authenticated (middleware skipped).
func GetAuthContext(ctx context.Context) *AuthContext {
	ac, _ := ctx.Value(authCtxKey).(*AuthContext)
	return ac
}

// ExtractUID reads the user ID injected by AuthMiddleware from X-Rpc-Uid.
func ExtractUID(r *http.Request) int64 {
	uidStr := r.Header.Get("X-Rpc-Uid")
	if uidStr == "" {
		return 0
	}
	uid, err := strconv.ParseInt(uidStr, 10, 64)
	if err != nil {
		return 0
	}
	return uid
}

// GetUserFromCookie reads the username injected by AuthMiddleware from X-Rpc-Username.
func GetUserFromCookie(r *http.Request) string {
	return r.Header.Get("X-Rpc-Username")
}
