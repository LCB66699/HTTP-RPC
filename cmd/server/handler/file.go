package handler

import (
	"bytes"
	"errors"
	"io"
	"net/http"

	"github.com/gin-gonic/gin"
	pb "gateway-grpc/gen/rpc"
)

const defaultMaxUploadBytes int64 = 50 * 1024 * 1024

func (h *Handlers) maxUploadBytes() int64 {
	if h.MaxUploadBytes > 0 {
		return h.MaxUploadBytes
	}
	return defaultMaxUploadBytes
}

func (h *Handlers) RegisterFileRoutes(auth *gin.RouterGroup) {
	auth.POST("/files/upload", h.UploadFile)
	auth.GET("/files", h.ListFiles)
	auth.GET("/files/:id", h.GetFile)
	auth.DELETE("/files/:id", h.DeleteFile)
	auth.PUT("/files/:id/move", h.MoveFile)
	auth.POST("/files/folder", h.CreateFolder)
}

func (h *Handlers) UploadFile(c *gin.Context) {
	idemKey := c.GetHeader("Idempotency-Key")
	maxBytes := h.maxUploadBytes()
	c.Request.Body = http.MaxBytesReader(c.Writer, c.Request.Body, maxBytes)
	if err := c.Request.ParseMultipartForm(maxBytes); err != nil {
		var maxBytesErr *http.MaxBytesError
		if errors.As(err, &maxBytesErr) {
			c.JSON(http.StatusRequestEntityTooLarge, gin.H{"success": false, "error": "upload too large"})
			return
		}
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid multipart upload"})
		return
	}
	if c.Request.MultipartForm != nil {
		defer c.Request.MultipartForm.RemoveAll()
	}

	f, fh, err := c.Request.FormFile("file")
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid file part"})
		return
	}
	defer func() { _ = f.Close() }()
	var data bytes.Buffer
	if _, err := io.Copy(&data, io.LimitReader(f, maxBytes+1)); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "failed to read file"})
		return
	}
	if int64(data.Len()) > maxBytes {
		c.JSON(http.StatusRequestEntityTooLarge, gin.H{"success": false, "error": "upload too large"})
		return
	}

	resp, err := h.File.CreateFile(h.token(c.Request.Context(), c), &pb.CreateFileRequest{
		UserId: 0, OriginalName: fh.Filename, Size: int64(data.Len()),
		MimeType: fh.Header.Get("Content-Type"), FileContent: data.Bytes(),
		IdempotencyKey: idemKey,
	})
	if grpcErr(c, err, "file operation failed") { return }
	writeProtoJSON(c, http.StatusOK, resp)
}

func (h *Handlers) GetFile(c *gin.Context) {
	id := parseID(c)
	if c.IsAborted() { return }
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
	writeProtoJSON(c, http.StatusOK, resp)
}

func (h *Handlers) DeleteFile(c *gin.Context) {
	id := parseID(c)
	if c.IsAborted() { return }
	resp, err := h.File.DeleteFile(h.token(c.Request.Context(), c), &pb.DeleteFileRequest{Id: id, UserId: 0})
	if grpcErr(c, err, "file operation failed") { return }
	writeProtoJSON(c, http.StatusOK, resp)
}

func (h *Handlers) ListFiles(c *gin.Context) {
	resp, err := h.File.ListFiles(h.token(c.Request.Context(), c), &pb.ListFilesRequest{UserId: 0})
	if grpcErr(c, err, "file operation failed") { return }
	writeProtoJSON(c, http.StatusOK, resp)
}

func (h *Handlers) MoveFile(c *gin.Context) {
	id := parseID(c)
	if c.IsAborted() { return }
	var body struct{ TargetFolderId int64 `json:"target_folder_id"` }
	if err := c.ShouldBindJSON(&body); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid request"})
		return
	}
	resp, err := h.File.MoveFile(h.token(c.Request.Context(), c), &pb.MoveFileRequest{Id: id, TargetFolderId: body.TargetFolderId})
	if grpcErr(c, err, "file operation failed") { return }
	writeProtoJSON(c, http.StatusOK, resp)
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
	writeProtoJSON(c, http.StatusOK, resp)
}
