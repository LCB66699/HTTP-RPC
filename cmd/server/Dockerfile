FROM golang:1.24-alpine AS builder
ENV GOPROXY=https://goproxy.cn,direct
ENV ALPINE_REPO=https://mirrors.aliyun.com/alpine

RUN sed -i "s|dl-cdn.alpinelinux.org|mirrors.aliyun.com|g" /etc/apk/repositories && \
    apk add --no-cache git

WORKDIR /build
COPY cmd/server/ .

RUN go mod tidy
RUN CGO_ENABLED=0 go build -o gin-gateway .

FROM golang:1.24-alpine AS runner
RUN sed -i "s|dl-cdn.alpinelinux.org|mirrors.aliyun.com|g" /etc/apk/repositories && \
    apk add --no-cache ca-certificates
COPY --from=builder /build/gin-gateway /app/gin-gateway
EXPOSE 8080
CMD ["/app/gin-gateway"]
