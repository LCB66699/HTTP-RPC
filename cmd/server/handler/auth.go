package handler

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"errors"
	"fmt"
	"log/slog"
	"math/big"
	"net"
	"net/http"
	"strconv"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/redis/go-redis/v9"
	pb "gateway-grpc/gen/rpc"
)

const (
	otpSendPhoneLimit     = 3
	otpSendIPLimit        = 10
	otpVerifyFailureLimit = 5
	otpSendWindow         = 10 * time.Minute
	otpVerifyWindow       = 15 * time.Minute
)

var incrementWithExpiry = redis.NewScript(`
local count = redis.call('INCR', KEYS[1])
if count == 1 then
  redis.call('PEXPIRE', KEYS[1], ARGV[1])
end
return count
`)

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
	c.JSON(http.StatusOK, authResponse(resp.GetSuccess(), resp.GetError(), resp.GetUserId(), resp.GetRole()))

	// Award login points (daily, fire-and-forget via gRPC)
	go func(uid int64) {
		if h.Points == nil || uid == 0 {
			return
		}
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer cancel()
		h.Points.Earn(ctx, &pb.EarnRequest{
			UserId: uid, Amount: 10, Reason: "daily_login",
			IdempotencyKey: fmt.Sprintf("login:%d:%s", uid, time.Now().Format("2006-01-02")),
		})
	}(resp.GetUserId())
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
	c.JSON(http.StatusOK, authResponse(resp.GetSuccess(), resp.GetError(), resp.GetUserId(), resp.GetRole()))
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
	if !resp.GetSuccess() {
		c.JSON(http.StatusUnauthorized, gin.H{"success": false, "error": resp.GetError()})
		return
	}
	h.setCookies(c, resp.GetAccessToken(), "")
	c.JSON(http.StatusOK, gin.H{"success": resp.GetSuccess(), "error": resp.GetError()})
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
	writeProtoJSON(c, http.StatusOK, resp)
}

func (h *Handlers) OTPSend(c *gin.Context) {
	var body struct{ Phone string `json:"phone"` }
	if err := c.ShouldBindJSON(&body); err != nil || !validPhone(body.Phone) {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid phone"})
		return
	}
	ctx := c.Request.Context()
	phoneLimited, err := h.consumeOTPLimit(ctx, "rate:otp:send:phone:"+body.Phone, otpSendPhoneLimit, otpSendWindow)
	if err != nil {
		otpUnavailable(c)
		return
	}
	ipLimited, err := h.consumeOTPLimit(ctx, "rate:otp:send:ip:"+sourceIP(c.Request), otpSendIPLimit, otpSendWindow)
	if err != nil {
		otpUnavailable(c)
		return
	}
	if phoneLimited || ipLimited {
		c.JSON(http.StatusTooManyRequests, gin.H{"success": false, "error": "Too many OTP requests"})
		return
	}
	code, err := randomOTP()
	if err != nil {
		slog.Error("otp generation failed", "error", err)
		otpUnavailable(c)
		return
	}
	if err := h.RDB.Set(ctx, "otp:"+body.Phone, code, 5*time.Minute).Err(); err != nil {
		otpUnavailable(c)
		return
	}
	slog.Info("otp generated")
	c.JSON(http.StatusOK, gin.H{"success": true})
}

func (h *Handlers) PhoneLogin(c *gin.Context) {
	var body struct {
		Phone string `json:"phone"`
		OTP   string `json:"otp"`
	}
	if err := c.ShouldBindJSON(&body); err != nil || !validPhone(body.Phone) || !validOTP(body.OTP) {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid request"})
		return
	}
	if h.RDB == nil {
		otpUnavailable(c)
		return
	}
	ctx := c.Request.Context()
	failureKey := "rate:otp:verify:" + body.Phone
	failures, err := h.RDB.Get(ctx, failureKey).Int64()
	if err != nil && !errors.Is(err, redis.Nil) {
		otpUnavailable(c)
		return
	}
	if failures >= otpVerifyFailureLimit {
		c.JSON(http.StatusTooManyRequests, gin.H{"success": false, "error": "Too many verification attempts"})
		return
	}
	stored, err := h.RDB.Get(ctx, "otp:"+body.Phone).Result()
	if err != nil && !errors.Is(err, redis.Nil) {
		otpUnavailable(c)
		return
	}
	if len(stored) != len(body.OTP) || subtle.ConstantTimeCompare([]byte(stored), []byte(body.OTP)) != 1 {
		if _, err := h.consumeOTPLimit(ctx, failureKey, otpVerifyFailureLimit, otpVerifyWindow); err != nil {
			otpUnavailable(c)
			return
		}
		c.JSON(http.StatusUnauthorized, gin.H{"success": false, "error": "Invalid OTP"})
		return
	}
	if _, err := h.RDB.TxPipelined(ctx, func(pipe redis.Pipeliner) error {
		pipe.Del(ctx, "otp:"+body.Phone)
		pipe.Del(ctx, failureKey)
		return nil
	}); err != nil {
		otpUnavailable(c)
		return
	}
	resp, err := h.Auth.LoginByPhone(ctx, &pb.PhoneLoginRequest{Phone: body.Phone, Otp: body.OTP})
	if grpcErr(c, err, "phone login failed") { return }
	h.setCookies(c, resp.GetAccessToken(), resp.GetRefreshToken())
	c.JSON(http.StatusOK, authResponse(resp.GetSuccess(), resp.GetError(), resp.GetUserId(), resp.GetRole()))
}

func randomOTP() (string, error) {
	n, err := rand.Int(rand.Reader, big.NewInt(1_000_000))
	if err != nil {
		return "", err
	}
	return fmt.Sprintf("%06d", n.Int64()), nil
}

func (h *Handlers) consumeOTPLimit(ctx context.Context, key string, limit int64, window time.Duration) (bool, error) {
	if h.RDB == nil {
		return false, errors.New("redis unavailable")
	}
	count, err := incrementWithExpiry.Run(ctx, h.RDB, []string{key}, window.Milliseconds()).Int64()
	if err != nil {
		return false, err
	}
	return count > limit, nil
}

func validPhone(phone string) bool {
	if len(phone) < 9 || len(phone) > 16 || phone[0] != '+' || phone[1] == '0' {
		return false
	}
	for i := 1; i < len(phone); i++ {
		if phone[i] < '0' || phone[i] > '9' {
			return false
		}
	}
	return true
}

func validOTP(code string) bool {
	if len(code) != 6 {
		return false
	}
	for i := range code {
		if code[i] < '0' || code[i] > '9' {
			return false
		}
	}
	return true
}

func sourceIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err == nil {
		if ip := net.ParseIP(host); ip != nil {
			return ip.String()
		}
	}
	if ip := net.ParseIP(r.RemoteAddr); ip != nil {
		return ip.String()
	}
	return "unknown"
}

func otpUnavailable(c *gin.Context) {
	c.JSON(http.StatusServiceUnavailable, gin.H{"success": false, "error": "OTP service unavailable"})
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

// Access and refresh tokens are intentionally omitted from JSON responses.
// They are delivered only through HttpOnly, Secure cookies in setCookies.
func authResponse(success bool, err string, userID int64, role string) gin.H {
	return gin.H{
		"success": success,
		"error":   err,
		"user_id": strconv.FormatInt(userID, 10),
		"role":    role,
	}
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
