#!/bin/bash
# HTTP-RPC 全项目测试入口
# 运行所有子系统的单元测试和集成测试。
#
# 用法:
#   bash run_tests.sh              # 运行所有测试
#   bash run_tests.sh cpp          # 仅 C++ 测试
#   bash run_tests.sh go           # 仅 Go 测试
#   bash run_tests.sh notify       # 仅 notify-service
#   bash run_tests.sh gateway      # 仅 gateway-grpc
#
# 环境变量:
#   SKIP_CPP_BUILD=1    跳过 cmake 构建步骤（直接跑 ctest）
#   GO_TEST_FLAGS="-v -count=1 -race"  传递给 go test 的额外参数

set -euo pipefail
cd "$(dirname "$0")"
ROOT="$PWD"

PASS=0
FAIL=0
RESULT_FILE=$(mktemp)
trap 'rm -f "$RESULT_FILE"' EXIT

green() { echo -e "\033[32m[PASS]\033[0m $1"; }
red()   { echo -e "\033[31m[FAIL]\033[0m $1"; }
title() { echo -e "\n\033[1;36m===== $1 =====\033[0m"; }

run_test() {
    local name="$1"
    local cmd="$2"
    echo "  RUN: $name"
    if eval "$cmd" >> "$RESULT_FILE" 2>&1; then
        green "$name"
        PASS=$((PASS + 1))
    else
        red "$name"
        echo "  └─ 输出:"
        sed 's/^/      /' "$RESULT_FILE"
        FAIL=$((FAIL + 1))
    fi
    : > "$RESULT_FILE"
}

# ---------- C++ ----------
run_cpp_tests() {
    title "C++ Unit Tests (ctest)"

    if [ "${SKIP_CPP_BUILD:-}" != "1" ]; then
        echo "  Building C++ tests..."
        cmake -B build -S . > /dev/null 2>&1 || true
        cmake --build build -j"$(nproc 2>/dev/null || echo 4)" --target cpp_test 2>/dev/null
    fi

    if [ -f build/cpp_test ]; then
        run_test "cpp_test (gtest)" "ctest --test-dir build --output-on-failure --timeout 60"
    else
        red "cpp_test binary not found — run cmake --build build first"
    fi
}

# ---------- Go ----------
run_go_tests() {
    local label="$1"
    local dir="$2"
    local pkg="${3:-./...}"
    shift 3

    if [ ! -d "$dir" ]; then
        red "$label: directory not found ($dir)"
        return
    fi

    pushd "$dir" > /dev/null || return
    run_test "$label (go test $pkg)" "go test $pkg ${GO_TEST_FLAGS:- -count=1}"
    popd > /dev/null || return
}

# ---------- 主流程 ----------
echo "=========================================="
echo "  HTTP-RPC Test Runner"
echo "  $(date)"
echo "=========================================="

case "${1:-all}" in
    all)
        run_cpp_tests
        run_go_tests "notify-service" "services/notify-service" "./..."
        run_go_tests "gateway-grpc"   "cmd/gateway-grpc"       "./..."
        ;;
    cpp)
        run_cpp_tests
        ;;
    go)
        run_go_tests "notify-service" "services/notify-service" "./..."
        run_go_tests "gateway-grpc"   "cmd/gateway-grpc"       "./..."
        ;;
    notify)
        run_go_tests "notify-service" "services/notify-service" "./..."
        ;;
    gateway)
        run_go_tests "gateway-grpc"   "cmd/gateway-grpc"       "./..."
        ;;
    *)
        echo "用法: $0 [all|cpp|go|notify|gateway]"
        exit 1
        ;;
esac

# ---------- 结果 ----------
echo ""
echo "=========================================="
echo "  结果: $PASS passed, $FAIL failed"
echo "=========================================="
[ "$FAIL" -eq 0 ]
