package handler

import (
	"context"
	"net/http"
	"strconv"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/redis/go-redis/v9"
	pb "gateway-grpc/gen/rpc"
	"google.golang.org/grpc/metadata"

	"github.com/lcb66699/http-rpc/server/middleware"
	"github.com/lcb66699/http-rpc/server/ws"
)

// Handlers holds all dependencies for HTTP handlers.
type Handlers struct {
	Auth   pb.AuthServiceClient
	Sheet  pb.SpreadsheetServiceClient
	File   pb.FileServiceClient
	SearchClient pb.SearchServiceClient
	Share  pb.SharingServiceClient
	RDB    *redis.Client

	CBAuth   *middleware.CBSlow
	CBSearch *middleware.CBSlow
	CBSheet  *middleware.CBSlow
	CBFile   *middleware.CBSlow

	WSHub *ws.Hub
	WS    *ws.Handler

	JWTSecret string
}

// ---- helpers ----

func (h *Handlers) token(ctx context.Context, c *gin.Context) context.Context {
	token, _ := c.Cookie("rpc_at")
	if token != "" {
		return metadata.AppendToOutgoingContext(ctx, "authorization", "Bearer "+token)
	}
	return ctx
}

func (h *Handlers) setCookies(c *gin.Context, at, rt string) {
	if at != "" {
		http.SetCookie(c.Writer, &http.Cookie{
			Name: "rpc_at", Value: at, Path: "/api",
			MaxAge: 900, HttpOnly: true, Secure: true,
			SameSite: http.SameSiteLaxMode,
		})
	}
	if rt != "" {
		http.SetCookie(c.Writer, &http.Cookie{
			Name: "rpc_rt", Value: rt, Path: "/api/v1/refresh",
			MaxAge: 604800, HttpOnly: true, Secure: true,
			SameSite: http.SameSiteStrictMode,
		})
	}
}

func (h *Handlers) uid(c *gin.Context) int64 {
	v, _ := c.Get("uid")
	uid, _ := v.(int64)
	return uid
}

func (h *Handlers) username(c *gin.Context) string {
	v, _ := c.Get("username")
	s, _ := v.(string)
	return s
}

func parseID(c *gin.Context) int64 {
	n, _ := strconv.ParseInt(c.Param("id"), 10, 64)
	return n
}

func (h *Handlers) checkLoginRate(ctx context.Context, username string) bool {
	if username == "" || h.RDB == nil {
		return false
	}
	blockKey := "rate:login:" + username + ":blocked"
	if n, _ := h.RDB.Exists(ctx, blockKey).Result(); n > 0 {
		return true
	}
	minKey := "rate:login:" + username + ":" + time.Now().Format("2006-01-02T15:04")
	n, _ := h.RDB.Incr(ctx, minKey).Result()
	h.RDB.Expire(ctx, minKey, 60*time.Second)
	if n > 5 {
		h.RDB.Set(ctx, blockKey, "1", 5*time.Minute)
		return true
	}
	return false
}
