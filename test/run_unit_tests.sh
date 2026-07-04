#!/usr/bin/env bash
# run_unit_tests.sh — Build & run all C++ unit tests (GoogleTest + CTest)
# Usage:
#   bash test/run_unit_tests.sh           # build + run all
#   bash test/run_unit_tests.sh --filter L1Cache.*  # specific module
#   bash test/run_unit_tests.sh --only-run          # skip build, run only
#   bash test/run_unit_tests.sh --build-only        # build only

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

FILTER=""
ONLY_RUN=false
BUILD_ONLY=false

for arg in "$@"; do
    case $arg in
        --filter)
            shift; FILTER="$1"; shift || true ;;
        --filter=*)
            FILTER="${arg#*=}" ;;
        --only-run)
            ONLY_RUN=true ;;
        --build-only)
            BUILD_ONLY=true ;;
        --help|-h)
            echo "Usage: bash test/run_unit_tests.sh [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --filter PATTERN    Run only tests matching GoogleTest filter"
            echo "  --only-run          Skip cmake build, run tests only"
            echo "  --build-only        Build only, don't run tests"
            echo "  --help, -h          Show this help"
            echo ""
            echo "Examples:"
            echo "  bash test/run_unit_tests.sh"
            echo "  bash test/run_unit_tests.sh --filter L1Cache.*"
            echo "  bash test/run_unit_tests.sh --filter 'SheetMock.*:AuthService.*'"
            exit 0 ;;
    esac
done

# ---- Build ----
if [ "$ONLY_RUN" = false ]; then
    echo -e "${YELLOW}=== Configuring CMake ===${NC}"
    cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug

    echo -e "${YELLOW}=== Building cpp_test ===${NC}"
    cmake --build "$BUILD_DIR" --target cpp_test -j"$(nproc)"
    echo -e "${GREEN}[OK] Build succeeded${NC}"
fi

if [ "$BUILD_ONLY" = true ]; then
    exit 0
fi

# ---- Run ----
echo -e "${YELLOW}=== Running unit tests ===${NC}"
echo ""

cd "$BUILD_DIR"

if [ -n "$FILTER" ]; then
    echo "Filter: $FILTER"
    echo ""
    ./cpp_test --gtest_filter="$FILTER" --gtest_color=yes
else
    # Run via ctest for CI-friendly output, then detailed gtest on failure
    if ctest --test-dir . --output-on-failure -R cpp_test 2>&1; then
        echo ""
        echo -e "${GREEN}=========================================="
        echo "  ALL UNIT TESTS PASSED"
        echo -e "==========================================${NC}"
    else
        echo ""
        echo -e "${RED}=========================================="
        echo "  TESTS FAILED"
        echo -e "==========================================${NC}"
        echo ""
        echo "Re-running with verbose output:"
        ./cpp_test --gtest_color=yes --gtest_print_time=1
        exit 1
    fi
fi
