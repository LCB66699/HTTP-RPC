FROM ubuntu:24.04 AS builder

RUN apt update && apt install -y \
    g++ make cmake git \
    protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev \
    libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev \
    libnghttp2-dev librabbitmq-dev libgtest-dev
RUN cd /usr/src/googletest && cmake . && make -j$(nproc) && cp lib/*.a /usr/lib

ARG SERVICE=auth
ARG DEBUG=false
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
    libnghttp2-14 librabbitmq4 apache2-utils curl \
    $(if [ "$DEBUG" = "true" ]; then echo gdb; fi) \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/libredis++.so* /usr/local/lib/
RUN ldconfig

ARG SERVICE=auth
WORKDIR /app
COPY --from=builder /src/build/rpc_${SERVICE} /app/rpc_server
COPY --from=builder /src/web-ui /app/web-ui

EXPOSE 50051
CMD ["/app/rpc_server"]
