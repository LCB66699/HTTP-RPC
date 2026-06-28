FROM ubuntu:24.04

# C++ toolchain + all project dependencies (dev headers)
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt update && apt install -y \
    g++ gdb make cmake git curl wget pkg-config \
    protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev \
    libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev \
    libnghttp2-dev librabbitmq-dev libgtest-dev libgmock-dev \
    libcurl4-openssl-dev nlohmann-json3-dev \
    clang-format clang-tidy \
    && rm -rf /var/lib/apt/lists/*

# Build gtest/gmock static libs
RUN cd /usr/src/googletest && cmake . && make -j$(nproc) && cp lib/*.a /usr/lib

# Go
ENV GO_VERSION=1.24.2
RUN curl -fsSL https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz | tar -C /usr/local -xz
ENV PATH=/usr/local/go/bin:$PATH

# protoc Go plugins
RUN go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.5 && \
    go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.5.1
ENV PATH=/root/go/bin:$PATH

# Non-root user for devcontainer
RUN useradd -m -s /bin/bash dev && echo "dev ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/dev
USER dev
WORKDIR /workspace
