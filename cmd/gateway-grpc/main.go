package main

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/signal"
	"regexp"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/redis/go-redis/v9"
	"github.com/sony/gobreaker/v2"
	"go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc"
	"go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracegrpc"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.24.0"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/connectivity"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/keepalive"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"

	pb "gateway-grpc/gen/rpc"
)

var jwtSecret []byte

// ---- DI: 可注入的 gRPC client 接口 ----
type SheetClient interface {
	ListSpreadsheets(ctx context.Context, req *pb.ListSpreadsheetsRequest, opts ...grpc.CallOption) (*pb.ListSpreadsheetsResponse, error)
	GetSpreadsheet(ctx context.Context, req *pb.GetSpreadsheetRequest, opts ...grpc.CallOption) (*pb.GetSpreadsheetResponse, error)
	CreateSpreadsheet(ctx context.Context, req *pb.CreateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.CreateSpreadsheetResponse, error)
	UpdateSpreadsheet(ctx context.Context, req *pb.UpdateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.UpdateSpreadsheetResponse, error)
	DeleteSpreadsheet(ctx context.Context, req *pb.DeleteSpreadsheetRequest, opts ...grpc.CallOption) (*pb.DeleteSpreadsheetResponse, error)
}
type FileClient interface {
	ListFiles(ctx context.Context, req *pb.ListFilesRequest, opts ...grpc.CallOption) (*pb.ListFilesResponse, error)
	GetFile(ctx context.Context, req *pb.GetFileRequest, opts ...grpc.CallOption) (*pb.GetFileResponse, error)
	CreateFile(ctx context.Context, req *pb.CreateFileRequest, opts ...grpc.CallOption) (*pb.CreateFileResponse, error)
	DeleteFile(ctx context.Context, req *pb.DeleteFileRequest, opts ...grpc.CallOption) (*pb.DeleteFileResponse, error)
	CreateFolder(ctx context.Context, req *pb.CreateFolderRequest, opts ...grpc.CallOption) (*pb.CreateFolderResponse, error)
	MoveFile(ctx context.Context, req *pb.MoveFileRequest, opts ...grpc.CallOption) (*pb.MoveFileResponse, error)
	BatchDelete(ctx context.Context, req *pb.BatchDeleteRequest, opts ...grpc.CallOption) (*pb.BatchDeleteResponse, error)
}
type AuthClientI interface {
	Login(ctx context.Context, req *pb.LoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error)
	Register(ctx context.Context, req *pb.RegisterRequest, opts ...grpc.CallOption) (*pb.RegisterResponse, error)
	RefreshToken(ctx context.Context, req *pb.RefreshTokenRequest, opts ...grpc.CallOption) (*pb.RefreshTokenResponse, error)
	ChangePassword(ctx context.Context, req *pb.ChangePasswordRequest, opts ...grpc.CallOption) (*pb.ChangePasswordResponse, error)
	LoginByPhone(ctx context.Context, req *pb.PhoneLoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error)
}

// 真实连接（包级变量，main() 里初始化）
var authConn, sheetConn, fileConn *grpc.ClientConn
var sheetClient SheetClient
var fileClient FileClient
var authClient AuthClientI
var rdb *redis.Client

// cbWithSlow wraps a circuit breaker with a slow-call counter
type cbWithSlow struct {
	*gobreaker.CircuitBreaker[any]
	slowCalls atomic.Int64
}

func initTracer() (*sdktrace.TracerProvider, error) {
	ctx := context.Background()
	endpoint := getenv("OTEL_EXPORTER_OTLP_ENDPOINT", "jaeger:4317")
	exporter, err := otlptracegrpc.New(ctx,
		otlptracegrpc.WithEndpoint(endpoint),
		otlptracegrpc.WithInsecure(),
	)
	if err != nil {
		log.Printf("[tracing] OTLP exporter error: %v", err)
		return nil, err
	}
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(exporter),
		sdktrace.WithSampler(sdktrace.AlwaysSample()),
		sdktrace.WithResource(resource.NewWithAttributes(
			semconv.SchemaURL,
			semconv.ServiceName("gateway-grpc"),
		)),
	)
	otel.SetTracerProvider(tp)
	otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(
		propagation.TraceContext{},
		propagation.Baggage{},
	))
	log.Printf("[tracing] initialized, endpoint=%s", endpoint)
	return tp, nil
}

func main() {
	jwtSecret = []byte(getenv("JWT_SECRET", "default-secret-32bytes-here!!!!!"))

	tp, err := initTracer()
	if err != nil {
		log.Printf("[tracing] WARNING: tracer not available: %v", err)
	}
	if tp != nil {
		defer func() {
			if err := tp.Shutdown(context.Background()); err != nil {
				log.Printf("[tracing] shutdown error: %v", err)
			}
		}()
	}

	mux := http.NewServeMux()

	kp := grpc.WithKeepaliveParams(keepalive.ClientParameters{
		Time: 10 * time.Second, Timeout: 3 * time.Second,
	})
	creds := grpc.WithTransportCredentials(insecure.NewCredentials())
	lb := grpc.WithDefaultServiceConfig(`{"loadBalancingConfig":[{"round_robin":{}}]}`)
	otelStats := grpc.WithStatsHandler(otelgrpc.NewClientHandler())
	grpcMetrics := grpc.WithUnaryInterceptor(grpcMetricsInterceptor())

	authAddr := getenv("AUTH_ADDR", "rpc-auth:50051")
	sheetAddr := getenv("SHEET_ADDR", "rpc-sheet:50051")
	fileAddr := getenv("FILE_ADDR", "rpc-file:50051")
	searchAddr := getenv("SEARCH_ADDR", "rpc-search:50051")
	log.Printf("Auth=%s Sheet=%s File=%s Search=%s", authAddr, sheetAddr, fileAddr, searchAddr)

	authConn, _ = grpc.NewClient("dns:///"+authAddr, creds, kp, lb, otelStats, grpcMetrics)
	authClient = pb.NewAuthServiceClient(authConn)

	sheetConn, _ = grpc.NewClient("dns:///"+sheetAddr, creds, kp, lb, otelStats, grpcMetrics)
	sheetClient = pb.NewSpreadsheetServiceClient(sheetConn)

	fileConn, _ = grpc.NewClient("dns:///"+fileAddr, creds, kp, lb, otelStats, grpcMetrics)
	fileClient = pb.NewFileServiceClient(fileConn)

	searchConn, _ := grpc.NewClient("dns:///"+searchAddr, creds, kp, lb, otelStats, grpcMetrics)
	searchClient := pb.NewSearchServiceClient(searchConn)

	sharingClient := pb.NewSharingServiceClient(authConn)

	// Redis
	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	rdb = redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})

	// 熔断器 + 慢调用计数器（每个 C++ 服务一个）
	newCB := func(name string) *cbWithSlow {
		cbs := &cbWithSlow{}
		cbs.CircuitBreaker = gobreaker.NewCircuitBreaker[any](gobreaker.Settings{
			Name:        name,
			MaxRequests: 3,
			Interval:    30 * time.Second,
			Timeout:     30 * time.Second,
			ReadyToTrip: func(counts gobreaker.Counts) bool {
				slow := float64(cbs.slowCalls.Load())
				total := float64(counts.Requests)
				return counts.ConsecutiveFailures >= 5 ||
					(total >= 10 && float64(counts.TotalFailures)/total >= 0.5) ||
					(total >= 10 && slow/total >= 0.8)
			},
			OnStateChange: func(name string, from, to gobreaker.State) {
				log.Printf("[cb] %s: %s → %s", name, from, to)
				if to == gobreaker.StateClosed {
					cbs.slowCalls.Store(0)
				}
				stateVal := 0.0
				if to == gobreaker.StateHalfOpen {
					stateVal = 1.0
				} else if to == gobreaker.StateOpen {
					stateVal = 2.0
				}
				circuitBreakerState.WithLabelValues(name).Set(stateVal)

			},
		})
		return cbs
	}
	_ = newCB("auth")
	cbSheet := newCB("sheet")
	cbFile := newCB("file")
	_ = newCB("search")  // 预留给 Search handler 重试

	// === Auth ===
	mux.HandleFunc("POST /api/register", func(w http.ResponseWriter, r *http.Request) {
		var req pb.RegisterRequest
		json.NewDecoder(r.Body).Decode(&req)
		if msg, code := validateRegister(req.Username, req.Password); msg != "" {
			writeJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
			return
		}

		resp, _ := authClient.Register(r.Context(), &req)
		if resp != nil && !resp.Success {
			writeError(w, nil, resp.GetError(), resp.GetErrorCode())
			return
		}
		setCookies(w, resp.AccessToken, resp.RefreshToken)
		writeJSON(w, resp)
	})
	mux.HandleFunc("POST /api/login", func(w http.ResponseWriter, r *http.Request) {
		var req pb.LoginRequest
		json.NewDecoder(r.Body).Decode(&req)
		if checkLoginRate(req.Username) {
			writeJSONStatus(w, http.StatusTooManyRequests,
				map[string]interface{}{"success": false, "error": "Too many attempts, try again later"})
			return
		}

		if msg, code := validateLogin(req.Username, req.Password); msg != "" {
			writeJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
			return
		}

		resp, _ := authClient.Login(r.Context(), &req)
		if resp != nil && !resp.Success {
			writeError(w, nil, resp.GetError(), resp.GetErrorCode())
			return
		}
		setCookies(w, resp.AccessToken, resp.RefreshToken)
		writeJSON(w, resp)
	})
	mux.HandleFunc("POST /api/refresh", func(w http.ResponseWriter, r *http.Request) {
		var req pb.RefreshTokenRequest
		json.NewDecoder(r.Body).Decode(&req)
		// body 优先，Cookie 兜底
		if req.RefreshToken == "" {
			if ck, err := r.Cookie("rpc_rt"); err == nil {
				req.RefreshToken = ck.Value
			}
		}
		if req.Username == "" {
			req.Username = getUserFromCookie(r)
		}
		resp, _ := authClient.RefreshToken(r.Context(), &req)
		setCookies(w, resp.AccessToken, "")
		writeJSON(w, resp)
	})
	mux.HandleFunc("PUT /api/me/password", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			OldPassword string `json:"old_password"`
			NewPassword string `json:"new_password"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		uid := extractUID(r)
		if msg, code := validateChangePassword(body.OldPassword, body.NewPassword); msg != "" {
			writeJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
			return
		}

		if uid == 0 {
			http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
			return
		}
		req := &pb.ChangePasswordRequest{UserId: uid, OldPassword: body.OldPassword, NewPassword: body.NewPassword}
		resp, err := authClient.ChangePassword(r.Context(), req)
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("POST /api/auth/otp/send", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ Phone string `json:"phone"` }
		json.NewDecoder(r.Body).Decode(&body)
		if body.Phone == "" {
			writeJSONStatus(w, http.StatusBadRequest, map[string]interface{}{"success": false, "error": "phone required"})
			return
		}
		code := fmt.Sprintf("%06d", time.Now().UnixNano()%1000000)
		rdb.Set(r.Context(), "otp:"+body.Phone, code, 5*time.Minute)
		log.Printf("[OTP] phone=%s code=%s", body.Phone, code)
		writeJSON(w, map[string]interface{}{"success": true})
	})
	mux.HandleFunc("POST /api/auth/phone/login", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Phone string `json:"phone"`
			OTP   string `json:"otp"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		stored, _ := rdb.Get(r.Context(), "otp:"+body.Phone).Result()
		if stored == "" || stored != body.OTP {
			writeJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{"success": false, "error": "Invalid OTP"})
			return
		}
		rdb.Del(r.Context(), "otp:"+body.Phone)
		resp, _ := authClient.LoginByPhone(r.Context(), &pb.PhoneLoginRequest{Phone: body.Phone, Otp: body.OTP})
		setCookies(w, resp.AccessToken, resp.RefreshToken)
		writeJSON(w, resp)
	})

	mux.Handle("GET /api/v1/metrics", metricsHandler())
	mux.HandleFunc("GET /api/health", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, map[string]string{"gateway": "READY"})
	})
	mux.HandleFunc("GET /api/health/ready", func(w http.ResponseWriter, r *http.Request) {
		checkConn := func(name string, conn *grpc.ClientConn) string {
			s := conn.GetState()
			if s == connectivity.Ready { return "OK" }
			return s.String()
		}
		status := map[string]string{"gateway": "OK"}
		status["auth"] = checkConn("auth", authConn)
		status["sheet"] = checkConn("sheet", sheetConn)
		status["file"] = checkConn("file", fileConn)
		allOK := status["auth"] == "OK" && status["sheet"] == "OK" && status["file"] == "OK"
		code := http.StatusOK
		if !allOK { code = http.StatusServiceUnavailable }
		writeJSONStatus(w, code, map[string]interface{}{"_all": allOK, "status": status})
	})
	mux.HandleFunc("GET /api/me", func(w http.ResponseWriter, r *http.Request) {
		user := getUserFromCookie(r)
		uid := extractUID(r)
		writeJSON(w, map[string]interface{}{"username": user, "user_id": uid})
	})
	mux.HandleFunc("GET /api/services", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, map[string]interface{}{"services": map[string][]string{
			"auth-service": {}, "sheet-service": {}, "file-service": {}, "search-service": {},
		}})
	})
	// === Sharing ===
	mux.HandleFunc("POST /api/sheets/{id}/share", func(w http.ResponseWriter, r *http.Request) {
		uid := extractUID(r)
		var body struct {
			Username   string `json:"username"`
			Permission string `json:"permission"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		id := parseInt64(r.PathValue("id"))
		resp, _ := sharingClient.Share(injectToken(r), &pb.ShareRequest{
			OwnerId: uid, ResourceType: "sheet", ResourceId: id,
			GranteeUsername: body.Username, Permission: body.Permission,
		})
		writeJSON(w, resp)
	})
	mux.HandleFunc("DELETE /api/sheets/{id}/share", func(w http.ResponseWriter, r *http.Request) {
		uid := extractUID(r)
		username := r.URL.Query().Get("username")
		id := parseInt64(r.PathValue("id"))
		resp, _ := sharingClient.Revoke(injectToken(r), &pb.RevokeRequest{
			OwnerId: uid, ResourceType: "sheet", ResourceId: id,
			GranteeUsername: username,
		})
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/sheets/{id}/share", func(w http.ResponseWriter, r *http.Request) {
		uid := extractUID(r)
		id := parseInt64(r.PathValue("id"))
		resp, _ := sharingClient.ListShares(injectToken(r), &pb.ResourceRequest{
			OwnerId: uid, ResourceType: "sheet", ResourceId: id,
		})
		writeJSON(w, resp)
	})
	mux.HandleFunc("POST /api/sheets/{id}/share-link", func(w http.ResponseWriter, r *http.Request) {
		uid := extractUID(r)
		id := parseInt64(r.PathValue("id"))
		resp, _ := sharingClient.CreateShareLink(injectToken(r), &pb.ShareLinkRequest{
			OwnerId: uid, ResourceType: "sheet", ResourceId: id, Permission: "view",
		})
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/s/{token}", func(w http.ResponseWriter, r *http.Request) {
		token := r.PathValue("token")
		resp, _ := sharingClient.GetByToken(injectToken(r), &pb.ShareTokenRequest{Token: token})
		if resp == nil || !resp.Success {
			writeJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
			return
		}
		info := resp.GetInfo()
		// Redirect to actual resource based on type
		switch info.GetResourceType() {
		case "sheet":
			http.Redirect(w, r, fmt.Sprintf("/api/v1/sheets/%d", info.GetResourceId()), http.StatusFound)
		case "file":
			http.Redirect(w, r, fmt.Sprintf("/api/v1/files/%d", info.GetResourceId()), http.StatusFound)
		}
	})

	mux.HandleFunc("GET /api/history", func(w http.ResponseWriter, r *http.Request) {
		user := getUserFromCookie(r)
		if user == "" {
			writeJSONStatus(w, http.StatusUnauthorized, map[string]string{"error": "login required"})
			return
		}
		entries, _ := rdb.LRange(r.Context(), "call_logs:"+user, -20, -1).Result()
		if entries == nil {
			entries = []string{}
		}
		writeJSON(w, map[string]interface{}{"user": user, "count": len(entries), "entries": entries})
	})

	// === Search ===
	mux.HandleFunc("POST /api/search", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Q    string `json:"q"`
			Sort string `json:"sort"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		uid := extractUID(r)
		if uid == 0 {
			http.Error(w, `{"error":"Jwt is missing"}`, http.StatusUnauthorized)
			return
		}
		if msg, code := validateSearch(body.Q); msg != "" {
			writeJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
			return
		}

		resp, err := searchClient.Search(injectToken(r), &pb.SearchRequest{
			Query: body.Q, UserId: uid, Sort: body.Sort,
		})
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})

	// === Sheet CRUD ===
	mux.HandleFunc("POST /api/sheets", func(w http.ResponseWriter, r *http.Request) {
		var req pb.CreateSpreadsheetRequest
		json.NewDecoder(r.Body).Decode(&req)
		uid := extractUID(r)
		req.UserId = uid
		resp, err := sheetClient.CreateSpreadsheet(injectToken(r), &req)
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/sheets", func(w http.ResponseWriter, r *http.Request) {
		resp, err := callWithRetry(r.Context(), cbSheet, 3, "sheet.list", func(ctx context.Context) (*pb.ListSpreadsheetsResponse, error) {
			return sheetClient.ListSpreadsheets(withAuth(ctx, r), &pb.ListSpreadsheetsRequest{UserId: extractUID(r)})
		})
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		uid := extractUID(r)
		caller := getUserFromCookie(r)
		if uid == 0 || caller == "" {
			writeJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{"success": false, "error": "Unauthorized"})
			return
		}
		resp, err := callWithRetry(r.Context(), cbSheet, 3, "sheet.get", func(ctx context.Context) (*pb.GetSpreadsheetResponse, error) {
			return sheetClient.GetSpreadsheet(withAuth(ctx, r), &pb.GetSpreadsheetRequest{Id: id, UserId: uid})
		})
		if err != nil {
			writeGRPCError(w, err, "Not found")
			return
		}
		if resp == nil || !resp.Success {
			writeJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
			return
		}
		if resp.Spreadsheet != nil && resp.Spreadsheet.Username != "" && resp.Spreadsheet.Username != caller {
			writeJSONStatus(w, http.StatusForbidden, map[string]interface{}{"success": false, "error": "Forbidden"})
			return
		}
		if resp.Spreadsheet != nil {
			writeJSON(w, resp)
		}
	})
	mux.HandleFunc("PUT /api/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		var req pb.UpdateSpreadsheetRequest
		json.NewDecoder(r.Body).Decode(&req)
		req.Id = parseInt64(r.PathValue("id"))
		req.UserId = extractUID(r)
		resp, err := sheetClient.UpdateSpreadsheet(injectToken(r), &req)
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("DELETE /api/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, err := callWithRetry(r.Context(), cbSheet, 2, "sheet.delete", func(ctx context.Context) (*pb.DeleteSpreadsheetResponse, error) {
			return sheetClient.DeleteSpreadsheet(withAuth(ctx, r), &pb.DeleteSpreadsheetRequest{Id: id, UserId: extractUID(r)})
		})
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})

	// === Photos (复用 File Service, 筛选 image/*) ===
	mux.HandleFunc("GET /api/photos", func(w http.ResponseWriter, r *http.Request) {
		resp, err := fileClient.ListFiles(injectToken(r), &pb.ListFilesRequest{
			UserId: extractUID(r), MimeFilter: "image/",
		})
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})

	// === File CRUD ===
	mux.HandleFunc("PUT /api/files/{id}/move", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		var body struct{ TargetFolderId int64 `json:"target_folder_id"` }
		json.NewDecoder(r.Body).Decode(&body)
		resp, err := fileClient.MoveFile(injectToken(r), &pb.MoveFileRequest{Id: id, TargetFolderId: body.TargetFolderId})
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("POST /api/files/folder", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Name           string `json:"name"`
			ParentFolderId int64  `json:"parent_folder_id"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		uid := extractUID(r)
		resp, err := fileClient.CreateFolder(injectToken(r), &pb.CreateFolderRequest{
			UserId: uid, Name: body.Name, ParentFolderId: body.ParentFolderId,
		})
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/files", func(w http.ResponseWriter, r *http.Request) {
		resp, err := callWithRetry(r.Context(), cbFile, 3, "file.list", func(ctx context.Context) (*pb.ListFilesResponse, error) {
			return fileClient.ListFiles(withAuth(ctx, r), &pb.ListFilesRequest{UserId: extractUID(r)})
		})
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("POST /api/files/upload", func(w http.ResponseWriter, r *http.Request) {
		uid := extractUID(r)
		log.Printf("[upload] uid=%d", uid)
		r.ParseMultipartForm(50 << 20)
		f, h, _ := r.FormFile("file")
		if f == nil {
			http.Error(w, `{"error":"no file"}`, 400)
			return
		}
		defer f.Close()
		data, _ := io.ReadAll(f)
		resp, err := fileClient.CreateFile(injectToken(r), &pb.CreateFileRequest{
			UserId: uid, OriginalName: h.Filename, Size: int64(len(data)),
			MimeType: h.Header.Get("Content-Type"), FileContent: data,
		})
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/files/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		uid := extractUID(r)
		log.Printf("[debug] GetFile id=%d uid=%d", id, uid)
		if uid == 0 {
			writeJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{"success": false, "error": "Unauthorized"})
			return
		}
		resp, err := callWithRetry(r.Context(), cbFile, 3, "file.get", func(ctx context.Context) (*pb.GetFileResponse, error) {
			return fileClient.GetFile(withAuth(ctx, r), &pb.GetFileRequest{Id: id, UserId: uid})
		})
		if err != nil {
			writeGRPCError(w, err, "Not found")
			return
		}
		if resp == nil || !resp.Success {
			writeJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
			return
		}
		if resp.GetDownloadUrl() != "" {
			dlResp, err := http.Get(resp.GetDownloadUrl())
			if err == nil {
				defer dlResp.Body.Close()
				w.Header().Set("Content-Type", resp.File.GetMimeType())
				io.Copy(w, dlResp.Body)
				return
			}
		}
		if resp.File != nil {
			w.Header().Set("Content-Type", resp.File.GetMimeType())
			w.Write(resp.FileContent)
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("DELETE /api/files/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		uid := extractUID(r)
		log.Printf("[debug] DeleteFile id=%d uid=%d", id, uid)
		resp, err := callWithRetry(r.Context(), cbFile, 2, "file.delete", func(ctx context.Context) (*pb.DeleteFileResponse, error) {
			return fileClient.DeleteFile(withAuth(ctx, r), &pb.DeleteFileRequest{Id: id, UserId: uid})
		})
		if err != nil || resp == nil || !resp.Success {
			writeError(w, err, resp.GetError(), resp.GetErrorCode())
			return
		}
		writeJSON(w, resp)
	})

	handler := metricsMiddleware(otelhttp.NewHandler(jwtMiddleware(corsMiddleware(mux)), "gateway-grpc",
		otelhttp.WithTracerProvider(otel.GetTracerProvider()),
		otelhttp.WithPropagators(otel.GetTextMapPropagator()),
	)

	port := getenv("PORT", "8080")
	srv := &http.Server{Addr: ":" + port, Handler: handler}
	go func() {
		log.Printf("[Gateway-gRPC] Listening on :%s", port)
		if err := srv.ListenAndServe(); err != http.ErrServerClosed {
			log.Fatalf("listen: %v", err)
		}
	}()

	// 优雅关闭：收到 SIGTERM/SIGINT → 停止接受新连接 → 等待已有请求完成
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit
	log.Println("Shutting down gracefully...")
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := srv.Shutdown(ctx); err != nil {
		log.Fatalf("shutdown: %v", err)
	}
	log.Println("Server stopped")
}

func setCookies(w http.ResponseWriter, at, rt string) {
	if at != "" {
		http.SetCookie(w, &http.Cookie{Name: "rpc_at", Value: at, Path: "/", HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode})
	}
	if rt != "" {
		http.SetCookie(w, &http.Cookie{Name: "rpc_rt", Value: rt, Path: "/api/v1/refresh", HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode})
	}
}

func withAuth(ctx context.Context, r *http.Request) context.Context {
	token := ""
	for _, c := range r.Cookies() {
		if c.Name == "rpc_at" {
			token = c.Value
			break
		}
	}
	if token != "" {
		return metadata.AppendToOutgoingContext(ctx, "authorization", "Bearer "+token)
	}
	return ctx
}

func injectToken(r *http.Request) context.Context {
	token := ""
	for _, c := range r.Cookies() {
		if c.Name == "rpc_at" {
			token = c.Value
			break
		}
	}
	if token != "" {
		return metadata.AppendToOutgoingContext(r.Context(), "authorization", "Bearer "+token)
	}
	return r.Context()
}

// gRPCResponse 是所有 protobuf 响应的公共接口
type gRPCResponse interface {
	GetSuccess() bool
	GetError() string
	GetErrorCode() int32
}

// writeGRPCResponse 统一处理 gRPC 成功/失败 → HTTP 响应
func writeGRPCResponse(w http.ResponseWriter, resp gRPCResponse, err error) {
	if err != nil {
		writeGRPCError(w, err, "internal error")
		return
	}
	if resp == nil || !resp.GetSuccess() {
		writeError(w, nil, resp.GetError(), resp.GetErrorCode())
		return
	}
	writeJSON(w, resp)
}

func writeJSON(w http.ResponseWriter, v interface{}) {
	writeJSONStatus(w, http.StatusOK, v)
}

func writeJSONStatus(w http.ResponseWriter, code int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	data, _ := json.Marshal(v)
	// Snowflake IDs (17+ digits) exceed JS precision, quote as strings
	re := regexp.MustCompile(`:(\d{16,})`)
	data = re.ReplaceAll(data, []byte(`:"$1"`))
	w.Write(data)
}

func writeGRPCError(w http.ResponseWriter, err error, fallback string) {
	code := http.StatusNotFound
	msg := fallback
	if err == gobreaker.ErrOpenState {
		code = http.StatusServiceUnavailable
		writeJSONStatus(w, code, map[string]interface{}{
			"success": false, "error": "Service temporarily unavailable (circuit open)",
			"degraded": true,
		})
		return
	} else if st, ok := status.FromError(err); ok {
		switch st.Code() {
		case codes.PermissionDenied, codes.Unauthenticated:
			code = http.StatusForbidden
			msg = st.Message()
		}
	}
	writeJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
}

// errorCodeToHTTP 将 C++ 服务返回的 error_code 枚举映射为 HTTP 状态码
// 0=OK, 1=BAD_REQUEST(400), 2=UNAUTH(401), 3=FORBIDDEN(403),
// 4=NOT_FOUND(404), 5=CONFLICT(409), 6=INTERNAL(500), 7=UNAVAILABLE(503)
func errorCodeToHTTP(code int32) int {
	switch code {
	case 1:
		return http.StatusBadRequest
	case 2:
		return http.StatusUnauthorized
	case 3:
		return http.StatusForbidden
	case 4:
		return http.StatusNotFound
	case 5:
		return http.StatusConflict
	case 6:
		return http.StatusInternalServerError
	case 7:
		return http.StatusServiceUnavailable
	default:
		return http.StatusOK
	}
}

// writeError 统一错误响应：优先用 error_code，fallback 到消息文本
func writeError(w http.ResponseWriter, err error, respErr string, errorCode int32) {
	msg := respErr
	if err != nil {
		msg = err.Error()
	}
	code := errorCodeToHTTP(errorCode)
	if code == http.StatusOK {
		// fallback: 如果 error_code 为 0 或未设置，用熔断/gRPC 错误判断
		writeGRPCError(w, err, msg)
		return
	}
	writeJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
}

func stringifyIDs(v interface{}) interface{} { return v }

func extractUID(r *http.Request) int64 {
	for _, c := range r.Cookies() {
		if c.Name != "rpc_at" {
			continue
		}
		parts := strings.SplitN(c.Value, ".", 3)
		if len(parts) != 3 {
			break
		}
		raw, err := base64.RawURLEncoding.DecodeString(parts[1])
		if err != nil {
			break
		}
		// JWT payload 里 uid 可能是裸数字也可能是字符串。
		// 裸数字在 Go JSON 解码时会被解析为 float64，
		// 对于 17 位 Snowflake ID 会精度丢失甚至溢出。
		// 用正则直接从 raw JSON 提取，避免 float64 截断。
		re := regexp.MustCompile(`"uid":("?)(\d+)("?)`)
		m := re.FindStringSubmatch(string(raw))
		if m != nil {
			n, _ := strconv.ParseInt(m[2], 10, 64)
			return n
		}
	}
	return 0
}

func getUserFromCookie(r *http.Request) string {
	for _, c := range r.Cookies() {
		if c.Name == "rpc_at" {
			claims := jwt.MapClaims{}
			jwt.ParseWithClaims(c.Value, &claims, func(t *jwt.Token) (interface{}, error) { return jwtSecret, nil })
			if u, ok := claims["username"].(string); ok {
				return u
			}
		}
	}
	return ""
}

func parseInt64(s string) int64 {
	n, _ := strconv.ParseInt(s, 10, 64)
	return n
}

func jwtMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		path := r.URL.Path
		if strings.HasPrefix(path, "/api/v1/login") || strings.HasPrefix(path, "/api/v1/register") ||
			strings.HasPrefix(path, "/api/v1/refresh") || strings.HasPrefix(path, "/api/v1/health") ||
			strings.HasPrefix(path, "/api/v1/me") || strings.HasPrefix(path, "/api/v1/metrics") {
			next.ServeHTTP(w, r)
			return
		}
		tokenStr := ""
		for _, c := range r.Cookies() {
			if c.Name == "rpc_at" {
				tokenStr = c.Value
			}
		}
		if tokenStr == "" {
			http.Error(w, `{"error":"Jwt is missing"}`, http.StatusUnauthorized)
			return
		}
		claims := jwt.MapClaims{}
		_, err := jwt.ParseWithClaims(tokenStr, &claims, func(t *jwt.Token) (interface{}, error) {
			return jwtSecret, nil
		})
		if err != nil {
			http.Error(w, `{"error":"Invalid token"}`, http.StatusUnauthorized)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func corsMiddleware(next http.Handler) http.Handler {
		allowedOrigin := getenv("CORS_ORIGIN", "*")
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", allowedOrigin)
		w.Header().Set("Access-Control-Allow-Credentials", "true")
		w.Header().Set("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type,Authorization")
		if r.Method == "OPTIONS" {
			w.WriteHeader(http.StatusOK)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// callWithRetry 熔断器（外）+ gRPC 重试（内）
// ctx 保留原始请求的 trace context，避免每次重试丢失链路。
func callWithRetry[T any](ctx context.Context, cb *cbWithSlow, maxAttempts int, label string, fn func(context.Context) (T, error)) (T, error) {
	result, err := cb.Execute(func() (any, error) {
		var lastErr error
		for attempt := 0; attempt < maxAttempts; attempt++ {
			if attempt > 0 {
				d := time.Duration(50<<(attempt-1)) * time.Millisecond
				time.Sleep(d)
				log.Printf("[retry] %s attempt %d/%d after %v", label, attempt+1, maxAttempts, d)
			}
			attemptCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
			start := time.Now()
			r, e := fn(attemptCtx)
			cancel()
			if time.Since(start) > 2*time.Second {
				cb.slowCalls.Add(1)
			}
			if e == nil {
				return r, nil
			}
			lastErr = e
			if st, ok := status.FromError(e); ok {
				switch st.Code() {
				case codes.Unavailable, codes.DeadlineExceeded,
					codes.ResourceExhausted, codes.Aborted:
					continue
				}
			}
			return nil, e
		}
		return nil, lastErr
	})
	if err != nil {
		var zero T
		return zero, err
	}
	return result.(T), nil
}

func getenv(key, fallback string) string {
	if v := os.Getenv(key); v != "" { return v }
	return fallback
}

func base64urlEncode(s string) string {
	return strings.TrimRight(base64.URLEncoding.EncodeToString([]byte(s)), "=")
}

// checkLoginRate 登录速率限制：每分钟 5 次，超限锁定 5 分钟
func checkLoginRate(username string) (blocked bool) {
	if username == "" || rdb == nil {
		return false
	}
	blockKey := "rate:login:" + username + ":blocked"
	if n, _ := rdb.Exists(context.Background(), blockKey).Result(); n > 0 {
		return true
	}
	minKey := "rate:login:" + username + ":" + time.Now().Format("2006-01-02T15:04")
	n, _ := rdb.Incr(context.Background(), minKey).Result()
	rdb.Expire(context.Background(), minKey, 60*time.Second)
	if n > 5 {
		rdb.Set(context.Background(), blockKey, "1", 5*time.Minute)
		return true
	}
	return false
}
