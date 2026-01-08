/*
 * Standalone reproducer for WorkStealingPool shutdown hang
 *
 * This test demonstrates the race condition where:
 * 1. Shutdown sets _shutdown = true
 * 2. Workers see flag and exit before processing all tasks
 * 3. Wait() spins forever on _pendingTasks > 0
 */

#include "Define.h"
#include "Errors.h"
#include "WorkStealingPool.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

class ShutdownHangTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// This test intentionally triggers the hang condition
// Run with: timeout 5 ./unit_tests --gtest_filter="*ReproduceShutdownHang*"
TEST_F(ShutdownHangTest, ReproduceShutdownHang)
{
    std::atomic<int> completed{0};
    std::atomic<int> started{0};

    std::cerr << "[DEBUG] Creating pool with 2 workers\n";

    {
        WorkStealingPool pool(2);

        // Give workers time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        std::cerr << "[DEBUG] Submitting 100 tasks\n";

        // Submit many tasks
        for (int i = 0; i < 100; ++i)
        {
            pool.Submit(TaskType::MAP, [&started, &completed, i]() {
                started.fetch_add(1);
                // Simulate some work
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                completed.fetch_add(1);
            });
        }

        std::cerr << "[DEBUG] Tasks submitted, started=" << started.load()
                  << " completed=" << completed.load() << "\n";

        // DON'T call Wait() - let destructor handle it
        // This is where the hang occurs
        std::cerr << "[DEBUG] Exiting scope (destructor will call Shutdown)\n";
    }

    std::cerr << "[DEBUG] After pool destruction: started=" << started.load()
              << " completed=" << completed.load() << "\n";

    // If we get here without timeout, the test passed
    // But with the bug, we'll hang in the destructor
    EXPECT_EQ(completed.load(), 100);
}

// This test shows the workaround
TEST_F(ShutdownHangTest, WorkaroundWithExplicitWait)
{
    std::atomic<int> completed{0};

    {
        WorkStealingPool pool(2);

        for (int i = 0; i < 100; ++i)
        {
            pool.Submit(TaskType::MAP, [&completed]() {
                completed.fetch_add(1);
            });
        }

        // Explicit Wait BEFORE destructor
        pool.Wait(TaskType::MAP);
    }

    EXPECT_EQ(completed.load(), 100);
}
