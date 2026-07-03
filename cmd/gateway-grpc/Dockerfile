FROM golang:1.24-alpine AS builder
ENV GOPROXY=https://goproxy.cn,direct
ENV ALPINE_REPO=https://mirrors.aliyun.com/alpine

RUN sed -i "s|dl-cdn.alpinelinux.org|mirrors.aliyun.com|g" /etc/apk/repositories && \
    apk add --no-cache git protobuf-dev

RUN go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.5
RUN go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.5.1
RUN go install github.com/grpc-ecosystem/grpc-gateway/v2/protoc-gen-grpc-gateway@v2.23.0

WORKDIR /build
COPY proto/ ./protos/
COPY cmd/gateway-grpc/ .

RUN mkdir -p gen/rpc
RUN PATH=$PATH:$GOPATH/bin protoc \
    -I protos -I /usr/include \
    --go_out=paths=source_relative:gen/rpc \
    --go-grpc_out=paths=source_relative:gen/rpc \
    --grpc-gateway_out=paths=source_relative:gen/rpc \
    protos/rpc_auth.proto protos/rpc_spreadsheet.proto protos/rpc_file.proto protos/rpc_search.proto protos/rpc_sharing.proto

RUN printf 'module gateway-grpc/gen/rpc\n\ngo 1.24\n' > gen/rpc/go.mod
RUN go mod edit -replace gateway-grpc/gen/rpc=./gen/rpc
RUN go mod tidy
RUN CGO_ENABLED=0 go build -o gateway-grpc .
RUN echo BUILD_OK

FROM golang:1.24-alpine AS runner
RUN sed -i "s|dl-cdn.alpinelinux.org|mirrors.aliyun.com|g" /etc/apk/repositories && \
    apk add --no-cache ca-certificates
COPY --from=builder /build/gateway-grpc /app/gateway-grpc
EXPOSE 8080
CMD ["/app/gateway-grpc"]
