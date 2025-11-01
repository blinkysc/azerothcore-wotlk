# Multithreading Test Suite Documentation

## Overview

This document describes the comprehensive test suite for the AzerothCore work-stealing multithreading system.

## Test Files

### 1. WorkStealingThreadPoolTest.cpp
Location: `src/test/common/Threading/WorkStealingThreadPoolTest.cpp`

**Purpose**: Unit tests for the WorkStealingThreadPool class

**Test Categories**:
- Basic functionality (activate, deactivate, task execution)
- Work stealing algorithm correctness
- Thread safety under concurrent access
- Performance and stress testing
- Edge cases (empty pool, single thread, exceptions)
- Throughput measurement

**Key Tests**:
- `ActivateAndDeactivate`: Verify pool lifecycle
- `MultipleTasksExecution`: Test 1000+ tasks complete correctly
- `WorkStealingLoadBalancing`: Verify work stealing occurs under imbalance
- `ConcurrentSubmits`: Test thread-safe submission from multiple threads
- `RaceConditionTest`: Detect race conditions with shared data
- `HighFrequencySubmission`: 100K tasks stress test
- `ThroughputMeasurement`: Measure operations per second

### 2. MapParallelUpdateTest.cpp
Location: `src/test/server/game/Maps/MapParallelUpdateTest.cpp`

**Purpose**: Integration tests for parallel map updates

**Test Categories**:
- Correctness (parallel vs sequential consistency)
- Performance comparison
- Stress testing with high entity counts
- Concurrency (object spawn/despawn during updates)
- Scalability with varying thread counts

**Key Tests**:
- `ParallelVsSequentialConsistency`: Verify identical results
- `SequentialUpdateBaseline` / `ParallelUpdatePerformance`: Compare timings
- `SpeedupCalculation`: Measure actual speedup (target >1.5x with 4 threads)
- `HighLoadStressTest`: 50K entities, 10 iterations
- `ExtendedStressTest`: Continuous updates for 1 minute
- `ConcurrentObjectModification`: Test mid-update entity changes
- `ThreadScalability`: Test 1, 2, 4, 8, 16 threads

### 3. MapUpdateBenchmark.cpp
Location: `src/test/server/game/Maps/MapUpdateBenchmark.cpp`

**Purpose**: Comprehensive performance benchmarks

**Benchmark Types**:
- Thread count scaling (1-16 threads)
- Object count scaling (100-50K objects)
- Variable workload distribution
- Latency measurement (mean, stddev, min/max)
- Sustained throughput testing

**Output**:
- Results table printed to console
- CSV export (`benchmark_results.csv`) for analysis
- Speedup, efficiency, and throughput metrics

## Building and Running Tests

### Build with Tests Enabled

```bash
cd build
cmake .. -DBUILD_TESTING=ON \
         -DCMAKE_INSTALL_PREFIX=$HOME/azerothcore/env/dist/ \
         -DCMAKE_C_COMPILER=/usr/bin/clang \
         -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
         -DTOOLS_BUILD=all \
         -DSCRIPTS=static \
         -DMODULES=static

make -j$(nproc)
```

### Run All Tests

```bash
cd build
ctest
```

Or directly:

```bash
./src/test/unit_tests
```

### Run Specific Test Suites

```bash
# Run only thread pool tests
./src/test/unit_tests --gtest_filter="WorkStealingThreadPoolTest.*"

# Run only map parallel update tests
./src/test/unit_tests --gtest_filter="MapParallelUpdateTest.*"

# Run benchmarks
./src/test/unit_tests --gtest_filter="MapUpdateBenchmark.*"
```

### Run with Verbose Output

```bash
./src/test/unit_tests --gtest_verbose
```

## Thread Safety Testing

### Using Thread Sanitizer (TSan)

Thread Sanitizer detects data races and other threading bugs:

```bash
# Build with TSan
cmake .. -DBUILD_TESTING=ON \
         -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
         -DCMAKE_C_FLAGS="-fsanitize=thread -g"

make -j$(nproc)

# Run tests
./src/test/unit_tests
```

**What TSan Detects**:
- Data races
- Deadlocks
- Use of destroyed mutexes
- Thread leaks

### Using Address Sanitizer (ASan)

Address Sanitizer detects memory errors:

```bash
# Build with ASan
cmake .. -DBUILD_TESTING=ON \
         -DCMAKE_CXX_FLAGS="-fsanitize=address -g -fno-omit-frame-pointer" \
         -DCMAKE_C_FLAGS="-fsanitize=address -g -fno-omit-frame-pointer"

make -j$(nproc)

# Run tests
./src/test/unit_tests
```

**What ASan Detects**:
- Use-after-free
- Heap buffer overflow
- Stack buffer overflow
- Memory leaks
- Use-after-return

### Using Valgrind

For comprehensive memory leak detection:

```bash
# Build with debug symbols
cmake .. -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug

make -j$(nproc)

# Run with Valgrind
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --verbose \
         ./src/test/unit_tests
```

**Valgrind Reports**:
- Memory leaks (definitely lost, possibly lost, still reachable)
- Invalid memory access
- Uninitialized value usage
- Thread errors (with --tool=helgrind or --tool=drd)

## Performance Analysis

### CPU Profiling

Using `perf` on Linux:

```bash
# Record profiling data
perf record -g ./src/test/unit_tests --gtest_filter="MapUpdateBenchmark.*"

# View report
perf report

# Generate flame graph (requires FlameGraph tools)
perf script | stackcollapse-perf.pl | flamegraph.pl > flamegraph.svg
```

### Benchmark Result Analysis

After running benchmarks, analyze `benchmark_results.csv`:

```python
import pandas as pd
import matplotlib.pyplot as plt

# Load results
df = pd.read_csv('benchmark_results.csv')

# Plot speedup vs thread count
plt.figure(figsize=(10, 6))
plt.plot(df['Threads'], df['Speedup'], marker='o')
plt.xlabel('Number of Threads')
plt.ylabel('Speedup')
plt.title('Multithreading Speedup')
plt.grid(True)
plt.savefig('speedup.png')

# Plot efficiency
plt.figure(figsize=(10, 6))
plt.plot(df['Threads'], df['Efficiency'] * 100, marker='o')
plt.xlabel('Number of Threads')
plt.ylabel('Efficiency (%)')
plt.title('Parallel Efficiency')
plt.grid(True)
plt.savefig('efficiency.png')
```

## Expected Performance Metrics

### Target Speedups (4 cores)
- **100-500 objects**: 1.3-2.0x (overhead dominates)
- **500-2000 objects**: 2.5-3.5x (good balance)
- **2000-10000 objects**: 3.5-6.0x (near-linear scaling)
- **10000+ objects**: 4.0-7.0x (ideal workload)

### Efficiency Targets
- **2 threads**: >75% efficiency
- **4 threads**: >60% efficiency
- **8 threads**: >50% efficiency
- **16 threads**: >40% efficiency (diminishing returns)

### Throughput Targets
With 4 threads and realistic workload:
- **Minimum**: 50K objects/second
- **Target**: 100K objects/second
- **Excellent**: 200K+ objects/second

## Continuous Integration

### CI Test Script

```bash
#!/bin/bash
# run_multithreading_tests.sh

set -e

echo "Building with tests enabled..."
cd build
cmake .. -DBUILD_TESTING=ON
make -j$(nproc)

echo "Running unit tests..."
./src/test/unit_tests --gtest_output=xml:test_results.xml

echo "Running with Thread Sanitizer..."
cmake .. -DCMAKE_CXX_FLAGS="-fsanitize=thread"
make clean && make -j$(nproc)
./src/test/unit_tests

echo "Running with Address Sanitizer..."
cmake .. -DCMAKE_CXX_FLAGS="-fsanitize=address"
make clean && make -j$(nproc)
./src/test/unit_tests

echo "All tests passed!"
```

### GitHub Actions Example

```yaml
name: Multithreading Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest

    steps:
    - uses: actions/checkout@v2

    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y cmake clang

    - name: Build and test
      run: |
        mkdir build && cd build
        cmake .. -DBUILD_TESTING=ON
        make -j$(nproc)
        ./src/test/unit_tests

    - name: Test with TSan
      run: |
        cd build
        cmake .. -DCMAKE_CXX_FLAGS="-fsanitize=thread"
        make clean && make -j$(nproc)
        ./src/test/unit_tests
```

## Interpreting Test Results

### Successful Test Run

```
[==========] Running 45 tests from 3 test suites.
[----------] Global test environment set-up.
[----------] 15 tests from WorkStealingThreadPoolTest
[ RUN      ] WorkStealingThreadPoolTest.ActivateAndDeactivate
[       OK ] WorkStealingThreadPoolTest.ActivateAndDeactivate (2 ms)
...
[----------] 15 tests from WorkStealingThreadPoolTest (450 ms total)
...
[==========] 45 tests from 3 test suites ran. (5234 ms total)
[  PASSED  ] 45 tests.
```

### Failure Indicators

**Race Condition Detected**:
```
[ RUN      ] WorkStealingThreadPoolTest.RaceConditionTest
Expected equality of these values:
  sharedData[42]
    Which is: 0
  42
test_file.cpp:123: Failure
[  FAILED  ] WorkStealingThreadPoolTest.RaceConditionTest (15 ms)
```

**Performance Regression**:
```
Expected: speedup > 1.5
  Actual: 1.2x
This indicates a performance regression - investigate overhead!
```

## Troubleshooting

### Tests Hang/Deadlock

**Symptoms**: Tests never complete

**Debug**:
```bash
# Attach debugger
gdb --args ./src/test/unit_tests --gtest_filter="HangingTest"
(gdb) run
# Wait for hang, then Ctrl+C
(gdb) thread apply all bt  # Show all thread backtraces
```

### Flaky Tests

**Symptoms**: Tests pass/fail randomly

**Causes**:
- Race conditions
- Timing dependencies
- Insufficient synchronization

**Fix**: Add proper synchronization, increase timeouts, use condition variables

### Low Speedup

**Symptoms**: Parallel version not faster than sequential

**Investigate**:
1. Check object count (too small?)
2. Verify thread count matches CPU cores
3. Profile with `perf` to find bottlenecks
4. Check for false sharing (align data structures)
5. Verify work is actually being distributed

## Additional Resources

- **Google Test Documentation**: https://google.github.io/googletest/
- **Thread Sanitizer**: https://clang.llvm.org/docs/ThreadSanitizer.html
- **Address Sanitizer**: https://clang.llvm.org/docs/AddressSanitizer.html
- **Valgrind**: https://valgrind.org/docs/manual/manual.html
- **Performance Analysis Tools**: https://perf.wiki.kernel.org/

## Contributing Tests

When adding new tests:

1. Follow existing test naming conventions
2. Add documentation comments
3. Include expected performance characteristics
4. Test edge cases and error conditions
5. Verify tests pass with TSan/ASan
6. Update this documentation

## Support

For issues with the test suite:
- Check existing tests for examples
- Review test output carefully
- Use sanitizers to detect issues
- Profile for performance problems
- Ask on Discord: https://discord.gg/gkt4y2x
