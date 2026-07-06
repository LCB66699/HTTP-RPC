package handler

import (
	"net/http"

	"github.com/gin-gonic/gin"
	pb "gateway-grpc/gen/rpc"
)

func (h *Handlers) RegisterMallRoutes(auth *gin.RouterGroup) {
	auth.GET("/mall/products", h.ListProducts)
	auth.GET("/mall/products/:id", h.GetProduct)
	auth.GET("/mall/seckills", h.ListSeckills)
	auth.POST("/mall/seckill/order", h.SeckillOrder)
	auth.GET("/mall/orders", h.ListOrders)
}

func (h *Handlers) ListProducts(c *gin.Context) {
	resp, err := h.Mall.ListProducts(c.Request.Context(), &pb.ListProductsRequest{})
	if grpcErr(c, err, "list products failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) GetProduct(c *gin.Context) {
	id := parseID(c)
	resp, err := h.Mall.GetProduct(c.Request.Context(), &pb.GetProductRequest{Id: id})
	if grpcErr(c, err, "get product failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) ListSeckills(c *gin.Context) {
	resp, err := h.Mall.ListSeckills(c.Request.Context(), &pb.ListSeckillsRequest{})
	if grpcErr(c, err, "list seckills failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) SeckillOrder(c *gin.Context) {
	var body struct {
		SeckillID      int64  `json:"seckill_id"`
		IdempotencyKey string `json:"idempotency_key"`
	}
	if err := c.ShouldBindJSON(&body); err != nil || body.SeckillID == 0 {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "seckill_id required"})
		return
	}
	if body.IdempotencyKey == "" {
		body.IdempotencyKey = c.GetHeader("Idempotency-Key")
	}
	resp, err := h.Mall.SeckillOrder(c.Request.Context(), &pb.SeckillOrderRequest{
		SeckillId:      body.SeckillID,
		IdempotencyKey: body.IdempotencyKey,
		UserId:         h.uid(c),
	})
	if grpcErr(c, err, "seckill order failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) ListOrders(c *gin.Context) {
	resp, err := h.Mall.ListOrders(c.Request.Context(), &pb.ListOrdersRequest{
		UserId: h.uid(c),
	})
	if grpcErr(c, err, "list orders failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}
