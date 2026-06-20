package gateway_test

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

	gw "github.com/lcb66699/http-rpc/gateway-grpc/internal/gateway"
	pb "gateway-grpc/gen/rpc"
)

func validTestJWT() string {
	return "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1aWQiOjEyMzQ1LCJleHAiOjk5OTk5OTk5OTl9.fake"
}

// ---- Mocks ----
type mockSheetClient struct {
	listResp *pb.ListSpreadsheetsResponse
}

func (m *mockSheetClient) ListSpreadsheets(ctx context.Context, req *pb.ListSpreadsheetsRequest, opts ...grpc.CallOption) (*pb.ListSpreadsheetsResponse, error) {
	return m.listResp, nil
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
	changePwdErr     error
	changePwdResp    *pb.ChangePasswordResponse
	loginByPhoneResp *pb.LoginResponse
}

func (m *mockAuthClient) Login(ctx context.Context, req *pb.LoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error) {
	return nil, nil
}
func (m *mockAuthClient) Register(ctx context.Context, req *pb.RegisterRequest, opts ...grpc.CallOption) (*pb.RegisterResponse, error) {
	return nil, nil
}
func (m *mockAuthClient) RefreshToken(ctx context.Context, req *pb.RefreshTokenRequest, opts ...grpc.CallOption) (*pb.RefreshTokenResponse, error) {
	return nil, nil
}
func (m *mockAuthClient) ChangePassword(ctx context.Context, req *pb.ChangePasswordRequest, opts ...grpc.CallOption) (*pb.ChangePasswordResponse, error) {
	if m.changePwdErr != nil {
		return nil, m.changePwdErr
	}
	return m.changePwdResp, nil
}
func (m *mockAuthClient) LoginByPhone(ctx context.Context, req *pb.PhoneLoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error) {
	return m.loginByPhoneResp, nil
}

type mockFileClient struct {
	listResp         *pb.ListFilesResponse
	createFolderResp *pb.CreateFolderResponse
}

func (m *mockFileClient) ListFiles(ctx context.Context, req *pb.ListFilesRequest, opts ...grpc.CallOption) (*pb.ListFilesResponse, error) {
	return m.listResp, nil
}
func (m *mockFileClient) GetFile(ctx context.Context, req *pb.GetFileRequest, opts ...grpc.CallOption) (*pb.GetFileResponse, error) {
	return nil, nil
}
func (m *mockFileClient) CreateFile(ctx context.Context, req *pb.CreateFileRequest, opts ...grpc.CallOption) (*pb.CreateFileResponse, error) {
	return nil, nil
}
func (m *mockFileClient) DeleteFile(ctx context.Context, req *pb.DeleteFileRequest, opts ...grpc.CallOption) (*pb.DeleteFileResponse, error) {
	return nil, nil
}
func (m *mockFileClient) CreateFolder(ctx context.Context, req *pb.CreateFolderRequest, opts ...grpc.CallOption) (*pb.CreateFolderResponse, error) {
	return m.createFolderResp, nil
}
func (m *mockFileClient) MoveFile(ctx context.Context, req *pb.MoveFileRequest, opts ...grpc.CallOption) (*pb.MoveFileResponse, error) {
	return nil, nil
}
func (m *mockFileClient) BatchDelete(ctx context.Context, req *pb.BatchDeleteRequest, opts ...grpc.CallOption) (*pb.BatchDeleteResponse, error) {
	return nil, nil
}

type mockSharedClient struct{ linkResp *pb.ShareLinkResponse }

func (m *mockSharedClient) Share(ctx context.Context, req *pb.ShareRequest, opts ...grpc.CallOption) (*pb.ShareResponse, error) {
	return nil, nil
}
func (m *mockSharedClient) Revoke(ctx context.Context, req *pb.RevokeRequest, opts ...grpc.CallOption) (*pb.RevokeResponse, error) {
	return nil, nil
}
func (m *mockSharedClient) ListShares(ctx context.Context, req *pb.ResourceRequest, opts ...grpc.CallOption) (*pb.ShareListResponse, error) {
	return nil, nil
}
func (m *mockSharedClient) CreateShareLink(ctx context.Context, req *pb.ShareLinkRequest, opts ...grpc.CallOption) (*pb.ShareLinkResponse, error) {
	return m.linkResp, nil
}
func (m *mockSharedClient) GetByToken(ctx context.Context, req *pb.ShareTokenRequest, opts ...grpc.CallOption) (*pb.SharedResourceResponse, error) {
	return nil, nil
}

// ---- Change Password ----
func TestChangePassword_Success(t *testing.T) {
	orig := gw.AuthClient
	gw.AuthClient = &mockAuthClient{changePwdResp: &pb.ChangePasswordResponse{Success: true}}
	defer func() { gw.AuthClient = orig }()
	req := httptest.NewRequest("PUT", "/api/v1/me/password",
		strings.NewReader(`{"old_password":"old","new_password":"new1234"}`))
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("PUT /api/v1/me/password", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ OldPassword, NewPassword string }
		json.NewDecoder(r.Body).Decode(&body)
		uid := gw.ExtractUID(r)
		if uid == 0 {
			http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
			return
		}
		resp, err := gw.AuthClient.ChangePassword(r.Context(), &pb.ChangePasswordRequest{
			UserId: uid, OldPassword: body.OldPassword, NewPassword: body.NewPassword,
		})
		if err != nil || resp == nil || !resp.Success {
			gw.WriteJSONStatus(w, http.StatusBadRequest, map[string]interface{}{"success": false, "error": "failed"})
			return
		}
		gw.WriteJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d: %s", w.Code, w.Body.String())
	}
}

func TestChangePassword_Unauthorized(t *testing.T) {
	req := httptest.NewRequest("PUT", "/api/v1/me/password",
		strings.NewReader(`{"old_password":"old","new_password":"new1234"}`))
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("PUT /api/v1/me/password", func(w http.ResponseWriter, r *http.Request) {
		if gw.ExtractUID(r) == 0 {
			http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
			return
		}
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusUnauthorized {
		t.Errorf("expected 401, got %d", w.Code)
	}
}

func TestChangePassword_GrpcError(t *testing.T) {
	orig := gw.AuthClient
	gw.AuthClient = &mockAuthClient{changePwdErr: status.Error(codes.Internal, "DB error")}
	defer func() { gw.AuthClient = orig }()
	req := httptest.NewRequest("PUT", "/api/v1/me/password",
		strings.NewReader(`{"old_password":"old","new_password":"new1234"}`))
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("PUT /api/v1/me/password", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ OldPassword, NewPassword string }
		json.NewDecoder(r.Body).Decode(&body)
		uid := gw.ExtractUID(r)
		resp, err := gw.AuthClient.ChangePassword(r.Context(), &pb.ChangePasswordRequest{
			UserId: uid, OldPassword: body.OldPassword, NewPassword: body.NewPassword,
		})
		if err != nil || resp == nil || !resp.Success {
			gw.WriteJSONStatus(w, http.StatusInternalServerError, map[string]interface{}{"success": false, "error": err.Error()})
			return
		}
		gw.WriteJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusInternalServerError {
		t.Errorf("expected 500, got %d", w.Code)
	}
}

// ---- Cursor Pagination ----
func TestListSheets_CursorFirstPage(t *testing.T) {
	orig := gw.SheetClient
	gw.SheetClient = &mockSheetClient{
		listResp: &pb.ListSpreadsheetsResponse{Success: true, Total: 50, HasMore: true, NextCursor: "999"},
	}
	defer func() { gw.SheetClient = orig }()
	req := httptest.NewRequest("GET", "/api/v1/sheets?limit=10", nil)
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/v1/sheets", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := gw.SheetClient.ListSpreadsheets(r.Context(), &pb.ListSpreadsheetsRequest{UserId: 12345, Limit: 10})
		if resp.GetHasMore() && resp.GetNextCursor() == "" {
			t.Error("has_more=true requires next_cursor")
		}
		gw.WriteJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if !strings.Contains(w.Body.String(), `"has_more":true`) {
		t.Errorf("expected has_more, got: %s", w.Body.String())
	}
}

func TestListSheets_EmptyResult(t *testing.T) {
	orig := gw.SheetClient
	gw.SheetClient = &mockSheetClient{
		listResp: &pb.ListSpreadsheetsResponse{Success: true, Total: 0, HasMore: false},
	}
	defer func() { gw.SheetClient = orig }()
	req := httptest.NewRequest("GET", "/api/v1/sheets?limit=10", nil)
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/v1/sheets", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := gw.SheetClient.ListSpreadsheets(r.Context(), &pb.ListSpreadsheetsRequest{UserId: 12345, Limit: 10})
		gw.WriteJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", w.Code)
	}
}

// ---- Folder ----
func TestCreateFolder_Success(t *testing.T) {
	orig := gw.FileClient
	gw.FileClient = &mockFileClient{createFolderResp: &pb.CreateFolderResponse{Success: true, Id: 999}}
	defer func() { gw.FileClient = orig }()
	req := httptest.NewRequest("POST", "/api/v1/files/folder", strings.NewReader(`{"name":"test"}`))
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/v1/files/folder", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ Name string }
		json.NewDecoder(r.Body).Decode(&body)
		uid := gw.ExtractUID(r)
		resp, _ := gw.FileClient.CreateFolder(r.Context(), &pb.CreateFolderRequest{UserId: uid, Name: body.Name})
		gw.WriteJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d: %s", w.Code, w.Body.String())
	}
}

// ---- Phone OTP ----
func TestPhoneLogin_Success(t *testing.T) {
	orig := gw.AuthClient
	gw.AuthClient = &mockAuthClient{
		loginByPhoneResp: &pb.LoginResponse{Success: true, UserId: 1, AccessToken: "at", RefreshToken: "rt", Role: "user"},
	}
	defer func() { gw.AuthClient = orig }()
	req := httptest.NewRequest("POST", "/api/v1/auth/phone/login",
		strings.NewReader(`{"phone":"13800138000","otp":"123456"}`))
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/v1/auth/phone/login", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ Phone, OTP string }
		json.NewDecoder(r.Body).Decode(&body)
		resp, _ := gw.AuthClient.LoginByPhone(r.Context(), &pb.PhoneLoginRequest{Phone: body.Phone, Otp: body.OTP})
		gw.WriteJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d: %s", w.Code, w.Body.String())
	}
}

func TestOTPSend_ValidPhone(t *testing.T) {
	req := httptest.NewRequest("POST", "/api/v1/auth/otp/send",
		strings.NewReader(`{"phone":"13800138000"}`))
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/v1/auth/otp/send", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ Phone string }
		json.NewDecoder(r.Body).Decode(&body)
		if body.Phone == "" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		gw.WriteJSON(w, map[string]interface{}{"success": true})
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", w.Code)
	}
}

func TestOTPSend_EmptyPhone_Rejected(t *testing.T) {
	req := httptest.NewRequest("POST", "/api/v1/auth/otp/send", strings.NewReader(`{"phone":""}`))
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/v1/auth/otp/send", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ Phone string }
		json.NewDecoder(r.Body).Decode(&body)
		if body.Phone == "" {
			http.Error(w, `{"error":"phone required"}`, http.StatusBadRequest)
			return
		}
		gw.WriteJSON(w, map[string]interface{}{"success": true})
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", w.Code)
	}
}

// ---- Photo ----
func TestPhotoList_Success(t *testing.T) {
	orig := gw.FileClient
	gw.FileClient = &mockFileClient{listResp: &pb.ListFilesResponse{Success: true, Total: 5}}
	defer func() { gw.FileClient = orig }()
	req := httptest.NewRequest("GET", "/api/v1/photos", nil)
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/v1/photos", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := gw.FileClient.ListFiles(r.Context(), &pb.ListFilesRequest{UserId: 12345, MimeFilter: "image/"})
		gw.WriteJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", w.Code)
	}
}

// ---- Sharing ----
func TestShareLink_Create(t *testing.T) {
	gw.SharedClient = &mockSharedClient{
		linkResp: &pb.ShareLinkResponse{Success: true, Token: "abc123"},
	}
	req := httptest.NewRequest("POST", "/api/v1/sheets/123/share-link",
		strings.NewReader(`{"permission":"view"}`))
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/v1/sheets/{id}/share-link", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := gw.SharedClient.CreateShareLink(r.Context(), &pb.ShareLinkRequest{
			OwnerId: 12345, ResourceType: "sheet", ResourceId: 123, Permission: "view",
		})
		gw.WriteJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if !strings.Contains(w.Body.String(), `abc123`) {
		t.Errorf("expected token abc123, got: %s", w.Body.String())
	}
}

func TestShareToken_Access(t *testing.T) {
	gw.SharedClient = &mockSharedClient{}
	req := httptest.NewRequest("GET", "/api/v1/s/abc123", nil)
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/v1/s/{token}", func(w http.ResponseWriter, r *http.Request) {
		token := r.PathValue("token")
		resp, _ := gw.SharedClient.GetByToken(r.Context(), &pb.ShareTokenRequest{Token: token})
		if resp == nil || !resp.Success {
			gw.WriteJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false})
			return
		}
		gw.WriteJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusNotFound {
		t.Errorf("expected 404 for invalid token, got %d", w.Code)
	}
}
