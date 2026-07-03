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

type mockAuthClient struct {
	registerResp     *pb.RegisterResponse
	loginResp        *pb.LoginResponse
	changePwdErr     error
	changePwdResp    *pb.ChangePasswordResponse
	refreshResp      *pb.RefreshTokenResponse
	loginByPhoneResp *pb.LoginResponse
}

func (m *mockAuthClient) Login(ctx context.Context, req *pb.LoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error) {
	return m.loginResp, nil
}
func (m *mockAuthClient) Register(ctx context.Context, req *pb.RegisterRequest, opts ...grpc.CallOption) (*pb.RegisterResponse, error) {
	return m.registerResp, nil
}
func (m *mockAuthClient) RefreshToken(ctx context.Context, req *pb.RefreshTokenRequest, opts ...grpc.CallOption) (*pb.RefreshTokenResponse, error) {
	return m.refreshResp, nil
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

type mockSheetClient struct {
	listResp   *pb.ListSpreadsheetsResponse
	getResp    *pb.GetSpreadsheetResponse
	createResp *pb.CreateSpreadsheetResponse
	updateResp *pb.UpdateSpreadsheetResponse
	deleteResp *pb.DeleteSpreadsheetResponse
	listErr    error
	getErr     error
}

func (m *mockSheetClient) ListSpreadsheets(ctx context.Context, req *pb.ListSpreadsheetsRequest, opts ...grpc.CallOption) (*pb.ListSpreadsheetsResponse, error) {
	return m.listResp, m.listErr
}
func (m *mockSheetClient) GetSpreadsheet(ctx context.Context, req *pb.GetSpreadsheetRequest, opts ...grpc.CallOption) (*pb.GetSpreadsheetResponse, error) {
	return m.getResp, m.getErr
}
func (m *mockSheetClient) CreateSpreadsheet(ctx context.Context, req *pb.CreateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.CreateSpreadsheetResponse, error) {
	return m.createResp, nil
}
func (m *mockSheetClient) UpdateSpreadsheet(ctx context.Context, req *pb.UpdateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.UpdateSpreadsheetResponse, error) {
	return m.updateResp, nil
}
func (m *mockSheetClient) DeleteSpreadsheet(ctx context.Context, req *pb.DeleteSpreadsheetRequest, opts ...grpc.CallOption) (*pb.DeleteSpreadsheetResponse, error) {
	return m.deleteResp, nil
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

type mockSearchClient struct {
	searchResp *pb.SearchResponse
	searchErr  error
}

func (m *mockSearchClient) Search(ctx context.Context, req *pb.SearchRequest, opts ...grpc.CallOption) (*pb.SearchResponse, error) {
	return m.searchResp, m.searchErr
}

type mockSharedClient struct {
	linkResp *pb.ShareLinkResponse
}

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

// ---- Tests ----

func TestLogin_Success(t *testing.T) {
	g := &gw.Gateway{
		AuthClient: &mockAuthClient{
			loginResp: &pb.LoginResponse{Success: true, UserId: 1, AccessToken: "at", RefreshToken: "rt", Role: "user"},
		},
	}
	req := httptest.NewRequest("POST", "/api/v1/login",
		strings.NewReader(`{"username":"alice","password":"secret123"}`))
	w := httptest.NewRecorder()
	g.Login(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d: %s", w.Code, w.Body.String())
	}
	// Check Set-Cookie headers
	cookies := w.Result().Cookies()
	hasAT, hasRT := false, false
	for _, c := range cookies {
		if c.Name == "rpc_at" && c.Value == "at" {
			hasAT = true
		}
		if c.Name == "rpc_rt" && c.Value == "rt" {
			hasRT = true
		}
	}
	if !hasAT {
		t.Error("missing rpc_at cookie")
	}
	if !hasRT {
		t.Error("missing rpc_rt cookie")
	}
}

func TestLogin_ValidationError(t *testing.T) {
	g := &gw.Gateway{AuthClient: &mockAuthClient{}}
	req := httptest.NewRequest("POST", "/api/v1/login",
		strings.NewReader(`{"username":"","password":""}`))
	w := httptest.NewRecorder()
	g.Login(w, req)
	if w.Code != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", w.Code)
	}
}

func TestRegister_Success(t *testing.T) {
	g := &gw.Gateway{
		AuthClient: &mockAuthClient{
			registerResp: &pb.RegisterResponse{Success: true, UserId: 2, AccessToken: "at2", RefreshToken: "rt2"},
		},
	}
	req := httptest.NewRequest("POST", "/api/v1/register",
		strings.NewReader(`{"username":"bob","password":"pass123"}`))
	w := httptest.NewRecorder()
	g.Register(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d: %s", w.Code, w.Body.String())
	}
}

func TestChangePassword_Unauthorized(t *testing.T) {
	g := &gw.Gateway{AuthClient: &mockAuthClient{}}
	req := httptest.NewRequest("PUT", "/api/v1/me/password",
		strings.NewReader(`{"old_password":"old","new_password":"new1234"}`))
	w := httptest.NewRecorder()
	g.ChangePassword(w, req)
	if w.Code != http.StatusUnauthorized {
		t.Errorf("expected 401, got %d", w.Code)
	}
}

func TestChangePassword_GrpcError(t *testing.T) {
	g := &gw.Gateway{
		AuthClient: &mockAuthClient{changePwdErr: status.Error(codes.Internal, "DB error")},
	}
	req := httptest.NewRequest("PUT", "/api/v1/me/password",
		strings.NewReader(`{"old_password":"old","new_password":"new1234"}`))
	req.Header.Set("X-Rpc-Uid", "12345")
	w := httptest.NewRecorder()
	g.ChangePassword(w, req)
	if w.Code != http.StatusInternalServerError {
		t.Errorf("expected 500, got %d: %s", w.Code, w.Body.String())
	}
}

func TestCreateFolder_Success(t *testing.T) {
	g := &gw.Gateway{
		FileClient: &mockFileClient{createFolderResp: &pb.CreateFolderResponse{Success: true, Id: 999}},
	}
	req := httptest.NewRequest("POST", "/api/v1/files/folder", strings.NewReader(`{"name":"test-folder"}`))
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: validTestJWT()})
	w := httptest.NewRecorder()
	g.CreateFolder(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d: %s", w.Code, w.Body.String())
	}
}

func TestHealth_Ready(t *testing.T) {
	g := &gw.Gateway{}
	req := httptest.NewRequest("GET", "/api/v1/health", nil)
	w := httptest.NewRecorder()
	g.Health(w, req)
	if w.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", w.Code)
	}
	if !strings.Contains(w.Body.String(), "READY") {
		t.Errorf("expected READY, got %s", w.Body.String())
	}
}

func TestShareLink_Create(t *testing.T) {
	g := &gw.Gateway{
		SharedClient: &mockSharedClient{
			linkResp: &pb.ShareLinkResponse{Success: true, Token: "abc123"},
		},
	}
	req := httptest.NewRequest("POST", "/api/v1/sheets/123/share-link",
		strings.NewReader(`{"permission":"view"}`))
	req.Header.Set("X-Rpc-Uid", "12345")
	w := httptest.NewRecorder()
	g.CreateShareLink(w, req)
	if !strings.Contains(w.Body.String(), "abc123") {
		t.Errorf("expected token abc123, got: %s", w.Body.String())
	}
}

func TestShareToken_NotFound(t *testing.T) {
	g := &gw.Gateway{SharedClient: &mockSharedClient{}}
	req := httptest.NewRequest("GET", "/api/v1/s/invalid-token", nil)
	w := httptest.NewRecorder()
	g.ShareByToken(w, req)
	if w.Code != http.StatusNotFound {
		t.Errorf("expected 404, got %d", w.Code)
	}
}

func TestSearch_Validation(t *testing.T) {
	g := &gw.Gateway{SearchClient: &mockSearchClient{}, CBSearch: gw.NewCBSlow("search-test", nil)}
	req := httptest.NewRequest("POST", "/api/v1/search",
		strings.NewReader(`{"q":"","sort":"relevance"}`))
	w := httptest.NewRecorder()
	g.Search(w, req)
	if w.Code != http.StatusBadRequest {
		t.Errorf("expected 400 for empty query, got %d", w.Code)
	}
}

func TestOTPSend_EmptyPhone_Rejected(t *testing.T) {
	g := &gw.Gateway{}
	req := httptest.NewRequest("POST", "/api/v1/auth/otp/send", strings.NewReader(`{"phone":""}`))
	w := httptest.NewRecorder()
	g.OTPSend(w, req)
	if w.Code != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", w.Code)
	}
	var resp map[string]interface{}
	json.NewDecoder(w.Body).Decode(&resp)
	errMsg, _ := resp["error"].(string)
	if errMsg == "" {
		t.Error("expected error message")
	}
}
