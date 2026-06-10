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
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/redis/go-redis/v9"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/keepalive"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"

	pb "gateway-grpc/gen/rpc"
)

var jwtSecret []byte

func main() {
	jwtSecret = []byte(getenv("JWT_SECRET", "default-secret-32bytes-here!!!!!"))

	mux := http.NewServeMux()

	kp := grpc.WithKeepaliveParams(keepalive.ClientParameters{
		Time: 10 * time.Second, Timeout: 3 * time.Second,
	})
	creds := grpc.WithTransportCredentials(insecure.NewCredentials())

	authAddr := getenv("AUTH_ADDR", "rpc-auth:50051")
	sheetAddr := getenv("SHEET_ADDR", "rpc-sheet:50051")
	fileAddr := getenv("FILE_ADDR", "rpc-file:50051")
	log.Printf("Auth=%s Sheet=%s File=%s", authAddr, sheetAddr, fileAddr)

	authConn, _ := grpc.NewClient(authAddr, creds, kp)
	authClient := pb.NewAuthServiceClient(authConn)

	sheetConn, _ := grpc.NewClient(sheetAddr, creds, kp)
	sheetClient := pb.NewSpreadsheetServiceClient(sheetConn)

	fileConn, _ := grpc.NewClient(fileAddr, creds, kp)
	fileClient := pb.NewFileServiceClient(fileConn)

	// Redis
	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	rdb := redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})

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
	mux.HandleFunc("GET /api/me", func(w http.ResponseWriter, r *http.Request) {
		user := getUserFromCookie(r)
		uid := extractUID(r)
		writeJSON(w, map[string]interface{}{"username": user, "user_id": uid})
	})
	mux.HandleFunc("GET /api/services", func(w http.ResponseWriter, r *http.Request) {
		resp, err := httpGet("http://consul:8500/v1/catalog/services")
		if err != nil {
			writeJSON(w, map[string]interface{}{"services": []string{}})
			return
		}
		var raw map[string][]string
		json.Unmarshal(resp, &raw)
		writeJSON(w, map[string]interface{}{"services": raw})
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
		var req map[string]interface{}
		json.NewDecoder(r.Body).Decode(&req)
		q, _ := req["q"].(string)
		uid := extractUID(r)
		log.Printf("[search] uid=%d q=%q", uid, q)
		if uid == 0 {
			http.Error(w, `{"error":"Jwt is missing"}`, http.StatusUnauthorized)
			return
		}
		must := []interface{}{}
		if q != "" {
			must = append(must, map[string]interface{}{
				"bool": map[string]interface{}{
					"should": []interface{}{
						map[string]interface{}{"match_phrase_prefix": map[string]interface{}{"name": q}},
						map[string]interface{}{"match_phrase_prefix": map[string]interface{}{"description": q}},
						map[string]interface{}{"match_phrase_prefix": map[string]interface{}{"original_name": q}},
						map[string]interface{}{"wildcard": map[string]interface{}{"name": map[string]interface{}{"value": "*" + q + "*"}}},
						map[string]interface{}{"wildcard": map[string]interface{}{"description": map[string]interface{}{"value": "*" + q + "*"}}},
						map[string]interface{}{"wildcard": map[string]interface{}{"original_name": map[string]interface{}{"value": "*" + q + "*"}}},
					},
					"minimum_should_match": 1,
				},
			})
		} else {
			writeJSON(w, map[string]interface{}{"total": 0, "results": []interface{}{}})
			return
		}
		body, _ := json.Marshal(map[string]interface{}{
			"query": map[string]interface{}{
				"bool": map[string]interface{}{
					"must": must,
					"filter": map[string]interface{}{"term": map[string]interface{}{"user_id": uid}},
				},
			},
			"size": 20,
		})
		log.Printf("[search-es] %s", string(body)); resp, err := httpPost("http://elasticsearch:9200/sheets_search,files_search/_search", body)
		if err != nil {
			writeJSON(w, map[string]interface{}{"error": "search unavailable"})
			return
		}
		var esResp struct {
			Hits struct {
				Total struct{ Value int } `json:"total"`
				Hits  []struct {
					Source map[string]interface{} `json:"_source"`
				} `json:"hits"`
			} `json:"hits"`
		}
		json.Unmarshal(resp, &esResp)
		results := []map[string]interface{}{}
		for _, h := range esResp.Hits.Hits {
			results = append(results, h.Source)
		}
		writeJSON(w, map[string]interface{}{
			"total":   esResp.Hits.Total.Value,
			"results": results,
		})
	})

	// === Sheet CRUD ===
	mux.HandleFunc("POST /api/sheets", func(w http.ResponseWriter, r *http.Request) {
		var req pb.CreateSpreadsheetRequest
		json.NewDecoder(r.Body).Decode(&req)
		uid := extractUID(r)
		req.UserId = uid
		resp, err := sheetClient.CreateSpreadsheet(injectToken(r), &req)
		if err != nil {
			writeJSON(w, map[string]interface{}{"success": false, "error": err.Error()})
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/sheets", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := sheetClient.ListSpreadsheets(injectToken(r), &pb.ListSpreadsheetsRequest{UserId: extractUID(r)})
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
		resp, err := sheetClient.GetSpreadsheet(injectToken(r), &pb.GetSpreadsheetRequest{Id: id, UserId: uid})
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
	})
	mux.HandleFunc("PUT /api/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		var req pb.UpdateSpreadsheetRequest
		json.NewDecoder(r.Body).Decode(&req)
		req.Id = parseInt64(r.PathValue("id"))
		req.UserId = extractUID(r)
		resp, err := sheetClient.UpdateSpreadsheet(injectToken(r), &req)
		if err != nil || resp == nil || !resp.Success {
			writeJSON(w, map[string]interface{}{"success": false, "error": "update failed"})
			return
		}
	})
	mux.HandleFunc("DELETE /api/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, err := sheetClient.DeleteSpreadsheet(injectToken(r), &pb.DeleteSpreadsheetRequest{Id: id, UserId: extractUID(r)})
		if err != nil {
			writeJSON(w, map[string]interface{}{"success": false, "error": err.Error()})
			return
		}
		writeJSON(w, resp)
	})

	// === File CRUD ===
	mux.HandleFunc("GET /api/files", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := fileClient.ListFiles(injectToken(r), &pb.ListFilesRequest{UserId: extractUID(r)})
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
			writeJSON(w, map[string]interface{}{"success": false, "error": "upload failed"})
			return
		}
		id := parseInt64(r.PathValue("id"))
		uid := extractUID(r)
		if uid == 0 {
			writeJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{"success": false, "error": "Unauthorized"})
			return
		}
		resp, err := fileClient.GetFile(injectToken(r), &pb.GetFileRequest{Id: id, UserId: uid})
		if err != nil {
			writeGRPCError(w, err, "Not found")
			return
		}
		if resp == nil || !resp.Success {
			writeJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
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
		resp, err := fileClient.DeleteFile(injectToken(r), &pb.DeleteFileRequest{Id: id, UserId: extractUID(r)})
		if err != nil {
			writeJSON(w, map[string]interface{}{"success": false, "error": err.Error()})
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
	if st, ok := status.FromError(err); ok {
		switch st.Code() {
		case codes.PermissionDenied, codes.Unauthenticated:
			code = http.StatusForbidden
			msg = "Forbidden"
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

func httpGet(url string) ([]byte, error) {
	resp, err := http.Get(url)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	return io.ReadAll(resp.Body)
}

func httpPost(url string, body []byte) ([]byte, error) {
	resp, err := http.Post(url, "application/json", strings.NewReader(string(body)))
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	return io.ReadAll(resp.Body)
}

func getenv(key, fallback string) string {
	if v := os.Getenv(key); v != "" { return v }
	return fallback
}

func base64urlEncode(s string) string {
	return strings.TrimRight(base64.URLEncoding.EncodeToString([]byte(s)), "=")
}
