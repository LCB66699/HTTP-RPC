package gateway

import (
	"net/http"
	"strconv"
)

// ExtractUID 从 Envoy 注入的 X-Rpc-Uid 请求头中提取 user_id。
// Envoy 的 jwt_authn filter 已验证 JWT 签名并将 claims 写入此 header。
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

// GetUserFromCookie 从 Envoy 注入的 X-Rpc-Username 请求头中提取 username。
// 注意：函数名保留向后兼容，实际不再解析 cookie。
func GetUserFromCookie(r *http.Request) string {
	return r.Header.Get("X-Rpc-Username")
}
