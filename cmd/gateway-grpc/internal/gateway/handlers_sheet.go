package gateway

import (
	"encoding/json"
	"net/http"

	pb "gateway-grpc/gen/rpc"
)

func (g *Gateway) CreateSheet(w http.ResponseWriter, r *http.Request) {
	var req pb.CreateSpreadsheetRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		WriteJSONStatus(w, http.StatusBadRequest,
			map[string]interface{}{"success": false, "error": "invalid request body"})
		return
	}

	req.UserId = ExtractUID(r)
	if req.IdempotencyKey == "" {
		req.IdempotencyKey = r.Header.Get("Idempotency-Key")
	}
	if msg, code := ValidateSheetCreate(req.Name, req.HeadersJson, req.DataJson); msg != "" {
		WriteJSONStatus(w, code, map[string]interface{}{"success": false, "error": msg})
		return
	}

	resp, err := g.SheetClient.CreateSpreadsheet(g.injectToken(r), &req)
	WriteGRPCResponse(w, resp, err)
}
