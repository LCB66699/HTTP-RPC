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
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/redis/go-redis/v9"
	"github.com/sony/gobreaker/v2"

	gw "github.com/lcb66699/http-rpc/gateway-grpc/internal/gateway"
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

// 真实连接（包级变量，main() 里初始化）
var authConn, sheetConn, fileConn *grpc.ClientConn
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
	gw.AuthClient = pb.NewAuthServiceClient(authConn)

	sheetConn, _ = grpc.NewClient("dns:///"+sheetAddr, creds, kp, lb, otelStats, grpcMetrics)
	gw.SheetClient = pb.NewSpreadsheetServiceClient(sheetConn)

	fileConn, _ = grpc.NewClient("dns:///"+fileAddr, creds, kp, lb, otelStats, grpcMetrics)
	gw.FileClient = pb.NewFileServiceClient(fileConn)

	searchConn, _ := grpc.NewClient("dns:///"+searchAddr, creds, kp, lb, otelStats, grpcMetrics)
	searchClient := pb.NewSearchServiceClient(searchConn)

	gw.SharedClient = pb.NewSharingServiceClient(authConn)

	// Redis
	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	rdb = redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})

	// Circuit breaker setup
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
	cbSearch := newCB("search")

	// === Auth ===
	mux.HandleFunc("POST /api/v1/register", func(w http.ResponseWriter, r *http.Request) {
		var req pb.RegisterRequest
		json.NewDecoder(r.Body).Decode(&req)
		if msg, code := gw.ValidateRegister(req.Username, req.Password); msg != "" {
			gw.WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
			return
		}
		resp, _ := gw.AuthClient.Register(r.Context(), &req)
		if resp != nil && !resp.Success {
			code := int32(0)
			if ec, ok := any(resp).(interface{ GetErrorCode() int32 }); ok {
				code = ec.GetErrorCode()
			}
			gw.WriteError(w, nil, resp.GetError(), code)
			return
		}
		setCookies(w, resp.AccessToken, resp.RefreshToken)
		gw.WriteJSON(w, resp)
	})
	mux.HandleFunc("POST /api/v1/login", func(w http.ResponseWriter, r *http.Request) {
		var req pb.LoginRequest
		json.NewDecoder(r.Body).Decode(&req)
		if checkLoginRate(req.Username) {
			gw.WriteJSONStatus(w, http.StatusTooManyRequests,
				map[string]interface{}{"success": false, "error": "Too many attempts, try again later"})
			return
		}
		if msg, code := gw.ValidateLogin(req.Username, req.Password); msg != "" {
			gw.WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
			return
		}
		resp, _ := gw.AuthClient.Login(r.Context(), &req)
		if resp != nil && !resp.Success {
			code := int32(0)
			if ec, ok := any(resp).(interface{ GetErrorCode() int32 }); ok {
				code = ec.GetErrorCode()
			}
			gw.WriteError(w, nil, resp.GetError(), code)
			return
		}
		setCookies(w, resp.AccessToken, resp.RefreshToken)
		gw.WriteJSON(w, resp)
	})
	mux.HandleFunc("POST /api/v1/refresh", func(w http.ResponseWriter, r *http.Request) {
		var req pb.RefreshTokenRequest
		json.NewDecoder(r.Body).Decode(&req)
		if req.RefreshToken == "" {
			if ck, err := r.Cookie("rpc_rt"); err == nil {
				req.RefreshToken = ck.Value
			}
		}
		if req.Username == "" {
			req.Username = gw.GetUserFromCookie(r)
		}
		resp, err := gw.AuthClient.RefreshToken(r.Context(), &req)
		if err != nil {
			log.Printf("[refresh] gRPC error: %v", err)
		}
		at := ""
		if resp != nil {
			at = resp.AccessToken
			if len(at) > 16 {
				log.Printf("[refresh] access_token (first 16): %s...", at[:16])
			} else {
				log.Printf("[refresh] access_token empty or short (len=%d)", len(at))
			}
		}
		setCookies(w, at, "")
		gw.WriteJSON(w, resp)
	})
	mux.HandleFunc("PUT /api/v1/me/password", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			OldPassword string `json:"old_password"`
			NewPassword string `json:"new_password"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		uid := gw.ExtractUID(r)
		if msg, code := gw.ValidateChangePassword(body.OldPassword, body.NewPassword); msg != "" {
			gw.WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
			return
		}
		if uid == 0 {
			http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
			return
		}
		req := &pb.ChangePasswordRequest{UserId: uid, OldPassword: body.OldPassword, NewPassword: body.NewPassword}
		resp, err := gw.AuthClient.ChangePassword(r.Context(), req)
		gw.WriteGRPCResponse(w, resp, err)
	})
	mux.HandleFunc("POST /api/v1/auth/otp/send", func(w http.ResponseWriter, r *http.Request) {
		var body struct{ Phone string `json:"phone"` }
		json.NewDecoder(r.Body).Decode(&body)
		if body.Phone == "" {
			gw.WriteJSONStatus(w, http.StatusBadRequest, map[string]interface{}{"success": false, "error": "phone required"})
			return
		}
		code := fmt.Sprintf("%06d", time.Now().UnixNano()%1000000)
		rdb.Set(r.Context(), "otp:"+body.Phone, code, 5*time.Minute)
		log.Printf("[OTP] phone=%s code=%s", body.Phone, code)
		gw.WriteJSON(w, map[string]interface{}{"success": true})
	})
	mux.HandleFunc("POST /api/v1/auth/phone/login", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Phone string `json:"phone"`
			OTP   string `json:"otp"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		stored, _ := rdb.Get(r.Context(), "otp:"+body.Phone).Result()
		if stored == "" || stored != body.OTP {
			gw.WriteJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{"success": false, "error": "Invalid OTP"})
			return
		}
		rdb.Del(r.Context(), "otp:"+body.Phone)
		resp, _ := gw.AuthClient.LoginByPhone(r.Context(), &pb.PhoneLoginRequest{Phone: body.Phone, Otp: body.OTP})
		setCookies(w, resp.AccessToken, resp.RefreshToken)
		gw.WriteJSON(w, resp)
	})

	mux.Handle("GET /api/v1/metrics", metricsHandler())
	mux.HandleFunc("GET /api/v1/health", func(w http.ResponseWriter, r *http.Request) {
		gw.WriteJSON(w, map[string]string{"gateway": "READY"})
	})
	mux.HandleFunc("GET /api/v1/health/ready", func(w http.ResponseWriter, r *http.Request) {
		checkConn := func(name string, conn *grpc.ClientConn) string {
			s := conn.GetState()
			if s == connectivity.Ready {
				return "OK"
			}
			return s.String()
		}
		status := map[string]string{"gateway": "OK"}
		status["auth"] = checkConn("auth", authConn)
		status["sheet"] = checkConn("sheet", sheetConn)
		status["file"] = checkConn("file", fileConn)
		allOK := status["auth"] == "OK" && status["sheet"] == "OK" && status["file"] == "OK"
		code := http.StatusOK
		if !allOK {
			code = http.StatusServiceUnavailable
		}
		gw.WriteJSONStatus(w, code, map[string]interface{}{"_all": allOK, "status": status})
	})
	mux.HandleFunc("GET /api/v1/me", func(w http.ResponseWriter, r *http.Request) {
		user := gw.GetUserFromCookie(r)
		gw.WriteJSON(w, map[string]interface{}{"username": user, "user_id": 0})
	})
	mux.HandleFunc("GET /api/v1/services", func(w http.ResponseWriter, r *http.Request) {
		gw.WriteJSON(w, map[string]interface{}{"services": map[string][]string{
			"auth-service": {}, "sheet-service": {}, "file-service": {}, "search-service": {},
		}})
	})

	// === Sharing ===
	mux.HandleFunc("POST /api/v1/sheets/{id}/share", func(w http.ResponseWriter, r *http.Request) {
		uid := gw.ExtractUID(r)
		var body struct {
			Username   string `json:"username"`
			Permission string `json:"permission"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		id := parseInt64(r.PathValue("id"))
		resp, _ := gw.SharedClient.Share(injectToken(r), &pb.ShareRequest{
			OwnerId: uid, ResourceType: "sheet", ResourceId: id,
			GranteeUsername: body.Username, Permission: body.Permission,
		})
		gw.WriteJSON(w, resp)
	})
	mux.HandleFunc("DELETE /api/v1/sheets/{id}/share", func(w http.ResponseWriter, r *http.Request) {
		uid := gw.ExtractUID(r)
		username := r.URL.Query().Get("username")
		id := parseInt64(r.PathValue("id"))
		resp, _ := gw.SharedClient.Revoke(injectToken(r), &pb.RevokeRequest{
			OwnerId: uid, ResourceType: "sheet", ResourceId: id,
			GranteeUsername: username,
		})
		gw.WriteJSON(w, resp)
	})
	mux.HandleFunc("GET /api/v1/sheets/{id}/share", func(w http.ResponseWriter, r *http.Request) {
		uid := gw.ExtractUID(r)
		id := parseInt64(r.PathValue("id"))
		resp, _ := gw.SharedClient.ListShares(injectToken(r), &pb.ResourceRequest{
			OwnerId: uid, ResourceType: "sheet", ResourceId: id,
		})
		gw.WriteJSON(w, resp)
	})
	mux.HandleFunc("POST /api/v1/sheets/{id}/share-link", func(w http.ResponseWriter, r *http.Request) {
		uid := gw.ExtractUID(r)
		id := parseInt64(r.PathValue("id"))
		resp, _ := gw.SharedClient.CreateShareLink(injectToken(r), &pb.ShareLinkRequest{
			OwnerId: uid, ResourceType: "sheet", ResourceId: id, Permission: "view",
		})
		gw.WriteJSON(w, resp)
	})
	mux.HandleFunc("GET /api/v1/s/{token}", func(w http.ResponseWriter, r *http.Request) {
		token := r.PathValue("token")
		resp, _ := gw.SharedClient.GetByToken(injectToken(r), &pb.ShareTokenRequest{Token: token})
		if resp == nil || !resp.Success {
			gw.WriteJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
			return
		}
		info := resp.GetInfo()
		switch info.GetResourceType() {
		case "sheet":
			http.Redirect(w, r, fmt.Sprintf("/api/v1/sheets/%d", info.GetResourceId()), http.StatusFound)
		case "file":
			http.Redirect(w, r, fmt.Sprintf("/api/v1/files/%d", info.GetResourceId()), http.StatusFound)
		}
	})

	mux.HandleFunc("GET /api/v1/history", func(w http.ResponseWriter, r *http.Request) {
		user := gw.GetUserFromCookie(r)
		if user == "" {
			gw.WriteJSONStatus(w, http.StatusUnauthorized, map[string]string{"error": "login required"})
			return
		}
		entries, _ := rdb.LRange(r.Context(), "call_logs:"+user, -20, -1).Result()
		if entries == nil {
			entries = []string{}
		}
		gw.WriteJSON(w, map[string]interface{}{"user": user, "count": len(entries), "entries": entries})
	})

	// === Search ===
	mux.HandleFunc("POST /api/v1/search", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Q    string `json:"q"`
			Sort string `json:"sort"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		if msg, code := gw.ValidateSearch(body.Q); msg != "" {
			gw.WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
			return
		}
		resp, err := callWithRetry(r.Context(), cbSearch, 3, "search", func(ctx context.Context) (*pb.SearchResponse, error) {
			return searchClient.Search(withAuth(ctx, r), &pb.SearchRequest{
				Query: body.Q, UserId: 0, Sort: body.Sort,
			})
		})
		gw.WriteGRPCResponse(w, resp, err)
	})

	// === Sheet CRUD ===
	mux.HandleFunc("POST /api/v1/sheets", func(w http.ResponseWriter, r *http.Request) {
		var req pb.CreateSpreadsheetRequest
		json.NewDecoder(r.Body).Decode(&req)
		req.UserId = 0
		resp, err := gw.SheetClient.CreateSpreadsheet(injectToken(r), &req)
		gw.WriteGRPCResponse(w, resp, err)
	})
	mux.HandleFunc("GET /api/v1/sheets", func(w http.ResponseWriter, r *http.Request) {
		resp, err := callWithRetry(r.Context(), cbSheet, 3, "sheet.list", func(ctx context.Context) (*pb.ListSpreadsheetsResponse, error) {
			return gw.SheetClient.ListSpreadsheets(withAuth(ctx, r), &pb.ListSpreadsheetsRequest{UserId: 0})
		})
		gw.WriteGRPCResponse(w, resp, err)
	})
	mux.HandleFunc("GET /api/v1/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		caller := gw.GetUserFromCookie(r)
		if caller == "" {
			gw.WriteJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{"success": false, "error": "Unauthorized"})
			return
		}
		resp, err := callWithRetry(r.Context(), cbSheet, 3, "sheet.get", func(ctx context.Context) (*pb.GetSpreadsheetResponse, error) {
			return gw.SheetClient.GetSpreadsheet(withAuth(ctx, r), &pb.GetSpreadsheetRequest{Id: id, UserId: 0})
		})
		if err != nil {
			gw.WriteGRPCError(w, err, "Not found")
			return
		}
		if resp == nil || !resp.Success {
			gw.WriteJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
			return
		}
		if resp.Spreadsheet != nil && resp.Spreadsheet.Username != "" && resp.Spreadsheet.Username != caller {
			gw.WriteJSONStatus(w, http.StatusForbidden, map[string]interface{}{"success": false, "error": "Forbidden"})
			return
		}
		if resp.Spreadsheet != nil {
			gw.WriteJSON(w, resp)
		}
	})
	mux.HandleFunc("PUT /api/v1/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		var req pb.UpdateSpreadsheetRequest
		json.NewDecoder(r.Body).Decode(&req)
		req.Id = parseInt64(r.PathValue("id"))
		req.UserId = 0
		resp, err := gw.SheetClient.UpdateSpreadsheet(injectToken(r), &req)
		gw.WriteGRPCResponse(w, resp, err)
	})
	mux.HandleFunc("DELETE /api/v1/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, err := callWithRetry(r.Context(), cbSheet, 2, "sheet.delete", func(ctx context.Context) (*pb.DeleteSpreadsheetResponse, error) {
			return gw.SheetClient.DeleteSpreadsheet(withAuth(ctx, r), &pb.DeleteSpreadsheetRequest{Id: id, UserId: 0})
		})
		gw.WriteGRPCResponse(w, resp, err)
	})

	// === Photos ===
	mux.HandleFunc("GET /api/v1/photos", func(w http.ResponseWriter, r *http.Request) {
		resp, err := gw.FileClient.ListFiles(injectToken(r), &pb.ListFilesRequest{
			UserId: 0, MimeFilter: "image/",
		})
		gw.WriteGRPCResponse(w, resp, err)
	})

	// === File CRUD ===
	mux.HandleFunc("PUT /api/v1/files/{id}/move", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		var body struct{ TargetFolderId int64 `json:"target_folder_id"` }
		json.NewDecoder(r.Body).Decode(&body)
		resp, err := gw.FileClient.MoveFile(injectToken(r), &pb.MoveFileRequest{Id: id, TargetFolderId: body.TargetFolderId})
		gw.WriteGRPCResponse(w, resp, err)
	})
	mux.HandleFunc("POST /api/v1/files/folder", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Name           string `json:"name"`
			ParentFolderId int64  `json:"parent_folder_id"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		resp, err := gw.FileClient.CreateFolder(injectToken(r), &pb.CreateFolderRequest{
			UserId: 0, Name: body.Name, ParentFolderId: body.ParentFolderId,
		})
		gw.WriteGRPCResponse(w, resp, err)
	})
	mux.HandleFunc("GET /api/v1/files", func(w http.ResponseWriter, r *http.Request) {
		resp, err := callWithRetry(r.Context(), cbFile, 3, "file.list", func(ctx context.Context) (*pb.ListFilesResponse, error) {
			return gw.FileClient.ListFiles(withAuth(ctx, r), &pb.ListFilesRequest{UserId: 0})
		})
		gw.WriteGRPCResponse(w, resp, err)
	})
	mux.HandleFunc("POST /api/v1/files/upload", func(w http.ResponseWriter, r *http.Request) {
		r.ParseMultipartForm(50 << 20)
		f, h, _ := r.FormFile("file")
		if f == nil {
			http.Error(w, `{"error":"no file"}`, 400)
			return
		}
		defer f.Close()
		data, _ := io.ReadAll(f)
		resp, err := gw.FileClient.CreateFile(injectToken(r), &pb.CreateFileRequest{
			UserId: 0, OriginalName: h.Filename, Size: int64(len(data)),
			MimeType: h.Header.Get("Content-Type"), FileContent: data,
		})
		gw.WriteGRPCResponse(w, resp, err)
	})
	mux.HandleFunc("GET /api/v1/files/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, err := callWithRetry(r.Context(), cbFile, 3, "file.get", func(ctx context.Context) (*pb.GetFileResponse, error) {
			return gw.FileClient.GetFile(withAuth(ctx, r), &pb.GetFileRequest{Id: id, UserId: 0})
		})
		if err != nil {
			gw.WriteGRPCError(w, err, "Not found")
			return
		}
		if resp == nil || !resp.Success {
			gw.WriteJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
			return
		}
		if resp.GetDownloadUrl() != "" {
			http.Redirect(w, r, resp.GetDownloadUrl(), http.StatusFound)
			return
		}
		if resp.File != nil {
			w.Header().Set("Content-Type", resp.File.GetMimeType())
			w.Write(resp.FileContent)
			return
		}
		gw.WriteJSON(w, resp)
	})
	mux.HandleFunc("DELETE /api/v1/files/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, err := callWithRetry(r.Context(), cbFile, 2, "file.delete", func(ctx context.Context) (*pb.DeleteFileResponse, error) {
			return gw.FileClient.DeleteFile(withAuth(ctx, r), &pb.DeleteFileRequest{Id: id, UserId: 0})
		})
		gw.WriteGRPCResponse(w, resp, err)
	})

	handler := metricsMiddleware(otelhttp.NewHandler(jwtMiddleware(corsMiddleware(mux)), "gateway-grpc",
		otelhttp.WithTracerProvider(otel.GetTracerProvider()),
		otelhttp.WithPropagators(otel.GetTextMapPropagator()),
	))

	port := getenv("PORT", "8080")
	srv := &http.Server{Addr: ":" + port, Handler: handler}
	go func() {
		log.Printf("[Gateway-gRPC] Listening on :%s", port)
		if err := srv.ListenAndServe(); err != http.ErrServerClosed {
			log.Fatalf("listen: %v", err)
		}
	}()

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
	if v := os.Getenv(key); v != "" {
		return v
	}
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
