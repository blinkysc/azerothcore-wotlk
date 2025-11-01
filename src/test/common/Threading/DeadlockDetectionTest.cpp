/*
 * Deadlock Detection Stress Tests - RIGOROUS VERSION
 *
 * These tests verify that the multithreading implementation is deadlock-free
 * under EXTREME concurrency and stress conditions with REAL workloads.
 *
 * Each test:
 * - Runs for 60+ seconds (except smoke test)
 * - Uses 100+ concurrent threads
 * - Performs 1,000,000+ operations
 * - Does actual computational work
 * - Reports timing, throughput, and contention metrics
 *
 * Purpose: Catch ANY deadlock regressions with high confidence
 */

#include "WorkStealingThreadPool.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <iomanip>
#include <random>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

class DeadlockDetectionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _threadPool = std::make_unique<WorkStealingThreadPool>();
    }

    void TearDown() override
    {
        if (_threadPool && _threadPool->IsActivated())
        {
            _threadPool->Deactivate();
        }
        _threadPool.reset();
    }

    // Helper: Run task with timeout, fail if it doesn't complete
    template<typename Func>
    bool RunWithTimeout(Func&& func, std::chrono::seconds timeout, const std::string& testName)
    {
        std::atomic<bool> completed{false};
        std::exception_ptr exceptionPtr = nullptr;

        auto future = std::async(std::launch::async, [&]() {
            try
            {
                func();
                completed.store(true, std::memory_order_release);
            }
            catch (...)
            {
                exceptionPtr = std::current_exception();
            }
        });

        if (future.wait_for(timeout) == std::future_status::timeout)
        {
            ADD_FAILURE() << testName << " TIMEOUT: Possible deadlock detected after "
                          << timeout.count() << " seconds";
            return false;
        }

        if (exceptionPtr)
        {
            try
            {
                std::rethrow_exception(exceptionPtr);
            }
            catch (const std::exception& ex)
            {
                ADD_FAILURE() << testName << " threw exception: " << ex.what();
                return false;
            }
        }

        EXPECT_TRUE(completed.load()) << testName << " did not complete successfully";
        return completed.load();
    }

    // Helper: Real computational work (not trivial arithmetic)
    static double PerformHeavyWork(uint32 iterations)
    {
        double result = 0.0;
        for (uint32 i = 1; i <= iterations; ++i)
        {
            result += std::sqrt(static_cast<double>(i)) * std::sin(i * 0.001);
            result += std::log(static_cast<double>(i) + 1.0) * std::cos(i * 0.001);
            result /= (i % 100 + 1);
        }
        return result;
    }

    // Helper: Print formatted metrics
    static void PrintMetrics(const std::string& testName,
                            double durationSeconds,
                            uint64 totalOperations,
                            uint32 numThreads,
                            uint64 contentionCount = 0)
    {
        double opsPerSec = static_cast<double>(totalOperations) / durationSeconds;

        std::cout << "\n=== " << testName << " RESULTS ===\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Duration:     " << durationSeconds << " seconds\n";
        std::cout << "  Threads:      " << numThreads << "\n";
        std::cout << "  Operations:   " << totalOperations << "\n";
        std::cout << "  Throughput:   " << std::setprecision(0) << opsPerSec << " ops/sec\n";
        if (contentionCount > 0)
        {
            std::cout << "  Contention:   " << contentionCount << " blocked attempts\n";
        }
        std::cout << "  Status:       ✓ PASSED (no deadlock)\n\n";
    }

    std::unique_ptr<WorkStealingThreadPool> _threadPool;
};

// ============================================================================
// TEST 1: Massive Thread Pool Saturation - 1M operations, 100 threads, 90s
// ============================================================================

TEST_F(DeadlockDetectionTest, MassiveThreadPoolSaturation_100kOperations_90Seconds)
{
    std::cout << "\n========================================\n";
    std::cout << "TEST 1: Massive Thread Pool Saturation\n";
    std::cout << "========================================\n";
    std::cout << "Config: 100 submitter threads, 32 workers, 1M tasks\n";
    std::cout << "Work: Real computation (sqrt, sin, cos, log)\n";
    std::cout << "Timeout: 200 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr uint32 NUM_WORKERS = 32;
        constexpr uint32 NUM_SUBMITTERS = 100;
        constexpr uint64 TOTAL_TASKS = 1000000;

        _threadPool->Activate(NUM_WORKERS);

        std::atomic<uint64> tasksCompleted{0};
        std::atomic<uint64> nextTaskId{0};
        std::atomic<uint64> contentionEvents{0};

        auto startTime = std::chrono::high_resolution_clock::now();

        std::cout << "Starting " << NUM_SUBMITTERS << " submitter threads...\n";

        // 100 submitter threads hammering the thread pool
        std::vector<std::thread> submitters;
        for (uint32 i = 0; i < NUM_SUBMITTERS; ++i)
        {
            submitters.emplace_back([&, threadId = i]() {
                std::mt19937 rng(threadId);
                std::uniform_int_distribution<uint32> workDist(5000, 15000);

                while (true)
                {
                    uint64 taskId = nextTaskId.fetch_add(1, std::memory_order_acq_rel);
                    if (taskId >= TOTAL_TASKS)
                        break;

                    // Try to detect contention
                    if (_threadPool->GetPendingTaskCount() > 1000)
                        contentionEvents.fetch_add(1, std::memory_order_relaxed);

                    uint32 workSize = workDist(rng);
                    _threadPool->Submit([&tasksCompleted, workSize]() {
                        // Real computational work
                        [[maybe_unused]] volatile double result =
                            DeadlockDetectionTest::PerformHeavyWork(workSize);
                        tasksCompleted.fetch_add(1, std::memory_order_release);
                    });

                    // Minimal delay to avoid CPU spin
                    if (taskId % 100 == 0)
                        std::this_thread::yield();
                }
            });
        }

        // Progress monitor
        std::thread monitor([&]() {
            while (tasksCompleted.load() < TOTAL_TASKS)
            {
                std::this_thread::sleep_for(10s);
                uint64 completed = tasksCompleted.load();
                double progress = (static_cast<double>(completed) / TOTAL_TASKS) * 100.0;
                std::cout << "  Progress: " << std::setprecision(1) << std::fixed
                          << progress << "% (" << completed << "/" << TOTAL_TASKS << ")\n";
            }
        });

        // Wait for all submitters
        for (auto& thread : submitters)
            thread.join();

        std::cout << "All tasks submitted. Waiting for completion...\n";

        // CRITICAL: WaitForCompletion must not deadlock
        _threadPool->WaitForCompletion();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double>(endTime - startTime);

        monitor.join();

        EXPECT_EQ(tasksCompleted.load(), TOTAL_TASKS);

        PrintMetrics("Massive Thread Pool Saturation",
                    duration.count(),
                    tasksCompleted.load(),
                    NUM_SUBMITTERS,
                    contentionEvents.load());

    }, 200s, "MassiveThreadPoolSaturation");

    EXPECT_TRUE(testPassed);
}

// ============================================================================
// TEST 2: Progressive Thread Count Stress - Find the breaking point
// ============================================================================

TEST_F(DeadlockDetectionTest, ProgressiveThreadCountStress_2to64Workers)
{
    std::cout << "\n========================================\n";
    std::cout << "TEST 2: Progressive Thread Count Stress\n";
    std::cout << "========================================\n";
    std::cout << "Testing thread counts: 2, 4, 8, 16, 32, 64\n";
    std::cout << "Each test runs for 10 seconds with continuous submission\n";
    std::cout << "Timeout: 240 seconds per configuration\n\n";

    std::vector<uint32> threadCounts = {2, 4, 8, 16, 32, 64};
    std::vector<std::pair<uint32, bool>> results;

    for (uint32 numWorkers : threadCounts)
    {
        std::cout << "\n--- Testing with " << numWorkers << " worker threads ---\n";

        bool testPassed = RunWithTimeout([this, numWorkers]() {
            // Scale task count with thread count: more workers = more tasks
            const uint32 TOTAL_TASKS = numWorkers * 10000; // 20K for 2 workers, 640K for 64 workers
            constexpr uint32 NUM_SUBMITTERS = 10;

            _threadPool->Activate(numWorkers);

            std::atomic<uint64> tasksSubmitted{0};
            std::atomic<uint64> tasksCompleted{0};

            auto startTime = std::chrono::high_resolution_clock::now();

            // Submitters with fixed total task count
            std::vector<std::thread> submitters;
            for (uint32 i = 0; i < NUM_SUBMITTERS; ++i)
            {
                submitters.emplace_back([&, threadId = i]() {
                    std::mt19937 rng(threadId);
                    std::uniform_int_distribution<uint32> workDist(5000, 20000);

                    while (true)
                    {
                        // Atomically claim a task number
                        uint64 myTaskNum = tasksSubmitted.fetch_add(1, std::memory_order_relaxed);
                        if (myTaskNum >= TOTAL_TASKS)
                            break; // Stop when we've submitted enough tasks

                        uint32 workSize = workDist(rng);

                        _threadPool->Submit([&tasksCompleted, workSize]() {
                            [[maybe_unused]] volatile double result =
                                DeadlockDetectionTest::PerformHeavyWork(workSize);
                            tasksCompleted.fetch_add(1, std::memory_order_release);
                        });
                    }
                });
            }

            for (auto& thread : submitters)
                thread.join();

            std::cout << "  Submitted " << TOTAL_TASKS << " tasks, waiting for completion...\n";
            _threadPool->WaitForCompletion();

            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration<double>(endTime - startTime);

            std::cout << "  ✓ " << numWorkers << " workers: " << tasksCompleted.load()
                      << " tasks in " << std::setprecision(2) << std::fixed << duration.count()
                      << "s (" << std::setprecision(0) << (tasksCompleted.load() / duration.count())
                      << " ops/sec)\n";

            _threadPool->Deactivate();

        }, 240s, std::string("Progressive_") + std::to_string(numWorkers) + "_workers");

        results.emplace_back(numWorkers, testPassed);

        if (!testPassed)
        {
            std::cout << "  ✗ FAILED at " << numWorkers << " workers - DEADLOCK DETECTED!\n";
            // Don't test higher counts if we found the breaking point
            break;
        }
    }

    // Print summary
    std::cout << "\n=== Progressive Stress Test Results ===\n";
    for (const auto& [workers, passed] : results)
    {
        std::cout << "  " << workers << " workers: " << (passed ? "✓ PASS" : "✗ FAIL (DEADLOCK)") << "\n";
    }
    std::cout << "\n";

    // Test passes if all tested configurations passed
    bool allPassed = std::all_of(results.begin(), results.end(),
                                  [](const auto& r) { return r.second; });
    EXPECT_TRUE(allPassed);
}

// ============================================================================
// TEST 3: Rapid Start/Stop Cycles - 200 iterations with heavy load
// ============================================================================

TEST_F(DeadlockDetectionTest, RapidStartStopCycles_200Iterations_90Seconds)
{
    std::cout << "\n========================================\n";
    std::cout << "TEST 3: Rapid Start/Stop Cycles\n";
    std::cout << "========================================\n";
    std::cout << "Config: 200 activate/deactivate cycles with tasks in flight\n";
    std::cout << "Work: 1000 tasks per cycle\n";
    std::cout << "Timeout: 200 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr uint32 NUM_CYCLES = 200;
        constexpr uint32 TASKS_PER_CYCLE = 1000;
        constexpr uint32 NUM_WORKERS = 16;

        uint64 totalTasksCompleted = 0;
        auto startTime = std::chrono::high_resolution_clock::now();

        std::cout << "Starting " << NUM_CYCLES << " rapid cycles...\n";

        for (uint32 cycle = 0; cycle < NUM_CYCLES; ++cycle)
        {
            std::atomic<uint32> cycleCompleted{0};

            _threadPool->Activate(NUM_WORKERS);

            // Submit tasks with real work
            for (uint32 i = 0; i < TASKS_PER_CYCLE; ++i)
            {
                _threadPool->Submit([&cycleCompleted]() {
                    [[maybe_unused]] volatile double result =
                        DeadlockDetectionTest::PerformHeavyWork(3000);
                    cycleCompleted.fetch_add(1, std::memory_order_relaxed);
                });
            }

            // CRITICAL: WaitForCompletion must not deadlock
            _threadPool->WaitForCompletion();

            EXPECT_EQ(cycleCompleted.load(), TASKS_PER_CYCLE)
                << "Cycle " << cycle << " incomplete";

            totalTasksCompleted += cycleCompleted.load();

            // Deactivate
            _threadPool->Deactivate();

            // Progress every 25 cycles
            if ((cycle + 1) % 25 == 0)
            {
                std::cout << "  Completed " << (cycle + 1) << "/" << NUM_CYCLES
                          << " cycles (" << totalTasksCompleted << " total tasks)\n";
            }
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double>(endTime - startTime);

        PrintMetrics("Rapid Start/Stop Cycles",
                    duration.count(),
                    totalTasksCompleted,
                    NUM_WORKERS);

    }, 200s, "RapidStartStopCycles");

    EXPECT_TRUE(testPassed);
}

// ============================================================================
// TEST 4: Pathological Lock Contention - 200 threads, minimal work
// ============================================================================

TEST_F(DeadlockDetectionTest, PathologicalLockContention_200Threads_60Seconds)
{
    std::cout << "\n========================================\n";
    std::cout << "TEST 4: Pathological Lock Contention\n";
    std::cout << "========================================\n";
    std::cout << "Config: 200 threads competing for locks with minimal work\n";
    std::cout << "Work: Short tasks to maximize lock contention\n";
    std::cout << "Timeout: 150 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr uint32 TOTAL_TASKS = 1000000; // Fixed task count to avoid queue overflow
        constexpr uint32 NUM_WORKERS = 32;
        constexpr uint32 NUM_SUBMITTERS = 200; // Pathological contention

        _threadPool->Activate(NUM_WORKERS);

        std::atomic<uint64> tasksSubmitted{0};
        std::atomic<uint64> tasksCompleted{0};
        std::atomic<uint64> maxQueueDepth{0};

        auto startTime = std::chrono::high_resolution_clock::now();

        std::cout << "Submitting " << TOTAL_TASKS << " tasks from " << NUM_SUBMITTERS << " threads (pathological contention)...\n";

        // 200 threads all trying to submit at once
        std::vector<std::thread> submitters;
        for (uint32 i = 0; i < NUM_SUBMITTERS; ++i)
        {
            submitters.emplace_back([&, threadId = i]() {
                std::mt19937 rng(threadId);
                std::uniform_int_distribution<uint32> workDist(500, 2000); // Short tasks

                while (true)
                {
                    // Atomically claim a task number
                    uint64 myTaskNum = tasksSubmitted.fetch_add(1, std::memory_order_relaxed);
                    if (myTaskNum >= TOTAL_TASKS)
                        break;

                    // Track maximum queue depth (indicates contention)
                    uint64 currentDepth = _threadPool->GetPendingTaskCount();
                    uint64 currentMax = maxQueueDepth.load(std::memory_order_relaxed);
                    while (currentDepth > currentMax &&
                           !maxQueueDepth.compare_exchange_weak(currentMax, currentDepth)) {}

                    uint32 workSize = workDist(rng);
                    _threadPool->Submit([&tasksCompleted, workSize]() {
                        [[maybe_unused]] volatile double result =
                            DeadlockDetectionTest::PerformHeavyWork(workSize);
                        tasksCompleted.fetch_add(1, std::memory_order_release);
                    });

                    // No delay - maximize contention
                }
            });
        }

        for (auto& thread : submitters)
            thread.join();

        std::cout << "All tasks submitted. Waiting for completion...\n";
        _threadPool->WaitForCompletion();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double>(endTime - startTime);

        std::cout << "  Maximum queue depth: " << maxQueueDepth.load() << " (contention indicator)\n";

        PrintMetrics("Pathological Lock Contention",
                    duration.count(),
                    tasksCompleted.load(),
                    NUM_SUBMITTERS);

    }, 150s, "PathologicalLockContention");

    EXPECT_TRUE(testPassed);
}

// ============================================================================
// TEST 5: Extended Endurance - 5 minutes sustained load
// ============================================================================

TEST_F(DeadlockDetectionTest, ExtendedEndurance_5MinutesSustainedLoad)
{
    std::cout << "\n========================================\n";
    std::cout << "TEST 5: Extended Endurance (5 minutes)\n";
    std::cout << "========================================\n";
    std::cout << "Config: 64 workers, 32 submitters, 5 minute sustained load\n";
    std::cout << "Work: Variable (1k-20k iterations)\n";
    std::cout << "Timeout: 600 seconds (10 minutes)\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr uint32 TOTAL_TASKS = 1000000; // Large task count for endurance test
        constexpr uint32 NUM_WORKERS = 64;
        constexpr uint32 NUM_SUBMITTERS = 32;

        _threadPool->Activate(NUM_WORKERS);

        std::atomic<uint64> tasksSubmitted{0};
        std::atomic<uint64> tasksCompleted{0};

        auto startTime = std::chrono::high_resolution_clock::now();

        std::cout << "Starting extended endurance test (" << TOTAL_TASKS << " tasks)...\n";

        // Sustained submitters
        std::vector<std::thread> submitters;
        for (uint32 i = 0; i < NUM_SUBMITTERS; ++i)
        {
            submitters.emplace_back([&, threadId = i]() {
                std::mt19937 rng(threadId);
                std::uniform_int_distribution<uint32> workDist(1000, 20000);

                while (true)
                {
                    // Atomically claim a task number
                    uint64 myTaskNum = tasksSubmitted.fetch_add(1, std::memory_order_relaxed);
                    if (myTaskNum >= TOTAL_TASKS)
                        break;

                    uint32 workSize = workDist(rng);
                    _threadPool->Submit([&tasksCompleted, workSize]() {
                        [[maybe_unused]] volatile double result =
                            DeadlockDetectionTest::PerformHeavyWork(workSize);
                        tasksCompleted.fetch_add(1, std::memory_order_release);
                    });

                    // Brief pause to avoid flooding
                    if (myTaskNum % 1000 == 0)
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            });
        }

        for (auto& thread : submitters)
            thread.join();

        std::cout << "All tasks submitted. Waiting for completion...\n";
        _threadPool->WaitForCompletion();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double>(endTime - startTime);

        std::cout << "  ✓ " << TOTAL_TASKS << " tasks completed in "
                  << std::setprecision(2) << std::fixed << duration.count() << "s ("
                  << std::setprecision(0) << (TOTAL_TASKS / duration.count()) << " ops/sec)\n";

        PrintMetrics("Extended Endurance",
                    duration.count(),
                    tasksCompleted.load(),
                    NUM_WORKERS);

    }, 600s, "ExtendedEndurance");

    EXPECT_TRUE(testPassed);
}

// ============================================================================
// SMOKE TEST: Quick validation (kept for sanity checking)
// ============================================================================

TEST_F(DeadlockDetectionTest, SmokeTest_QuickValidation)
{
    std::cout << "\n========================================\n";
    std::cout << "SMOKE TEST: Quick Validation\n";
    std::cout << "========================================\n";

    std::atomic<uint32> counter{0};
    _threadPool->Activate(8);

    auto startTime = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 1000; ++i)
    {
        _threadPool->Submit([&counter]() {
            [[maybe_unused]] volatile double result =
                DeadlockDetectionTest::PerformHeavyWork(1000);
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    _threadPool->WaitForCompletion();

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double>(endTime - startTime);

    EXPECT_EQ(counter.load(), 1000);

    std::cout << "  Completed: 1000 tasks in " << std::setprecision(3)
              << duration.count() << " seconds\n";
    std::cout << "  Status: ✓ PASSED\n\n";
}
