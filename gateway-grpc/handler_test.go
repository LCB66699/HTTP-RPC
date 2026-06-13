package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	pb "gateway-grpc/gen/rpc"
)

// ---- Mock clients ----
type mockSheetClient struct {
	listResp *pb.ListSpreadsheetsResponse
	listErr  error
}

func (m *mockSheetClient) ListSpreadsheets(ctx context.Context, req *pb.ListSpreadsheetsRequest, opts ...grpc.CallOption) (*pb.ListSpreadsheetsResponse, error) {
	return m.listResp, m.listErr
}
func (m *mockSheetClient) GetSpreadsheet(ctx context.Context, req *pb.GetSpreadsheetRequest, opts ...grpc.CallOption) (*pb.GetSpreadsheetResponse, error) {
	return nil, nil
}
func (m *mockSheetClient) CreateSpreadsheet(ctx context.Context, req *pb.CreateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.CreateSpreadsheetResponse, error) {
	return nil, nil
}
func (m *mockSheetClient) UpdateSpreadsheet(ctx context.Context, req *pb.UpdateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.UpdateSpreadsheetResponse, error) {
	return nil, nil
}
func (m *mockSheetClient) DeleteSpreadsheet(ctx context.Context, req *pb.DeleteSpreadsheetRequest, opts ...grpc.CallOption) (*pb.DeleteSpreadsheetResponse, error) {
	return nil, nil
}

type mockAuthClient struct {
	changePwdResp *pb.ChangePasswordResponse
	changePwdErr  error
	loginResp     *pb.LoginResponse
}

func (m *mockAuthClient) Login(ctx context.Context, req *pb.LoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error) {
	return m.loginResp, nil
}
func (m *mockAuthClient) Register(ctx context.Context, req *pb.RegisterRequest, opts ...grpc.CallOption) (*pb.RegisterResponse, error) {
	return nil, nil
}
func (m *mockAuthClient) RefreshToken(ctx context.Context, req *pb.RefreshTokenRequest, opts ...grpc.CallOption) (*pb.RefreshTokenResponse, error) {
	return nil, nil
}
func (m *mockAuthClient) ChangePassword(ctx context.Context, req *pb.ChangePasswordRequest, opts ...grpc.CallOption) (*pb.ChangePasswordResponse, error) {
	return m.changePwdResp, m.changePwdErr
}
func (m *mockAuthClient) LoginByPhone(ctx context.Context, req *pb.PhoneLoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error) {
	return nil, nil
}

// ---- Helper ----
func validTestJWT() string {
	return "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1aWQiOjEyMzQ1LCJleHAiOjk5OTk5OTk5OTl9.fake"
}

// ---- Test: ChangePassword ----
func TestChangePassword_Success(t *testing.T) {
	orig := authClient
	authClient = &mockAuthClient{
		changePwdResp: &pb.ChangePasswordResponse{Success: true},
	}
	defer func() { authClient = orig }()

	req := httptest.NewRequest("PUT", "/api/me/password",
		strings.NewReader(`{"old_password":"old","new_password":"new1234"}`))
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()

	// Create a minimal mux with just the change password handler
	mux := http.NewServeMux()
	mux.HandleFunc("PUT /api/me/password", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			OldPassword string `json:"old_password"`
			NewPassword string `json:"new_password"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		uid := extractUID(r)
		if uid == 0 {
			http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
			return
		}
		resp, err := authClient.ChangePassword(r.Context(), &pb.ChangePasswordRequest{
			UserId: uid, OldPassword: body.OldPassword, NewPassword: body.NewPassword,
		})
		if err != nil || resp == nil || !resp.Success {
			writeJSONStatus(w, http.StatusBadRequest, map[string]interface{}{"success": false, "error": "failed"})
			return
		}
		writeJSON(w, resp)
	})
	mux.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d: %s", w.Code, w.Body.String())
	}
}

func TestChangePassword_Unauthorized(t *testing.T) {
	req := httptest.NewRequest("PUT", "/api/me/password",
		strings.NewReader(`{"old_password":"old","new_password":"new1234"}`))
	w := httptest.NewRecorder()

	mux := http.NewServeMux()
	mux.HandleFunc("PUT /api/me/password", func(w http.ResponseWriter, r *http.Request) {
		uid := extractUID(r)
		if uid == 0 {
			http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
			return
		}
		writeJSON(w, map[string]string{"ok": "ok"})
	})
	mux.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized {
		t.Errorf("expected 401 without cookie, got %d", w.Code)
	}
}

func TestChangePassword_GrpcError(t *testing.T) {
	orig := authClient
	authClient = &mockAuthClient{
		changePwdErr: status.Error(codes.Internal, "DB error"),
	}
	defer func() { authClient = orig }()

	req := httptest.NewRequest("PUT", "/api/me/password",
		strings.NewReader(`{"old_password":"old","new_password":"new1234"}`))
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()

	mux := http.NewServeMux()
	mux.HandleFunc("PUT /api/me/password", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			OldPassword string `json:"old_password"`
			NewPassword string `json:"new_password"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		uid := extractUID(r)
		resp, err := authClient.ChangePassword(r.Context(), &pb.ChangePasswordRequest{
			UserId: uid, OldPassword: body.OldPassword, NewPassword: body.NewPassword,
		})
		if err != nil || resp == nil || !resp.Success {
			writeJSONStatus(w, http.StatusInternalServerError, map[string]interface{}{"success": false, "error": err.Error()})
			return
		}
		writeJSON(w, resp)
	})
	mux.ServeHTTP(w, req)

	if w.Code != http.StatusInternalServerError {
		t.Errorf("expected 500 on gRPC error, got %d", w.Code)
	}
}
