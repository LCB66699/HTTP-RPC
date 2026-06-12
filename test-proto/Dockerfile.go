FROM golang:1.24-alpine
ENV GOPROXY=https://goproxy.cn,direct
RUN apk add --no-cache git protobuf-dev protoc
RUN go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.34.1
RUN go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.4.0
WORKDIR /build
COPY rpc_auth.proto .
COPY google/ ./google/
RUN protoc -I . --go_out=. --go_opt=paths=source_relative --go-grpc_out=. --go-grpc_opt=paths=source_relative rpc_auth.proto && echo SUCCESS
CMD echo ok
