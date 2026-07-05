package handler

import (
	"encoding/json"
	"net/http"

	"github.com/gin-gonic/gin"
	pb "gateway-grpc/gen/rpc"
)

func (h *Handlers) CreateSheet(c *gin.Context) {
	var req pb.CreateSpreadsheetRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid request body"})
		return
	}
	req.UserId = h.uid(c)
	if req.IdempotencyKey == "" {
		req.IdempotencyKey = c.GetHeader("Idempotency-Key")
	}
	if msg, code := validateSheet(req.Name, req.HeadersJson, req.DataJson); msg != "" {
		c.JSON(code, gin.H{"success": false, "error": msg})
		return
	}
	resp, err := h.Sheet.CreateSpreadsheet(h.token(c.Request.Context(), c), &req)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"success": false, "error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) GetSheet(c *gin.Context) {
	id := parseID(c)
	resp, err := h.Sheet.GetSpreadsheet(h.token(c.Request.Context(), c), &pb.GetSpreadsheetRequest{Id: id})
	if err != nil {
		c.JSON(http.StatusNotFound, gin.H{"success": false, "error": "Not found"})
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) ListSheets(c *gin.Context) {
	resp, err := h.Sheet.ListSpreadsheets(
		h.token(c.Request.Context(), c),
		&pb.ListSpreadsheetsRequest{UserId: 0},
	)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"success": false, "error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) UpdateSheet(c *gin.Context) {
	id := parseID(c)
	var req pb.UpdateSpreadsheetRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"success": false, "error": "invalid request body"})
		return
	}
	req.Id = id
	req.UserId = h.uid(c)
	resp, err := h.Sheet.UpdateSpreadsheet(h.token(c.Request.Context(), c), &req)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"success": false, "error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, resp)
}

func (h *Handlers) DeleteSheet(c *gin.Context) {
	id := parseID(c)
	resp, err := h.Sheet.DeleteSpreadsheet(
		h.token(c.Request.Context(), c),
		&pb.DeleteSpreadsheetRequest{Id: id, UserId: h.uid(c)},
	)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"success": false, "error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, resp)
}

func validateSheet(name, headersJSON, dataJSON string) (string, int) {
	if name == "" {
		return "name required", http.StatusBadRequest
	}
	if !json.Valid([]byte(headersJSON)) {
		return "headers_json is not valid JSON", http.StatusBadRequest
	}
	if !json.Valid([]byte(dataJSON)) {
		return "data_json is not valid JSON", http.StatusBadRequest
	}
	return "", 0
}
