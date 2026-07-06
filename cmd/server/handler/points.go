package handler

import (
	"context"
	"encoding/json"
	"net/http"

	"github.com/gin-gonic/gin"
	pb "gateway-grpc/gen/rpc"
)

func (h *Handlers) RegisterPointsRoutes(auth *gin.RouterGroup) {
	auth.GET("/points/balance", h.GetBalance)
	auth.GET("/points/transactions", h.GetTransactions)
	auth.GET("/points/leaderboard", h.GetLeaderboard)
}

func (h *Handlers) GetBalance(c *gin.Context) {
	resp, err := h.Points.GetBalance(c.Request.Context(), &pb.GetBalanceRequest{
		UserId: h.uid(c),
	})
	if grpcErr(c, err, "get balance failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) GetTransactions(c *gin.Context) {
	limit := int32(20)
	resp, err := h.Points.GetTransactions(c.Request.Context(), &pb.GetTransactionsRequest{
		UserId: h.uid(c),
		Limit:  limit,
	})
	if grpcErr(c, err, "get transactions failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) GetLeaderboard(c *gin.Context) {
	resp, err := h.Points.GetLeaderboard(c.Request.Context(), &pb.LeaderboardRequest{Limit: 20})
	if grpcErr(c, err, "get leaderboard failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

// publishPointEvent sends a point-earning event to Redis "pts:earn" channel.
// The points service consumes these events independently.
// Uses background context — fire-and-forget, don't block user request.
func (h *Handlers) publishPointEvent(uid int64, eventType, idempotencyKey string) {
	if h.RDB == nil || uid == 0 {
		return
	}
	body, _ := json.Marshal(map[string]interface{}{
		"type":    eventType,
		"user_id": uid,
		"key":     idempotencyKey,
	})
	h.RDB.Publish(context.Background(), "pts:earn", string(body))
}
