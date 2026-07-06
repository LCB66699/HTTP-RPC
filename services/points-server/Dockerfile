FROM golang:1.24-alpine AS builder
ENV GOPROXY=https://goproxy.cn,direct

RUN sed -i "s|dl-cdn.alpinelinux.org|mirrors.aliyun.com|g" /etc/apk/repositories && \
    apk add --no-cache git

WORKDIR /build
COPY services/points-server/ ./
COPY cmd/server/gen /cmd/server/gen
RUN go mod download
RUN CGO_ENABLED=0 go build -o points-server .

FROM alpine:latest
RUN apk add --no-cache curl ca-certificates
ADD https://github.com/grpc-ecosystem/grpc-health-probe/releases/download/v0.4.37/grpc_health_probe-linux-amd64 /usr/local/bin/grpc_health_probe
RUN chmod +x /usr/local/bin/grpc_health_probe
COPY --from=builder /build/points-server /app/points-server
EXPOSE 50052
CMD ["/app/points-server"]
