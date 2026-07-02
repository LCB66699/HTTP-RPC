package gateway

import (
	"encoding/json"
	"net/http"
	"regexp"

	"github.com/sony/gobreaker/v2"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

// GRPCResponse 是所有 protobuf 响应的公共接口
type GRPCResponse interface {
	GetSuccess() bool
	GetError() string
}

// WriteJSON writes a JSON response with status 200.
func WriteJSON(w http.ResponseWriter, v interface{}) {
	WriteJSONStatus(w, http.StatusOK, v)
}

var largeIntRe = regexp.MustCompile(`:(\d{16,})`)

// WriteJSONStatus writes a JSON response with the given HTTP status code.
// Large integers (>= 10^16) are quoted to preserve precision in JavaScript.
func WriteJSONStatus(w http.ResponseWriter, code int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	data, _ := json.Marshal(v)
	data = largeIntRe.ReplaceAll(data, []byte(`:"$1"`))
	w.Write(data)
}

// WriteGRPCResponse 统一处理 gRPC 成功/失败 → HTTP 响应
func WriteGRPCResponse(w http.ResponseWriter, resp GRPCResponse, err error) {
	if err != nil {
		WriteGRPCError(w, err, "internal error")
		return
	}
	if resp == nil || !resp.GetSuccess() {
		code := int32(0)
		if ec, ok := any(resp).(interface{ GetErrorCode() int32 }); ok {
			code = ec.GetErrorCode()
		}
		WriteError(w, nil, resp.GetError(), code)
		return
	}
	WriteJSON(w, resp)
}

// grpcStatusHTTP maps gRPC status codes to HTTP status codes.
func grpcStatusHTTP(code codes.Code) int {
	switch code {
	case codes.OK:
		return http.StatusOK
	case codes.Canceled:
		return 499
	case codes.Unknown:
		return http.StatusInternalServerError
	case codes.InvalidArgument:
		return http.StatusBadRequest
	case codes.DeadlineExceeded:
		return http.StatusGatewayTimeout
	case codes.NotFound:
		return http.StatusNotFound
	case codes.AlreadyExists:
		return http.StatusConflict
	case codes.PermissionDenied:
		return http.StatusForbidden
	case codes.Unauthenticated:
		return http.StatusUnauthorized
	case codes.ResourceExhausted:
		return http.StatusTooManyRequests
	case codes.FailedPrecondition:
		return http.StatusBadRequest
	case codes.Aborted:
		return http.StatusConflict
	case codes.OutOfRange:
		return http.StatusBadRequest
	case codes.Unimplemented:
		return http.StatusNotImplemented
	case codes.Internal:
		return http.StatusInternalServerError
	case codes.Unavailable:
		return http.StatusServiceUnavailable
	case codes.DataLoss:
		return http.StatusInternalServerError
	default:
		return http.StatusInternalServerError
	}
}

// WriteGRPCError writes a gRPC error as a JSON response.
func WriteGRPCError(w http.ResponseWriter, err error, fallback string) {
	code := http.StatusInternalServerError
	msg := fallback
	if err == gobreaker.ErrOpenState {
		code = http.StatusServiceUnavailable
		WriteJSONStatus(w, code, map[string]interface{}{
			"success":  false,
			"error":    "Service temporarily unavailable (circuit open)",
			"degraded": true,
		})
		return
	} else if st, ok := status.FromError(err); ok {
		code = grpcStatusHTTP(st.Code())
		msg = st.Message()
	}
	WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
}

// WriteError 统一错误响应：优先用 error_code，fallback 到消息文本
func WriteError(w http.ResponseWriter, err error, respErr string, errorCode int32) {
	msg := respErr
	if err != nil {
		msg = err.Error()
	}
	code := ErrorCodeToHTTP(errorCode)
	if code == http.StatusOK {
		WriteGRPCError(w, err, msg)
		return
	}
	WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
}

// ErrorCodeToHTTP 将 C++ 服务返回的 error_code 枚举映射为 HTTP 状态码
func ErrorCodeToHTTP(code int32) int {
	switch code {
	case 1:
		return http.StatusBadRequest
	case 2:
		return http.StatusUnauthorized
	case 3:
		return http.StatusForbidden
	case 4:
		return http.StatusNotFound
	case 5:
		return http.StatusConflict
	case 6:
		return http.StatusInternalServerError
	case 7:
		return http.StatusServiceUnavailable
	default:
		return http.StatusOK
	}
}
