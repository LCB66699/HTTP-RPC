#!/bin/bash
# 本地开发快捷命令
# 用法:
#   bash dev.sh up         启动全部服务
#   bash dev.sh down       停止
#   bash dev.sh gateway    改 Go 代码后热更 (2s)
#   bash dev.sh auth       重建 + 重启 Auth
#   bash dev.sh sheet      重建 + 重启 Sheet
#   bash dev.sh file       重建 + 重启 File
#   bash dev.sh search     重建 + 重启 Search
#   bash dev.sh test       跑功能测试

set -e

case "${1:-up}" in
  up)
    docker compose up -d
    ;;

  down)
    docker compose down
    ;;

  gateway)
    docker compose build grpc-gateway
    docker compose up -d grpc-gateway grpc-gateway-2
    echo "=== Gateway reloaded ==="
    ;;

  auth)
    docker compose build auth-1
    docker compose up -d auth-1 auth-2
    ;;

  sheet)
    docker compose build sheet-1
    docker compose up -d sheet-1 sheet-2
    ;;

  file)
    docker compose build file-1
    docker compose up -d file-1 file-2
    ;;

  search)
    docker compose build search
    docker compose up -d search
    ;;

  notify)
    docker compose build notify-service
    docker compose up -d notify-service
    ;;

  test)
    bash test/functional_test.sh "${2:-https://localhost}"
    ;;

  debug)
    SERVICE="${2:-sheet}"
    echo "=== Building DEBUG $SERVICE (gdb symbols) ==="
    docker compose build --build-arg DEBUG=true --build-arg "SERVICE=$SERVICE" "${SERVICE}-1"
    docker compose up -d "${SERVICE}-1" "${SERVICE}-2"
    echo "=== Attach: docker exec -it \$(docker ps -qf name=${SERVICE}-1) gdb -p 1 /app/rpc_server ==="
    ;;

  *)
    echo "Usage: bash dev.sh {up|down|gateway|auth|sheet|file|search|notify|test|debug <service>}"
    exit 1
    ;;
esac
