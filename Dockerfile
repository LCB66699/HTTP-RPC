FROM ubuntu:24.04 AS builder

RUN apt update && apt install -y \
    g++ make cmake git \
    protobuf-compiler-grpc libgrpc++-dev \
    libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev \
    libnghttp2-dev

# Build and install redis-plus-plus (use mirror if GitHub unreachable)
ARG REDIS_PP_REPO=https://github.com/sewenew/redis-plus-plus.git
RUN git clone --depth 1 ${REDIS_PP_REPO} /tmp/redis-plus-plus \
    && cd /tmp/redis-plus-plus \
    && mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=Release \
             -DREDIS_PLUS_PLUS_CXX_STANDARD=20 \
             -DREDIS_PLUS_PLUS_BUILD_TEST=OFF \
             -DREDIS_PLUS_PLUS_BUILD_STATIC=OFF \
             .. \
    && make -j$(nproc) \
    && make install \
    && ldconfig \
    && rm -rf /tmp/redis-plus-plus

WORKDIR /src
COPY . .
RUN make clean && make

FROM ubuntu:24.04

RUN apt update && apt install -y \
    libgrpc++1.51t64 libmysqlclient21 libhiredis-dev libssl3t64 zlib1g \
    libnghttp2-14 apache2-utils \
    && rm -rf /var/lib/apt/lists/*

# Copy redis-plus-plus runtime library
COPY --from=builder /usr/local/lib/libredis++.so* /usr/local/lib/
RUN ldconfig

WORKDIR /app
COPY --from=builder /src/rpc_server /app/rpc_server
COPY --from=builder /src/rpc_gateway /app/rpc_gateway
COPY --from=builder /src/web-ui /app/web-ui

EXPOSE 50051 8080
