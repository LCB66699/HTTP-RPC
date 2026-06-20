package gateway

import (
	"context"

	"google.golang.org/grpc"

	pb "gateway-grpc/gen/rpc"
)

// SheetClientI is the gRPC client interface for spreadsheet operations.
type SheetClientI interface {
	ListSpreadsheets(ctx context.Context, req *pb.ListSpreadsheetsRequest, opts ...grpc.CallOption) (*pb.ListSpreadsheetsResponse, error)
	GetSpreadsheet(ctx context.Context, req *pb.GetSpreadsheetRequest, opts ...grpc.CallOption) (*pb.GetSpreadsheetResponse, error)
	CreateSpreadsheet(ctx context.Context, req *pb.CreateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.CreateSpreadsheetResponse, error)
	UpdateSpreadsheet(ctx context.Context, req *pb.UpdateSpreadsheetRequest, opts ...grpc.CallOption) (*pb.UpdateSpreadsheetResponse, error)
	DeleteSpreadsheet(ctx context.Context, req *pb.DeleteSpreadsheetRequest, opts ...grpc.CallOption) (*pb.DeleteSpreadsheetResponse, error)
}

// FileClientI is the gRPC client interface for file operations.
type FileClientI interface {
	ListFiles(ctx context.Context, req *pb.ListFilesRequest, opts ...grpc.CallOption) (*pb.ListFilesResponse, error)
	GetFile(ctx context.Context, req *pb.GetFileRequest, opts ...grpc.CallOption) (*pb.GetFileResponse, error)
	CreateFile(ctx context.Context, req *pb.CreateFileRequest, opts ...grpc.CallOption) (*pb.CreateFileResponse, error)
	DeleteFile(ctx context.Context, req *pb.DeleteFileRequest, opts ...grpc.CallOption) (*pb.DeleteFileResponse, error)
	CreateFolder(ctx context.Context, req *pb.CreateFolderRequest, opts ...grpc.CallOption) (*pb.CreateFolderResponse, error)
	MoveFile(ctx context.Context, req *pb.MoveFileRequest, opts ...grpc.CallOption) (*pb.MoveFileResponse, error)
	BatchDelete(ctx context.Context, req *pb.BatchDeleteRequest, opts ...grpc.CallOption) (*pb.BatchDeleteResponse, error)
}

// AuthClientI is the gRPC client interface for auth operations.
type AuthClientI interface {
	Login(ctx context.Context, req *pb.LoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error)
	Register(ctx context.Context, req *pb.RegisterRequest, opts ...grpc.CallOption) (*pb.RegisterResponse, error)
	RefreshToken(ctx context.Context, req *pb.RefreshTokenRequest, opts ...grpc.CallOption) (*pb.RefreshTokenResponse, error)
	ChangePassword(ctx context.Context, req *pb.ChangePasswordRequest, opts ...grpc.CallOption) (*pb.ChangePasswordResponse, error)
	LoginByPhone(ctx context.Context, req *pb.PhoneLoginRequest, opts ...grpc.CallOption) (*pb.LoginResponse, error)
}

// SharedClientI is the gRPC client interface for sharing operations.
type SharedClientI interface {
	Share(ctx context.Context, req *pb.ShareRequest, opts ...grpc.CallOption) (*pb.ShareResponse, error)
	Revoke(ctx context.Context, req *pb.RevokeRequest, opts ...grpc.CallOption) (*pb.RevokeResponse, error)
	ListShares(ctx context.Context, req *pb.ResourceRequest, opts ...grpc.CallOption) (*pb.ShareListResponse, error)
	CreateShareLink(ctx context.Context, req *pb.ShareLinkRequest, opts ...grpc.CallOption) (*pb.ShareLinkResponse, error)
	GetByToken(ctx context.Context, req *pb.ShareTokenRequest, opts ...grpc.CallOption) (*pb.SharedResourceResponse, error)
}

// Injectable gRPC client instances (set by main, overridden by tests).
var (
	AuthClient   AuthClientI
	SheetClient  SheetClientI
	FileClient   FileClientI
	SharedClient SharedClientI
)
