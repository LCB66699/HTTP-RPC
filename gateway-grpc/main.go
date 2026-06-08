package main

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/redis/go-redis/v9"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/keepalive"
	"google.golang.org/grpc/metadata"

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
		if ck, err := r.Cookie("rpc_rt"); err == nil {
			req.RefreshToken = ck.Value
		}
		json.NewDecoder(r.Body).Decode(&req)
		resp, _ := authClient.RefreshToken(r.Context(), &req)
		setCookies(w, resp.AccessToken, "")
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/health", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, map[string]string{"gateway": "READY"})
	})

	// Redis 客户端 — 读调用日志
	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	rdb := redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})

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

	// === Sheet CRUD ===
	mux.HandleFunc("POST /api/sheets", func(w http.ResponseWriter, r *http.Request) {
		var req pb.CreateSpreadsheetRequest
		json.NewDecoder(r.Body).Decode(&req)
		resp, _ := sheetClient.CreateSpreadsheet(injectToken(r), &req)
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/sheets", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := sheetClient.ListSpreadsheets(injectToken(r), &pb.ListSpreadsheetsRequest{})
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, _ := sheetClient.GetSpreadsheet(injectToken(r), &pb.GetSpreadsheetRequest{Id: id})
		writeJSON(w, resp)
	})
	mux.HandleFunc("PUT /api/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		var req pb.UpdateSpreadsheetRequest
		json.NewDecoder(r.Body).Decode(&req)
		req.Id = parseInt64(r.PathValue("id"))
		resp, _ := sheetClient.UpdateSpreadsheet(injectToken(r), &req)
		writeJSON(w, resp)
	})
	mux.HandleFunc("DELETE /api/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, err := sheetClient.DeleteSpreadsheet(injectToken(r), &pb.DeleteSpreadsheetRequest{Id: id})
		if err != nil {
			writeJSON(w, map[string]interface{}{"success": false, "error": err.Error()})
			return
		}
		writeJSON(w, resp)
	})

	// === File CRUD ===
	mux.HandleFunc("GET /api/files", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := fileClient.ListFiles(injectToken(r), &pb.ListFilesRequest{})
		writeJSON(w, resp)
	})
	mux.HandleFunc("POST /api/files/upload", func(w http.ResponseWriter, r *http.Request) {
		r.ParseMultipartForm(50 << 20)
		f, h, _ := r.FormFile("file")
		if f == nil {
			http.Error(w, `{"error":"no file"}`, 400)
			return
		}
		defer f.Close()
		data, _ := io.ReadAll(f)
		resp, _ := fileClient.CreateFile(injectToken(r), &pb.CreateFileRequest{
			UserId: 0, OriginalName: h.Filename, Size: int64(len(data)),
			MimeType: h.Header.Get("Content-Type"), FileContent: data,
		})
		writeJSON(w, map[string]interface{}{"success": true, "id": resp.Id})
	})
	mux.HandleFunc("GET /api/files/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, _ := fileClient.GetFile(injectToken(r), &pb.GetFileRequest{Id: id})
		if resp.GetFileContent() != nil {
			w.Header().Set("Content-Type", resp.File.MimeType)
			w.Write(resp.FileContent)
			return
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("DELETE /api/files/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, _ := fileClient.DeleteFile(injectToken(r), &pb.DeleteFileRequest{Id: id})
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
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
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
			strings.HasPrefix(path, "/api/refresh") || strings.HasPrefix(path, "/api/health") {
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

func getenv(key, fallback string) string {
	if v := os.Getenv(key); v != "" { return v }
	return fallback
}

func base64urlEncode(s string) string {
	return strings.TrimRight(base64.URLEncoding.EncodeToString([]byte(s)), "=")
}
