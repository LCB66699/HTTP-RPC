# syntax=docker/dockerfile:1
# ── Stage 1: shared runtime base (built once, reused by auth/sheet/file/search) ──
FROM ubuntu:24.04 AS base-runtime
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt update && apt install -y \
    libgrpc++1.51 libmysqlclient21 libhiredis0.14 libssl3 zlib1g \
    libnghttp2-14 librabbitmq4 libcurl4 \
    ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# ── Stage 2: builder (inherits base-runtime, adds -dev headers + toolchain) ──
FROM base-runtime AS builder
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt update && apt install -y \
    g++ cmake make git \
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

RUN if [ "$DEBUG" = "true" ]; then \
      sed -i 's/-O2/-g -O0/g' CMakeLists.txt; \
    fi
RUN cmake -B build && cmake --build build --target rpc_${SERVICE} -j$(nproc)

# ── Stage 3: runtime (inherits base-runtime, copies only the binary) ──
FROM base-runtime
ARG DEBUG=false
RUN if [ "$DEBUG" = "true" ]; then apt update && apt install -y gdb && rm -rf /var/lib/apt/lists/*; fi

ARG SERVICE=auth
WORKDIR /app
COPY --from=builder /src/build/rpc_${SERVICE} /app/rpc_server
COPY --from=builder /src/web-ui /app/web-ui

EXPOSE 50051
CMD ["/app/rpc_server"]
