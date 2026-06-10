FROM ubuntu:24.04 AS builder

RUN apt update && apt install -y \
    g++ make cmake git \
    protobuf-compiler-grpc libgrpc++-dev \
    libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev \
    libnghttp2-dev librabbitmq-dev

# Build and install redis-plus-plus from local source
COPY third_party/redis-plus-plus/ /tmp/redis-plus-plus/
RUN cd /tmp/redis-plus-plus \
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

ARG SERVICE=auth
WORKDIR /src
COPY . .
RUN make clean && make ${SERVICE}

FROM ubuntu:24.04

RUN apt update && apt install -y \
    libgrpc++1.51t64 libmysqlclient21 libhiredis-dev libssl3t64 zlib1g \
    libnghttp2-14 librabbitmq4 apache2-utils curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/libredis++.so* /usr/local/lib/
RUN ldconfig

ARG SERVICE=auth
WORKDIR /app
COPY --from=builder /src/rpc_${SERVICE} /app/rpc_server
COPY --from=builder /src/web-ui /app/web-ui

EXPOSE 50051
CMD ["/app/rpc_server"]
