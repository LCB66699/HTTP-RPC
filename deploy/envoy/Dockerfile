# Envoy + proto descriptor (gRPC-JSON 转码所需)
# Stage 1: 生成 proto descriptor
FROM ubuntu:22.04 AS proto-builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    protobuf-compiler git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY proto/ ./proto/
RUN git clone --depth 1 https://github.com/googleapis/googleapis.git /tmp/googleapis 2>/dev/null || true
RUN protoc \
    -I proto \
    -I /tmp/googleapis \
    -I /usr/include \
    --include_imports \
    --include_source_info \
    --descriptor_set_out=proto_descriptor.pb \
    proto/rpc_auth.proto proto/rpc_spreadsheet.proto proto/rpc_file.proto proto/rpc_health.proto

# Stage 2: Envoy 运行镜像
FROM envoyproxy/envoy:v1.31-latest
COPY --from=proto-builder /build/proto_descriptor.pb /etc/envoy/proto_descriptor.pb
COPY deploy/envoy/envoy.yaml /etc/envoy/envoy.yaml
CMD ["-c", "/etc/envoy/envoy.yaml", "--log-level", "info"]
