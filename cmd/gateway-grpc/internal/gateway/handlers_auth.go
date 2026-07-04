package gateway

import (
	"encoding/json"
	"log/slog"
	"net/http"
	"time"

	pb "gateway-grpc/gen/rpc"
)

func (g *Gateway) Register(w http.ResponseWriter, r *http.Request) {
	var req pb.RegisterRequest
	json.NewDecoder(r.Body).Decode(&req)
	if msg, code := ValidateRegister(req.Username, req.Password); msg != "" {
		WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
		return
	}
	resp, err := g.AuthClient.Register(r.Context(), &req)
	if err != nil {
		WriteGRPCError(w, err, "register failed")
		return
	}
	if !resp.GetSuccess() {
		WriteError(w, nil, resp.GetError(), 0)
		return
	}
	g.setCookies(w, resp.GetAccessToken(), resp.GetRefreshToken())
	WriteJSON(w, resp)
}

func (g *Gateway) Login(w http.ResponseWriter, r *http.Request) {
	var req pb.LoginRequest
	json.NewDecoder(r.Body).Decode(&req)
	if g.checkLoginRate(r.Context(), req.Username) {
		WriteJSONStatus(w, http.StatusTooManyRequests,
			map[string]interface{}{"success": false, "error": "Too many attempts, try again later"})
		return
	}
	if msg, code := ValidateLogin(req.Username, req.Password); msg != "" {
		WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
		return
	}
	resp, err := g.AuthClient.Login(r.Context(), &req)
	if err != nil {
		WriteGRPCError(w, err, "login failed")
		return
	}
	if !resp.GetSuccess() {
		WriteError(w, nil, resp.GetError(), 0)
		return
	}
	g.setCookies(w, resp.GetAccessToken(), resp.GetRefreshToken())
	WriteJSON(w, resp)
}

func (g *Gateway) Refresh(w http.ResponseWriter, r *http.Request) {
	var req pb.RefreshTokenRequest
	json.NewDecoder(r.Body).Decode(&req)
	if req.RefreshToken == "" {
		if ck, err := r.Cookie("rpc_rt"); err == nil {
			req.RefreshToken = ck.Value
		}
	}
	if req.Username == "" {
		req.Username = GetUserFromCookie(r)
	}
	resp, err := g.AuthClient.RefreshToken(r.Context(), &req)
	if err != nil {
		slog.Error("refresh gRPC error", "error", err)
		WriteGRPCError(w, err, "refresh failed")
		return
	}
	g.setCookies(w, resp.GetAccessToken(), "")
	WriteJSON(w, resp)
}

func (g *Gateway) ChangePassword(w http.ResponseWriter, r *http.Request) {
	var body struct {
		OldPassword string `json:"old_password"`
		NewPassword string `json:"new_password"`
	}
	json.NewDecoder(r.Body).Decode(&body)
	uid := ExtractUID(r)
	if uid == 0 {
		WriteJSONStatus(w, http.StatusUnauthorized,
			map[string]interface{}{"success": false, "error": "authentication required"})
		return
	}
	if msg, code := ValidateChangePassword(body.OldPassword, body.NewPassword); msg != "" {
		WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
		return
	}
	req := &pb.ChangePasswordRequest{UserId: uid, OldPassword: body.OldPassword, NewPassword: body.NewPassword}
	resp, err := g.AuthClient.ChangePassword(r.Context(), req)
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) OTPSend(w http.ResponseWriter, r *http.Request) {
	var body struct{ Phone string `json:"phone"` }
	json.NewDecoder(r.Body).Decode(&body)
	if body.Phone == "" {
		WriteJSONStatus(w, http.StatusBadRequest, map[string]interface{}{"success": false, "error": "phone required"})
		return
	}
	code := randomOTP()
	g.RDB.Set(r.Context(), "otp:"+body.Phone, code, 5*time.Minute)
	slog.Info("otp sent", "phone", body.Phone, "code", code)
	WriteJSON(w, map[string]interface{}{"success": true})
}

func (g *Gateway) PhoneLogin(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Phone string `json:"phone"`
		OTP   string `json:"otp"`
	}
	json.NewDecoder(r.Body).Decode(&body)
	stored, _ := g.RDB.Get(r.Context(), "otp:"+body.Phone).Result()
	if stored == "" || stored != body.OTP {
		WriteJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{"success": false, "error": "Invalid OTP"})
		return
	}
	g.RDB.Del(r.Context(), "otp:"+body.Phone)
	resp, err := g.AuthClient.LoginByPhone(r.Context(), &pb.PhoneLoginRequest{Phone: body.Phone, Otp: body.OTP})
	if err != nil {
		WriteGRPCError(w, err, "phone login failed")
		return
	}
	g.setCookies(w, resp.GetAccessToken(), resp.GetRefreshToken())
	WriteJSON(w, resp)
}
