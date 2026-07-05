package middleware

import (
	"fmt"
	"net/http"
	"strings"

	"github.com/gin-gonic/gin"
	"github.com/golang-jwt/jwt/v5"
)

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

func isPublic(path string) bool {
	if publicPaths[path] {
		return true
	}
	if strings.HasPrefix(path, "/api/v1/s/") {
		return true
	}
	return false
}

func verifyJWT(tokenStr string, secret []byte) (int64, string, error) {
	token, err := jwt.Parse(tokenStr, func(t *jwt.Token) (interface{}, error) {
		if _, ok := t.Method.(*jwt.SigningMethodHMAC); !ok {
			return nil, fmt.Errorf("unexpected signing method: %v", t.Header["alg"])
		}
		return secret, nil
	})
	if err != nil {
		return 0, "", fmt.Errorf("jwt parse: %w", err)
	}
	claims, ok := token.Claims.(jwt.MapClaims)
	if !ok || !token.Valid {
		return 0, "", fmt.Errorf("invalid token claims")
	}

	var uid int64
	if v, ok := claims["uid"].(float64); ok {
		uid = int64(v)
	}
	var username string
	if v, ok := claims["username"].(string); ok {
		username = v
	}
	return uid, username, nil
}

// Auth is a Gin middleware that verifies the rpc_at JWT cookie.
// Public endpoints pass through. Missing/invalid token returns 401.
func Auth(jwtSecret string) gin.HandlerFunc {
	secret := []byte(jwtSecret)
	return func(c *gin.Context) {
		if isPublic(c.Request.URL.Path) {
			c.Next()
			return
		}

		token, err := c.Cookie("rpc_at")
		if err != nil || token == "" {
			c.AbortWithStatusJSON(http.StatusUnauthorized, gin.H{
				"success": false, "error": "authentication required",
			})
			return
		}

		uid, username, err := verifyJWT(token, secret)
		if err != nil {
			c.AbortWithStatusJSON(http.StatusUnauthorized, gin.H{
				"success": false, "error": "invalid or expired token",
			})
			return
		}

		c.Set("uid", uid)
		c.Set("username", username)
		c.Next()
	}
}
