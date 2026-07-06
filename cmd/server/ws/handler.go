package ws

import (
	"net/http"

	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	ReadBufferSize:  1024,
	WriteBufferSize: 1024,
	CheckOrigin:     func(r *http.Request) bool { return true },
}

// Handler handles WebSocket upgrade requests.
type Handler struct {
	Hub *Hub
}

// ServeWS upgrades the HTTP connection to WebSocket.
func (h *Handler) ServeWS(c *gin.Context) {
	uid, _ := c.Get("uid")
	userID, _ := uid.(int64)
	if userID == 0 {
		c.JSON(http.StatusUnauthorized, gin.H{"error": "authentication required"})
		return
	}

	conn, err := upgrader.Upgrade(c.Writer, c.Request, nil)
	if err != nil {
		return
	}

	client := NewClient(h.Hub, conn, userID)
	go client.WritePump()
	go client.ReadPump()
}
