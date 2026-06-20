package gateway

import (
	"encoding/base64"
	"log"
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
			log.Printf("[ExtractUID] cookie value has %d parts (expected 3)", len(parts))
			break
		}
		raw, err := base64.RawURLEncoding.DecodeString(parts[1])
		if err != nil {
			log.Printf("[ExtractUID] base64 decode error: %v", err)
			break
		}
		re := regexp.MustCompile(`"uid":("?)(\d+)("?)`)
		m := re.FindStringSubmatch(string(raw))
		if m != nil {
			n, _ := strconv.ParseInt(m[2], 10, 64)
			return n
		}
		log.Printf("[ExtractUID] uid not found in payload: %s", string(raw))
		return 0
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
