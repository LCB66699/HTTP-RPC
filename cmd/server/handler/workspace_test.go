package handler

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	pb "gateway-grpc/gen/rpc"
	"google.golang.org/grpc"
)

func TestAddWorkspaceMemberDefersUserResolutionToWorkspaceService(t *testing.T) {
	h := newTestHandlers()
	h.Workspace = &mockWorkspaceClient{
		addMemberFn: func(_ context.Context, req *pb.AddMemberRequest, _ ...grpc.CallOption) (*pb.WorkspaceResponse, error) {
			if req.GetUserId() != 0 || req.GetUsername() != "target-user" {
				t.Fatalf("unexpected member request: %#v", req)
			}
			return &pb.WorkspaceResponse{Success: true}, nil
		},
	}

	r := setupGin()
	r.POST("/workspaces/:id/members", h.AddWorkspaceMember)
	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/workspaces/7/members", strings.NewReader(`{"username":"target-user","role":"viewer"}`))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String())
	}
}
