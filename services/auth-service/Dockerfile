FROM ubuntu:24.04 AS builder
RUN apt update && apt install -y \
    g++ make cmake git \
    protobuf-compiler-grpc libgrpc++-dev \
    libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev \
    libnghttp2-dev librabbitmq-dev curl
COPY third_party/redis-plus-plus/ /tmp/redis-plus-plus/
RUN cd /tmp/redis-plus-plus && mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=Release -DREDIS_PLUS_PLUS_CXX_STANDARD=20 \
       -DREDIS_PLUS_PLUS_BUILD_TEST=OFF .. \
    && make -j$(nproc) && make install && ldconfig && rm -rf /tmp/redis-plus-plus
WORKDIR /src
COPY . .
# 编译全量，只取 rpc_server (auth 模式)
RUN make auth

FROM ubuntu:24.04
RUN apt update && apt install -y \
    libgrpc++1.51t64 libmysqlclient21 libhiredis-dev libssl3t64 zlib1g \
    libnghttp2-14 curl && rm -rf /var/lib/apt/lists/*
COPY --from=builder /usr/local/lib/libredis++.so* /usr/local/lib/
RUN ldconfig
COPY --from=builder /src/rpc_auth /app/rpc_auth
EXPOSE 50051
CMD ["/app/rpc_auth", "--port", "50051"]
