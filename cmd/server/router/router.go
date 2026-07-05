package router

import (
	"github.com/gin-gonic/gin"
	"github.com/lcb66699/http-rpc/server/handler"
	"github.com/lcb66699/http-rpc/server/middleware"
)

func Setup(h *handler.Handlers, jwtSecret string) *gin.Engine {
	r := gin.New()
	r.Use(middleware.CORSMiddleware())
	r.Use(middleware.RequestID())
	r.Use(middleware.Logger())
	r.Use(gin.Recovery())

	api := r.Group("/api/v1")
	{
		// Public
		api.POST("/login", h.Login)
		api.POST("/register", h.Register)
		api.POST("/refresh", h.Refresh)
		api.POST("/auth/otp/send", h.OTPSend)
		api.POST("/auth/phone/login", h.PhoneLogin)
		api.GET("/health", h.Health)
		api.GET("/health/ready", h.HealthReady)
		api.GET("/metrics", h.Metrics)

		// Share by token (public)
		api.GET("/s/:token", h.ShareByToken)

		// Authenticated
		auth := api.Group("")
		auth.Use(middleware.Auth(jwtSecret))
		{
			// User
			auth.PUT("/me/password", h.ChangePassword)
			auth.GET("/me", h.Me)
			auth.GET("/services", h.Services)
			auth.GET("/history", h.History)

			// Sheet CRUD
			auth.POST("/sheets", h.CreateSheet)
			auth.GET("/sheets", h.ListSheets)
			auth.GET("/sheets/:id", h.GetSheet)
			auth.PUT("/sheets/:id", h.UpdateSheet)
			auth.DELETE("/sheets/:id", h.DeleteSheet)

			// Sharing
			auth.POST("/sheets/:id/share", h.ShareSheet)
			auth.GET("/sheets/:id/share", h.ListShares)
			auth.DELETE("/sheets/:id/share/:username", h.RevokeShare)
			auth.POST("/sheets/:id/share-link", h.CreateShareLink)

			// File
			auth.GET("/files", h.ListFiles)
			auth.POST("/files/upload", h.UploadFile)
			auth.GET("/files/:id", h.GetFile)
			auth.DELETE("/files/:id", h.DeleteFile)
			auth.PUT("/files/:id/move", h.MoveFile)
			auth.POST("/files/folder", h.CreateFolder)

			// Search
			auth.POST("/search", h.Search)
		}
	}

	return r
}
