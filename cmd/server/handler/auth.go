package handler

import (
	"crypto/rand"
	"encoding/binary"
	"fmt"
	"log/slog"
	"net/http"
	"time"

	"github.com/gin-gonic/gin"
	pb "gateway-grpc/gen/rpc"
)

func (h *Handlers) Login(c *gin.Context) {
	var req pb.LoginRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid request"})
		return
	}
	if h.checkLoginRate(c.Request.Context(), req.Username) {
		c.JSON(http.StatusTooManyRequests, gin.H{"success": false, "error": "Too many attempts"})
		return
	}
	if msg, code := validateLogin(req.Username, req.Password); msg != "" {
		c.JSON(code, gin.H{"success": false, "error": msg})
		return
	}
	resp, err := h.Auth.Login(c.Request.Context(), &req)
	if grpcErr(c, err, "auth operation failed") { return }
	if !resp.GetSuccess() {
		c.JSON(http.StatusUnauthorized, gin.H{"success": false, "error": resp.GetError()})
		return
	}
	h.setCookies(c, resp.GetAccessToken(), resp.GetRefreshToken())
	c.JSON(http.StatusOK, resp)

	// Award login points (daily)
	go h.earnPoints(resp.GetUserId(), 10, "daily_login",
		fmt.Sprintf("login:%d:%s", resp.GetUserId(), time.Now().Format("2006-01-02")))
}

func (h *Handlers) Register(c *gin.Context) {
	var req pb.RegisterRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid request"})
		return
	}
	if msg, code := validateRegister(req.Username, req.Password); msg != "" {
		c.JSON(code, gin.H{"success": false, "error": msg})
		return
	}
	resp, err := h.Auth.Register(c.Request.Context(), &req)
	if grpcErr(c, err, "auth operation failed") { return }
	if !resp.GetSuccess() {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": resp.GetError()})
		return
	}
	h.setCookies(c, resp.GetAccessToken(), resp.GetRefreshToken())
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) Refresh(c *gin.Context) {
	var req pb.RefreshTokenRequest
	c.ShouldBindJSON(&req)
	if req.RefreshToken == "" {
		if ck, _ := c.Cookie("rpc_rt"); ck != "" {
			req.RefreshToken = ck
		}
	}
	if req.Username == "" {
		req.Username = h.username(c)
	}
	resp, err := h.Auth.RefreshToken(c.Request.Context(), &req)
	if err != nil {
		slog.Error("refresh gRPC error", "error", err)
	}
	if grpcErr(c, err, "refresh failed") { return }
	h.setCookies(c, resp.GetAccessToken(), "")
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) ChangePassword(c *gin.Context) {
	var body struct {
		OldPassword string `json:"old_password"`
		NewPassword string `json:"new_password"`
	}
	if err := c.ShouldBindJSON(&body); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid request"})
		return
	}
	uid := h.uid(c)
	if uid == 0 {
		c.JSON(http.StatusUnauthorized, gin.H{"success": false, "error": "authentication required"})
		return
	}
	if msg, code := validateChangePassword(body.OldPassword, body.NewPassword); msg != "" {
		c.JSON(code, gin.H{"success": false, "error": msg})
		return
	}
	req := &pb.ChangePasswordRequest{UserId: uid, OldPassword: body.OldPassword, NewPassword: body.NewPassword}
	resp, err := h.Auth.ChangePassword(c.Request.Context(), req)
	if grpcErr(c, err, "auth operation failed") { return }
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) OTPSend(c *gin.Context) {
	var body struct{ Phone string `json:"phone"` }
	if err := c.ShouldBindJSON(&body); err != nil || body.Phone == "" {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "phone required"})
		return
	}
	code := randomOTP()
	h.RDB.Set(c.Request.Context(), "otp:"+body.Phone, code, 5*time.Minute)
	slog.Info("otp sent", "phone", body.Phone, "code", code)
	c.JSON(http.StatusOK, gin.H{"success": true})
}

func (h *Handlers) PhoneLogin(c *gin.Context) {
	var body struct {
		Phone string `json:"phone"`
		OTP   string `json:"otp"`
	}
	if err := c.ShouldBindJSON(&body); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid request"})
		return
	}
	stored, _ := h.RDB.Get(c.Request.Context(), "otp:"+body.Phone).Result()
	if stored == "" || stored != body.OTP {
		c.JSON(http.StatusUnauthorized, gin.H{"success": false, "error": "Invalid OTP"})
		return
	}
	h.RDB.Del(c.Request.Context(), "otp:"+body.Phone)
	resp, err := h.Auth.LoginByPhone(c.Request.Context(), &pb.PhoneLoginRequest{Phone: body.Phone, Otp: body.OTP})
	if grpcErr(c, err, "phone login failed") { return }
	h.setCookies(c, resp.GetAccessToken(), resp.GetRefreshToken())
	c.JSON(http.StatusOK, resp)
}

func randomOTP() string {
	b := make([]byte, 4)
	if _, err := rand.Read(b); err != nil {
		return fmt.Sprintf("%06d", time.Now().UnixNano()%1000000)
	}
	return fmt.Sprintf("%06d", binary.BigEndian.Uint32(b)%1000000)
}

func validateLogin(username, password string) (string, int) {
	if username == "" {
		return "username required", http.StatusBadRequest
	}
	if password == "" {
		return "password required", http.StatusBadRequest
	}
	return "", 0
}

func validateRegister(username, password string) (string, int) {
	if len(username) < 3 || len(username) > 20 {
		return "username must be 3-20 characters", http.StatusBadRequest
	}
	if len(password) < 6 {
		return "password must be at least 6 characters", http.StatusBadRequest
	}
	return "", 0
}

func (h *Handlers) RegisterAuthRoutes(public, auth *gin.RouterGroup) {
	public.POST("/login", h.Login)
	public.POST("/register", h.Register)
	public.POST("/refresh", h.Refresh)
	public.POST("/auth/otp/send", h.OTPSend)
	public.POST("/auth/phone/login", h.PhoneLogin)
	auth.PUT("/me/password", h.ChangePassword)
}

func validateChangePassword(oldPwd, newPwd string) (string, int) {
	if oldPwd == "" || newPwd == "" {
		return "old_password and new_password required", http.StatusBadRequest
	}
	if len(newPwd) < 6 {
		return "new password must be at least 6 characters", http.StatusBadRequest
	}
	return "", 0
}
