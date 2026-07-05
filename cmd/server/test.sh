#!/usr/bin/env bash
# test.sh — Run all Gin server tests
# Usage: bash test.sh            # all tests
#        bash test.sh --handler  # handler tests only
#        bash test.sh --router   # router tests only
#        bash test.sh --build    # build only

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

BUILD_ONLY=false
TARGET=""

for arg in "$@"; do
    case $arg in
        --handler) TARGET="./handler/" ;;
        --router)  TARGET="./router/" ;;
        --build)   BUILD_ONLY=true ;;
        --help|-h)
            echo "Usage: bash test.sh [OPTIONS]"
            echo "  --handler  Run handler tests only"
            echo "  --router   Run router tests only"
            echo "  --build    Build only, skip tests"
            echo "  --help     Show this help"
            exit 0 ;;
    esac
done

echo -e "${YELLOW}=== Building ===${NC}"
go build ./...
echo -e "${GREEN}[OK] Build passed${NC}"

if [ "$BUILD_ONLY" = true ]; then exit 0; fi

echo ""
echo -e "${YELLOW}=== Running tests ===${NC}"

PASS=0
FAIL=0

run_test() {
    local pkg=$1
    echo ""
    echo "--- $pkg ---"
    if go test "$pkg" -v -count=1 2>&1; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

if [ -n "$TARGET" ]; then
    run_test "$TARGET"
else
    run_test "./router/"
    run_test "./handler/"
fi

echo ""
echo -e "${YELLOW}=========================================="
echo -e "  PASS: ${GREEN}$PASS${YELLOW}  FAIL: ${RED}$FAIL${YELLOW}"
echo -e "==========================================${NC}"

if [ "$FAIL" -gt 0 ]; then exit 1; fi
