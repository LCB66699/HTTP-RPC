package handler

import (
	"io"
	"net/http"

	"github.com/gin-gonic/gin"
	pb "gateway-grpc/gen/rpc"
)

func (h *Handlers) UploadFile(c *gin.Context) {
	idemKey := c.GetHeader("Idempotency-Key")

	f, fh, err := c.Request.FormFile("file")
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "no file"})
		return
	}
	defer f.Close()
	data, _ := io.ReadAll(f)

	resp, err := h.File.CreateFile(h.token(c.Request.Context(), c), &pb.CreateFileRequest{
		UserId: 0, OriginalName: fh.Filename, Size: int64(len(data)),
		MimeType: fh.Header.Get("Content-Type"), FileContent: data,
		IdempotencyKey: idemKey,
	})
	if grpcErr(c, err, "file operation failed") { return }
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) GetFile(c *gin.Context) {
	id := parseID(c)
	resp, err := h.File.GetFile(h.token(c.Request.Context(), c), &pb.GetFileRequest{Id: id})
	if err != nil {
		c.JSON(http.StatusNotFound, gin.H{"success": false, "error": "Not found"})
		return
	}
	if resp == nil || !resp.Success {
		c.JSON(http.StatusNotFound, gin.H{"success": false, "error": "Not found"})
		return
	}
	if resp.GetDownloadUrl() != "" {
		c.Redirect(http.StatusFound, resp.GetDownloadUrl())
		return
	}
	if resp.File != nil {
		c.Data(http.StatusOK, resp.File.GetMimeType(), resp.FileContent)
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) DeleteFile(c *gin.Context) {
	id := parseID(c)
	resp, err := h.File.DeleteFile(h.token(c.Request.Context(), c), &pb.DeleteFileRequest{Id: id, UserId: 0})
	if grpcErr(c, err, "file operation failed") { return }
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) ListFiles(c *gin.Context) {
	resp, err := h.File.ListFiles(h.token(c.Request.Context(), c), &pb.ListFilesRequest{UserId: 0})
	if grpcErr(c, err, "file operation failed") { return }
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) MoveFile(c *gin.Context) {
	id := parseID(c)
	var body struct{ TargetFolderId int64 `json:"target_folder_id"` }
	if err := c.ShouldBindJSON(&body); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid request"})
		return
	}
	resp, err := h.File.MoveFile(h.token(c.Request.Context(), c), &pb.MoveFileRequest{Id: id, TargetFolderId: body.TargetFolderId})
	if grpcErr(c, err, "file operation failed") { return }
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) CreateFolder(c *gin.Context) {
	var body struct {
		Name           string `json:"name"`
		ParentFolderId int64  `json:"parent_folder_id"`
	}
	if err := c.ShouldBindJSON(&body); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid request"})
		return
	}
	resp, err := h.File.CreateFolder(h.token(c.Request.Context(), c), &pb.CreateFolderRequest{
		UserId: 0, Name: body.Name, ParentFolderId: body.ParentFolderId,
	})
	if grpcErr(c, err, "file operation failed") { return }
	c.JSON(http.StatusOK, resp)
}
