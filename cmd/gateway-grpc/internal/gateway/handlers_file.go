package gateway

import (
	"encoding/json"
	"io"
	"net/http"

	pb "gateway-grpc/gen/rpc"
)

func (g *Gateway) MoveFile(w http.ResponseWriter, r *http.Request) {
	id := parseInt64(r.PathValue("id"))
	var body struct{ TargetFolderId int64 `json:"target_folder_id"` }
	json.NewDecoder(r.Body).Decode(&body)
	resp, err := g.FileClient.MoveFile(g.injectToken(r), &pb.MoveFileRequest{Id: id, TargetFolderId: body.TargetFolderId})
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) CreateFolder(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Name           string `json:"name"`
		ParentFolderId int64  `json:"parent_folder_id"`
	}
	json.NewDecoder(r.Body).Decode(&body)
	resp, err := g.FileClient.CreateFolder(g.injectToken(r), &pb.CreateFolderRequest{
		UserId: 0, Name: body.Name, ParentFolderId: body.ParentFolderId,
	})
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) ListFiles(w http.ResponseWriter, r *http.Request) {
	resp, err := g.FileClient.ListFiles(g.withAuth(r.Context(), r), &pb.ListFilesRequest{UserId: 0})
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) UploadFile(w http.ResponseWriter, r *http.Request) {
	idemKey := r.Header.Get("Idempotency-Key")

	r.ParseMultipartForm(50 << 20)
	f, h, _ := r.FormFile("file")
	if f == nil {
		http.Error(w, `{"error":"no file"}`, 400)
		return
	}
	defer f.Close()
	data, _ := io.ReadAll(f)
	resp, err := g.FileClient.CreateFile(g.injectToken(r), &pb.CreateFileRequest{
		UserId: 0, OriginalName: h.Filename, Size: int64(len(data)),
		MimeType: h.Header.Get("Content-Type"), FileContent: data,
		IdempotencyKey: idemKey,
	})
	WriteGRPCResponse(w, resp, err)
}

func (g *Gateway) GetFile(w http.ResponseWriter, r *http.Request) {
	id := parseInt64(r.PathValue("id"))
	resp, err := g.FileClient.GetFile(g.withAuth(r.Context(), r), &pb.GetFileRequest{Id: id})
	if err != nil {
		WriteGRPCError(w, err, "Not found")
		return
	}
	if resp == nil || !resp.Success {
		WriteJSONStatus(w, http.StatusNotFound, map[string]interface{}{"success": false, "error": "Not found"})
		return
	}
	if resp.GetDownloadUrl() != "" {
		http.Redirect(w, r, resp.GetDownloadUrl(), http.StatusFound)
		return
	}
	if resp.File != nil {
		w.Header().Set("Content-Type", resp.File.GetMimeType())
		w.Write(resp.FileContent)
		return
	}
	WriteJSON(w, resp)
}

func (g *Gateway) DeleteFile(w http.ResponseWriter, r *http.Request) {
	id := parseInt64(r.PathValue("id"))
	resp, err := g.FileClient.DeleteFile(g.withAuth(r.Context(), r), &pb.DeleteFileRequest{Id: id, UserId: 0})
	WriteGRPCResponse(w, resp, err)
}
