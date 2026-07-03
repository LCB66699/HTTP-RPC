#!/bin/bash
# 基于 mock 的服务单元测试（无需基础设施）
# 自动发现 services/ 下所有含 go.mod 的目录并运行 go test

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

PASS=0
FAIL=0
RESULTS=""

green() { echo -e "\033[32m[PASS]\033[0m $1"; ((PASS++)); }
red()   { echo -e "\033[31m[FAIL]\033[0m $1"; ((FAIL++)); }
title() { echo -e "\n\033[1;36m=== $1 ===\033[0m"; }

echo "=========================================="
echo "  Mock-based Service Tests"
echo "  $(date)"
echo "=========================================="

if ! command -v go &>/dev/null; then
    echo "  Go 编译器未安装，请在宿主机运行此脚本"
    echo "  （项目工作流：Docker 只跑基础服务，Go 在宿主机原生编译）"
    exit 1
fi

for svc in */; do
    svc="${svc%/}"
    [ "$svc" = "tests" ] && continue
    [ ! -f "$svc/go.mod" ] && continue

    title "$svc"
    pushd "$svc" > /dev/null

    # 确保 go.sum 与 go.mod 一致（避免缺少新依赖的 checksum）
    go mod tidy > /dev/null 2>&1 || true

    test_output=$(go test ./... -count=1 2>&1) && {
        green "$svc — all tests passed"
        RESULTS="$RESULTS  [PASS] $svc"$'\n'
    } || {
        red "$svc — tests failed"
        echo ""
        echo "$test_output"
        RESULTS="$RESULTS  [FAIL] $svc"$'\n'
    }

    popd > /dev/null
done

echo ""
echo "=========================================="
echo -n "$RESULTS"
echo "=========================================="
echo "  Total: $((PASS + FAIL)) services, $PASS passed, $FAIL failed"
echo "=========================================="
[ "$FAIL" -eq 0 ]
