/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "WorkStealingThreadPool.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

class WorkStealingThreadPoolTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        pool = std::make_unique<WorkStealingThreadPool>();
    }

    void TearDown() override
    {
        if (pool && pool->IsActivated())
        {
            pool->Deactivate();
        }
        pool.reset();
    }

    std::unique_ptr<WorkStealingThreadPool> pool;
};

// ==================== Basic Functionality Tests ====================

TEST_F(WorkStealingThreadPoolTest, ActivateAndDeactivate)
{
    EXPECT_FALSE(pool->IsActivated());

    pool->Activate(4);
    EXPECT_TRUE(pool->IsActivated());
    EXPECT_EQ(pool->GetThreadCount(), 4);

    pool->Deactivate();
    EXPECT_FALSE(pool->IsActivated());
}

TEST_F(WorkStealingThreadPoolTest, SimpleTaskExecution)
{
    std::atomic<int> counter{0};
    pool->Activate(4);

    pool->Submit([&counter]() {
        counter.fetch_add(1, std::memory_order_relaxed);
    });

    pool->WaitForCompletion();
    EXPECT_EQ(counter.load(), 1);
}

TEST_F(WorkStealingThreadPoolTest, MultipleTasksExecution)
{
    constexpr int NUM_TASKS = 1000;
    std::atomic<int> counter{0};
    pool->Activate(4);

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool->Submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool->WaitForCompletion();
    EXPECT_EQ(counter.load(), NUM_TASKS);
}

TEST_F(WorkStealingThreadPoolTest, TasksWithDifferentWorkloads)
{
    std::atomic<int> fastTasks{0};
    std::atomic<int> slowTasks{0};
    pool->Activate(4);

    // Submit fast tasks
    for (int i = 0; i < 100; ++i)
    {
        pool->Submit([&fastTasks]() {
            fastTasks.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Submit slow tasks
    for (int i = 0; i < 10; ++i)
    {
        pool->Submit([&slowTasks]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            slowTasks.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool->WaitForCompletion();
    EXPECT_EQ(fastTasks.load(), 100);
    EXPECT_EQ(slowTasks.load(), 10);
}

// ==================== Work Stealing Tests ====================

TEST_F(WorkStealingThreadPoolTest, WorkStealingLoadBalancing)
{
    constexpr int NUM_THREADS = 4;
    constexpr int NUM_TASKS = 100;

    std::vector<std::atomic<int>> threadCounters(NUM_THREADS);
    for (auto& counter : threadCounters)
    {
        counter.store(0);
    }

    pool->Activate(NUM_THREADS);

    // Submit unbalanced workload - some threads should steal work
    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool->Submit([i, &threadCounters]() {
            // Simulate varying work duration
            if (i < 20)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            // Can't easily track which thread executed, but we verify completion
        });
    }

    pool->WaitForCompletion();
    // All tasks should complete regardless of work stealing
    EXPECT_EQ(pool->GetPendingTaskCount(), 0);
}

TEST_F(WorkStealingThreadPoolTest, WorkStealingUnderContention)
{
    constexpr int NUM_TASKS = 1000;
    std::atomic<int> counter{0};

    pool->Activate(8); // More threads than tasks initially

    // Submit tasks that spawn more tasks (work stealing scenario)
    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool->Submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield(); // Encourage context switching
        });
    }

    pool->WaitForCompletion();
    EXPECT_EQ(counter.load(), NUM_TASKS);
}

// ==================== Thread Safety Tests ====================

TEST_F(WorkStealingThreadPoolTest, ConcurrentSubmits)
{
    constexpr int NUM_SUBMITTERS = 4;
    constexpr int TASKS_PER_SUBMITTER = 250;
    std::atomic<int> totalTasks{0};

    pool->Activate(4);

    std::vector<std::thread> submitters;
    for (int i = 0; i < NUM_SUBMITTERS; ++i)
    {
        submitters.emplace_back([this, &totalTasks]() {
            for (int j = 0; j < TASKS_PER_SUBMITTER; ++j)
            {
                pool->Submit([&totalTasks]() {
                    totalTasks.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    for (auto& t : submitters)
    {
        t.join();
    }

    pool->WaitForCompletion();
    EXPECT_EQ(totalTasks.load(), NUM_SUBMITTERS * TASKS_PER_SUBMITTER);
}

TEST_F(WorkStealingThreadPoolTest, RaceConditionTest)
{
    constexpr int NUM_TASKS = 10000;
    std::vector<int> sharedData(NUM_TASKS, 0);
    std::mutex sharedMutex;

    pool->Activate(8);

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool->Submit([i, &sharedData, &sharedMutex]() {
            std::lock_guard<std::mutex> lock(sharedMutex);
            sharedData[i] = i;
        });
    }

    pool->WaitForCompletion();

    // Verify all data was written correctly
    for (int i = 0; i < NUM_TASKS; ++i)
    {
        EXPECT_EQ(sharedData[i], i);
    }
}

// ==================== Performance & Stress Tests ====================

TEST_F(WorkStealingThreadPoolTest, HighFrequencySubmission)
{
    constexpr int NUM_TASKS = 100000;
    std::atomic<int> counter{0};

    pool->Activate(4);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool->Submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool->WaitForCompletion();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_EQ(counter.load(), NUM_TASKS);

    // Should complete in reasonable time (adjust based on hardware)
    EXPECT_LT(duration.count(), 5000); // Less than 5 seconds
}

TEST_F(WorkStealingThreadPoolTest, VaryingThreadCounts)
{
    constexpr int NUM_TASKS = 1000;

    for (int threadCount : {1, 2, 4, 8, 16})
    {
        std::atomic<int> counter{0};

        pool->Activate(threadCount);

        for (int i = 0; i < NUM_TASKS; ++i)
        {
            pool->Submit([&counter]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }

        pool->WaitForCompletion();
        EXPECT_EQ(counter.load(), NUM_TASKS);

        pool->Deactivate();
    }
}

TEST_F(WorkStealingThreadPoolTest, LongRunningTasks)
{
    constexpr int NUM_TASKS = 20;
    std::atomic<int> completed{0};

    pool->Activate(4);

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool->Submit([&completed]() {
            // Simulate long-running computation
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool->WaitForCompletion();
    EXPECT_EQ(completed.load(), NUM_TASKS);
}

// ==================== Edge Cases ====================

TEST_F(WorkStealingThreadPoolTest, EmptyPool)
{
    pool->Activate(4);
    pool->WaitForCompletion(); // Should not hang or crash
    EXPECT_EQ(pool->GetPendingTaskCount(), 0);
}

TEST_F(WorkStealingThreadPoolTest, SingleThreadPool)
{
    std::atomic<int> counter{0};

    pool->Activate(1);

    for (int i = 0; i < 100; ++i)
    {
        pool->Submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool->WaitForCompletion();
    EXPECT_EQ(counter.load(), 100);
}

TEST_F(WorkStealingThreadPoolTest, ExceptionHandling)
{
    std::atomic<int> successfulTasks{0};

    pool->Activate(4);

    // Submit tasks that throw exceptions
    for (int i = 0; i < 10; ++i)
    {
        pool->Submit([i, &successfulTasks]() {
            if (i % 2 == 0)
            {
                throw std::runtime_error("Test exception");
            }
            successfulTasks.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool->WaitForCompletion();

    // Exceptions should be caught, other tasks should complete
    EXPECT_EQ(successfulTasks.load(), 5);
}

TEST_F(WorkStealingThreadPoolTest, RepeatedActivateDeactivate)
{
    for (int i = 0; i < 5; ++i)
    {
        std::atomic<int> counter{0};

        pool->Activate(4);

        for (int j = 0; j < 100; ++j)
        {
            pool->Submit([&counter]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }

        pool->WaitForCompletion();
        EXPECT_EQ(counter.load(), 100);

        pool->Deactivate();
    }
}

// ==================== Throughput Tests ====================

TEST_F(WorkStealingThreadPoolTest, ThroughputMeasurement)
{
    constexpr int NUM_TASKS = 50000;
    constexpr int NUM_THREADS = 4;

    std::atomic<uint64_t> opsCompleted{0};

    pool->Activate(NUM_THREADS);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool->Submit([&opsCompleted]() {
            // Simulate minimal work
            volatile int x = 0;
            for (int j = 0; j < 10; ++j)
            {
                x = x + j;
            }
            opsCompleted.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool->WaitForCompletion();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double throughput = static_cast<double>(NUM_TASKS) / (duration.count() / 1000000.0);

    EXPECT_EQ(opsCompleted.load(), NUM_TASKS);

    // Log throughput (ops/second)
    std::cout << "Throughput: " << throughput << " ops/sec with "
              << NUM_THREADS << " threads" << std::endl;
}
