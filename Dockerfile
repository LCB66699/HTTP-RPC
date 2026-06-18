FROM ubuntu:24.04 AS builder

RUN apt update && apt install -y \
    g++ make cmake git \
    protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev \
    libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev \
    libnghttp2-dev librabbitmq-dev libgtest-dev libgmock-dev libcurl4-openssl-dev
RUN cd /usr/src/googletest && cmake . && make -j$(nproc) && cp lib/*.a /usr/lib

# OpenTelemetry C++ (OTLP HTTP exporter, static libs)
# OpenTelemetry C++ — not built (OTel-CPP v1.18 proto/grpc linker issues)
# C++ tracing handled by Go Gateway's otelgrpc interceptor (traceparent injection)
# If OTel-CPP headers are installed externally, CMake will auto-detect and link

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

FROM ubuntu:24.04

ARG DEBUG=false
RUN apt update && apt install -y \
    libgrpc++1.51t64 libmysqlclient21 libhiredis-dev libssl3t64 zlib1g \
    libnghttp2-14 librabbitmq4 apache2-utils curl libcurl4 \
    $(if [ "$DEBUG" = "true" ]; then echo gdb; fi) \
    && rm -rf /var/lib/apt/lists/*

ARG SERVICE=auth
WORKDIR /app
COPY --from=builder /src/build/rpc_${SERVICE} /app/rpc_server
COPY --from=builder /src/web-ui /app/web-ui

EXPOSE 50051
CMD ["/app/rpc_server"]
