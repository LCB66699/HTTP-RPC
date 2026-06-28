FROM ubuntu:24.04

# Use Aliyun mirror for faster apt in China
RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.aliyun.com|g' /etc/apt/sources.list.d/ubuntu.sources && \
    sed -i 's|http://security.ubuntu.com|http://mirrors.aliyun.com|g' /etc/apt/sources.list.d/ubuntu.sources

# C++ toolchain + all project dependencies (dev headers)
RUN apt update && apt install -y \
    g++ gdb make cmake git curl wget pkg-config \
    protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev \
    libmysqlclient-dev libhiredis-dev libssl-dev zlib1g-dev \
    libnghttp2-dev librabbitmq-dev libgtest-dev libgmock-dev \
    libcurl4-openssl-dev \
    clang-format clang-tidy \
    && rm -rf /var/lib/apt/lists/*

# Build gtest/gmock static libs
RUN cd /usr/src/googletest && cmake . && make -j$(nproc) && cp lib/*.a /usr/lib

# Go (use Aliyun mirror + goproxy.cn)
ENV GO_VERSION=1.24.2
ENV GOPROXY=https://goproxy.cn,direct
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
