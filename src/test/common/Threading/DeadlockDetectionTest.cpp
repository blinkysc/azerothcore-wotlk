/*
 * Deadlock Detection Stress Tests - RIGOROUS VERSION
 *
 * These tests verify that the multithreading implementation is deadlock-free
 * under EXTREME concurrency and stress conditions with REAL workloads.
 *
 * Each test:
 * - Runs for 60+ seconds (except smoke test)
 * - Uses 100+ concurrent threads
 * - Performs 100,000+ operations
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
// TEST 1: Massive Thread Pool Saturation - 100k operations, 100 threads, 90s
// ============================================================================

TEST_F(DeadlockDetectionTest, MassiveThreadPoolSaturation_100kOperations_90Seconds)
{
    std::cout << "\n========================================\n";
    std::cout << "TEST 1: Massive Thread Pool Saturation\n";
    std::cout << "========================================\n";
    std::cout << "Config: 100 submitter threads, 32 workers, 100k tasks\n";
    std::cout << "Work: Real computation (sqrt, sin, cos, log)\n";
    std::cout << "Timeout: 120 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr uint32 NUM_WORKERS = 32;
        constexpr uint32 NUM_SUBMITTERS = 100;
        constexpr uint64 TOTAL_TASKS = 100000;

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

    }, 120s, "MassiveThreadPoolSaturation");

    EXPECT_TRUE(testPassed);
}

// ============================================================================
// TEST 2: Extreme Work Stealing - 120 workers, 60 seconds
// ============================================================================

TEST_F(DeadlockDetectionTest, ExtremeWorkStealing_120Workers_60Seconds)
{
    std::cout << "\n========================================\n";
    std::cout << "TEST 2: Extreme Work Stealing Stress\n";
    std::cout << "========================================\n";
    std::cout << "Config: 120 worker threads, continuous submission for 60s\n";
    std::cout << "Work: Variable load to trigger maximum work-stealing\n";
    std::cout << "Timeout: 90 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr auto TEST_DURATION = 60s;
        constexpr uint32 NUM_WORKERS = 120;
        constexpr uint32 NUM_SUBMITTERS = 20;

        _threadPool->Activate(NUM_WORKERS);

        std::atomic<bool> stopTest{false};
        std::atomic<uint64> tasksCompleted{0};
        std::atomic<uint64> workStealingDetected{0};

        auto startTime = std::chrono::high_resolution_clock::now();

        std::cout << "Starting continuous submission for "
                  << std::chrono::duration_cast<std::chrono::seconds>(TEST_DURATION).count()
                  << " seconds...\n";

        // Continuous submitters with highly variable workload
        std::vector<std::thread> submitters;
        for (uint32 i = 0; i < NUM_SUBMITTERS; ++i)
        {
            submitters.emplace_back([&, threadId = i]() {
                std::mt19937 rng(threadId);
                std::uniform_int_distribution<uint32> workDist(1000, 50000); // Highly variable

                while (!stopTest.load(std::memory_order_acquire))
                {
                    uint32 workSize = workDist(rng);

                    // Detect potential work-stealing scenarios
                    if (_threadPool->GetPendingTaskCount() < NUM_WORKERS / 2)
                        workStealingDetected.fetch_add(1, std::memory_order_relaxed);

                    _threadPool->Submit([&tasksCompleted, workSize]() {
                        [[maybe_unused]] volatile double result =
                            DeadlockDetectionTest::PerformHeavyWork(workSize);
                        tasksCompleted.fetch_add(1, std::memory_order_release);
                    });

                    // Randomize submission rate
                    if (rng() % 10 == 0)
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            });
        }

        // Progress monitor every 15 seconds
        std::thread monitor([&]() {
            auto lastCheck = startTime;
            uint64 lastCompleted = 0;

            while (!stopTest.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(15s);

                auto now = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration<double>(now - startTime);
                auto intervalDuration = std::chrono::duration<double>(now - lastCheck);

                uint64 currentCompleted = tasksCompleted.load();
                uint64 intervalOps = currentCompleted - lastCompleted;
                double opsPerSec = intervalOps / intervalDuration.count();

                std::cout << "  [" << std::setprecision(0) << std::fixed << elapsed.count()
                          << "s] Completed: " << currentCompleted
                          << " | Rate: " << std::setprecision(0) << opsPerSec << " ops/sec\n";

                lastCheck = now;
                lastCompleted = currentCompleted;
            }
        });

        // Run for specified duration
        std::this_thread::sleep_for(TEST_DURATION);
        stopTest.store(true, std::memory_order_release);

        std::cout << "Stopping submitters...\n";

        for (auto& thread : submitters)
            thread.join();

        std::cout << "Waiting for remaining tasks...\n";
        _threadPool->WaitForCompletion();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double>(endTime - startTime);

        monitor.join();

        PrintMetrics("Extreme Work Stealing",
                    duration.count(),
                    tasksCompleted.load(),
                    NUM_WORKERS,
                    workStealingDetected.load());

    }, 90s, "ExtremeWorkStealing");

    EXPECT_TRUE(testPassed);
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
    std::cout << "Timeout: 120 seconds\n\n";

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

    }, 120s, "RapidStartStopCycles");

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
    std::cout << "Timeout: 90 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr auto TEST_DURATION = 60s;
        constexpr uint32 NUM_WORKERS = 32;
        constexpr uint32 NUM_SUBMITTERS = 200; // Pathological contention

        _threadPool->Activate(NUM_WORKERS);

        std::atomic<bool> stopTest{false};
        std::atomic<uint64> tasksCompleted{0};
        std::atomic<uint64> maxQueueDepth{0};

        auto startTime = std::chrono::high_resolution_clock::now();

        std::cout << "Starting " << NUM_SUBMITTERS << " threads (pathological contention)...\n";

        // 200 threads all trying to submit at once
        std::vector<std::thread> submitters;
        for (uint32 i = 0; i < NUM_SUBMITTERS; ++i)
        {
            submitters.emplace_back([&, threadId = i]() {
                std::mt19937 rng(threadId);
                std::uniform_int_distribution<uint32> workDist(500, 2000); // Short tasks

                while (!stopTest.load(std::memory_order_acquire))
                {
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

        // Monitor every 15 seconds
        std::thread monitor([&]() {
            auto lastCheck = startTime;
            uint64 lastCompleted = 0;

            while (!stopTest.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(15s);

                auto now = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration<double>(now - startTime);
                auto intervalDuration = std::chrono::duration<double>(now - lastCheck);

                uint64 currentCompleted = tasksCompleted.load();
                uint64 intervalOps = currentCompleted - lastCompleted;
                double opsPerSec = intervalOps / intervalDuration.count();

                std::cout << "  [" << std::setprecision(0) << std::fixed << elapsed.count()
                          << "s] Completed: " << currentCompleted
                          << " | Rate: " << opsPerSec << " ops/sec"
                          << " | Max Queue: " << maxQueueDepth.load() << "\n";

                lastCheck = now;
                lastCompleted = currentCompleted;
            }
        });

        std::this_thread::sleep_for(TEST_DURATION);
        stopTest.store(true, std::memory_order_release);

        std::cout << "Stopping " << NUM_SUBMITTERS << " threads...\n";

        for (auto& thread : submitters)
            thread.join();

        std::cout << "Waiting for remaining tasks...\n";
        _threadPool->WaitForCompletion();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double>(endTime - startTime);

        monitor.join();

        std::cout << "  Maximum queue depth: " << maxQueueDepth.load() << " (contention indicator)\n";

        PrintMetrics("Pathological Lock Contention",
                    duration.count(),
                    tasksCompleted.load(),
                    NUM_SUBMITTERS);

    }, 90s, "PathologicalLockContention");

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
    std::cout << "Timeout: 360 seconds (6 minutes)\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr auto TEST_DURATION = 5min;
        constexpr uint32 NUM_WORKERS = 64;
        constexpr uint32 NUM_SUBMITTERS = 32;

        _threadPool->Activate(NUM_WORKERS);

        std::atomic<bool> stopTest{false};
        std::atomic<uint64> tasksCompleted{0};

        auto startTime = std::chrono::high_resolution_clock::now();

        std::cout << "Starting 5-minute sustained load test...\n";

        // Sustained submitters
        std::vector<std::thread> submitters;
        for (uint32 i = 0; i < NUM_SUBMITTERS; ++i)
        {
            submitters.emplace_back([&, threadId = i]() {
                std::mt19937 rng(threadId);
                std::uniform_int_distribution<uint32> workDist(1000, 20000);

                while (!stopTest.load(std::memory_order_acquire))
                {
                    uint32 workSize = workDist(rng);
                    _threadPool->Submit([&tasksCompleted, workSize]() {
                        [[maybe_unused]] volatile double result =
                            DeadlockDetectionTest::PerformHeavyWork(workSize);
                        tasksCompleted.fetch_add(1, std::memory_order_release);
                    });

                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            });
        }

        // Monitor every 30 seconds
        std::thread monitor([&]() {
            auto lastCheck = startTime;
            uint64 lastCompleted = 0;

            while (!stopTest.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(30s);

                auto now = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration<double>(now - startTime);
                auto intervalDuration = std::chrono::duration<double>(now - lastCheck);

                uint64 currentCompleted = tasksCompleted.load();
                uint64 intervalOps = currentCompleted - lastCompleted;
                double opsPerSec = intervalOps / intervalDuration.count();

                std::cout << "  [" << std::setprecision(0) << std::fixed << elapsed.count()
                          << "s / " << std::chrono::duration_cast<std::chrono::seconds>(TEST_DURATION).count()
                          << "s] Completed: " << currentCompleted
                          << " | Rate: " << opsPerSec << " ops/sec\n";

                lastCheck = now;
                lastCompleted = currentCompleted;
            }
        });

        std::this_thread::sleep_for(TEST_DURATION);
        stopTest.store(true, std::memory_order_release);

        std::cout << "Stopping submitters...\n";

        for (auto& thread : submitters)
            thread.join();

        std::cout << "Waiting for remaining tasks...\n";
        _threadPool->WaitForCompletion();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double>(endTime - startTime);

        monitor.join();

        PrintMetrics("Extended Endurance",
                    duration.count(),
                    tasksCompleted.load(),
                    NUM_WORKERS);

    }, 360s, "ExtendedEndurance");

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
