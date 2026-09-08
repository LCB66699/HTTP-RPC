package handler

import (
	"net/http"

	"github.com/gin-gonic/gin"
	"github.com/sony/gobreaker/v2"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/encoding/protojson"
	"google.golang.org/protobuf/proto"
)

var responseJSON = protojson.MarshalOptions{UseProtoNames: true}

// writeProtoJSON preserves protobuf's canonical JSON mapping, including int64
// values encoded as strings so browser clients cannot lose precision.
func writeProtoJSON(c *gin.Context, statusCode int, message proto.Message) {
	payload, err := responseJSON.Marshal(message)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"success": false, "error": "response serialization failed"})
		return
	}
	c.Data(statusCode, "application/json; charset=utf-8", payload)
}

// grpcCodeToHTTP maps gRPC status codes to HTTP status codes.
func grpcCodeToHTTP(code codes.Code) int {
	switch code {
	case codes.OK:
		return http.StatusOK
	case codes.Canceled:
		return 499
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
	case codes.Unimplemented:
		return http.StatusNotImplemented
	case codes.Unavailable:
		return http.StatusServiceUnavailable
	default:
		return http.StatusInternalServerError
	}
}

// grpcErr writes a gRPC error as a JSON response.
// Returns true if an error was written (caller should return).
func grpcErr(c *gin.Context, err error, fallback string) bool {
	if err == nil {
		return false
	}
	if err == gobreaker.ErrOpenState {
		c.JSON(http.StatusServiceUnavailable, gin.H{
			"success": false, "error": "Service temporarily unavailable", "degraded": true,
		})
		return true
	}
	st, ok := status.FromError(err)
	if !ok {
		c.JSON(http.StatusInternalServerError, gin.H{"success": false, "error": fallback})
		return true
	}
	c.JSON(grpcCodeToHTTP(st.Code()), gin.H{"success": false, "error": st.Message()})
	return true
}
