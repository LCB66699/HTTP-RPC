package handler

import (
	"net/http"
	"strings"

	"github.com/gin-gonic/gin"
	pb "gateway-grpc/gen/rpc"

	"github.com/lcb66699/http-rpc/server/middleware"
)

func cbStatus(cb *middleware.CBSlow) gin.H {
	if cb == nil {
		return gin.H{"breaker": "unknown"}
	}
	state := cb.State().String()
	channel := "READY"
	if state == "open" {
		channel = "DEGRADED"
	}
	return gin.H{"channel": channel, "breaker": strings.ToUpper(state)}
}

func (h *Handlers) Health(c *gin.Context) {
	c.JSON(http.StatusOK, gin.H{
		"gateway": "READY",
		"auth":    cbStatus(h.CBAuth),
		"sheet":   cbStatus(h.CBSheet),
		"file":    cbStatus(h.CBFile),
		"search":  cbStatus(h.CBSearch),
	})
}

func (h *Handlers) HealthReady(c *gin.Context) {
	allReady := true
	for _, cb := range []*middleware.CBSlow{h.CBAuth, h.CBSheet, h.CBFile, h.CBSearch} {
		if cb != nil && cb.State().String() == "open" {
			allReady = false
			break
		}
	}
	status := http.StatusOK
	if !allReady {
		status = http.StatusServiceUnavailable
	}
	c.JSON(status, gin.H{"ready": allReady})
}

func (h *Handlers) Me(c *gin.Context) {
	c.JSON(http.StatusOK, gin.H{
		"username": h.username(c),
		"user_id":  h.uid(c),
	})
}

func (h *Handlers) Services(c *gin.Context) {
	c.JSON(http.StatusOK, gin.H{
		"services": map[string][]string{
			"auth-service": {}, "sheet-service": {}, "file-service": {}, "search-service": {},
		},
	})
}

func (h *Handlers) History(c *gin.Context) {
	user := h.username(c)
	entries, _ := h.RDB.LRange(c.Request.Context(), "call_logs:"+user, -20, -1).Result()
	if entries == nil {
		entries = []string{}
	}
	c.JSON(http.StatusOK, gin.H{
		"user": user, "count": len(entries), "entries": entries,
	})
}

func (h *Handlers) Search(c *gin.Context) {
	var body struct {
		Q    string `json:"q"`
		Sort string `json:"sort"`
	}
	if err := c.ShouldBindJSON(&body); err != nil || body.Q == "" {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "q required"})
		return
	}
	resp, err := h.SearchClient.Search(h.token(c.Request.Context(), c), &pb.SearchRequest{
		Query: body.Q, UserId: 0, Sort: body.Sort,
	})
	if grpcErr(c, err, "search failed") { return }
	c.JSON(http.StatusOK, resp)
}
