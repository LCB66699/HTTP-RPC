package gateway

import (
	"encoding/json"
	"fmt"
	"net/http"

	pb "gateway-grpc/gen/rpc"
)

func (g *Gateway) ShareSheet(w http.ResponseWriter, r *http.Request) {
	uid := ExtractUID(r)
	var body struct {
		Username   string `json:"username"`
		Permission string `json:"permission"`
	}
	json.NewDecoder(r.Body).Decode(&body)
	id := parseInt64(r.PathValue("id"))
	resp, err := g.SharedClient.Share(g.injectToken(r), &pb.ShareRequest{
		OwnerId: uid, ResourceType: "sheet", ResourceId: id,
		GranteeUsername: body.Username, Permission: body.Permission,
	})
	if err != nil {
		WriteGRPCError(w, err, "share failed")
		return
	}
	WriteJSON(w, resp)
}

func (g *Gateway) RevokeShare(w http.ResponseWriter, r *http.Request) {
	uid := ExtractUID(r)
	username := r.PathValue("username")
	id := parseInt64(r.PathValue("id"))
	resp, err := g.SharedClient.Revoke(g.injectToken(r), &pb.RevokeRequest{
		OwnerId: uid, ResourceType: "sheet", ResourceId: id,
		GranteeUsername: username,
	})
	if err != nil {
		WriteGRPCError(w, err, "revoke failed")
		return
	}
	WriteJSON(w, resp)
}

func (g *Gateway) ListShares(w http.ResponseWriter, r *http.Request) {
	uid := ExtractUID(r)
	id := parseInt64(r.PathValue("id"))
	resp, err := g.SharedClient.ListShares(g.injectToken(r), &pb.ResourceRequest{
		OwnerId: uid, ResourceType: "sheet", ResourceId: id,
	})
	if err != nil {
		WriteGRPCError(w, err, "list shares failed")
		return
	}
	WriteJSON(w, resp)
}

func (g *Gateway) CreateShareLink(w http.ResponseWriter, r *http.Request) {
	uid := ExtractUID(r)
	id := parseInt64(r.PathValue("id"))
	resp, err := g.SharedClient.CreateShareLink(g.injectToken(r), &pb.ShareLinkRequest{
		OwnerId: uid, ResourceType: "sheet", ResourceId: id, Permission: "view",
	})
	if err != nil {
		WriteGRPCError(w, err, "create share link failed")
		return
	}
	WriteJSON(w, resp)
}

func (g *Gateway) ShareByToken(w http.ResponseWriter, r *http.Request) {
	token := r.PathValue("token")
	resp, err := g.SharedClient.GetByToken(g.injectToken(r), &pb.ShareTokenRequest{Token: token})
	if err != nil {
		WriteGRPCError(w, err, "share link failed")
		return
	}
	if !resp.GetSuccess() {
		WriteJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
		return
	}
	info := resp.GetInfo()
	if info == nil {
		WriteJSONStatus(w, http.StatusInternalServerError, map[string]interface{}{"success": false, "error": "invalid response"})
		return
	}
	switch info.GetResourceType() {
	case "sheet":
		http.Redirect(w, r, fmt.Sprintf("/api/v1/sheets/%d", info.GetResourceId()), http.StatusFound)
	case "file":
		http.Redirect(w, r, fmt.Sprintf("/api/v1/files/%d", info.GetResourceId()), http.StatusFound)
	default:
		WriteJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "unknown resource type"})
	}
}
