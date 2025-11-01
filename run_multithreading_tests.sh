#!/bin/bash
#
# Comprehensive test runner for AzerothCore multithreading system
# Usage: ./run_multithreading_tests.sh [options]
#
# Options:
#   --quick     Run quick tests only (skip extended stress tests)
#   --benchmark Run performance benchmarks
#   --deadlock  Run deadlock detection stress tests only
#   --tsan      Run with Thread Sanitizer
#   --asan      Run with Address Sanitizer
#   --valgrind  Run with Valgrind memory checker
#   --all       Run all test modes (default)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
TEST_BIN="$BUILD_DIR/src/test/unit_tests"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Parse arguments
RUN_QUICK=false
RUN_BENCHMARK=false
RUN_DEADLOCK=false
RUN_TSAN=false
RUN_ASAN=false
RUN_VALGRIND=false
RUN_ALL=true

for arg in "$@"; do
    case $arg in
        --quick)
            RUN_QUICK=true
            RUN_ALL=false
            ;;
        --benchmark)
            RUN_BENCHMARK=true
            RUN_ALL=false
            ;;
        --deadlock)
            RUN_DEADLOCK=true
            RUN_ALL=false
            ;;
        --tsan)
            RUN_TSAN=true
            RUN_ALL=false
            ;;
        --asan)
            RUN_ASAN=true
            RUN_ALL=false
            ;;
        --valgrind)
            RUN_VALGRIND=true
            RUN_ALL=false
            ;;
        --all)
            RUN_ALL=true
            ;;
        *)
            echo "Unknown option: $arg"
            echo "Usage: $0 [--quick|--benchmark|--deadlock|--tsan|--asan|--valgrind|--all]"
            exit 1
            ;;
    esac
done

# Function to print section header
print_header() {
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}$1${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
}

# Function to print success
print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

# Function to print error
print_error() {
    echo -e "${RED}✗ $1${NC}"
}

# Function to print warning
print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    print_error "Build directory not found. Please build the project first."
    exit 1
fi

# Change to build directory
cd "$BUILD_DIR"

# ==================== Standard Tests ====================
if [ "$RUN_ALL" = true ] || [ "$RUN_QUICK" = true ]; then
    print_header "Running Standard Unit Tests"

    if [ ! -f "$TEST_BIN" ]; then
        print_error "Test binary not found. Build with -DBUILD_TESTING=ON"
        exit 1
    fi

    # Run all tests
    "$TEST_BIN" --gtest_output=xml:test_results.xml
    TEST_RESULT=$?

    if [ $TEST_RESULT -eq 0 ]; then
        print_success "All standard tests passed!"
    else
        print_error "Some tests failed. Check output above."
        exit 1
    fi
fi

# ==================== Benchmark Tests ====================
if [ "$RUN_ALL" = true ] || [ "$RUN_BENCHMARK" = true ]; then
    print_header "Running Performance Benchmarks"

    "$TEST_BIN" --gtest_filter="MapUpdateBenchmark.*"
    BENCH_RESULT=$?

    if [ $BENCH_RESULT -eq 0 ]; then
        print_success "Benchmarks completed!"

        if [ -f "benchmark_results.csv" ]; then
            print_success "Results saved to benchmark_results.csv"
        fi
    else
        print_error "Benchmarks failed."
        exit 1
    fi
fi

# ==================== Deadlock Detection Tests ====================
if [ "$RUN_DEADLOCK" = true ]; then
    print_header "Running Deadlock Detection Stress Tests"

    print_warning "These tests verify thread pool remains deadlock-free under stress"
    echo "Test suite includes:"
    echo "  - Thread pool saturation (10k tasks)"
    echo "  - Rapid start/stop cycles (100 iterations)"
    echo "  - Work-stealing stress tests"
    echo "  - Nested task submission"
    echo "  - 5-minute sustained load test"
    echo ""

    "$TEST_BIN" --gtest_filter="DeadlockDetectionTest.*"
    DEADLOCK_RESULT=$?

    if [ $DEADLOCK_RESULT -eq 0 ]; then
        print_success "All deadlock tests passed! No deadlocks detected."
    else
        print_error "Deadlock tests failed. Check output above."
        exit 1
    fi
fi

# ==================== Thread Sanitizer ====================
if [ "$RUN_ALL" = true ] || [ "$RUN_TSAN" = true ]; then
    print_header "Running Tests with Thread Sanitizer"

    print_warning "Rebuilding with TSan..."

    cmake .. -DBUILD_TESTING=ON \
             -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
             -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1" \
             > /dev/null 2>&1

    make -j$(nproc) > /dev/null 2>&1

    if [ $? -ne 0 ]; then
        print_error "Failed to build with TSan"
        exit 1
    fi

    TSAN_OPTIONS="halt_on_error=1 report_bugs=1" "$TEST_BIN" \
        --gtest_filter="-*ExtendedStressTest*" 2>&1 | tee tsan_output.log

    if [ ${PIPESTATUS[0]} -eq 0 ]; then
        print_success "No data races detected!"
    else
        print_error "Thread Sanitizer detected issues. Check tsan_output.log"
        exit 1
    fi
fi

# ==================== Address Sanitizer ====================
if [ "$RUN_ALL" = true ] || [ "$RUN_ASAN" = true ]; then
    print_header "Running Tests with Address Sanitizer"

    print_warning "Rebuilding with ASan..."

    cmake .. -DBUILD_TESTING=ON \
             -DCMAKE_CXX_FLAGS="-fsanitize=address -g -fno-omit-frame-pointer -O1" \
             -DCMAKE_C_FLAGS="-fsanitize=address -g -fno-omit-frame-pointer -O1" \
             > /dev/null 2>&1

    make -j$(nproc) > /dev/null 2>&1

    if [ $? -ne 0 ]; then
        print_error "Failed to build with ASan"
        exit 1
    fi

    ASAN_OPTIONS="detect_leaks=1" "$TEST_BIN" \
        --gtest_filter="-*ExtendedStressTest*" 2>&1 | tee asan_output.log

    if [ ${PIPESTATUS[0]} -eq 0 ]; then
        print_success "No memory errors detected!"
    else
        print_error "Address Sanitizer detected issues. Check asan_output.log"
        exit 1
    fi
fi

# ==================== Valgrind ====================
if [ "$RUN_VALGRIND" = true ]; then
    print_header "Running Tests with Valgrind"

    if ! command -v valgrind &> /dev/null; then
        print_error "Valgrind not found. Install with: sudo apt-get install valgrind"
        exit 1
    fi

    print_warning "Rebuilding with debug symbols..."

    cmake .. -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1
    make -j$(nproc) > /dev/null 2>&1

    valgrind --leak-check=full \
             --show-leak-kinds=all \
             --track-origins=yes \
             --verbose \
             --log-file=valgrind_output.log \
             "$TEST_BIN" --gtest_filter="-*ExtendedStressTest*:*HighLoadStressTest*"

    if [ $? -eq 0 ]; then
        print_success "Valgrind analysis complete. Check valgrind_output.log"
    else
        print_warning "Valgrind detected potential issues. Check valgrind_output.log"
    fi
fi

# ==================== Final Summary ====================
print_header "Test Suite Complete"

echo "Test results summary:"
if [ "$RUN_ALL" = true ] || [ "$RUN_QUICK" = true ]; then
    echo "  ✓ Unit tests: PASSED"
fi
if [ "$RUN_ALL" = true ] || [ "$RUN_BENCHMARK" = true ]; then
    echo "  ✓ Benchmarks: COMPLETED"
fi
if [ "$RUN_DEADLOCK" = true ]; then
    echo "  ✓ Deadlock Detection: PASSED"
fi
if [ "$RUN_ALL" = true ] || [ "$RUN_TSAN" = true ]; then
    echo "  ✓ Thread Sanitizer: PASSED"
fi
if [ "$RUN_ALL" = true ] || [ "$RUN_ASAN" = true ]; then
    echo "  ✓ Address Sanitizer: PASSED"
fi
if [ "$RUN_VALGRIND" = true ]; then
    echo "  ✓ Valgrind: COMPLETED"
fi

echo ""
print_success "All requested tests completed successfully!"
echo ""

exit 0
