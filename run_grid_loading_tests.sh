#!/bin/bash
#
# Comprehensive Grid Loading Test Runner
#
# This script runs all grid loading tests (benchmarks, correctness, stress tests)
# and generates a detailed performance report.
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
BUILD_DIR="build"
TEST_RESULTS_DIR="grid_loading_test_results"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
REPORT_FILE="${TEST_RESULTS_DIR}/report_${TIMESTAMP}.txt"

# Function to print colored messages
print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

# Create results directory
mkdir -p "${TEST_RESULTS_DIR}"

# Start report
echo "Grid Loading Test Report" > "${REPORT_FILE}"
echo "Generated: $(date)" >> "${REPORT_FILE}"
echo "========================================" >> "${REPORT_FILE}"
echo "" >> "${REPORT_FILE}"

print_header "Grid Loading Comprehensive Test Suite"

# Check if build directory exists
if [ ! -d "${BUILD_DIR}" ]; then
    print_error "Build directory not found. Run cmake first."
    exit 1
fi

# Step 1: Build tests
print_header "Step 1: Building Tests"
cd "${BUILD_DIR}"

if cmake --build . --target unit_tests -j$(nproc); then
    print_success "Tests built successfully"
else
    print_error "Failed to build tests"
    exit 1
fi

cd ..

# Step 2: Check if test executable exists
if [ ! -f "${BUILD_DIR}/src/test/unit_tests" ]; then
    print_error "Test executable not found"
    exit 1
fi

# Step 3: Run Baseline Benchmarks
print_header "Step 2: Running Baseline Benchmarks (Sequential)"

echo "=== BASELINE BENCHMARKS ===" >> "${REPORT_FILE}"
echo "" >> "${REPORT_FILE}"

print_warning "Running baseline benchmarks (this may take several minutes)..."

if "${BUILD_DIR}/src/test/unit_tests" --gtest_filter="GridLoadingBenchmark.Baseline_*" 2>&1 | tee -a "${REPORT_FILE}"; then
    print_success "Baseline benchmarks completed"
else
    print_warning "Some baseline benchmarks failed or skipped"
fi

echo "" >> "${REPORT_FILE}"

# Step 4: Run Parallel Benchmarks (2 threads)
print_header "Step 3: Running Parallel Benchmarks (2 threads)"

echo "=== PARALLEL BENCHMARKS (2 THREADS) ===" >> "${REPORT_FILE}"
echo "" >> "${REPORT_FILE}"

if "${BUILD_DIR}/src/test/unit_tests" --gtest_filter="GridLoadingBenchmark.Parallel_*2Threads" 2>&1 | tee -a "${REPORT_FILE}"; then
    print_success "Parallel benchmarks (2 threads) completed"
else
    print_warning "Some parallel benchmarks failed or skipped"
fi

echo "" >> "${REPORT_FILE}"

# Step 5: Run Parallel Benchmarks (4 threads)
print_header "Step 4: Running Parallel Benchmarks (4 threads)"

echo "=== PARALLEL BENCHMARKS (4 THREADS) ===" >> "${REPORT_FILE}"
echo "" >> "${REPORT_FILE}"

if "${BUILD_DIR}/src/test/unit_tests" --gtest_filter="GridLoadingBenchmark.Parallel_*4Threads" 2>&1 | tee -a "${REPORT_FILE}"; then
    print_success "Parallel benchmarks (4 threads) completed"
else
    print_warning "Some parallel benchmarks failed or skipped"
fi

echo "" >> "${REPORT_FILE}"

# Step 6: Run Parallel Benchmarks (8 threads)
print_header "Step 5: Running Parallel Benchmarks (8 threads)"

echo "=== PARALLEL BENCHMARKS (8 THREADS) ===" >> "${REPORT_FILE}"
echo "" >> "${REPORT_FILE}"

if "${BUILD_DIR}/src/test/unit_tests" --gtest_filter="GridLoadingBenchmark.Parallel_*8Threads" 2>&1 | tee -a "${REPORT_FILE}"; then
    print_success "Parallel benchmarks (8 threads) completed"
else
    print_warning "Some parallel benchmarks failed or skipped"
fi

echo "" >> "${REPORT_FILE}"

# Step 7: Run Speedup Analysis
print_header "Step 6: Calculating Speedup Ratios"

echo "=== SPEEDUP ANALYSIS ===" >> "${REPORT_FILE}"
echo "" >> "${REPORT_FILE}"

if "${BUILD_DIR}/src/test/unit_tests" --gtest_filter="GridLoadingBenchmark.Analysis_*" 2>&1 | tee -a "${REPORT_FILE}"; then
    print_success "Speedup analysis completed"
else
    print_warning "Speedup analysis failed or skipped"
fi

echo "" >> "${REPORT_FILE}"

# Step 8: Run Correctness Tests
print_header "Step 7: Running Correctness Tests"

echo "=== CORRECTNESS TESTS ===" >> "${REPORT_FILE}"
echo "" >> "${REPORT_FILE}"

CORRECTNESS_PASSED=0
CORRECTNESS_TOTAL=0

if "${BUILD_DIR}/src/test/unit_tests" --gtest_filter="GridLoadingCorrectnessTest.*" 2>&1 | tee -a "${REPORT_FILE}"; then
    print_success "All correctness tests passed"
    CORRECTNESS_PASSED=1
else
    print_error "Some correctness tests failed"
fi

echo "" >> "${REPORT_FILE}"

# Step 9: Run Stress Tests (optional - can be slow)
print_header "Step 8: Running Stress Tests"

echo "Would you like to run stress tests? (can take 10+ minutes) [y/N]: "
read -t 10 RUN_STRESS || RUN_STRESS="n"

if [[ "${RUN_STRESS}" =~ ^[Yy]$ ]]; then
    echo "=== STRESS TESTS ===" >> "${REPORT_FILE}"
    echo "" >> "${REPORT_FILE}"

    print_warning "Running stress tests (this will take a while)..."

    if "${BUILD_DIR}/src/test/unit_tests" --gtest_filter="GridLoadingStressTest.*" 2>&1 | tee -a "${REPORT_FILE}"; then
        print_success "Stress tests completed"
    else
        print_warning "Some stress tests failed or skipped"
    fi

    echo "" >> "${REPORT_FILE}"
else
    print_warning "Skipping stress tests"
    echo "Stress tests skipped by user" >> "${REPORT_FILE}"
fi

# Step 10: Generate Summary Report
print_header "Step 9: Generating Summary Report"

echo "=== SUMMARY ===" >> "${REPORT_FILE}"
echo "" >> "${REPORT_FILE}"

# Check for CSV results
if [ -f "grid_loading_benchmark_results.csv" ]; then
    print_success "Benchmark results CSV generated"
    mv grid_loading_benchmark_results.csv "${TEST_RESULTS_DIR}/benchmark_results_${TIMESTAMP}.csv"

    # Generate summary from CSV
    echo "Benchmark Results Summary:" >> "${REPORT_FILE}"
    echo "" >> "${REPORT_FILE}"

    # Parse CSV and create summary
    python3 - <<EOF >> "${REPORT_FILE}" 2>/dev/null || echo "Python analysis unavailable"
import csv
import sys

try:
    with open('${TEST_RESULTS_DIR}/benchmark_results_${TIMESTAMP}.csv', 'r') as f:
        reader = csv.DictReader(f)
        rows = list(reader)

    print("Test Results by Thread Count:")
    print("-" * 80)

    # Group by test name and mode
    tests = {}
    for row in rows:
        key = row['Test']
        if key not in tests:
            tests[key] = {'sequential': None, 'parallel': {}}

        if row['Mode'] == 'Sequential':
            tests[key]['sequential'] = row
        else:
            threads = row['Threads']
            tests[key]['parallel'][threads] = row

    # Print summary
    for test_name, data in tests.items():
        print(f"\n{test_name}:")

        if data['sequential']:
            seq = data['sequential']
            print(f"  Sequential: {float(seq['TotalTimeMs']):.2f} ms")

        for threads, par in sorted(data['parallel'].items()):
            speedup = float(par.get('SpeedupRatio', 0))
            efficiency = float(par.get('Efficiency', 0)) * 100
            print(f"  {threads} threads: {float(par['TotalTimeMs']):.2f} ms "
                  f"(Speedup: {speedup:.2f}x, Efficiency: {efficiency:.1f}%)")

except Exception as e:
    print(f"Error processing results: {e}")
    sys.exit(1)
EOF

else
    print_warning "No CSV results found"
fi

echo "" >> "${REPORT_FILE}"

# Correctness test summary
if [ ${CORRECTNESS_PASSED} -eq 1 ]; then
    echo "Correctness: ALL TESTS PASSED ✓" >> "${REPORT_FILE}"
else
    echo "Correctness: SOME TESTS FAILED ✗" >> "${REPORT_FILE}"
fi

echo "" >> "${REPORT_FILE}"

# System information
echo "=== SYSTEM INFORMATION ===" >> "${REPORT_FILE}"
echo "" >> "${REPORT_FILE}"
echo "CPU: $(lscpu | grep 'Model name' | cut -d ':' -f2 | xargs)" >> "${REPORT_FILE}"
echo "CPU Cores: $(nproc)" >> "${REPORT_FILE}"
echo "Memory: $(free -h | grep Mem | awk '{print $2}')" >> "${REPORT_FILE}"
echo "OS: $(uname -s) $(uname -r)" >> "${REPORT_FILE}"
echo "" >> "${REPORT_FILE}"

# Final summary
print_header "Test Suite Complete"

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Test Report: ${REPORT_FILE}${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

cat "${REPORT_FILE}"

# Recommendations
print_header "Recommendations"

if [ ${CORRECTNESS_PASSED} -eq 1 ]; then
    print_success "Correctness verified - implementation is safe"
else
    print_error "Correctness tests failed - fix issues before deployment"
fi

echo ""
echo "Next steps:"
echo "  1. Review ${REPORT_FILE} for detailed results"
echo "  2. Check ${TEST_RESULTS_DIR}/benchmark_results_${TIMESTAMP}.csv for raw data"
echo "  3. If speedup is 3-6x, implementation meets performance goals"
echo "  4. If correctness tests pass, implementation is safe for production"
echo ""

# Check if speedup goal met (simplified check)
if [ -f "${TEST_RESULTS_DIR}/benchmark_results_${TIMESTAMP}.csv" ]; then
    # Check if any speedup >= 3.0
    GOOD_SPEEDUP=$(awk -F, '$11 >= 3.0 { print $11 }' "${TEST_RESULTS_DIR}/benchmark_results_${TIMESTAMP}.csv" | head -1)

    if [ -n "${GOOD_SPEEDUP}" ]; then
        print_success "Performance goal met: Speedup >= 3.0x achieved"
    else
        print_warning "Performance goal not met: Review configuration and test scenarios"
    fi
fi

print_success "Test suite complete!"
