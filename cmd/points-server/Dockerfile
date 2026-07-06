FROM golang:1.24-alpine AS builder
ENV GOPROXY=https://goproxy.cn,direct

RUN sed -i "s|dl-cdn.alpinelinux.org|mirrors.aliyun.com|g" /etc/apk/repositories && \
    apk add --no-cache git

WORKDIR /build
COPY cmd/server/go.mod cmd/server/go.sum ./
COPY cmd/server/gen ./gen
RUN go mod download

COPY cmd/points-server/ ./
RUN CGO_ENABLED=0 go build -o points-server .

FROM alpine:latest
RUN apk add --no-cache curl ca-certificates
COPY --from=builder /build/points-server /app/points-server
EXPOSE 50052
CMD ["/app/points-server"]
