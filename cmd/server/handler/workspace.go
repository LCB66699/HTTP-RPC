package handler

import (
	"net/http"

	"github.com/gin-gonic/gin"
	pb "gateway-grpc/gen/rpc"
)

func (h *Handlers) RegisterWorkspaceRoutes(auth *gin.RouterGroup) {
	auth.POST("/workspaces", h.CreateWorkspace)
	auth.GET("/workspaces", h.ListWorkspaces)
	auth.GET("/workspaces/:id", h.GetWorkspace)
	auth.PUT("/workspaces/:id", h.UpdateWorkspace)
	auth.DELETE("/workspaces/:id", h.DeleteWorkspace)
	auth.POST("/workspaces/:id/members", h.AddWorkspaceMember)
	auth.DELETE("/workspaces/:id/members/:uid", h.RemoveWorkspaceMember)
}

func (h *Handlers) CreateWorkspace(c *gin.Context) {
	var body struct {
		Name string `json:"name"`
	}
	if err := c.ShouldBindJSON(&body); err != nil || body.Name == "" {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "name required"})
		return
	}
	uid := h.uid(c)
	resp, err := h.Workspace.Create(h.token(c.Request.Context(), c), &pb.CreateWorkspaceRequest{
		Name: body.Name, OwnerId: uid,
	})
	if grpcErr(c, err, "create workspace failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) ListWorkspaces(c *gin.Context) {
	uid := h.uid(c)
	resp, err := h.Workspace.List(h.token(c.Request.Context(), c), &pb.ListWorkspacesRequest{
		UserId: uid,
	})
	if grpcErr(c, err, "list workspaces failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) GetWorkspace(c *gin.Context) {
	id := parseID(c)
	resp, err := h.Workspace.Get(h.token(c.Request.Context(), c), &pb.GetWorkspaceRequest{Id: id})
	if grpcErr(c, err, "get workspace failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) UpdateWorkspace(c *gin.Context) {
	id := parseID(c)
	var body struct{ Name string `json:"name"` }
	if err := c.ShouldBindJSON(&body); err != nil || body.Name == "" {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "name required"})
		return
	}
	resp, err := h.Workspace.Update(h.token(c.Request.Context(), c), &pb.UpdateWorkspaceRequest{Id: id, Name: body.Name})
	if grpcErr(c, err, "update workspace failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) DeleteWorkspace(c *gin.Context) {
	id := parseID(c)
	resp, err := h.Workspace.Delete(h.token(c.Request.Context(), c), &pb.DeleteWorkspaceRequest{Id: id})
	if grpcErr(c, err, "delete workspace failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) AddWorkspaceMember(c *gin.Context) {
	id := parseID(c)
	var body struct {
		Username string `json:"username"`
		Role     string `json:"role"`
	}
	if err := c.ShouldBindJSON(&body); err != nil || body.Username == "" {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "username required"})
		return
	}
	resp, err := h.Workspace.AddMember(h.token(c.Request.Context(), c), &pb.AddMemberRequest{
		Id: id, Username: body.Username, Role: body.Role,
	})
	if grpcErr(c, err, "add member failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) RemoveWorkspaceMember(c *gin.Context) {
	id := parseID(c)
	uidParam, _ := strconv.ParseInt(c.Param("uid"), 10, 64)
	resp, err := h.Workspace.RemoveMember(h.token(c.Request.Context(), c), &pb.RemoveMemberRequest{
		Id: id, UserId: uidParam,
	})
	if grpcErr(c, err, "remove member failed") {
		return
	}
	c.JSON(http.StatusOK, resp)
}
