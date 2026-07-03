package gateway

import (
	"context"
	"fmt"
	"net/http"
	"strconv"

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

// AuthMiddleware is HTTP middleware that verifies the rpc_at JWT cookie and
// injects X-Rpc-Uid / X-Rpc-Username headers for downstream handlers.
//
// If the cookie is missing or the token is invalid, the middleware silently
// passes through — handlers without auth simply see empty headers and
// respond with 401 as they already do.
//
// If a previous layer (e.g. Envoy) already set these headers, the middleware
// overwrites them with the verified value, providing defense-in-depth.
func AuthMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		cookie, err := r.Cookie("rpc_at")
		if err == nil && cookie.Value != "" {
			if ac, err := verifyJWT(cookie.Value); err == nil {
				r.Header.Set("X-Rpc-Uid", strconv.FormatInt(ac.UserID, 10))
				r.Header.Set("X-Rpc-Username", ac.Username)
				ctx := context.WithValue(r.Context(), authCtxKey, ac)
				r = r.WithContext(ctx)
			}
		}
		next.ServeHTTP(w, r)
	})
}

// GetAuthContext extracts the verified AuthContext from the request context.
// Returns nil if the request was not authenticated (middleware skipped).
func GetAuthContext(ctx context.Context) *AuthContext {
	ac, _ := ctx.Value(authCtxKey).(*AuthContext)
	return ac
}
