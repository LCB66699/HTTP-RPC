FROM ubuntu:24.04 AS builder
RUN apt update && apt install -y \
    g++ make cmake git \
    protobuf-compiler-grpc libgrpc++-dev \
    libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev \
    libnghttp2-dev curl
COPY third_party/redis-plus-plus/ /tmp/redis-plus-plus/
RUN cd /tmp/redis-plus-plus && mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=Release -DREDIS_PLUS_PLUS_CXX_STANDARD=20 \
       -DREDIS_PLUS_PLUS_BUILD_TEST=OFF .. \
    && make -j1 && make install && ldconfig && rm -rf /tmp/redis-plus-plus
WORKDIR /src
COPY . .
RUN make clean && make server

FROM ubuntu:24.04
RUN apt update && apt install -y \
    libgrpc++1.51t64 libmysqlclient21 libhiredis-dev libssl3t64 zlib1g \
    libnghttp2-14 curl && rm -rf /var/lib/apt/lists/*
COPY --from=builder /usr/local/lib/libredis++.so* /usr/local/lib/
RUN ldconfig
COPY --from=builder /src/rpc_server /app/rpc_server
EXPOSE 50051
ENV ES_HOST=http://elasticsearch:9200
CMD ["/app/rpc_server"]
CMD ["/app/rpc_server", "--service", "search", "--port", "50051"]
