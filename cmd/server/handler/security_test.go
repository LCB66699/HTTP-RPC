package handler

import (
	"bytes"
	"context"
	"mime/multipart"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/alicebob/miniredis/v2"
	"github.com/gin-gonic/gin"
	"github.com/redis/go-redis/v9"
	pb "gateway-grpc/gen/rpc"
	"google.golang.org/grpc"
)

func multipartUpload(t *testing.T, contents string) (*bytes.Buffer, string) {
	t.Helper()
	var body bytes.Buffer
	w := multipart.NewWriter(&body)
	part, err := w.CreateFormFile("file", "test.txt")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := part.Write([]byte(contents)); err != nil {
		t.Fatal(err)
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	return &body, w.FormDataContentType()
}

func TestUploadFileUsesDefaultLimit(t *testing.T) {
	h := newTestHandlers()
	if got := h.maxUploadBytes(); got != 50*1024*1024 {
		t.Fatalf("expected 50 MiB default, got %d", got)
	}
}

func TestUploadFileRejectsRequestOverConfiguredLimit(t *testing.T) {
	called := false
	h := newTestHandlers()
	h.MaxUploadBytes = 128
	h.File = &mockFileClient{createFn: func(context.Context, *pb.CreateFileRequest, ...grpc.CallOption) (*pb.CreateFileResponse, error) {
		called = true
		return &pb.CreateFileResponse{Success: true}, nil
	}}
	body, contentType := multipartUpload(t, strings.Repeat("x", 256))
	r := setupGin()
	r.POST("/files/upload", h.UploadFile)
	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/files/upload", body)
	req.Header.Set("Content-Type", contentType)
	r.ServeHTTP(w, req)

	if w.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("expected 413, got %d: %s", w.Code, w.Body.String())
	}
	if called {
		t.Fatal("file backend must not be called for an oversized request")
	}
}

func TestUploadFileRejectsMalformedMultipart(t *testing.T) {
	h := newTestHandlers()
	r := setupGin()
	r.POST("/files/upload", h.UploadFile)
	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/files/upload", strings.NewReader("broken"))
	req.Header.Set("Content-Type", "multipart/form-data; boundary=missing")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d", w.Code)
	}
}

func setupOTPHandlers(t *testing.T) (*Handlers, *miniredis.Miniredis) {
	t.Helper()
	mr := miniredis.RunT(t)
	rdb := redis.NewClient(&redis.Options{Addr: mr.Addr()})
	t.Cleanup(func() { _ = rdb.Close() })
	return &Handlers{RDB: rdb, Auth: &mockAuthClient{}}, mr
}

func performJSON(r http.Handler, method, path, body, remoteAddr string) *httptest.ResponseRecorder {
	w := httptest.NewRecorder()
	req := httptest.NewRequest(method, path, strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.RemoteAddr = remoteAddr
	r.ServeHTTP(w, req)
	return w
}

func TestOTPSendValidatesPhone(t *testing.T) {
	h, _ := setupOTPHandlers(t)
	r := setupGin()
	r.POST("/otp", h.OTPSend)
	w := performJSON(r, http.MethodPost, "/otp", `{"phone":"123"}`, "192.0.2.1:1000")
	if w.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d", w.Code)
	}
}

func TestOTPSendLimitsEachPhone(t *testing.T) {
	h, _ := setupOTPHandlers(t)
	r := setupGin()
	r.POST("/otp", h.OTPSend)
	for i := 0; i < otpSendPhoneLimit; i++ {
		w := performJSON(r, http.MethodPost, "/otp", `{"phone":"+8613800138000"}`, "192.0.2.1:1000")
		if w.Code != http.StatusOK {
			t.Fatalf("request %d: expected 200, got %d", i+1, w.Code)
		}
	}
	w := performJSON(r, http.MethodPost, "/otp", `{"phone":"+8613800138000"}`, "192.0.2.2:1000")
	if w.Code != http.StatusTooManyRequests {
		t.Fatalf("expected 429, got %d", w.Code)
	}
}

func TestOTPSendLimitsEachSourceIP(t *testing.T) {
	h, _ := setupOTPHandlers(t)
	r := setupGin()
	r.POST("/otp", h.OTPSend)
	for i := 0; i < otpSendIPLimit; i++ {
		// Use distinct valid numbers while retaining one source address.
		phone := "+861390000000" + string(rune('0'+i))
		w := performJSON(r, http.MethodPost, "/otp", `{"phone":"`+phone+`"}`, "192.0.2.10:1000")
		if w.Code != http.StatusOK {
			t.Fatalf("request %d: expected 200, got %d for %s", i+1, w.Code, phone)
		}
	}
	w := performJSON(r, http.MethodPost, "/otp", `{"phone":"+8613800138999"}`, "192.0.2.10:1000")
	if w.Code != http.StatusTooManyRequests {
		t.Fatalf("expected 429, got %d", w.Code)
	}
}

func TestOTPRedisFailureFailsClosed(t *testing.T) {
	rdb := redis.NewClient(&redis.Options{
		Addr:        "127.0.0.1:1",
		DialTimeout: 10 * time.Millisecond,
		ReadTimeout: 10 * time.Millisecond,
		MaxRetries:  0,
	})
	t.Cleanup(func() { _ = rdb.Close() })
	h := &Handlers{RDB: rdb, Auth: &mockAuthClient{}}
	r := setupGin()
	r.POST("/otp", h.OTPSend)
	w := performJSON(r, http.MethodPost, "/otp", `{"phone":"+8613800138000"}`, "192.0.2.1:1000")
	if w.Code != http.StatusServiceUnavailable {
		t.Fatalf("expected 503, got %d", w.Code)
	}
}

func TestPhoneLoginLimitsVerificationFailures(t *testing.T) {
	h, _ := setupOTPHandlers(t)
	r := setupGin()
	r.POST("/phone-login", h.PhoneLogin)
	for i := 0; i < otpVerifyFailureLimit; i++ {
		w := performJSON(r, http.MethodPost, "/phone-login", `{"phone":"+8613800138000","otp":"000000"}`, "192.0.2.1:1000")
		if w.Code != http.StatusUnauthorized {
			t.Fatalf("attempt %d: expected 401, got %d", i+1, w.Code)
		}
	}
	w := performJSON(r, http.MethodPost, "/phone-login", `{"phone":"+8613800138000","otp":"000000"}`, "192.0.2.1:1000")
	if w.Code != http.StatusTooManyRequests {
		t.Fatalf("expected 429, got %d", w.Code)
	}
}

func TestParseIDRejectsInvalidAndNonPositiveValues(t *testing.T) {
	for _, value := range []string{"abc", "0", "-1", ""} {
		t.Run(value, func(t *testing.T) {
			w := httptest.NewRecorder()
			c, _ := gin.CreateTestContext(w)
			c.Params = gin.Params{{Key: "id", Value: value}}
			if got := parseID(c); got != 0 {
				t.Fatalf("expected zero sentinel, got %d", got)
			}
			if w.Code != http.StatusBadRequest || !c.IsAborted() {
				t.Fatalf("expected aborted 400, got status=%d aborted=%v", w.Code, c.IsAborted())
			}
		})
	}
}

func TestParseIDAcceptsPositiveValue(t *testing.T) {
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Params = gin.Params{{Key: "id", Value: "42"}}
	if got := parseID(c); got != 42 || c.IsAborted() {
		t.Fatalf("expected id 42 without abort, got id=%d aborted=%v", got, c.IsAborted())
	}
}

func TestRemoteIPIgnoresForwardedHeader(t *testing.T) {
	req := httptest.NewRequest(http.MethodPost, "/", nil)
	req.RemoteAddr = "192.0.2.20:1234"
	req.Header.Set("X-Forwarded-For", "203.0.113.9")
	if got := sourceIP(req); got != net.ParseIP("192.0.2.20").String() {
		t.Fatalf("expected socket peer IP, got %q", got)
	}
}
