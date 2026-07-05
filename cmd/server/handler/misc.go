package handler

import (
	"net/http"

	"github.com/gin-gonic/gin"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	pb "gateway-grpc/gen/rpc"
)

func (h *Handlers) Health(c *gin.Context) {
	c.JSON(http.StatusOK, gin.H{"gateway": "READY"})
}

func (h *Handlers) HealthReady(c *gin.Context) {
	// Simple readiness — in a full implementation, check gRPC connections.
	c.JSON(http.StatusOK, gin.H{"gateway": "READY"})
}

func (h *Handlers) Metrics(c *gin.Context) {
	promhttp.Handler().ServeHTTP(c.Writer, c.Request)
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
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"success": false, "error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, resp)
}
