package ws

import (
	"net/http"
	"net/url"

	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"
)

// Handler handles WebSocket upgrade requests.
type Handler struct {
	Hub *Hub
	AllowedOrigins []string
}

func (h *Handler) checkOrigin(r *http.Request) bool {
	origin := r.Header.Get("Origin")
	if origin == "" {
		return true
	}
	for _, allowed := range h.AllowedOrigins {
		if origin == allowed {
			return true
		}
	}
	if len(h.AllowedOrigins) > 0 {
		return false
	}
	parsed, err := url.Parse(origin)
	if err != nil || parsed.Scheme == "" || parsed.Host == "" {
		return false
	}
	scheme := "http"
	if r.TLS != nil {
		scheme = "https"
	}
	return parsed.Scheme == scheme && parsed.Host == r.Host && parsed.Path == "" && parsed.RawQuery == "" && parsed.Fragment == ""
}

// ServeWS upgrades the HTTP connection to WebSocket.
func (h *Handler) ServeWS(c *gin.Context) {
	uid, _ := c.Get("uid")
	userID, _ := uid.(int64)
	if userID == 0 {
		c.JSON(http.StatusUnauthorized, gin.H{"error": "authentication required"})
		return
	}

	upgrader := websocket.Upgrader{
		ReadBufferSize:  1024,
		WriteBufferSize: 1024,
		CheckOrigin:     h.checkOrigin,
	}
	conn, err := upgrader.Upgrade(c.Writer, c.Request, nil)
	if err != nil {
		return
	}

	client := NewClient(h.Hub, conn, userID)
	go client.WritePump()
	go client.ReadPump()
}
