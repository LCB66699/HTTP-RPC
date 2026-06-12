package main

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/redis/go-redis/v9"
	"github.com/sony/gobreaker/v2"
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
var authConn, sheetConn, fileConn *grpc.ClientConn

// cbWithSlow wraps a circuit breaker with a slow-call counter
type cbWithSlow struct {
	*gobreaker.CircuitBreaker[any]
	slowCalls atomic.Int64
}

func main() {
	jwtSecret = []byte(getenv("JWT_SECRET", "default-secret-32bytes-here!!!!!"))

	mux := http.NewServeMux()

	kp := grpc.WithKeepaliveParams(keepalive.ClientParameters{
		Time: 10 * time.Second, Timeout: 3 * time.Second,
	})
	creds := grpc.WithTransportCredentials(insecure.NewCredentials())
	lb := grpc.WithDefaultServiceConfig(`{"loadBalancingConfig":[{"round_robin":{}}]}`)

	authAddr := getenv("AUTH_ADDR", "rpc-auth:50051")
	sheetAddr := getenv("SHEET_ADDR", "rpc-sheet:50051")
	fileAddr := getenv("FILE_ADDR", "rpc-file:50051")
	searchAddr := getenv("SEARCH_ADDR", "rpc-search:50051")
	log.Printf("Auth=%s Sheet=%s File=%s Search=%s", authAddr, sheetAddr, fileAddr, searchAddr)

	authConn, _ = grpc.NewClient("dns:///"+authAddr, creds, kp, lb)
	authClient := pb.NewAuthServiceClient(authConn)

	sheetConn, _ = grpc.NewClient("dns:///"+sheetAddr, creds, kp, lb)
	sheetClient := pb.NewSpreadsheetServiceClient(sheetConn)

	fileConn, _ = grpc.NewClient("dns:///"+fileAddr, creds, kp, lb)
	fileClient := pb.NewFileServiceClient(fileConn)

	searchConn, _ := grpc.NewClient("dns:///"+searchAddr, creds, kp, lb)
	searchClient := pb.NewSearchServiceClient(searchConn)

	// Redis
	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	rdb := redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})

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
		resp, _ := authClient.Register(r.Context(), &req)
		setCookies(w, resp.AccessToken, resp.RefreshToken)
		writeJSON(w, resp)
	})
	mux.HandleFunc("POST /api/login", func(w http.ResponseWriter, r *http.Request) {
		var req pb.LoginRequest
		json.NewDecoder(r.Body).Decode(&req)
		resp, _ := authClient.Login(r.Context(), &req)
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
	mux.HandleFunc("GET /api/history", func(w http.ResponseWriter, r *http.Request) {
		user := getUserFromCookie(r)
		if user == "" {
			writeJSON(w, map[string]string{"error": "login required"})
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
		resp, err := searchClient.Search(injectToken(r), &pb.SearchRequest{
			Query: body.Q, UserId: uid, Sort: body.Sort,
		})
		if err != nil || resp == nil || !resp.Success {
			msg := "search unavailable"
			if err != nil {
				msg = err.Error()
			} else if resp != nil && resp.GetError() != "" {
				msg = resp.GetError()
			}
			writeJSON(w, map[string]interface{}{"success": false, "error": msg})
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
			msg := "create failed"
			if err != nil {
				msg = err.Error()
			} else if resp != nil && resp.GetError() != "" {
				msg = resp.GetError()
			}
			writeJSON(w, map[string]interface{}{"success": false, "error": msg})
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/sheets", func(w http.ResponseWriter, r *http.Request) {
		resp, err := callWithRetry(cbSheet, 3, "sheet.list", func(ctx context.Context) (*pb.ListSpreadsheetsResponse, error) {
			return sheetClient.ListSpreadsheets(withAuth(ctx, r), &pb.ListSpreadsheetsRequest{UserId: extractUID(r)})
		})
		if err != nil || resp == nil || !resp.Success {
			msg := "list failed"
			if err != nil {
				msg = err.Error()
			} else if resp != nil && resp.GetError() != "" {
				msg = resp.GetError()
			}
			writeJSON(w, map[string]interface{}{"success": false, "error": msg})
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
		resp, err := callWithRetry(cbSheet, 3, "sheet.get", func(ctx context.Context) (*pb.GetSpreadsheetResponse, error) {
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
			msg := "update failed"
			if err != nil {
				msg = err.Error()
			} else if resp != nil && resp.GetError() != "" {
				msg = resp.GetError()
			}
			writeJSON(w, map[string]interface{}{"success": false, "error": msg})
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("DELETE /api/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, err := callWithRetry(cbSheet, 2, "sheet.delete", func(ctx context.Context) (*pb.DeleteSpreadsheetResponse, error) {
			return sheetClient.DeleteSpreadsheet(withAuth(ctx, r), &pb.DeleteSpreadsheetRequest{Id: id, UserId: extractUID(r)})
		})
		if err != nil || resp == nil || !resp.Success {
			msg := "delete failed"
			if err != nil {
				msg = err.Error()
			} else if resp != nil && resp.GetError() != "" {
				msg = resp.GetError()
			}
			writeJSON(w, map[string]interface{}{"success": false, "error": msg})
			return
		}
		writeJSON(w, resp)
	})

	// === File CRUD ===
	mux.HandleFunc("GET /api/files", func(w http.ResponseWriter, r *http.Request) {
		resp, err := callWithRetry(cbFile, 3, "file.list", func(ctx context.Context) (*pb.ListFilesResponse, error) {
			return fileClient.ListFiles(withAuth(ctx, r), &pb.ListFilesRequest{UserId: extractUID(r)})
		})
		if err != nil || resp == nil || !resp.Success {
			msg := "list failed"
			if err != nil {
				msg = err.Error()
			} else if resp != nil && resp.GetError() != "" {
				msg = resp.GetError()
			}
			writeJSON(w, map[string]interface{}{"success": false, "error": msg})
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
			msg := "upload failed"
			if err != nil {
				msg = err.Error()
			} else if resp != nil && resp.GetError() != "" {
				msg = resp.GetError()
			}
			writeJSON(w, map[string]interface{}{"success": false, "error": msg})
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/files/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		uid := extractUID(r)
		if uid == 0 {
			writeJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{"success": false, "error": "Unauthorized"})
			return
		}
		resp, err := callWithRetry(cbFile, 3, "file.get", func(ctx context.Context) (*pb.GetFileResponse, error) {
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
			http.Redirect(w, r, resp.GetDownloadUrl(), http.StatusFound)
			return
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
		resp, err := callWithRetry(cbFile, 2, "file.delete", func(ctx context.Context) (*pb.DeleteFileResponse, error) {
			return fileClient.DeleteFile(withAuth(ctx, r), &pb.DeleteFileRequest{Id: id, UserId: extractUID(r)})
		})
		if err != nil || resp == nil || !resp.Success {
			msg := "delete failed"
			if err != nil {
				msg = err.Error()
			} else if resp != nil && resp.GetError() != "" {
				msg = resp.GetError()
			}
			writeJSON(w, map[string]interface{}{"success": false, "error": msg})
			return
		}
		writeJSON(w, resp)
	})

	log.Printf("[Gateway-gRPC] Listening on :%s", getenv("PORT", "8080"))
	log.Fatal(http.ListenAndServe(":"+getenv("PORT", "8080"), jwtMiddleware(corsMiddleware(mux))))
}

func setCookies(w http.ResponseWriter, at, rt string) {
	if at != "" {
		http.SetCookie(w, &http.Cookie{Name: "rpc_at", Value: at, Path: "/", HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode})
	}
	if rt != "" {
		http.SetCookie(w, &http.Cookie{Name: "rpc_rt", Value: rt, Path: "/api/refresh", HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode})
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
		var claims map[string]interface{}
		dec := json.NewDecoder(strings.NewReader(string(raw)))
		dec.UseNumber()
		if dec.Decode(&claims) != nil {
			break
		}
		switch v := claims["uid"].(type) {
		case json.Number:
			n, _ := v.Int64()
			return n
		case float64:
			return int64(v)
		case string:
			n, _ := strconv.ParseInt(v, 10, 64)
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
		if strings.HasPrefix(path, "/api/login") || strings.HasPrefix(path, "/api/register") ||
			strings.HasPrefix(path, "/api/refresh") || strings.HasPrefix(path, "/api/health") ||
			strings.HasPrefix(path, "/api/me") {
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
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
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
// 每轮重试创建独立 3s deadline，避免慢实例阻塞全部重试
// 只重试可恢复错误：Unavailable / DeadlineExceeded / ResourceExhausted / Aborted
// 调用超过 2s 记入慢调用计数器，供熔断器 ReadyToTrip 使用
func callWithRetry[T any](cb *cbWithSlow, maxAttempts int, label string, fn func(context.Context) (T, error)) (T, error) {
	result, err := cb.Execute(func() (any, error) {
		var lastErr error
		for attempt := 0; attempt < maxAttempts; attempt++ {
			if attempt > 0 {
				d := time.Duration(50<<(attempt-1)) * time.Millisecond
				time.Sleep(d)
				log.Printf("[retry] %s attempt %d/%d after %v", label, attempt+1, maxAttempts, d)
			}
			attemptCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
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
