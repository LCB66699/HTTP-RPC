#!/bin/bash
# 在 Ubuntu 服务器上执行
cd ~/HTTP-RPC/cmd/gateway-grpc
go mod tidy
go build -o gateway-grpc ./ 2>&1
