/*
 * Deadlock Detection Stress Tests
 *
 * These tests verify that the multithreading implementation is deadlock-free
 * under extreme concurrency and stress conditions. All tests have strict timeouts
 * and will fail if any thread hangs on a lock.
 *
 * Purpose: Safety net to catch deadlock regressions if locking patterns change
 */

#include "WorkStealingThreadPool.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
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

    std::unique_ptr<WorkStealingThreadPool> _threadPool;
};

// ============================================================================
// SMOKE TEST: Basic functionality without timeout wrapper
// ============================================================================

TEST_F(DeadlockDetectionTest, SmokeTest_BasicFunctionality)
{
    std::cout << "\n=== Smoke Test: Basic Functionality ===\n";

    std::atomic<int> counter{0};
    _threadPool->Activate(4);

    for (int i = 0; i < 100; ++i)
    {
        _threadPool->Submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    _threadPool->WaitForCompletion();
    EXPECT_EQ(counter.load(), 100);

    std::cout << "Smoke test passed: Submitted 100 tasks, all completed\n";
}

// ============================================================================
// TEST 1: Concurrent Work Queue Contention
// ============================================================================

TEST_F(DeadlockDetectionTest, DISABLED_ConcurrentWorkQueueContention_50Threads_60Seconds)
{
    std::cout << "\n=== Deadlock Test 1: Concurrent Work Queue Contention ===\n";
    std::cout << "Testing: 50 threads submitting 10k tasks with random delays\n";
    std::cout << "Purpose: Verify no deadlock on WorkQueue locks\n";
    std::cout << "Timeout: 90 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr uint32 NUM_THREADS = 50;
        constexpr uint32 TASKS_PER_THREAD = 200;  // 10k total tasks
        constexpr auto TEST_DURATION = 60s;

        _threadPool->Activate(NUM_THREADS);

        std::atomic<uint32> tasksCompleted{0};
        std::atomic<uint32> tasksSubmitted{0};
        std::atomic<bool> stopTest{false};

        auto startTime = std::chrono::steady_clock::now();

        // Submitter threads (stress the work queue locks)
        std::vector<std::thread> submitters;
        for (uint32 i = 0; i < NUM_THREADS; ++i)
        {
            submitters.emplace_back([&, threadId = i]() {
                std::mt19937 rng(threadId);
                std::uniform_int_distribution<int> delay(0, 100); // 0-100μs

                while (!stopTest.load(std::memory_order_acquire))
                {
                    // Random delay to create contention
                    std::this_thread::sleep_for(std::chrono::microseconds(delay(rng)));

                    _threadPool->Submit([&tasksCompleted]() {
                        // Simulate work
                        uint64 sum = 0;
                        for (int i = 0; i < 1000; ++i)
                            sum += i;
                        [[maybe_unused]] volatile uint64 dummy = sum; // Prevent optimization

                        tasksCompleted.fetch_add(1, std::memory_order_release);
                    });

                    uint32 submitted = tasksSubmitted.fetch_add(1, std::memory_order_acq_rel);
                    if (submitted >= NUM_THREADS * TASKS_PER_THREAD)
                        break;
                }
            });
        }

        // Wait for test duration
        std::this_thread::sleep_for(TEST_DURATION);
        stopTest.store(true, std::memory_order_release);

        // Join submitters
        for (auto& thread : submitters)
            thread.join();

        std::cout << "  Tasks submitted: " << tasksSubmitted.load() << "\n";
        std::cout << "  Waiting for task completion...\n";

        // Wait for all tasks to complete (this tests WaitForCompletion deadlock)
        _threadPool->WaitForCompletion();

        std::cout << "  Tasks completed: " << tasksCompleted.load() << "\n";

        EXPECT_GT(tasksSubmitted.load(), 0u) << "No tasks submitted";
        EXPECT_EQ(tasksCompleted.load(), tasksSubmitted.load())
            << "Not all tasks completed";

    }, 90s, "ConcurrentWorkQueueContention");

    EXPECT_TRUE(testPassed) << "Test failed or timed out (possible deadlock)";
}

// ============================================================================
// TEST 2: Thread Pool Saturation with Work Stealing
// ============================================================================

TEST_F(DeadlockDetectionTest, ThreadPoolSaturation_10kTasks_MultipleSubmitters)
{
    std::cout << "\n=== Deadlock Test 2: Thread Pool Saturation ===\n";
    std::cout << "Testing: 10k tasks submitted by 20 threads, all workers stealing\n";
    std::cout << "Purpose: Verify no deadlock during heavy work-stealing\n";
    std::cout << "Timeout: 90 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr uint32 NUM_WORKERS = 16;
        constexpr uint32 NUM_SUBMITTERS = 20;
        constexpr uint32 TOTAL_TASKS = 10000;

        _threadPool->Activate(NUM_WORKERS);

        std::atomic<uint32> tasksCompleted{0};
        std::atomic<uint32> nextTaskId{0};

        auto startTime = std::chrono::steady_clock::now();

        // Multiple submitter threads (create contention on Submit)
        std::vector<std::thread> submitters;
        for (uint32 i = 0; i < NUM_SUBMITTERS; ++i)
        {
            submitters.emplace_back([&]() {
                while (true)
                {
                    uint32 taskId = nextTaskId.fetch_add(1, std::memory_order_acq_rel);
                    if (taskId >= TOTAL_TASKS)
                        break;

                    _threadPool->Submit([&tasksCompleted, taskId]() {
                        // Variable work to ensure uneven load (triggers work stealing)
                        uint64 sum = 0;
                        uint32 iterations = 100 + (taskId % 900); // 100-1000 iterations

                        for (uint32 i = 0; i < iterations; ++i)
                            sum += i * taskId;
                        [[maybe_unused]] volatile uint64 dummy = sum; // Prevent optimization

                        tasksCompleted.fetch_add(1, std::memory_order_release);
                    });

                    // Burst submit (no delay) to maximize contention
                }
            });
        }

        // Join submitters
        for (auto& thread : submitters)
            thread.join();

        std::cout << "  All tasks submitted (" << nextTaskId.load() << ")\n";
        std::cout << "  Waiting for completion...\n";

        // Critical test: WaitForCompletion must not deadlock
        _threadPool->WaitForCompletion();

        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        std::cout << "  Tasks completed: " << tasksCompleted.load() << "\n";
        std::cout << "  Duration: " << duration.count() << " ms\n";

        EXPECT_EQ(tasksCompleted.load(), TOTAL_TASKS) << "Not all tasks completed";

    }, 90s, "ThreadPoolSaturation");

    EXPECT_TRUE(testPassed) << "Test failed or timed out (possible deadlock)";
}

// ============================================================================
// TEST 3: Rapid Activate/Deactivate Cycles
// ============================================================================

TEST_F(DeadlockDetectionTest, RapidStartStopCycles_100Iterations)
{
    std::cout << "\n=== Deadlock Test 3: Rapid Start/Stop Cycles ===\n";
    std::cout << "Testing: 100 activate/deactivate cycles with task submission\n";
    std::cout << "Purpose: Verify no deadlock during shutdown transitions\n";
    std::cout << "Timeout: 90 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr uint32 NUM_CYCLES = 100;
        constexpr uint32 NUM_THREADS = 8;

        for (uint32 cycle = 0; cycle < NUM_CYCLES; ++cycle)
        {
            // Activate
            _threadPool->Activate(NUM_THREADS);

            std::atomic<uint32> tasksCompleted{0};

            // Submit tasks rapidly
            for (uint32 i = 0; i < 50; ++i)
            {
                _threadPool->Submit([&tasksCompleted]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    tasksCompleted.fetch_add(1, std::memory_order_release);
                });
            }

            // Wait for tasks (tests shutdown while tasks are running)
            _threadPool->WaitForCompletion();

            EXPECT_EQ(tasksCompleted.load(), 50u) << "Cycle " << cycle << " failed";

            // Deactivate (critical: must not deadlock during shutdown)
            _threadPool->Deactivate();

            if (cycle % 10 == 0)
                std::cout << "  Completed " << (cycle + 1) << " cycles\n";
        }

        std::cout << "  All " << NUM_CYCLES << " cycles completed successfully\n";

    }, 90s, "RapidStartStopCycles");

    EXPECT_TRUE(testPassed) << "Test failed or timed out (possible deadlock)";
}

// ============================================================================
// TEST 4: Concurrent Submit During Deactivation (Edge Case)
// ============================================================================

TEST_F(DeadlockDetectionTest, ConcurrentSubmitDuringDeactivation)
{
    std::cout << "\n=== Deadlock Test 4: Submit During Deactivation ===\n";
    std::cout << "Testing: Threads submitting tasks while pool is shutting down\n";
    std::cout << "Purpose: Verify no deadlock on shutdown flag races\n";
    std::cout << "Timeout: 30 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr uint32 NUM_THREADS = 8;
        constexpr uint32 NUM_SUBMITTERS = 10;

        _threadPool->Activate(NUM_THREADS);

        std::atomic<bool> stopSubmitting{false};
        std::atomic<uint32> successfulSubmits{0};
        std::atomic<uint32> failedSubmits{0};

        // Submitter threads
        std::vector<std::thread> submitters;
        for (uint32 i = 0; i < NUM_SUBMITTERS; ++i)
        {
            submitters.emplace_back([&]() {
                while (!stopSubmitting.load(std::memory_order_acquire))
                {
                    try
                    {
                        _threadPool->Submit([]() {
                            std::this_thread::sleep_for(std::chrono::microseconds(10));
                        });
                        successfulSubmits.fetch_add(1, std::memory_order_release);
                    }
                    catch (...)
                    {
                        // Expected: may throw if pool is deactivating
                        failedSubmits.fetch_add(1, std::memory_order_release);
                    }

                    std::this_thread::yield();
                }
            });
        }

        // Let submitters run briefly
        std::this_thread::sleep_for(100ms);

        // Deactivate while threads are still submitting
        std::cout << "  Deactivating pool while threads submit...\n";
        _threadPool->Deactivate();

        stopSubmitting.store(true, std::memory_order_release);

        // Join submitters
        for (auto& thread : submitters)
            thread.join();

        std::cout << "  Successful submits: " << successfulSubmits.load() << "\n";
        std::cout << "  Failed submits: " << failedSubmits.load() << "\n";

        EXPECT_GT(successfulSubmits.load(), 0u) << "No successful submits";

    }, 30s, "ConcurrentSubmitDuringDeactivation");

    EXPECT_TRUE(testPassed) << "Test failed or timed out (possible deadlock)";
}

// ============================================================================
// TEST 5: Pathological Work Stealing (Empty Queues)
// ============================================================================

TEST_F(DeadlockDetectionTest, PathologicalWorkStealing_MostlyEmptyQueues)
{
    std::cout << "\n=== Deadlock Test 5: Pathological Work Stealing ===\n";
    std::cout << "Testing: Heavy work-stealing with mostly empty queues\n";
    std::cout << "Purpose: Verify no deadlock when threads constantly steal\n";
    std::cout << "Timeout: 60 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr uint32 NUM_WORKERS = 32;  // More workers than tasks
        constexpr uint32 NUM_TASKS = 100;    // Few tasks = mostly empty queues

        _threadPool->Activate(NUM_WORKERS);

        std::atomic<uint32> tasksCompleted{0};

        // Submit tasks slowly to maximize work-stealing attempts
        std::thread submitter([&]() {
            for (uint32 i = 0; i < NUM_TASKS; ++i)
            {
                _threadPool->Submit([&tasksCompleted]() {
                    // Long task to keep workers busy stealing
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    tasksCompleted.fetch_add(1, std::memory_order_release);
                });

                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

        submitter.join();

        std::cout << "  Waiting for completion (workers stealing work)...\n";

        // Critical: WaitForCompletion while workers are constantly stealing
        _threadPool->WaitForCompletion();

        std::cout << "  Tasks completed: " << tasksCompleted.load() << "\n";

        EXPECT_EQ(tasksCompleted.load(), NUM_TASKS);

    }, 60s, "PathologicalWorkStealing");

    EXPECT_TRUE(testPassed) << "Test failed or timed out (possible deadlock)";
}

// ============================================================================
// TEST 6: Nested Task Submission (Tasks Submit More Tasks)
// ============================================================================

TEST_F(DeadlockDetectionTest, NestedTaskSubmission_RecursiveWorkload)
{
    std::cout << "\n=== Deadlock Test 6: Nested Task Submission ===\n";
    std::cout << "Testing: Tasks that submit more tasks (recursive workload)\n";
    std::cout << "Purpose: Verify no deadlock when tasks call Submit()\n";
    std::cout << "Timeout: 60 seconds\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr uint32 NUM_WORKERS = 8;
        constexpr uint32 INITIAL_TASKS = 10;
        constexpr uint32 DEPTH = 3;  // Each task spawns 2 more, 3 levels deep

        _threadPool->Activate(NUM_WORKERS);

        std::atomic<uint32> totalTasksCompleted{0};

        // Recursive task lambda
        std::function<void(uint32)> recursiveTask = [&](uint32 depth) {
            if (depth == 0)
            {
                totalTasksCompleted.fetch_add(1, std::memory_order_release);
                return;
            }

            // Spawn 2 more tasks
            _threadPool->Submit([&, depth]() { recursiveTask(depth - 1); });
            _threadPool->Submit([&, depth]() { recursiveTask(depth - 1); });

            totalTasksCompleted.fetch_add(1, std::memory_order_release);
        };

        // Submit initial tasks
        for (uint32 i = 0; i < INITIAL_TASKS; ++i)
        {
            _threadPool->Submit([&]() { recursiveTask(DEPTH); });
        }

        std::cout << "  Waiting for recursive task completion...\n";

        // Critical: WaitForCompletion while tasks are submitting more tasks
        _threadPool->WaitForCompletion();

        std::cout << "  Total tasks completed: " << totalTasksCompleted.load() << "\n";

        // Expected: INITIAL_TASKS * (2^0 + 2^1 + 2^2 + 2^3) = 10 * 15 = 150
        uint32 expectedTasks = INITIAL_TASKS * ((1 << (DEPTH + 1)) - 1);
        EXPECT_EQ(totalTasksCompleted.load(), expectedTasks);

    }, 60s, "NestedTaskSubmission");

    EXPECT_TRUE(testPassed) << "Test failed or timed out (possible deadlock)";
}

// ============================================================================
// TEST 7: Stress Test All Locks Simultaneously
// ============================================================================

TEST_F(DeadlockDetectionTest, StressAllLocksConcurrently_5Minutes)
{
    std::cout << "\n=== Deadlock Test 7: Stress All Locks Simultaneously ===\n";
    std::cout << "Testing: 5 minute sustained load on all locks\n";
    std::cout << "Purpose: Real-world simulation of server conditions\n";
    std::cout << "Timeout: 330 seconds (5.5 minutes)\n\n";

    bool testPassed = RunWithTimeout([this]() {
        constexpr auto TEST_DURATION = 5min;
        constexpr uint32 NUM_WORKERS = 16;

        _threadPool->Activate(NUM_WORKERS);

        std::atomic<bool> stopTest{false};
        std::atomic<uint32> totalTasksCompleted{0};

        auto startTime = std::chrono::steady_clock::now();

        // Continuous submitter threads
        std::vector<std::thread> submitters;
        for (uint32 i = 0; i < 8; ++i)
        {
            submitters.emplace_back([&, threadId = i]() {
                std::mt19937 rng(threadId);
                std::uniform_int_distribution<int> workload(100, 5000);

                while (!stopTest.load(std::memory_order_acquire))
                {
                    _threadPool->Submit([&, iterations = workload(rng)]() {
                        // Variable workload to trigger work-stealing
                        uint64 sum = 0;
                        for (int i = 0; i < iterations; ++i)
                            sum += i;
                        [[maybe_unused]] volatile uint64 dummy = sum; // Prevent optimization

                        totalTasksCompleted.fetch_add(1, std::memory_order_release);
                    });

                    // Small delay to avoid overwhelming the pool
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            });
        }

        // Monitor thread (prints status every 30 seconds)
        std::thread monitor([&]() {
            while (!stopTest.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(30s);

                auto elapsed = std::chrono::steady_clock::now() - startTime;
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

                std::cout << "  [" << seconds << "s] Tasks completed: "
                          << totalTasksCompleted.load()
                          << " | Pending: " << _threadPool->GetPendingTaskCount() << "\n";
            }
        });

        // Run for 5 minutes
        std::this_thread::sleep_for(TEST_DURATION);
        stopTest.store(true, std::memory_order_release);

        std::cout << "  Stopping submitters...\n";

        // Join submitters
        for (auto& thread : submitters)
            thread.join();

        std::cout << "  Waiting for remaining tasks...\n";

        // Final wait (critical test)
        _threadPool->WaitForCompletion();

        monitor.join();

        auto totalTime = std::chrono::steady_clock::now() - startTime;
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(totalTime).count();

        std::cout << "\n  === Final Results ===\n";
        std::cout << "  Total runtime: " << seconds << " seconds\n";
        std::cout << "  Total tasks completed: " << totalTasksCompleted.load() << "\n";
        std::cout << "  Average throughput: " << (totalTasksCompleted.load() / seconds) << " tasks/sec\n";

        EXPECT_GT(totalTasksCompleted.load(), 0u);

    }, 330s, "StressAllLocksConcurrently");

    EXPECT_TRUE(testPassed) << "Test failed or timed out (possible deadlock)";
}
