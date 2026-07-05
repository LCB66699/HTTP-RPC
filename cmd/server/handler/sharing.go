package handler

import (
	"fmt"
	"net/http"

	"github.com/gin-gonic/gin"
	pb "gateway-grpc/gen/rpc"
)

func (h *Handlers) ShareSheet(c *gin.Context) {
	id := parseID(c)
	var body struct {
		Username   string `json:"username"`
		Permission string `json:"permission"`
	}
	if err := c.ShouldBindJSON(&body); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid request"})
		return
	}
	resp, err := h.Share.Share(h.token(c.Request.Context(), c), &pb.ShareRequest{
		OwnerId: h.uid(c), ResourceType: "sheet", ResourceId: id,
		GranteeUsername: body.Username, Permission: body.Permission,
	})
	if grpcErr(c, err, "sharing operation failed") { return }
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) RevokeShare(c *gin.Context) {
	id := parseID(c)
	username := c.Param("username")
	resp, err := h.Share.Revoke(h.token(c.Request.Context(), c), &pb.RevokeRequest{
		OwnerId: h.uid(c), ResourceType: "sheet", ResourceId: id,
		GranteeUsername: username,
	})
	if grpcErr(c, err, "sharing operation failed") { return }
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) ListShares(c *gin.Context) {
	id := parseID(c)
	resp, err := h.Share.ListShares(h.token(c.Request.Context(), c), &pb.ResourceRequest{
		OwnerId: h.uid(c), ResourceType: "sheet", ResourceId: id,
	})
	if grpcErr(c, err, "sharing operation failed") { return }
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) CreateShareLink(c *gin.Context) {
	id := parseID(c)
	resp, err := h.Share.CreateShareLink(h.token(c.Request.Context(), c), &pb.ShareLinkRequest{
		OwnerId: h.uid(c), ResourceType: "sheet", ResourceId: id, Permission: "view",
	})
	if grpcErr(c, err, "sharing operation failed") { return }
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) ShareByToken(c *gin.Context) {
	token := c.Param("token")
	if h.Share == nil {
		c.JSON(http.StatusNotFound, gin.H{"success": false, "error": "Not found"})
		return
	}
	resp, err := h.Share.GetByToken(h.token(c.Request.Context(), c), &pb.ShareTokenRequest{Token: token})
	if err != nil || !resp.GetSuccess() {
		c.JSON(http.StatusNotFound, gin.H{"success": false, "error": "Not found"})
		return
	}
	info := resp.GetInfo()
	if info == nil {
		c.JSON(http.StatusInternalServerError, gin.H{"success": false, "error": "invalid response"})
		return
	}
	switch info.GetResourceType() {
	case "sheet":
		c.Redirect(http.StatusFound, fmt.Sprintf("/api/v1/sheets/%d", info.GetResourceId()))
	case "file":
		c.Redirect(http.StatusFound, fmt.Sprintf("/api/v1/files/%d", info.GetResourceId()))
	default:
		c.JSON(http.StatusNotFound, gin.H{"success": false, "error": "unknown resource type"})
	}
}
