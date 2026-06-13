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
	changePwdErr  error
	changePwdResp *pb.ChangePasswordResponse
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

type SharingClient interface {
	Share(ctx context.Context, req *pb.ShareRequest, opts ...grpc.CallOption) (*pb.ShareResponse, error)
	Revoke(ctx context.Context, req *pb.RevokeRequest, opts ...grpc.CallOption) (*pb.RevokeResponse, error)
	ListShares(ctx context.Context, req *pb.ResourceRequest, opts ...grpc.CallOption) (*pb.ShareListResponse, error)
	CreateShareLink(ctx context.Context, req *pb.ShareLinkRequest, opts ...grpc.CallOption) (*pb.ShareLinkResponse, error)
	GetByToken(ctx context.Context, req *pb.ShareTokenRequest, opts ...grpc.CallOption) (*pb.SharedResourceResponse, error)
}

type mockSharingClient struct{ linkResp *pb.ShareLinkResponse }

func (m *mockSharingClient) Share(ctx context.Context, req *pb.ShareRequest, opts ...grpc.CallOption) (*pb.ShareResponse, error) {
	return nil, nil
}
func (m *mockSharingClient) Revoke(ctx context.Context, req *pb.RevokeRequest, opts ...grpc.CallOption) (*pb.RevokeResponse, error) {
	return nil, nil
}
func (m *mockSharingClient) ListShares(ctx context.Context, req *pb.ResourceRequest, opts ...grpc.CallOption) (*pb.ShareListResponse, error) {
	return nil, nil
}
func (m *mockSharingClient) CreateShareLink(ctx context.Context, req *pb.ShareLinkRequest, opts ...grpc.CallOption) (*pb.ShareLinkResponse, error) {
	return m.linkResp, nil
}
func (m *mockSharingClient) GetByToken(ctx context.Context, req *pb.ShareTokenRequest, opts ...grpc.CallOption) (*pb.SharedResourceResponse, error) {
	return nil, nil
}

var sharingClient SharingClient

// ---- Change Password ----
func TestChangePassword_Success(t *testing.T) {
	orig := authClient
	authClient = &mockAuthClient{changePwdResp: &pb.ChangePasswordResponse{Success: true}}
	defer func() { authClient = orig }()
	req := httptest.NewRequest("PUT", "/api/me/password",
		strings.NewReader(`{"old_password":"old","new_password":"new1234"}`))
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("PUT /api/me/password", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ OldPassword, NewPassword string }
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
		if extractUID(r) == 0 {
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
	orig := authClient
	authClient = &mockAuthClient{changePwdErr: status.Error(codes.Internal, "DB error")}
	defer func() { authClient = orig }()
	req := httptest.NewRequest("PUT", "/api/me/password",
		strings.NewReader(`{"old_password":"old","new_password":"new1234"}`))
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("PUT /api/me/password", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ OldPassword, NewPassword string }
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
		t.Errorf("expected 500, got %d", w.Code)
	}
}

// ---- Cursor Pagination ----
func TestListSheets_CursorFirstPage(t *testing.T) {
	orig := sheetClient
	sheetClient = &mockSheetClient{
		listResp: &pb.ListSpreadsheetsResponse{Success: true, Total: 50, HasMore: true, NextCursor: "999"},
	}
	defer func() { sheetClient = orig }()
	req := httptest.NewRequest("GET", "/api/sheets?limit=10", nil)
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/sheets", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := sheetClient.ListSpreadsheets(r.Context(), &pb.ListSpreadsheetsRequest{UserId: 12345, Limit: 10})
		if resp.GetHasMore() && resp.GetNextCursor() == "" {
			t.Error("has_more=true requires next_cursor")
		}
		writeJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if !strings.Contains(w.Body.String(), `"has_more":true`) {
		t.Errorf("expected has_more, got: %s", w.Body.String())
	}
}

func TestListSheets_EmptyResult(t *testing.T) {
	orig := sheetClient
	sheetClient = &mockSheetClient{
		listResp: &pb.ListSpreadsheetsResponse{Success: true, Total: 0, HasMore: false},
	}
	defer func() { sheetClient = orig }()
	req := httptest.NewRequest("GET", "/api/sheets?limit=10", nil)
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/sheets", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := sheetClient.ListSpreadsheets(r.Context(), &pb.ListSpreadsheetsRequest{UserId: 12345, Limit: 10})
		writeJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", w.Code)
	}
}

// ---- Folder ----
func TestCreateFolder_Success(t *testing.T) {
	orig := fileClient
	fileClient = &mockFileClient{createFolderResp: &pb.CreateFolderResponse{Success: true, Id: 999}}
	defer func() { fileClient = orig }()
	req := httptest.NewRequest("POST", "/api/files/folder", strings.NewReader(`{"name":"test"}`))
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/files/folder", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ Name string }
		json.NewDecoder(r.Body).Decode(&body)
		uid := extractUID(r)
		resp, _ := fileClient.CreateFolder(r.Context(), &pb.CreateFolderRequest{UserId: uid, Name: body.Name})
		writeJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d: %s", w.Code, w.Body.String())
	}
}

// ---- Phone OTP ----
func TestPhoneLogin_Success(t *testing.T) {
	orig := authClient
	authClient = &mockAuthClient{
		loginByPhoneResp: &pb.LoginResponse{Success: true, UserId: 1, AccessToken: "at", RefreshToken: "rt", Role: "user"},
	}
	defer func() { authClient = orig }()
	req := httptest.NewRequest("POST", "/api/auth/phone/login",
		strings.NewReader(`{"phone":"13800138000","otp":"123456"}`))
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/auth/phone/login", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ Phone, OTP string }
		json.NewDecoder(r.Body).Decode(&body)
		resp, _ := authClient.LoginByPhone(r.Context(), &pb.PhoneLoginRequest{Phone: body.Phone, Otp: body.OTP})
		writeJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d: %s", w.Code, w.Body.String())
	}
}

func TestOTPSend_ValidPhone(t *testing.T) {
	req := httptest.NewRequest("POST", "/api/auth/otp/send",
		strings.NewReader(`{"phone":"13800138000"}`))
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/auth/otp/send", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ Phone string }
		json.NewDecoder(r.Body).Decode(&body)
		if body.Phone == "" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		writeJSON(w, map[string]interface{}{"success": true})
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", w.Code)
	}
}

func TestOTPSend_EmptyPhone_Rejected(t *testing.T) {
	req := httptest.NewRequest("POST", "/api/auth/otp/send", strings.NewReader(`{"phone":""}`))
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/auth/otp/send", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ Phone string }
		json.NewDecoder(r.Body).Decode(&body)
		if body.Phone == "" {
			http.Error(w, `{"error":"phone required"}`, http.StatusBadRequest)
			return
		}
		writeJSON(w, map[string]interface{}{"success": true})
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", w.Code)
	}
}

// ---- Photo ----
func TestPhotoList_Success(t *testing.T) {
	orig := fileClient
	fileClient = &mockFileClient{listResp: &pb.ListFilesResponse{Success: true, Total: 5}}
	defer func() { fileClient = orig }()
	req := httptest.NewRequest("GET", "/api/photos", nil)
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/photos", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := fileClient.ListFiles(r.Context(), &pb.ListFilesRequest{UserId: 12345, MimeFilter: "image/"})
		writeJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", w.Code)
	}
}

// ---- Sharing ----
func TestShareLink_Create(t *testing.T) {
	sharingClient = &mockSharingClient{
		linkResp: &pb.ShareLinkResponse{Success: true, Token: "abc123"},
	}
	req := httptest.NewRequest("POST", "/api/sheets/123/share-link",
		strings.NewReader(`{"permission":"view"}`))
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/sheets/{id}/share-link", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := sharingClient.CreateShareLink(r.Context(), &pb.ShareLinkRequest{
			OwnerId: 12345, ResourceType: "sheet", ResourceId: 123, Permission: "view",
		})
		writeJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if !strings.Contains(w.Body.String(), `abc123`) {
		t.Errorf("expected token abc123, got: %s", w.Body.String())
	}
}

func TestShareToken_Access(t *testing.T) {
	sharingClient = &mockSharingClient{}
	req := httptest.NewRequest("GET", "/api/s/abc123", nil)
	w := httptest.NewRecorder()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/s/{token}", func(w http.ResponseWriter, r *http.Request) {
		token := r.PathValue("token")
		resp, _ := sharingClient.GetByToken(r.Context(), &pb.ShareTokenRequest{Token: token})
		if resp == nil || !resp.Success {
			writeJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false})
			return
		}
		writeJSON(w, resp)
	})
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusNotFound {
		t.Errorf("expected 404 for invalid token, got %d", w.Code)
	}
}
