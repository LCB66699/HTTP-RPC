package gateway

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net/http"
	"strconv"
	"time"

	"github.com/redis/go-redis/v9"
	"google.golang.org/grpc/metadata"

	pb "gateway-grpc/gen/rpc"
)

// Gateway holds all dependencies for HTTP handlers.
type Gateway struct {
	AuthClient   AuthClientI
	SheetClient  SheetClientI
	FileClient   FileClientI
	SearchClient SearchClientI
	SharedClient SharedClientI
	RDB          *redis.Client

	CBSearch *CBSlow
	CBSheet  *CBSlow
	CBFile   *CBSlow
}

// ---- Helpers ----

func (g *Gateway) setCookies(w http.ResponseWriter, at, rt string) {
	if at != "" {
		http.SetCookie(w, &http.Cookie{
			Name: "rpc_at", Value: at, Path: "/api",
			MaxAge: 900, HttpOnly: true, Secure: true,
			SameSite: http.SameSiteLaxMode,
		})
	}
	if rt != "" {
		http.SetCookie(w, &http.Cookie{
			Name: "rpc_rt", Value: rt, Path: "/api/v1/refresh",
			MaxAge: 604800, HttpOnly: true, Secure: true,
			SameSite: http.SameSiteStrictMode,
		})
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

func randomOTP() string {
	b := make([]byte, 4)
	if _, err := rand.Read(b); err != nil {
		return fmt.Sprintf("%06d", time.Now().UnixNano()%1000000)
	}
	n := binary.BigEndian.Uint32(b) % 1000000
	return fmt.Sprintf("%06d", n)
}

func (g *Gateway) checkLoginRate(ctx context.Context, username string) bool {
	if username == "" || g.RDB == nil {
		return false
	}
	blockKey := "rate:login:" + username + ":blocked"
	if n, _ := g.RDB.Exists(ctx, blockKey).Result(); n > 0 {
		return true
	}
	minKey := "rate:login:" + username + ":" + time.Now().Format("2006-01-02T15:04")
	n, _ := g.RDB.Incr(ctx, minKey).Result()
	g.RDB.Expire(ctx, minKey, 60*time.Second)
	if n > 5 {
		g.RDB.Set(ctx, blockKey, "1", 5*time.Minute)
		return true
	}
	return false
}

// ---- Health / Me / Services ----

func (g *Gateway) Health(w http.ResponseWriter, r *http.Request) {
	WriteJSON(w, map[string]string{"gateway": "READY"})
}

func (g *Gateway) Me(w http.ResponseWriter, r *http.Request) {
	user := GetUserFromCookie(r)
	uid := ExtractUID(r)
	WriteJSON(w, map[string]interface{}{"username": user, "user_id": uid})
}

func (g *Gateway) Services(w http.ResponseWriter, r *http.Request) {
	WriteJSON(w, map[string]interface{}{"services": map[string][]string{
		"auth-service": {}, "sheet-service": {}, "file-service": {}, "search-service": {},
	}})
}

// ---- History ----

func (g *Gateway) History(w http.ResponseWriter, r *http.Request) {
	user := GetUserFromCookie(r)
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
	resp, err := g.SearchClient.Search(g.withAuth(r.Context(), r), &pb.SearchRequest{
		Query: body.Q, UserId: 0, Sort: body.Sort,
	})
	WriteGRPCResponse(w, resp, err)
}
