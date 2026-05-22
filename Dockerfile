FROM ubuntu:24.04 AS builder

RUN apt update && apt install -y \
    g++ make protobuf-compiler-grpc libgrpc++-dev \
    libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev \
    libnghttp2-dev

WORKDIR /src
COPY . .
RUN make clean && make

FROM ubuntu:24.04

RUN apt update && apt install -y \
    libgrpc++1.51t64 libmysqlclient21 libhiredis-dev libssl3t64 zlib1g \
    libnghttp2-14 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/rpc_server /app/rpc_server
COPY --from=builder /src/rpc_gateway /app/rpc_gateway
COPY --from=builder /src/web-ui /app/web-ui

EXPOSE 50051 8080
