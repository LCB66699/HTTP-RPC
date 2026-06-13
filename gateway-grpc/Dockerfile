FROM golang:1.24-alpine AS builder
ENV GOPROXY=https://goproxy.cn,direct

RUN apk add --no-cache git protobuf-dev

RUN go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.5
RUN go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.5.1

WORKDIR /build
COPY proto/ ./protos/
COPY gateway-grpc/ .

RUN mkdir -p gen/rpc
RUN PATH=$PATH:$GOPATH/bin protoc \
    -I protos \
    --go_out=paths=source_relative:gen/rpc \
    --go-grpc_out=paths=source_relative:gen/rpc \
    protos/rpc_auth.proto protos/rpc_spreadsheet.proto protos/rpc_file.proto protos/rpc_search.proto

RUN printf 'module gateway-grpc/gen/rpc\n\ngo 1.24\n' > gen/rpc/go.mod
RUN go mod edit -replace gateway-grpc/gen/rpc=./gen/rpc
RUN go mod tidy
RUN CGO_ENABLED=0 go build -o gateway-grpc .
RUN echo BUILD_OK

FROM alpine:latest
RUN apk add --no-cache ca-certificates
COPY --from=builder /build/gateway-grpc /app/gateway-grpc
EXPOSE 8080
CMD ["/app/gateway-grpc"]
