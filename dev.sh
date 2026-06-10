#!/bin/bash
# 本地开发环境管理
# 用法:
#   bash dev.sh up       启动基础服务 (MySQL/Redis/ES/RabbitMQ/Mongo)
#   bash dev.sh down     停止
#   bash dev.sh gateway  启动 Go Gateway (热重载)
#   bash dev.sh auth     编译+启动 Auth 服务
#   bash dev.sh sheet    编译+启动 Sheet 服务
#   bash dev.sh file     编译+启动 File 服务
#   bash dev.sh test     跑功能测试
set -e

INFRA_COMPOSE="docker compose -f docker-compose.yml -f docker-compose.dev.yml"

case "${1:-up}" in
  up)
    echo "=== Starting infra services ==="
    $INFRA_COMPOSE up -d --wait mysql-auth mysql-spreadsheet-0 mysql-file-0 \
      redis-cluster-1 redis-cluster-2 redis-cluster-3 \
      elasticsearch rabbitmq mongodb redis-cluster-init
    echo "=== Infra ready ==="
    ;;

  down)
    $INFRA_COMPOSE down
    ;;

  gateway)
    cd gateway-grpc
    AUTH_ADDR="${AUTH_ADDR:-localhost:50051}" \
    SHEET_ADDR="${SHEET_ADDR:-localhost:50052}" \
    FILE_ADDR="${FILE_ADDR:-localhost:50053}" \
    REDIS_ADDR="${REDIS_ADDR:-localhost:7000}" \
    REDIS_PASSWORD="${REDIS_PASSWORD:-rpc-redis-123456}" \
    JWT_SECRET="${JWT_SECRET:-default-secret-32bytes-here!!!!!}" \
    go run .
    ;;

  gateway-live)
    cd gateway-grpc
    if command -v air &>/dev/null; then
      AUTH_ADDR="${AUTH_ADDR:-localhost:50051}" \
      SHEET_ADDR="${SHEET_ADDR:-localhost:50052}" \
      FILE_ADDR="${FILE_ADDR:-localhost:50053}" \
      REDIS_ADDR="${REDIS_ADDR:-localhost:7000}" \
      REDIS_PASSWORD="${REDIS_PASSWORD:-rpc-redis-123456}" \
      JWT_SECRET="${JWT_SECRET:-default-secret-32bytes-here!!!!!}" \
      air
    else
      echo "air not installed. Run: go install github.com/air-verse/air@latest"
      echo "Falling back to go run..."
      bash "$0" gateway
    fi
    ;;

  auth)
    make auth
    echo "=== Starting Auth on :50051 ==="
    AUTH_SVC_ADDR=localhost:50051 \
    JWT_SECRET="${JWT_SECRET:-default-secret-32bytes-here!!!!!}" \
    ./rpc_auth --service auth --port 50051 \
      --mysql-write-host localhost --mysql-password "${MYSQL_PASSWORD:-123456}" \
      --mysql-db rpc_auth --mysql-shards 1 \
      --redis-cluster localhost:7000 --redis-password "${REDIS_PASSWORD:-rpc-redis-123456}"
    ;;

  sheet)
    make sheet
    echo "=== Starting Sheet on :50052 ==="
    AUTH_SVC_ADDR=localhost:50051 \
    JWT_SECRET="${JWT_SECRET:-default-secret-32bytes-here!!!!!}" \
    RABBITMQ_HOST=localhost \
    ./rpc_sheet --service spreadsheet --port 50052 \
      --mysql-write-host localhost --mysql-port 3307 --mysql-password "${MYSQL_PASSWORD:-123456}" \
      --mysql-db rpc_spreadsheet --mysql-shards 2 \
      --redis-cluster localhost:7000 --redis-password "${REDIS_PASSWORD:-rpc-redis-123456}"
    ;;

  file)
    make file
    echo "=== Starting File on :50053 ==="
    AUTH_SVC_ADDR=localhost:50051 \
    JWT_SECRET="${JWT_SECRET:-default-secret-32bytes-here!!!!!}" \
    RABBITMQ_HOST=localhost \
    ./rpc_file --service file --port 50053 \
      --mysql-write-host localhost --mysql-port 3308 --mysql-password "${MYSQL_PASSWORD:-123456}" \
      --mysql-db rpc_file --mysql-shards 2 \
      --redis-cluster localhost:7000 --redis-password "${REDIS_PASSWORD:-rpc-redis-123456}"
    ;;

  search)
    make search
    echo "=== Starting Search on :50054 ==="
    ./rpc_search --service search --port 50054
    ;;

  test)
    API="${2:-http://localhost:8080}"
    echo "=== Running functional tests against $API ==="
    bash test/functional_test.sh "$API"
    ;;

  *)
    echo "Usage: bash dev.sh {up|down|gateway|gateway-live|auth|sheet|file|search|test}"
    exit 1
    ;;
esac
