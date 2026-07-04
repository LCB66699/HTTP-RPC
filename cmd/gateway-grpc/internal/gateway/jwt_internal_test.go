package gateway

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"
)

func TestVerifyJWTValid(t *testing.T) {
	SetJWTSecret("test-secret-32bytes-here-abcdef!")

	claims := jwt.MapClaims{
		"username": "tester",
		"uid":      float64(12345),
		"role":     "user",
		"exp":      float64(time.Now().Add(15 * time.Minute).Unix()),
	}
	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	tokenStr, err := token.SignedString([]byte("test-secret-32bytes-here-abcdef!"))
	if err != nil {
		t.Fatalf("sign failed: %v", err)
	}

	ac, err := verifyJWT(tokenStr)
	if err != nil {
		t.Fatalf("verifyJWT failed: %v", err)
	}
	if ac.UserID != 12345 {
		t.Fatalf("expected uid 12345, got %d", ac.UserID)
	}
	if ac.Username != "tester" {
		t.Fatalf("expected username tester, got %s", ac.Username)
	}
	if ac.Role != "user" {
		t.Fatalf("expected role user, got %s", ac.Role)
	}
}

func TestVerifyJWTWrongSecret(t *testing.T) {
	SetJWTSecret("correct-secret-32bytes-here!!!")

	claims := jwt.MapClaims{"uid": float64(1)}
	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	tokenStr, _ := token.SignedString([]byte("wrong-secret-32bytes-here!!!!!"))

	_, err := verifyJWT(tokenStr)
	if err == nil {
		t.Fatal("expected error for wrong secret")
	}
}

func TestVerifyJWTExpired(t *testing.T) {
	SetJWTSecret("test-secret-32bytes-here-abcdef!")

	claims := jwt.MapClaims{
		"exp": float64(time.Now().Add(-1 * time.Hour).Unix()),
	}
	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	tokenStr, _ := token.SignedString([]byte("test-secret-32bytes-here-abcdef!"))

	_, err := verifyJWT(tokenStr)
	if err == nil {
		t.Fatal("expected error for expired token")
	}
}

func TestVerifyJWTMalformed(t *testing.T) {
	SetJWTSecret("test-secret-32bytes-here-abcdef!")

	_, err := verifyJWT("not.a.jwt")
	if err == nil {
		t.Fatal("expected error for malformed token")
	}
}

func TestAuthMiddlewareSetsHeaders(t *testing.T) {
	SetJWTSecret("test-secret-32bytes-here-abcdef!")

	claims := jwt.MapClaims{
		"username": "alice",
		"uid":      float64(42),
		"exp":      float64(time.Now().Add(15 * time.Minute).Unix()),
	}
	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	tokenStr, _ := token.SignedString([]byte("test-secret-32bytes-here-abcdef!"))

	// Create a test request with the cookie
	req, _ := http.NewRequest("GET", "/", nil)
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: tokenStr})

	var headers http.Header
	var ac *AuthContext
	rec := httptest.NewRecorder()
	middleware := AuthMiddleware(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		headers = r.Header.Clone()
		ac = GetAuthContext(r.Context())
	}))
	middleware.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200 OK, got %d", rec.Code)
	}
	if headers.Get("X-Rpc-Uid") != "42" {
		t.Fatalf("expected X-Rpc-Uid=42, got %q", headers.Get("X-Rpc-Uid"))
	}
	if headers.Get("X-Rpc-Username") != "alice" {
		t.Fatalf("expected X-Rpc-Username=alice, got %q", headers.Get("X-Rpc-Username"))
	}
	if ac == nil {
		t.Fatal("expected AuthContext in request context")
	}
	if ac.UserID != 42 || ac.Username != "alice" {
		t.Fatalf("unexpected auth context: %+v", ac)
	}
}

func TestAuthMiddlewareMissingCookie(t *testing.T) {
	SetJWTSecret("test-secret-32bytes-here-abcdef!")

	req, _ := http.NewRequest("GET", "/api/v1/me", nil)
	rec := httptest.NewRecorder()
	middleware := AuthMiddleware(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		t.Fatal("next handler should not be called")
	}))
	middleware.ServeHTTP(rec, req)

	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401, got %d", rec.Code)
	}
}

func TestAuthMiddlewareOverridesHeader(t *testing.T) {
	SetJWTSecret("test-secret-32bytes-here-abcdef!")

	claims := jwt.MapClaims{
		"username": "bob",
		"uid":      float64(99),
		"exp":      float64(time.Now().Add(15 * time.Minute).Unix()),
	}
	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	tokenStr, _ := token.SignedString([]byte("test-secret-32bytes-here-abcdef!"))

	// Request with forged X-Rpc-Uid header AND valid cookie
	req, _ := http.NewRequest("GET", "/", nil)
	req.Header.Set("X-Rpc-Uid", "99999")
	req.Header.Set("X-Rpc-Username", "attacker")
	req.AddCookie(&http.Cookie{Name: "rpc_at", Value: tokenStr})

	var headers http.Header
	rec := httptest.NewRecorder()
	middleware := AuthMiddleware(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		headers = r.Header.Clone()
	}))
	middleware.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200 OK, got %d", rec.Code)
	}

	// Middleware should overwrite forged headers with verified values
	if headers.Get("X-Rpc-Uid") != "99" {
		t.Fatalf("expected X-Rpc-Uid=99 (overridden), got %q", headers.Get("X-Rpc-Uid"))
	}
	if headers.Get("X-Rpc-Username") != "bob" {
		t.Fatalf("expected X-Rpc-Username=bob (overridden), got %q", headers.Get("X-Rpc-Username"))
	}
}
