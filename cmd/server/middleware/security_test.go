package middleware

import (
	"reflect"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/gin-gonic/gin"
)

func TestParseOriginAllowlistKeepsOnlyHTTPOrigins(t *testing.T) {
	got := ParseOriginAllowlist(" https://app.example.com,*,https://bad.example/path,javascript:alert(1),http://localhost:3000 ")
	want := []string{"https://app.example.com", "http://localhost:3000"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %#v, want %#v", got, want)
	}
}

func TestCORSAllowsExactConfiguredOriginWithCredentials(t *testing.T) {
	gin.SetMode(gin.TestMode)
	r := gin.New()
	r.Use(CORSMiddleware([]string{"https://app.example.com"}))
	r.GET("/resource", func(c *gin.Context) { c.Status(http.StatusOK) })
	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/resource", nil)
	req.Header.Set("Origin", "https://app.example.com")
	r.ServeHTTP(w, req)

	if got := w.Header().Get("Access-Control-Allow-Origin"); got != "https://app.example.com" {
		t.Fatalf("unexpected allow origin %q", got)
	}
	if got := w.Header().Get("Access-Control-Allow-Credentials"); got != "true" {
		t.Fatalf("expected credentials=true, got %q", got)
	}
}

func TestCORSRejectsUnconfiguredAndPrefixOrigins(t *testing.T) {
	gin.SetMode(gin.TestMode)
	r := gin.New()
	r.Use(CORSMiddleware([]string{"https://app.example.com"}))
	r.GET("/resource", func(c *gin.Context) { c.Status(http.StatusOK) })
	for _, origin := range []string{"https://evil.example.com", "https://app.example.com.evil.test"} {
		w := httptest.NewRecorder()
		req := httptest.NewRequest(http.MethodGet, "/resource", nil)
		req.Header.Set("Origin", origin)
		r.ServeHTTP(w, req)
		if got := w.Header().Get("Access-Control-Allow-Origin"); got != "" {
			t.Fatalf("origin %q was unexpectedly allowed as %q", origin, got)
		}
	}
}

func TestPositiveIDParamRejectsInvalidValuesBeforeHandler(t *testing.T) {
	gin.SetMode(gin.TestMode)
	r := gin.New()
	r.Use(PositiveIDParam())
	called := false
	r.GET("/resources/:id", func(c *gin.Context) { called = true })
	for _, value := range []string{"abc", "0", "-1"} {
		called = false
		w := httptest.NewRecorder()
		r.ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/resources/"+value, nil))
		if w.Code != http.StatusBadRequest || called {
			t.Fatalf("id %q: expected 400 before handler, got status=%d called=%v", value, w.Code, called)
		}
	}
}
