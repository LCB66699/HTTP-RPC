module github.com/lcb66699/http-rpc/mall-server

go 1.24

require (
	gateway-grpc/gen/rpc v0.0.0
	github.com/alicebob/miniredis/v2 v2.34.0
	github.com/redis/go-redis/v9 v9.7.0
	google.golang.org/grpc v1.67.1
)

require (
	github.com/alicebob/gopher-json v0.0.0-20230218143504-906a9b012302 // indirect
	github.com/cespare/xxhash/v2 v2.3.0 // indirect
	github.com/dgryski/go-rendezvous v0.0.0-20200823014737-9f7001d12a5f // indirect
	github.com/yuin/gopher-lua v1.1.1 // indirect
	golang.org/x/net v0.28.0 // indirect
	golang.org/x/sys v0.24.0 // indirect
	golang.org/x/text v0.17.0 // indirect
	google.golang.org/genproto/googleapis/api v0.0.0-20240814211410-ddb44dafa142 // indirect
	google.golang.org/genproto/googleapis/rpc v0.0.0-20240814211410-ddb44dafa142 // indirect
	google.golang.org/protobuf v1.35.1 // indirect
)

replace gateway-grpc/gen/rpc => ../../cmd/server/gen/rpc
