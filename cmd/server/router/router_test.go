package router

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/lcb66699/http-rpc/server/handler"
	"github.com/lcb66699/http-rpc/server/middleware"
)

func testSecret() string { return "test-secret-32bytes-here-abcdef" }

func setupTestRouter() http.Handler {
	// Use a minimal handler with nil gRPC clients — tests only validate routing + middleware,
	// not actual gRPC calls. Public endpoints and auth enforcement can still be tested.
	h := &handler.Handlers{
		JWTSecret: testSecret(),
		CBSearch:  middleware.NewCBSlow("search", nil),
		CBSheet:   middleware.NewCBSlow("sheet", nil),
		CBFile:    middleware.NewCBSlow("file", nil),
	}

	r := Setup(h, testSecret())
	return r
}

func TestHealthEndpoint(t *testing.T) {
	r := setupTestRouter()
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/api/v1/health", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String())
	}
	var body map[string]string
	json.Unmarshal(w.Body.Bytes(), &body)
	if body["gateway"] != "READY" {
		t.Fatalf("expected gateway=READY, got %v", body)
	}
}

func TestMetricsEndpoint(t *testing.T) {
	r := setupTestRouter()
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/api/v1/metrics", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", w.Code)
	}
}

func TestAuthRequiredWithoutCookie(t *testing.T) {
	r := setupTestRouter()
	tests := []struct {
		method, path string
	}{
		{"GET", "/api/v1/me"},
		{"GET", "/api/v1/sheets"},
		{"POST", "/api/v1/sheets"},
		{"GET", "/api/v1/files"},
		{"POST", "/api/v1/search"},
		{"GET", "/api/v1/services"},
		{"GET", "/api/v1/history"},
		{"POST", "/api/v1/sheets/1/share"},
		{"POST", "/api/v1/files/upload"},
	}

	for _, tt := range tests {
		t.Run(tt.method+" "+tt.path, func(t *testing.T) {
			w := httptest.NewRecorder()
			req, _ := http.NewRequest(tt.method, tt.path, nil)
			r.ServeHTTP(w, req)

			if w.Code != http.StatusUnauthorized {
				t.Errorf("%s %s: expected 401, got %d", tt.method, tt.path, w.Code)
			}
			var body map[string]interface{}
			json.Unmarshal(w.Body.Bytes(), &body)
			if body["success"] != false {
				t.Errorf("%s %s: expected success=false", tt.method, tt.path)
			}
		})
	}
}

func TestPublicEndpointsNoAuth(t *testing.T) {
	r := setupTestRouter()
	tests := []struct {
		method, path string
		expectedCode int
	}{
		{"GET", "/api/v1/health", 200},
		{"GET", "/api/v1/health/ready", 200},
		{"GET", "/api/v1/metrics", 200},
		{"GET", "/api/v1/s/test-token", 404}, // not found, but not 401
	}

	for _, tt := range tests {
		t.Run(tt.method+" "+tt.path, func(t *testing.T) {
			w := httptest.NewRecorder()
			req, _ := http.NewRequest(tt.method, tt.path, nil)
			r.ServeHTTP(w, req)

			if w.Code != tt.expectedCode {
				t.Errorf("%s %s: expected %d, got %d", tt.method, tt.path, tt.expectedCode, w.Code)
			}
		})
	}
}

func TestRequestIDInResponse(t *testing.T) {
	r := setupTestRouter()
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/api/v1/health", nil)
	r.ServeHTTP(w, req)

	if w.Header().Get("X-Request-ID") == "" {
		t.Fatal("expected X-Request-ID in response header")
	}
}

func TestCORSMiddleware(t *testing.T) {
	r := setupTestRouter()
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("OPTIONS", "/api/v1/health", nil)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusNoContent {
		t.Fatalf("expected 204 for OPTIONS, got %d", w.Code)
	}
	if w.Header().Get("Access-Control-Allow-Origin") != "*" {
		t.Fatal("expected CORS header")
	}
}
