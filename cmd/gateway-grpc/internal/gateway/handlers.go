package gateway

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"strconv"
	"sync/atomic"
	"time"

	"github.com/redis/go-redis/v9"
	"github.com/sony/gobreaker/v2"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"

	pb "gateway-grpc/gen/rpc"
)

// CBSlow wraps a circuit breaker with a slow-call counter.
type CBSlow struct {
	*gobreaker.CircuitBreaker[any]
	slowCalls atomic.Int64
}

// NewCBSlow creates a CBSlow with sensible defaults.
func NewCBSlow(name string, metricsCb func(name string, val float64)) *CBSlow {
	cbs := &CBSlow{}
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
			log.Printf("[cb] %s: %s -> %s", name, from, to)
			if to == gobreaker.StateClosed {
				cbs.slowCalls.Store(0)
			}
			if metricsCb != nil {
				stateVal := 0.0
				if to == gobreaker.StateHalfOpen {
					stateVal = 1.0
				} else if to == gobreaker.StateOpen {
					stateVal = 2.0
				}
				metricsCb(name, stateVal)
			}
		},
	})
	return cbs
}

// Gateway holds all dependencies for HTTP handlers.
type Gateway struct {
	AuthClient   AuthClientI
	SheetClient  SheetClientI
	FileClient   FileClientI
	SearchClient SearchClientI
	SharedClient SharedClientI
	RDB          *redis.Client

	JWTSecret []byte
	CBSearch  *CBSlow
	CBSheet   *CBSlow
	CBFile    *CBSlow
}

// ---- Helpers ----

func (g *Gateway) setCookies(w http.ResponseWriter, at, rt string) {
	if at != "" {
		http.SetCookie(w, &http.Cookie{Name: "rpc_at", Value: at, Path: "/", HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode})
	}
	if rt != "" {
		http.SetCookie(w, &http.Cookie{Name: "rpc_rt", Value: rt, Path: "/api/v1/refresh", HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode})
	}
}

func (g *Gateway) withAuth(ctx context.Context, r *http.Request) context.Context {
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

func (g *Gateway) injectToken(r *http.Request) context.Context {
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

func (g *Gateway) checkLoginRate(username string) bool {
	if username == "" || g.RDB == nil {
		return false
	}
	blockKey := "rate:login:" + username + ":blocked"
	if n, _ := g.RDB.Exists(context.Background(), blockKey).Result(); n > 0 {
		return true
	}
	minKey := "rate:login:" + username + ":" + time.Now().Format("2006-01-02T15:04")
	n, _ := g.RDB.Incr(context.Background(), minKey).Result()
	g.RDB.Expire(context.Background(), minKey, 60*time.Second)
	if n > 5 {
		g.RDB.Set(context.Background(), blockKey, "1", 5*time.Minute)
		return true
	}
	return false
}

// callWithRetry wraps a circuit breaker with gRPC retry logic.
func callWithRetry[T any](ctx context.Context, cb *CBSlow, maxAttempts int, label string, fn func(context.Context) (T, error)) (T, error) {
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

// ---- Auth Handlers ----

func (g *Gateway) Register(w http.ResponseWriter, r *http.Request) {
	var req pb.RegisterRequest
	json.NewDecoder(r.Body).Decode(&req)
	if msg, code := ValidateRegister(req.Username, req.Password); msg != "" {
		WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
		return
	}
	resp, _ := g.AuthClient.Register(r.Context(), &req)
	if resp != nil && !resp.Success {
		code := int32(0)
		if ec, ok := any(resp).(interface{ GetErrorCode() int32 }); ok {
			code = ec.GetErrorCode()
		}
		WriteError(w, nil, resp.GetError(), code)
		return
	}
	g.setCookies(w, resp.AccessToken, resp.RefreshToken)
	WriteJSON(w, resp)
}

func (g *Gateway) Login(w http.ResponseWriter, r *http.Request) {
	var req pb.LoginRequest
	json.NewDecoder(r.Body).Decode(&req)
	if g.checkLoginRate(req.Username) {
		WriteJSONStatus(w, http.StatusTooManyRequests,
			map[string]interface{}{"success": false, "error": "Too many attempts, try again later"})
		return
	}
	if msg, code := ValidateLogin(req.Username, req.Password); msg != "" {
		WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
		return
	}
	resp, _ := g.AuthClient.Login(r.Context(), &req)
	if resp != nil && !resp.Success {
		code := int32(0)
		if ec, ok := any(resp).(interface{ GetErrorCode() int32 }); ok {
			code = ec.GetErrorCode()
		}
		WriteError(w, nil, resp.GetError(), code)
		return
	}
	g.setCookies(w, resp.AccessToken, resp.RefreshToken)
	WriteJSON(w, resp)
}

func (g *Gateway) Refresh(w http.ResponseWriter, r *http.Request) {
	var req pb.RefreshTokenRequest
	json.NewDecoder(r.Body).Decode(&req)
	if req.RefreshToken == "" {
		if ck, err := r.Cookie("rpc_rt"); err == nil {
			req.RefreshToken = ck.Value
		}
	}
	if req.Username == "" {
		req.Username = GetUserFromCookie(r)
	}
	resp, err := g.AuthClient.RefreshToken(r.Context(), &req)
	if err != nil {
		log.Printf("[refresh] gRPC error: %v", err)
	}
	at := ""
	if resp != nil {
		at = resp.AccessToken
	}
	g.setCookies(w, at, "")
	WriteJSON(w, resp)
}

func (g *Gateway) ChangePassword(w http.ResponseWriter, r *http.Request) {
	var body struct {
		OldPassword string `json:"old_password"`
		NewPassword string `json:"new_password"`
	}
	json.NewDecoder(r.Body).Decode(&body)
	uid := ExtractUID(r)
	if msg, code := ValidateChangePassword(body.OldPassword, body.NewPassword); msg != "" {
		WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
		return
	}
	if uid == 0 {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}
	req := &pb.ChangePasswordRequest{UserId: uid, OldPassword: body.OldPassword, NewPassword: body.NewPassword}
	resp, err := g.AuthClient.ChangePassword(r.Context(), req)
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) OTPSend(w http.ResponseWriter, r *http.Request) {
	var body struct{ Phone string `json:"phone"` }
	json.NewDecoder(r.Body).Decode(&body)
	if body.Phone == "" {
		WriteJSONStatus(w, http.StatusBadRequest, map[string]interface{}{"success": false, "error": "phone required"})
		return
	}
	code := fmt.Sprintf("%06d", time.Now().UnixNano()%1000000)
	g.RDB.Set(r.Context(), "otp:"+body.Phone, code, 5*time.Minute)
	log.Printf("[OTP] phone=%s code=%s", body.Phone, code)
	WriteJSON(w, map[string]interface{}{"success": true})
}

func (g *Gateway) PhoneLogin(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Phone string `json:"phone"`
		OTP   string `json:"otp"`
	}
	json.NewDecoder(r.Body).Decode(&body)
	stored, _ := g.RDB.Get(r.Context(), "otp:"+body.Phone).Result()
	if stored == "" || stored != body.OTP {
		WriteJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{"success": false, "error": "Invalid OTP"})
		return
	}
	g.RDB.Del(r.Context(), "otp:"+body.Phone)
	resp, _ := g.AuthClient.LoginByPhone(r.Context(), &pb.PhoneLoginRequest{Phone: body.Phone, Otp: body.OTP})
	g.setCookies(w, resp.AccessToken, resp.RefreshToken)
	WriteJSON(w, resp)
}

// ---- Health / Me / Services ----

func (g *Gateway) Health(w http.ResponseWriter, r *http.Request) {
	WriteJSON(w, map[string]string{"gateway": "READY"})
}

func (g *Gateway) Me(w http.ResponseWriter, r *http.Request) {
	user := GetUserFromCookie(r)
	WriteJSON(w, map[string]interface{}{"username": user, "user_id": 0})
}

func (g *Gateway) Services(w http.ResponseWriter, r *http.Request) {
	WriteJSON(w, map[string]interface{}{"services": map[string][]string{
		"auth-service": {}, "sheet-service": {}, "file-service": {}, "search-service": {},
	}})
}

// ---- History ----

func (g *Gateway) History(w http.ResponseWriter, r *http.Request) {
	user := GetUserFromCookie(r)
	if user == "" {
		WriteJSONStatus(w, http.StatusUnauthorized, map[string]string{"error": "login required"})
		return
	}
	entries, _ := g.RDB.LRange(r.Context(), "call_logs:"+user, -20, -1).Result()
	if entries == nil {
		entries = []string{}
	}
	WriteJSON(w, map[string]interface{}{"user": user, "count": len(entries), "entries": entries})
}

// ---- Search ----

func (g *Gateway) Search(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Q    string `json:"q"`
		Sort string `json:"sort"`
	}
	json.NewDecoder(r.Body).Decode(&body)
	if msg, code := ValidateSearch(body.Q); msg != "" {
		WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
		return
	}
	resp, err := callWithRetry(r.Context(), g.CBSearch, 3, "search", func(ctx context.Context) (*pb.SearchResponse, error) {
		return g.SearchClient.Search(g.withAuth(ctx, r), &pb.SearchRequest{
			Query: body.Q, UserId: 0, Sort: body.Sort,
		})
	})
	WriteGRPCResponse(w, resp, err)
}

// ---- Sheet CRUD ----

func (g *Gateway) CreateSheet(w http.ResponseWriter, r *http.Request) {
	var req pb.CreateSpreadsheetRequest
	json.NewDecoder(r.Body).Decode(&req)
	req.UserId = 0
	resp, err := g.SheetClient.CreateSpreadsheet(g.injectToken(r), &req)
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) ListSheets(w http.ResponseWriter, r *http.Request) {
	resp, err := callWithRetry(r.Context(), g.CBSheet, 3, "sheet.list", func(ctx context.Context) (*pb.ListSpreadsheetsResponse, error) {
		return g.SheetClient.ListSpreadsheets(g.withAuth(ctx, r), &pb.ListSpreadsheetsRequest{UserId: 0})
	})
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) GetSheet(w http.ResponseWriter, r *http.Request) {
	id := parseInt64(r.PathValue("id"))
	caller := GetUserFromCookie(r)
	if caller == "" {
		WriteJSONStatus(w, http.StatusUnauthorized, map[string]interface{}{"success": false, "error": "Unauthorized"})
		return
	}
	resp, err := callWithRetry(r.Context(), g.CBSheet, 3, "sheet.get", func(ctx context.Context) (*pb.GetSpreadsheetResponse, error) {
		return g.SheetClient.GetSpreadsheet(g.withAuth(ctx, r), &pb.GetSpreadsheetRequest{Id: id, UserId: 0})
	})
	if err != nil {
		WriteGRPCError(w, err, "Not found")
		return
	}
	if resp == nil || !resp.Success {
		WriteJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
		return
	}
	if resp.Spreadsheet != nil && resp.Spreadsheet.Username != "" && resp.Spreadsheet.Username != caller {
		WriteJSONStatus(w, http.StatusForbidden, map[string]interface{}{"success": false, "error": "Forbidden"})
		return
	}
	if resp.Spreadsheet != nil {
		WriteJSON(w, resp)
	}
}

func (g *Gateway) UpdateSheet(w http.ResponseWriter, r *http.Request) {
	var req pb.UpdateSpreadsheetRequest
	json.NewDecoder(r.Body).Decode(&req)
	req.Id = parseInt64(r.PathValue("id"))
	req.UserId = 0
	resp, err := g.SheetClient.UpdateSpreadsheet(g.injectToken(r), &req)
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) DeleteSheet(w http.ResponseWriter, r *http.Request) {
	id := parseInt64(r.PathValue("id"))
	resp, err := callWithRetry(r.Context(), g.CBSheet, 2, "sheet.delete", func(ctx context.Context) (*pb.DeleteSpreadsheetResponse, error) {
		return g.SheetClient.DeleteSpreadsheet(g.withAuth(ctx, r), &pb.DeleteSpreadsheetRequest{Id: id, UserId: 0})
	})
	WriteGRPCResponse(w, resp, err)
}

// ---- Sharing ----

func (g *Gateway) ShareSheet(w http.ResponseWriter, r *http.Request) {
	uid := ExtractUID(r)
	var body struct {
		Username   string `json:"username"`
		Permission string `json:"permission"`
	}
	json.NewDecoder(r.Body).Decode(&body)
	id := parseInt64(r.PathValue("id"))
	resp, _ := g.SharedClient.Share(g.injectToken(r), &pb.ShareRequest{
		OwnerId: uid, ResourceType: "sheet", ResourceId: id,
		GranteeUsername: body.Username, Permission: body.Permission,
	})
	WriteJSON(w, resp)
}

func (g *Gateway) RevokeShare(w http.ResponseWriter, r *http.Request) {
	uid := ExtractUID(r)
	username := r.URL.Query().Get("username")
	id := parseInt64(r.PathValue("id"))
	resp, _ := g.SharedClient.Revoke(g.injectToken(r), &pb.RevokeRequest{
		OwnerId: uid, ResourceType: "sheet", ResourceId: id,
		GranteeUsername: username,
	})
	WriteJSON(w, resp)
}

func (g *Gateway) ListShares(w http.ResponseWriter, r *http.Request) {
	uid := ExtractUID(r)
	id := parseInt64(r.PathValue("id"))
	resp, _ := g.SharedClient.ListShares(g.injectToken(r), &pb.ResourceRequest{
		OwnerId: uid, ResourceType: "sheet", ResourceId: id,
	})
	WriteJSON(w, resp)
}

func (g *Gateway) CreateShareLink(w http.ResponseWriter, r *http.Request) {
	uid := ExtractUID(r)
	id := parseInt64(r.PathValue("id"))
	resp, _ := g.SharedClient.CreateShareLink(g.injectToken(r), &pb.ShareLinkRequest{
		OwnerId: uid, ResourceType: "sheet", ResourceId: id, Permission: "view",
	})
	WriteJSON(w, resp)
}

func (g *Gateway) ShareByToken(w http.ResponseWriter, r *http.Request) {
	token := r.PathValue("token")
	resp, _ := g.SharedClient.GetByToken(g.injectToken(r), &pb.ShareTokenRequest{Token: token})
	if resp == nil || !resp.Success {
		WriteJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
		return
	}
	info := resp.GetInfo()
	switch info.GetResourceType() {
	case "sheet":
		http.Redirect(w, r, fmt.Sprintf("/api/v1/sheets/%d", info.GetResourceId()), http.StatusFound)
	case "file":
		http.Redirect(w, r, fmt.Sprintf("/api/v1/files/%d", info.GetResourceId()), http.StatusFound)
	}
}

// ---- File CRUD ----

func (g *Gateway) Photos(w http.ResponseWriter, r *http.Request) {
	resp, err := g.FileClient.ListFiles(g.injectToken(r), &pb.ListFilesRequest{
		UserId: 0, MimeFilter: "image/",
	})
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) MoveFile(w http.ResponseWriter, r *http.Request) {
	id := parseInt64(r.PathValue("id"))
	var body struct{ TargetFolderId int64 `json:"target_folder_id"` }
	json.NewDecoder(r.Body).Decode(&body)
	resp, err := g.FileClient.MoveFile(g.injectToken(r), &pb.MoveFileRequest{Id: id, TargetFolderId: body.TargetFolderId})
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) CreateFolder(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Name           string `json:"name"`
		ParentFolderId int64  `json:"parent_folder_id"`
	}
	json.NewDecoder(r.Body).Decode(&body)
	resp, err := g.FileClient.CreateFolder(g.injectToken(r), &pb.CreateFolderRequest{
		UserId: 0, Name: body.Name, ParentFolderId: body.ParentFolderId,
	})
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) ListFiles(w http.ResponseWriter, r *http.Request) {
	resp, err := callWithRetry(r.Context(), g.CBFile, 3, "file.list", func(ctx context.Context) (*pb.ListFilesResponse, error) {
		return g.FileClient.ListFiles(g.withAuth(ctx, r), &pb.ListFilesRequest{UserId: 0})
	})
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) UploadFile(w http.ResponseWriter, r *http.Request) {
	r.ParseMultipartForm(50 << 20)
	f, h, _ := r.FormFile("file")
	if f == nil {
		http.Error(w, `{"error":"no file"}`, 400)
		return
	}
	defer f.Close()
	data, _ := io.ReadAll(f)
	resp, err := g.FileClient.CreateFile(g.injectToken(r), &pb.CreateFileRequest{
		UserId: 0, OriginalName: h.Filename, Size: int64(len(data)),
		MimeType: h.Header.Get("Content-Type"), FileContent: data,
	})
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) GetFile(w http.ResponseWriter, r *http.Request) {
	id := parseInt64(r.PathValue("id"))
	resp, err := callWithRetry(r.Context(), g.CBFile, 3, "file.get", func(ctx context.Context) (*pb.GetFileResponse, error) {
		return g.FileClient.GetFile(g.withAuth(ctx, r), &pb.GetFileRequest{Id: id, UserId: 0})
	})
	if err != nil {
		WriteGRPCError(w, err, "Not found")
		return
	}
	if resp == nil || !resp.Success {
		WriteJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
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
	WriteJSON(w, resp)
}

func (g *Gateway) DeleteFile(w http.ResponseWriter, r *http.Request) {
	id := parseInt64(r.PathValue("id"))
	resp, err := callWithRetry(r.Context(), g.CBFile, 2, "file.delete", func(ctx context.Context) (*pb.DeleteFileResponse, error) {
		return g.FileClient.DeleteFile(g.withAuth(ctx, r), &pb.DeleteFileRequest{Id: id, UserId: 0})
	})
	WriteGRPCResponse(w, resp, err)
}
