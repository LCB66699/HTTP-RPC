# syntax=docker/dockerfile:1
FROM ubuntu:24.04 AS builder

# cache mounts: apt 下载的 .deb 包和 build 的 .o 文件跨构建持久化
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt update && apt install -y \
    g++ make cmake git \
    protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev \
    libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev \
    libnghttp2-dev librabbitmq-dev libgtest-dev libgmock-dev libcurl4-openssl-dev
RUN cd /usr/src/googletest && cmake . && make -j$(nproc) && cp lib/*.a /usr/lib

ARG SERVICE=auth
ARG DEBUG=false
ARG CACHEBUST=1
WORKDIR /src
COPY . .
RUN rm -rf server/generated/*

# cache mount on build/: .o 文件跨构建缓存，只重编译变更的 .cpp
# 构建完成后二进制 COPY 到 /out 持久化，供运行时阶段使用
RUN if [ "$DEBUG" = "true" ]; then \
      sed -i 's/-O2/-g -O0/g' CMakeLists.txt; \
    fi
RUN --mount=type=cache,target=/src/build \
    cmake -B build && cmake --build build --target rpc_${SERVICE} -j$(nproc) \
    && mkdir -p /out && cp build/rpc_${SERVICE} /out/rpc_server

FROM ubuntu:24.04

ARG DEBUG=false
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt update && apt install -y \
    libgrpc++1.51t64 libmysqlclient21 libhiredis-dev libssl3t64 zlib1g \
    libnghttp2-14 librabbitmq4 apache2-utils curl libcurl4 \
    $(if [ "$DEBUG" = "true" ]; then echo gdb; fi) \
    && rm -rf /var/lib/apt/lists/*

ARG SERVICE=auth
WORKDIR /app
COPY --from=builder /out/rpc_server /app/rpc_server
COPY --from=builder /src/web-ui /app/web-ui

EXPOSE 50051
CMD ["/app/rpc_server"]
