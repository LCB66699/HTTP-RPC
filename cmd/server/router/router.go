package router

import (
	"github.com/gin-gonic/gin"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"github.com/lcb66699/http-rpc/server/handler"
	"github.com/lcb66699/http-rpc/server/middleware"
)

func MetricsHandler() gin.HandlerFunc {
	h := promhttp.Handler()
	return func(c *gin.Context) {
		h.ServeHTTP(c.Writer, c.Request)
	}
}

func Setup(h *handler.Handlers, jwtSecret string) *gin.Engine {
	r := gin.New()
	r.Use(middleware.CORSMiddleware())
	r.Use(middleware.RequestID())
	r.Use(middleware.GinMetrics())
	r.Use(middleware.Logger())
	r.Use(gin.Recovery())

	api := r.Group("/api/v1")
	api.GET("/metrics", MetricsHandler())

	auth := api.Group("")
	auth.Use(middleware.Auth(jwtSecret))

	// Resource routes — each domain registers its own.
	h.RegisterAuthRoutes(api, auth)     // login, register, refresh, OTP, change-password
	h.RegisterMiscRoutes(api, auth)     // health, me, services, history, search
	h.RegisterSharingRoutes(api, auth)  // share, revoke, share-link, share-by-token
	h.RegisterSheetRoutes(auth)         // sheet CRUD
	h.RegisterFileRoutes(auth)          // file CRUD + folder
	h.RegisterWorkspaceRoutes(auth)      // workspace CRUD + members

	// WebSocket
	auth.GET("/ws", h.WS.ServeWS)

	return r
}
