package gateway

import (
	"encoding/base64"
	"net/http"
	"regexp"
	"strconv"
	"strings"

	"github.com/golang-jwt/jwt/v5"
)

// ExtractUID 从 JWT cookie (rpc_at) 中提取 user_id
func ExtractUID(r *http.Request) int64 {
	for _, c := range r.Cookies() {
		if c.Name != "rpc_at" {
			continue
		}
		parts := strings.SplitN(c.Value, ".", 3)
		if len(parts) != 3 {
			break
		}
		raw, err := base64.RawURLEncoding.DecodeString(parts[1])
		if err != nil {
			break
		}
		re := regexp.MustCompile(`"uid":("?)(\d+)("?)`)
		m := re.FindStringSubmatch(string(raw))
		if m != nil {
			n, _ := strconv.ParseInt(m[2], 10, 64)
			return n
		}
	}
	return 0
}

// GetUserFromCookie 从 JWT cookie 中提取 username
func GetUserFromCookie(r *http.Request) string {
	for _, c := range r.Cookies() {
		if c.Name == "rpc_at" {
			claims := jwt.MapClaims{}
			jwt.ParseWithClaims(c.Value, &claims, func(t *jwt.Token) (interface{}, error) { return []byte(""), nil })
			if u, ok := claims["username"].(string); ok {
				return u
			}
		}
	}
	return ""
}
