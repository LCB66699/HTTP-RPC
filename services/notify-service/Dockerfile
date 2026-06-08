# ============================================================
# Notify Service — 事件消费与文档解析 (Go 实现, 轻量)
# 订阅 RabbitMQ 事件, 解析文档, 写入 MongoDB + ES
# ============================================================
FROM golang:1.22-alpine AS builder
ENV GOPROXY=https://goproxy.cn,direct
RUN apk add --no-cache git
WORKDIR /build
COPY . .
RUN go mod tidy && go mod download && CGO_ENABLED=0 go build -o notify-service .

FROM alpine:latest
RUN apk add --no-cache ca-certificates
COPY --from=builder /build/notify-service /app/notify-service
EXPOSE 9002
CMD ["/app/notify-service"]
