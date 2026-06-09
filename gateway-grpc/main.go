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
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/primitive"
	"go.mongodb.org/mongo-driver/mongo"
	"go.mongodb.org/mongo-driver/mongo/options"
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

	// MongoDB
	mongoURI := getenv("MONGO_URI", "mongodb://mongodb:27017")
	mongoClient, _ := mongo.Connect(context.Background(), options.Client().ApplyURI(mongoURI))
	db := mongoClient.Database("rpc_search")
	filesColl := db.Collection("doc_contents")
	sheetsColl := db.Collection("sheet_contents")

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
		q, _ := req["q"].(string)
		uid := extractUID(r)
		must := []interface{}{}
		if q != "" {
			must = append(must, map[string]interface{}{
				"bool": map[string]interface{}{
					"should": []interface{}{
						map[string]interface{}{"match_phrase": map[string]interface{}{"name": q}},
						map[string]interface{}{"match_phrase": map[string]interface{}{"description": q}},
						map[string]interface{}{"match_phrase": map[string]interface{}{"original_name": q}},
					},
					"minimum_should_match": 1,
				},
			})
		} else {
			must = append(must, map[string]interface{}{"match_all": map[string]interface{}{}})
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
		resp, err := httpPost("http://elasticsearch:9200/sheets_search,files_search/_search", body)
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
		if resp.Success {
			sheetsColl.UpdateOne(r.Context(),
				bson.M{"sheet_id": resp.Id},
				bson.M{"$set": bson.M{
					"sheet_id": resp.Id, "user_id": uid,
					"name": req.Name, "description": req.Description,
					"headers_json": req.HeadersJson, "data_json": req.DataJson,
					"updated_at": time.Now().UTC().Format(time.RFC3339),
				}},
				options.Update().SetUpsert(true))
		}
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/sheets", func(w http.ResponseWriter, r *http.Request) {
		resp, _ := sheetClient.ListSpreadsheets(injectToken(r), &pb.ListSpreadsheetsRequest{UserId: extractUID(r)})
		writeJSON(w, resp)
	})
	mux.HandleFunc("GET /api/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, err := sheetClient.GetSpreadsheet(injectToken(r), &pb.GetSpreadsheetRequest{Id: id, UserId: extractUID(r)})
		if err != nil || resp == nil || !resp.Success {
			writeJSON(w, map[string]interface{}{"success": false, "error": "Not found"})
			return
		}
		if resp.Spreadsheet != nil {
			var doc bson.M
			if sheetsColl.FindOne(r.Context(), bson.M{"sheet_id": id}).Decode(&doc) == nil {
				if v, ok := doc["headers_json"].(string); ok { resp.Spreadsheet.HeadersJson = v }
				if v, ok := doc["data_json"].(string); ok { resp.Spreadsheet.DataJson = v }
				resp.CacheSource = "mongodb"
			}
		}
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
		sheetsColl.UpdateOne(r.Context(),
				bson.M{"sheet_id": req.Id},
				bson.M{"$set": bson.M{
					"name": req.Name, "description": req.Description,
					"headers_json": req.HeadersJson, "data_json": req.DataJson,
					"updated_at": time.Now().UTC().Format(time.RFC3339),
				}})
		writeJSON(w, resp)
	})
	mux.HandleFunc("DELETE /api/sheets/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, err := sheetClient.DeleteSpreadsheet(injectToken(r), &pb.DeleteSpreadsheetRequest{Id: id, UserId: extractUID(r)})
		if err != nil {
			writeJSON(w, map[string]interface{}{"success": false, "error": err.Error()})
			return
		}
		sheetsColl.DeleteOne(r.Context(), bson.M{"sheet_id": id})
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
		filesColl.UpdateOne(r.Context(),
			bson.M{"file_id": resp.Id},
			bson.M{"$set": bson.M{
				"file_id": resp.Id, "user_id": uid,
				"original_name": h.Filename, "mime_type": h.Header.Get("Content-Type"),
				"size": len(data), "file_content": data,
				"parsed_at": time.Now().UTC(),
			}},
			options.Update().SetUpsert(true))
		writeJSON(w, map[string]interface{}{"success": resp.Success, "id": resp.Id})
	})
	mux.HandleFunc("GET /api/files/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := parseInt64(r.PathValue("id"))
		resp, _ := fileClient.GetFile(injectToken(r), &pb.GetFileRequest{Id: id, UserId: extractUID(r)})
		if resp.Success && resp.File != nil {
			var doc bson.M
			if filesColl.FindOne(r.Context(), bson.M{"file_id": id}).Decode(&doc) == nil {
				if raw, ok := doc["file_content"].(primitive.Binary); ok {
					resp.FileContent = raw.Data
				}
			}
		}
		if resp.GetFileContent() != nil {
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
		filesColl.DeleteOne(r.Context(), bson.M{"file_id": id})
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
	data, _ := json.Marshal(v)
	// Snowflake IDs (17+ digits) exceed JS precision, quote as strings
	re := regexp.MustCompile(`:(\d{16,})`)
	data = re.ReplaceAll(data, []byte(`:"$1"`))
	w.Write(data)
}

func stringifyIDs(v interface{}) interface{} { return v }

func extractUID(r *http.Request) int64 {
	for _, c := range r.Cookies() {
		if c.Name == "rpc_at" {
			parts := strings.SplitN(c.Value, ".", 3)
			if len(parts) != 3 {
				break
			}
			raw, _ := base64.RawURLEncoding.DecodeString(parts[1])
			var claims map[string]interface{}
			dec := json.NewDecoder(strings.NewReader(string(raw)))
			dec.UseNumber()
			dec.Decode(&claims)
			if num, ok := claims["uid"].(json.Number); ok {
				n, _ := num.Int64()
				return n
			}
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
