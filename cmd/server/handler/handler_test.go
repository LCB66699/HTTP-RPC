package handler

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"
	pb "gateway-grpc/gen/rpc"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

// ---- mock gRPC clients ----

type mockAuthClient struct {
	loginFn     func(context.Context, *pb.LoginRequest, ...grpc.CallOption) (*pb.LoginResponse, error)
	registerFn  func(context.Context, *pb.RegisterRequest, ...grpc.CallOption) (*pb.RegisterResponse, error)
	refreshFn   func(context.Context, *pb.RefreshTokenRequest, ...grpc.CallOption) (*pb.RefreshTokenResponse, error)
	chgPwdFn    func(context.Context, *pb.ChangePasswordRequest, ...grpc.CallOption) (*pb.ChangePasswordResponse, error)
	phoneLoginFn func(context.Context, *pb.PhoneLoginRequest, ...grpc.CallOption) (*pb.LoginResponse, error)
}

func (m *mockAuthClient) Login(ctx context.Context, req *pb.LoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error) {
	if m.loginFn != nil { return m.loginFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockAuthClient) Register(ctx context.Context, req *pb.RegisterRequest, opts ...grpc.CallOption) (*pb.RegisterResponse, error) {
	if m.registerFn != nil { return m.registerFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockAuthClient) RefreshToken(ctx context.Context, req *pb.RefreshTokenRequest, opts ...grpc.CallOption) (*pb.RefreshTokenResponse, error) {
	if m.refreshFn != nil { return m.refreshFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockAuthClient) ChangePassword(ctx context.Context, req *pb.ChangePasswordRequest, opts ...grpc.CallOption) (*pb.ChangePasswordResponse, error) {
	if m.chgPwdFn != nil { return m.chgPwdFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockAuthClient) LoginByPhone(ctx context.Context, req *pb.PhoneLoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error) {
	if m.phoneLoginFn != nil { return m.phoneLoginFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockAuthClient) ValidateUser(ctx context.Context, req *pb.ValidateUserRequest, opts ...grpc.CallOption) (*pb.ValidateUserResponse, error) { return nil, nil }
func (m *mockAuthClient) SendOTP(ctx context.Context, req *pb.SendOTPRequest, opts ...grpc.CallOption) (*pb.SendOTPResponse, error) { return nil, nil }
func (m *mockAuthClient) BindPhone(ctx context.Context, req *pb.BindPhoneRequest, opts ...grpc.CallOption) (*pb.BindPhoneResponse, error) { return nil, nil }
func (m *mockAuthClient) UpdateProfile(ctx context.Context, req *pb.UpdateProfileRequest, opts ...grpc.CallOption) (*pb.UpdateProfileResponse, error) { return nil, nil }

type mockSheetClient struct {
	createFn func(context.Context, *pb.CreateSpreadsheetRequest, ...grpc.CallOption) (*pb.CreateSpreadsheetResponse, error)
	getFn    func(context.Context, *pb.GetSpreadsheetRequest, ...grpc.CallOption) (*pb.GetSpreadsheetResponse, error)
	listFn   func(context.Context, *pb.ListSpreadsheetsRequest, ...grpc.CallOption) (*pb.ListSpreadsheetsResponse, error)
	updateFn func(context.Context, *pb.UpdateSpreadsheetRequest, ...grpc.CallOption) (*pb.UpdateSpreadsheetResponse, error)
	deleteFn func(context.Context, *pb.DeleteSpreadsheetRequest, ...grpc.CallOption) (*pb.DeleteSpreadsheetResponse, error)
}

func (m *mockSheetClient) CreateSpreadsheet(ctx context.Context, req *pb.CreateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.CreateSpreadsheetResponse, error) {
	if m.createFn != nil { return m.createFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockSheetClient) GetSpreadsheet(ctx context.Context, req *pb.GetSpreadsheetRequest, opts ...grpc.CallOption) (*pb.GetSpreadsheetResponse, error) {
	if m.getFn != nil { return m.getFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockSheetClient) ListSpreadsheets(ctx context.Context, req *pb.ListSpreadsheetsRequest, opts ...grpc.CallOption) (*pb.ListSpreadsheetsResponse, error) {
	if m.listFn != nil { return m.listFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockSheetClient) UpdateSpreadsheet(ctx context.Context, req *pb.UpdateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.UpdateSpreadsheetResponse, error) {
	if m.updateFn != nil { return m.updateFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockSheetClient) DeleteSpreadsheet(ctx context.Context, req *pb.DeleteSpreadsheetRequest, opts ...grpc.CallOption) (*pb.DeleteSpreadsheetResponse, error) {
	if m.deleteFn != nil { return m.deleteFn(ctx, req, opts...) }
	return nil, nil
}

type mockShareClient struct {
	shareFn       func(context.Context, *pb.ShareRequest, ...grpc.CallOption) (*pb.ShareResponse, error)
	revokeFn      func(context.Context, *pb.RevokeRequest, ...grpc.CallOption) (*pb.RevokeResponse, error)
	listSharesFn  func(context.Context, *pb.ResourceRequest, ...grpc.CallOption) (*pb.ShareListResponse, error)
	createLinkFn  func(context.Context, *pb.ShareLinkRequest, ...grpc.CallOption) (*pb.ShareLinkResponse, error)
	getByTokenFn  func(context.Context, *pb.ShareTokenRequest, ...grpc.CallOption) (*pb.SharedResourceResponse, error)
}

func (m *mockShareClient) Share(ctx context.Context, req *pb.ShareRequest, opts ...grpc.CallOption) (*pb.ShareResponse, error) {
	if m.shareFn != nil { return m.shareFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockShareClient) Revoke(ctx context.Context, req *pb.RevokeRequest, opts ...grpc.CallOption) (*pb.RevokeResponse, error) {
	if m.revokeFn != nil { return m.revokeFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockShareClient) ListShares(ctx context.Context, req *pb.ResourceRequest, opts ...grpc.CallOption) (*pb.ShareListResponse, error) {
	if m.listSharesFn != nil { return m.listSharesFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockShareClient) CreateShareLink(ctx context.Context, req *pb.ShareLinkRequest, opts ...grpc.CallOption) (*pb.ShareLinkResponse, error) {
	if m.createLinkFn != nil { return m.createLinkFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockShareClient) GetByToken(ctx context.Context, req *pb.ShareTokenRequest, opts ...grpc.CallOption) (*pb.SharedResourceResponse, error) {
	if m.getByTokenFn != nil { return m.getByTokenFn(ctx, req, opts...) }
	return nil, nil
}

type mockFileClient struct {
	createFn func(context.Context, *pb.CreateFileRequest, ...grpc.CallOption) (*pb.CreateFileResponse, error)
	getFn    func(context.Context, *pb.GetFileRequest, ...grpc.CallOption) (*pb.GetFileResponse, error)
	deleteFn func(context.Context, *pb.DeleteFileRequest, ...grpc.CallOption) (*pb.DeleteFileResponse, error)
	listFn   func(context.Context, *pb.ListFilesRequest, ...grpc.CallOption) (*pb.ListFilesResponse, error)
	moveFn   func(context.Context, *pb.MoveFileRequest, ...grpc.CallOption) (*pb.MoveFileResponse, error)
	folderFn func(context.Context, *pb.CreateFolderRequest, ...grpc.CallOption) (*pb.CreateFolderResponse, error)
}

func (m *mockFileClient) CreateFile(ctx context.Context, req *pb.CreateFileRequest, opts ...grpc.CallOption) (*pb.CreateFileResponse, error) {
	if m.createFn != nil { return m.createFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockFileClient) GetFile(ctx context.Context, req *pb.GetFileRequest, opts ...grpc.CallOption) (*pb.GetFileResponse, error) {
	if m.getFn != nil { return m.getFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockFileClient) DeleteFile(ctx context.Context, req *pb.DeleteFileRequest, opts ...grpc.CallOption) (*pb.DeleteFileResponse, error) {
	if m.deleteFn != nil { return m.deleteFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockFileClient) ListFiles(ctx context.Context, req *pb.ListFilesRequest, opts ...grpc.CallOption) (*pb.ListFilesResponse, error) {
	if m.listFn != nil { return m.listFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockFileClient) MoveFile(ctx context.Context, req *pb.MoveFileRequest, opts ...grpc.CallOption) (*pb.MoveFileResponse, error) {
	if m.moveFn != nil { return m.moveFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockFileClient) CreateFolder(ctx context.Context, req *pb.CreateFolderRequest, opts ...grpc.CallOption) (*pb.CreateFolderResponse, error) {
	if m.folderFn != nil { return m.folderFn(ctx, req, opts...) }
	return nil, nil
}
func (m *mockFileClient) BatchDelete(ctx context.Context, req *pb.BatchDeleteRequest, opts ...grpc.CallOption) (*pb.BatchDeleteResponse, error) { return nil, nil }

type mockSearchClient struct {
	searchFn func(context.Context, *pb.SearchRequest, ...grpc.CallOption) (*pb.SearchResponse, error)
}

func (m *mockSearchClient) Search(ctx context.Context, req *pb.SearchRequest, opts ...grpc.CallOption) (*pb.SearchResponse, error) {
	if m.searchFn != nil { return m.searchFn(ctx, req, opts...) }
	return nil, nil
}

// ---- test helpers ----

func newTestHandlers() *Handlers {
	return &Handlers{JWTSecret: "test"}
}

func setupGin() *gin.Engine {
	gin.SetMode(gin.TestMode)
	return gin.New()
}

// ---- Auth handler tests ----

func TestLoginSuccess(t *testing.T) {
	h := newTestHandlers()
	h.Auth = &mockAuthClient{
		loginFn: func(ctx context.Context, req *pb.LoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error) {
			return &pb.LoginResponse{Success: true, AccessToken: "tok", RefreshToken: "rt"}, nil
		},
	}

	r := setupGin()
	r.POST("/login", h.Login)

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/login", strings.NewReader(`{"username":"test","password":"123456"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String()) }
}

func TestLoginWrongPassword(t *testing.T) {
	h := newTestHandlers()
	h.Auth = &mockAuthClient{
		loginFn: func(ctx context.Context, req *pb.LoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error) {
			return &pb.LoginResponse{Success: false, Error: "Invalid credentials"}, nil
		},
	}

	r := setupGin()
	r.POST("/login", h.Login)

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/login", strings.NewReader(`{"username":"test","password":"wrong"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized { t.Fatalf("expected 401, got %d", w.Code) }
}

func TestLoginValidation(t *testing.T) {
	tests := []struct {
		name string
		body string
		code int
	}{
		{"empty username", `{"username":"","password":"x"}`, 400},
		{"empty password", `{"username":"x","password":""}`, 400},
		{"empty body", `{}`, 400},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			h := newTestHandlers()
			r := setupGin()
			r.POST("/login", h.Login)
			w := httptest.NewRecorder()
			req, _ := http.NewRequest("POST", "/login", strings.NewReader(tt.body))
			req.Header.Set("Content-Type", "application/json")
			r.ServeHTTP(w, req)
			if w.Code != tt.code { t.Errorf("expected %d, got %d", tt.code, w.Code) }
		})
	}
}

func TestLoginGrpcUnavailable(t *testing.T) {
	h := newTestHandlers()
	h.Auth = &mockAuthClient{
		loginFn: func(ctx context.Context, req *pb.LoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error) {
			return nil, status.Error(codes.Unavailable, "backend down")
		},
	}

	r := setupGin()
	r.POST("/login", h.Login)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/login", strings.NewReader(`{"username":"test","password":"123456"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusServiceUnavailable { t.Fatalf("expected 503, got %d", w.Code) }
}

func TestRegisterSuccess(t *testing.T) {
	h := newTestHandlers()
	h.Auth = &mockAuthClient{
		registerFn: func(ctx context.Context, req *pb.RegisterRequest, opts ...grpc.CallOption) (*pb.RegisterResponse, error) {
			return &pb.RegisterResponse{Success: true, AccessToken: "tok", RefreshToken: "rt"}, nil
		},
	}

	r := setupGin()
	r.POST("/register", h.Register)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/register", strings.NewReader(`{"username":"tester","password":"abcdef"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

func TestRegisterUsernameTooShort(t *testing.T) {
	h := newTestHandlers()
	r := setupGin()
	r.POST("/register", h.Register)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/register", strings.NewReader(`{"username":"ab","password":"abcdef"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusBadRequest { t.Fatalf("expected 400, got %d", w.Code) }
}

func TestChangePasswordUnauthorized(t *testing.T) {
	h := newTestHandlers()
	r := setupGin()
	r.PUT("/me/password", h.ChangePassword)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("PUT", "/me/password", strings.NewReader(`{"old_password":"o","new_password":"new1234"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized { t.Fatalf("expected 401, got %d", w.Code) }
}

func TestChangePasswordSuccess(t *testing.T) {
	h := newTestHandlers()
	h.Auth = &mockAuthClient{
		chgPwdFn: func(ctx context.Context, req *pb.ChangePasswordRequest, opts ...grpc.CallOption) (*pb.ChangePasswordResponse, error) {
			return &pb.ChangePasswordResponse{Success: true}, nil
		},
	}
	r := setupGin()
	r.PUT("/me/password", func(c *gin.Context) {
		c.Set("uid", int64(42))
		h.ChangePassword(c)
	})
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("PUT", "/me/password", strings.NewReader(`{"old_password":"o","new_password":"new1234"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String()) }
}

// ---- Sheet handler tests ----

func TestSheetCreateSuccess(t *testing.T) {
	h := newTestHandlers()
	h.Sheet = &mockSheetClient{
		createFn: func(ctx context.Context, req *pb.CreateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.CreateSpreadsheetResponse, error) {
			return &pb.CreateSpreadsheetResponse{Success: true, Id: 1}, nil
		},
	}
	r := setupGin()
	r.POST("/sheets", h.CreateSheet)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/sheets", strings.NewReader(`{"name":"Test","headers_json":"[]","data_json":"[]"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String()) }
}

func TestSheetCreateValidation(t *testing.T) {
	h := newTestHandlers()
	r := setupGin()
	r.POST("/sheets", h.CreateSheet)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/sheets", strings.NewReader(`{"name":"","headers_json":"[]","data_json":"[]"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusBadRequest { t.Fatalf("expected 400, got %d", w.Code) }
}

func TestSheetGetSuccess(t *testing.T) {
	h := newTestHandlers()
	h.Sheet = &mockSheetClient{
		getFn: func(ctx context.Context, req *pb.GetSpreadsheetRequest, opts ...grpc.CallOption) (*pb.GetSpreadsheetResponse, error) {
			return &pb.GetSpreadsheetResponse{Success: true, Spreadsheet: &pb.Spreadsheet{Id: 1, Name: "Test"}}, nil
		},
	}
	r := setupGin()
	r.GET("/sheets/:id", h.GetSheet)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/sheets/1", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

func TestSheetGetNotFound(t *testing.T) {
	h := newTestHandlers()
	h.Sheet = &mockSheetClient{
		getFn: func(ctx context.Context, req *pb.GetSpreadsheetRequest, opts ...grpc.CallOption) (*pb.GetSpreadsheetResponse, error) {
			return nil, status.Error(codes.NotFound, "not found")
		},
	}
	r := setupGin()
	r.GET("/sheets/:id", h.GetSheet)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/sheets/999", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusNotFound { t.Fatalf("expected 404, got %d", w.Code) }
}

func TestSheetDeleteSuccess(t *testing.T) {
	h := newTestHandlers()
	h.Sheet = &mockSheetClient{
		deleteFn: func(ctx context.Context, req *pb.DeleteSpreadsheetRequest, opts ...grpc.CallOption) (*pb.DeleteSpreadsheetResponse, error) {
			return &pb.DeleteSpreadsheetResponse{Success: true}, nil
		},
	}
	r := setupGin()
	r.DELETE("/sheets/:id", h.DeleteSheet)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("DELETE", "/sheets/1", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

// ---- Share handler tests ----

func TestShareSheetSuccess(t *testing.T) {
	h := newTestHandlers()
	h.Share = &mockShareClient{
		shareFn: func(ctx context.Context, req *pb.ShareRequest, opts ...grpc.CallOption) (*pb.ShareResponse, error) {
			return &pb.ShareResponse{Success: true}, nil
		},
	}
	r := setupGin()
	r.POST("/sheets/:id/share", func(c *gin.Context) {
		c.Set("uid", int64(42))
		h.ShareSheet(c)
	})
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/sheets/1/share", strings.NewReader(`{"username":"bob","permission":"view"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String()) }
}

func TestRevokeShareSuccess(t *testing.T) {
	h := newTestHandlers()
	h.Share = &mockShareClient{
		revokeFn: func(ctx context.Context, req *pb.RevokeRequest, opts ...grpc.CallOption) (*pb.RevokeResponse, error) {
			return &pb.RevokeResponse{Success: true}, nil
		},
	}
	r := setupGin()
	r.DELETE("/sheets/:id/share/:username", func(c *gin.Context) {
		c.Set("uid", int64(42))
		h.RevokeShare(c)
	})
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("DELETE", "/sheets/1/share/bob", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

// ---- File handler tests ----

func TestFileCreateFolderSuccess(t *testing.T) {
	h := newTestHandlers()
	h.File = &mockFileClient{
		folderFn: func(ctx context.Context, req *pb.CreateFolderRequest, opts ...grpc.CallOption) (*pb.CreateFolderResponse, error) {
			return &pb.CreateFolderResponse{Success: true}, nil
		},
	}
	r := setupGin()
	r.POST("/files/folder", h.CreateFolder)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/files/folder", strings.NewReader(`{"name":"docs","parent_folder_id":0}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

func TestFileDeleteSuccess(t *testing.T) {
	h := newTestHandlers()
	h.File = &mockFileClient{
		deleteFn: func(ctx context.Context, req *pb.DeleteFileRequest, opts ...grpc.CallOption) (*pb.DeleteFileResponse, error) {
			return &pb.DeleteFileResponse{Success: true}, nil
		},
	}
	r := setupGin()
	r.DELETE("/files/:id", h.DeleteFile)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("DELETE", "/files/1", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

func TestFileGetNotFound(t *testing.T) {
	h := newTestHandlers()
	h.File = &mockFileClient{
		getFn: func(ctx context.Context, req *pb.GetFileRequest, opts ...grpc.CallOption) (*pb.GetFileResponse, error) {
			return nil, status.Error(codes.NotFound, "not found")
		},
	}
	r := setupGin()
	r.GET("/files/:id", h.GetFile)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/files/999", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusNotFound { t.Fatalf("expected 404, got %d", w.Code) }
}

func TestFileListSuccess(t *testing.T) {
	h := newTestHandlers()
	h.File = &mockFileClient{
		listFn: func(ctx context.Context, req *pb.ListFilesRequest, opts ...grpc.CallOption) (*pb.ListFilesResponse, error) {
			return &pb.ListFilesResponse{Success: true}, nil
		},
	}
	r := setupGin()
	r.GET("/files", h.ListFiles)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/files", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

// ---- Search handler tests ----

func TestSearchSuccess(t *testing.T) {
	h := newTestHandlers()
	h.SearchClient = &mockSearchClient{
		searchFn: func(ctx context.Context, req *pb.SearchRequest, opts ...grpc.CallOption) (*pb.SearchResponse, error) {
			return &pb.SearchResponse{Success: true}, nil
		},
	}
	r := setupGin()
	r.POST("/search", h.Search)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/search", strings.NewReader(`{"q":"test"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

func TestSearchEmptyQuery(t *testing.T) {
	h := newTestHandlers()
	r := setupGin()
	r.POST("/search", h.Search)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/search", strings.NewReader(`{"q":""}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusBadRequest { t.Fatalf("expected 400, got %d", w.Code) }
}

// ---- Error mapping tests ----

func TestGrpcNotFoundReturns404(t *testing.T) {
	h := newTestHandlers()
	h.Sheet = &mockSheetClient{
		getFn: func(ctx context.Context, req *pb.GetSpreadsheetRequest, opts ...grpc.CallOption) (*pb.GetSpreadsheetResponse, error) {
			return nil, status.Error(codes.NotFound, "sheet not found")
		},
	}
	r := setupGin()
	r.GET("/sheets/:id", h.GetSheet)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/sheets/999", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusNotFound { t.Fatalf("expected 404, got %d: %s", w.Code, w.Body.String()) }
}

func TestGrpcPermissionDeniedReturns403(t *testing.T) {
	h := newTestHandlers()
	h.Sheet = &mockSheetClient{
		updateFn: func(ctx context.Context, req *pb.UpdateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.UpdateSpreadsheetResponse, error) {
			return nil, status.Error(codes.PermissionDenied, "access denied")
		},
	}
	r := setupGin()
	r.PUT("/sheets/:id", h.UpdateSheet)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("PUT", "/sheets/1", strings.NewReader(`{"name":"x"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden { t.Fatalf("expected 403, got %d", w.Code) }
}

func TestGrpcUnauthenticatedReturns401(t *testing.T) {
	h := newTestHandlers()
	h.Auth = &mockAuthClient{
		loginFn: func(ctx context.Context, req *pb.LoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error) {
			return nil, status.Error(codes.Unauthenticated, "invalid token")
		},
	}
	r := setupGin()
	r.POST("/login", h.Login)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/login", strings.NewReader(`{"username":"test","password":"123456"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized { t.Fatalf("expected 401, got %d", w.Code) }
}

func TestGrpcInvalidArgumentReturns400(t *testing.T) {
	h := newTestHandlers()
	h.Sheet = &mockSheetClient{
		createFn: func(ctx context.Context, req *pb.CreateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.CreateSpreadsheetResponse, error) {
			return nil, status.Error(codes.InvalidArgument, "bad data")
		},
	}
	r := setupGin()
	r.POST("/sheets", h.CreateSheet)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/sheets", strings.NewReader(`{"name":"Test","headers_json":"[]","data_json":"[]"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusBadRequest { t.Fatalf("expected 400, got %d", w.Code) }
}

func TestGrpcDeadlineExceededReturns504(t *testing.T) {
	h := newTestHandlers()
	h.SearchClient = &mockSearchClient{
		searchFn: func(ctx context.Context, req *pb.SearchRequest, opts ...grpc.CallOption) (*pb.SearchResponse, error) {
			return nil, status.Error(codes.DeadlineExceeded, "timeout")
		},
	}
	r := setupGin()
	r.POST("/search", h.Search)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/search", strings.NewReader(`{"q":"test"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusGatewayTimeout { t.Fatalf("expected 504, got %d", w.Code) }
}

// ---- Health / misc tests ----

func TestHealthOK(t *testing.T) {
	h := newTestHandlers()
	r := setupGin()
	r.GET("/health", h.Health)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/health", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

func TestMeUnauthenticated(t *testing.T) {
	h := newTestHandlers()
	r := setupGin()
	r.GET("/me", h.Me)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/me", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

func TestServicesOK(t *testing.T) {
	h := newTestHandlers()
	r := setupGin()
	r.GET("/services", h.Services)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/services", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

// ---- mock points client ----

type mockPointsClient struct {
	balanceFn      func(context.Context, *pb.GetBalanceRequest, ...grpc.CallOption) (*pb.BalanceResponse, error)
	transactionsFn func(context.Context, *pb.GetTransactionsRequest, ...grpc.CallOption) (*pb.TransactionsResponse, error)
	earnFn         func(context.Context, *pb.EarnRequest, ...grpc.CallOption) (*pb.BalanceResponse, error)
	deductFn       func(context.Context, *pb.DeductRequest, ...grpc.CallOption) (*pb.BalanceResponse, error)
}

func (m *mockPointsClient) GetBalance(ctx context.Context, req *pb.GetBalanceRequest, opts ...grpc.CallOption) (*pb.BalanceResponse, error) {
	if m.balanceFn != nil { return m.balanceFn(ctx, req, opts...) }
	return &pb.BalanceResponse{Success: true, Balance: 0}, nil
}
func (m *mockPointsClient) GetTransactions(ctx context.Context, req *pb.GetTransactionsRequest, opts ...grpc.CallOption) (*pb.TransactionsResponse, error) {
	if m.transactionsFn != nil { return m.transactionsFn(ctx, req, opts...) }
	return &pb.TransactionsResponse{Success: true}, nil
}
func (m *mockPointsClient) Earn(ctx context.Context, req *pb.EarnRequest, opts ...grpc.CallOption) (*pb.BalanceResponse, error) {
	if m.earnFn != nil { return m.earnFn(ctx, req, opts...) }
	return &pb.BalanceResponse{Success: true, Balance: req.Amount}, nil
}
func (m *mockPointsClient) Deduct(ctx context.Context, req *pb.DeductRequest, opts ...grpc.CallOption) (*pb.BalanceResponse, error) {
	if m.deductFn != nil { return m.deductFn(ctx, req, opts...) }
	return &pb.BalanceResponse{Success: true, Balance: 0}, nil
}
func (m *mockPointsClient) GetLeaderboard(ctx context.Context, req *pb.LeaderboardRequest, opts ...grpc.CallOption) (*pb.LeaderboardResponse, error) {
	return &pb.LeaderboardResponse{Success: true}, nil
}

// ---- Points handler tests ----

func TestGetBalance(t *testing.T) {
	h := newTestHandlers()
	h.Points = &mockPointsClient{
		balanceFn: func(ctx context.Context, req *pb.GetBalanceRequest, opts ...grpc.CallOption) (*pb.BalanceResponse, error) {
			return &pb.BalanceResponse{Success: true, UserId: 42, Balance: 100, TotalEarned: 250}, nil
		},
	}

	r := setupGin()
	r.GET("/points/balance", h.GetBalance)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/points/balance", nil)
	req.Header.Set("X-Request-ID", "test")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String()) }
}

func TestGetTransactions(t *testing.T) {
	h := newTestHandlers()
	h.Points = &mockPointsClient{
		transactionsFn: func(ctx context.Context, req *pb.GetTransactionsRequest, opts ...grpc.CallOption) (*pb.TransactionsResponse, error) {
			return &pb.TransactionsResponse{
				Success: true,
				Transactions: []*pb.Transaction{
					{Id: 1, Type: "earn", Amount: 10, Reason: "daily_login"},
					{Id: 2, Type: "deduct", Amount: -5, Reason: "seckill_order"},
				},
			}, nil
		},
	}

	r := setupGin()
	r.GET("/points/transactions", h.GetTransactions)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/points/transactions", nil)
	req.Header.Set("X-Request-ID", "test")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

func TestGetLeaderboard(t *testing.T) {
	h := newTestHandlers()
	h.Points = &mockPointsClient{}

	r := setupGin()
	r.GET("/points/leaderboard", h.GetLeaderboard)
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/points/leaderboard", nil)
	req.Header.Set("X-Request-ID", "test")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK { t.Fatalf("expected 200, got %d", w.Code) }
}

// ---- Points event publishing tests ----

func TestPublishPointEventNilRedis(t *testing.T) {
	h := newTestHandlers()
	// RDB is nil — should not panic
	h.publishPointEvent(42, "user.logged_in", "test-key")
}

func TestPublishPointEventZeroUID(t *testing.T) {
	h := newTestHandlers()
	// uid=0 — should not publish
	h.publishPointEvent(0, "user.logged_in", "test-key")
}
