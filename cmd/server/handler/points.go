package handler

import (
	"context"
	"net/http"
	"time"

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

// earnPoints is called internally after user actions that grant points.
// Uses background context — fire-and-forget, don't block user request.
func (h *Handlers) earnPoints(uid int64, amount int64, reason, idempotencyKey string) {
	if h.Points == nil || uid == 0 || amount <= 0 {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	_, _ = h.Points.Earn(ctx, &pb.EarnRequest{
		UserId:         uid,
		Amount:         amount,
		Reason:         reason,
		IdempotencyKey: idempotencyKey,
	})
}
