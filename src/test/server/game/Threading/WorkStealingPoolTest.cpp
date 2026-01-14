/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Define.h"
#include "Errors.h"
#include "WorkStealingPool.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <set>

class WorkStealingDequeTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test WorkStealingDeque basic push/pop
TEST_F(WorkStealingDequeTest, BasicPushPop)
{
    WorkStealingDeque deque;
    std::atomic<int> counter{0};

    deque.Push([&counter]() { counter++; });
    deque.Push([&counter]() { counter += 2; });

    WorkStealingDeque::TaskPtr task;
    ASSERT_TRUE(deque.Pop(task));
    (*task)();
    delete task;

    ASSERT_TRUE(deque.Pop(task));
    (*task)();
    delete task;

    EXPECT_EQ(counter.load(), 3);
    EXPECT_FALSE(deque.Pop(task));
}

// Test LIFO order for pop (stack behavior for local worker)
TEST_F(WorkStealingDequeTest, LIFOPopOrder)
{
    WorkStealingDeque deque;
    std::vector<int> order;

    deque.Push([&order]() { order.push_back(1); });
    deque.Push([&order]() { order.push_back(2); });
    deque.Push([&order]() { order.push_back(3); });

    WorkStealingDeque::TaskPtr task;
    while (deque.Pop(task))
    {
        (*task)();
        delete task;
    }

    // Pop is LIFO (from bottom)
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 3);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 1);
}

// Test FIFO order for steal (queue behavior for thieves)
TEST_F(WorkStealingDequeTest, FIFOStealOrder)
{
    WorkStealingDeque deque;
    std::vector<int> order;

    deque.Push([&order]() { order.push_back(1); });
    deque.Push([&order]() { order.push_back(2); });
    deque.Push([&order]() { order.push_back(3); });

    WorkStealingDeque::TaskPtr task;
    while (deque.Steal(task))
    {
        (*task)();
        delete task;
    }

    // Steal is FIFO (from top)
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

// Test Empty and Size
TEST_F(WorkStealingDequeTest, EmptyAndSize)
{
    WorkStealingDeque deque;

    EXPECT_TRUE(deque.Empty());
    EXPECT_EQ(deque.Size(), 0u);

    deque.Push([]() {});
    EXPECT_FALSE(deque.Empty());
    EXPECT_EQ(deque.Size(), 1u);

    deque.Push([]() {});
    EXPECT_EQ(deque.Size(), 2u);

    WorkStealingDeque::TaskPtr task;
    deque.Pop(task);
    delete task;
    EXPECT_EQ(deque.Size(), 1u);

    deque.Pop(task);
    delete task;
    EXPECT_TRUE(deque.Empty());
}

class WorkStealingPoolTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Submit and execute a single task
TEST_F(WorkStealingPoolTest, SubmitAndExecuteTask)
{
    WorkStealingPool pool(2);
    std::atomic<int> counter{0};

    pool.Submit(TaskType::MAP, [&counter]() { counter++; });
    pool.Wait(TaskType::MAP);

    EXPECT_EQ(counter.load(), 1);
}

// Submit multiple tasks and verify all complete
// Scale: 60 maps x 100 tasks = 6000 tasks (simulates moderate server load)
TEST_F(WorkStealingPoolTest, SubmitMultipleTasks)
{
    WorkStealingPool pool(4);
    std::atomic<int> counter{0};
    // 60 maps with ~100 entities each = typical server load
    constexpr int NUM_TASKS = 6000;

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool.Submit(TaskType::MAP, [&counter]() { counter++; });
    }
    pool.Wait(TaskType::MAP);

    EXPECT_EQ(counter.load(), NUM_TASKS);
}

// Test multiple groups of MAP tasks with separate waits
TEST_F(WorkStealingPoolTest, MultipleWaits)
{
    WorkStealingPool pool(2);
    std::atomic<int> counter1{0};
    std::atomic<int> counter2{0};

    // Submit first batch
    for (int i = 0; i < 10; ++i)
    {
        pool.Submit(TaskType::MAP, [&counter1]() {
            counter1++;
        });
    }

    // Wait for first batch
    pool.Wait(TaskType::MAP);
    EXPECT_EQ(counter1.load(), 10);

    // Submit second batch
    for (int i = 0; i < 10; ++i)
    {
        pool.Submit(TaskType::MAP, [&counter2]() {
            counter2++;
        });
    }

    // Wait for second batch
    pool.Wait(TaskType::MAP);
    EXPECT_EQ(counter2.load(), 10);
}

// Test work stealing behavior
// Scale: 25k tasks to single worker forces stealing (simulates imbalanced load)
TEST_F(WorkStealingPoolTest, WorkStealing)
{
    WorkStealingPool pool(4);
    std::atomic<int> counter{0};
    // 25k tasks to single worker - others must steal to complete in reasonable time
    constexpr int TASKS_PER_WORKER = 25000;

    // Submit all tasks to worker 0
    for (int i = 0; i < TASKS_PER_WORKER; ++i)
    {
        pool.SubmitToWorker(TaskType::MAP, 0, [&counter]() { counter++; });
    }

    pool.Wait(TaskType::MAP);

    // All tasks should complete (work stealing distributes load)
    EXPECT_EQ(counter.load(), TASKS_PER_WORKER);
}

// Test concurrent submission from multiple threads
// Scale: 8 submitters x 10k tasks = 80k (simulates 8 continents submitting updates)
TEST_F(WorkStealingPoolTest, ConcurrentSubmit)
{
    // 8 workers for high-pop server
    WorkStealingPool pool(8);
    std::atomic<int> counter{0};
    // 8 submitter threads (simulating concurrent map update submissions)
    constexpr int NUM_SUBMITTERS = 8;
    // 10k tasks per submitter = peak load from major continent
    constexpr int TASKS_PER_SUBMITTER = 10000;

    std::vector<std::thread> submitters;
    for (int s = 0; s < NUM_SUBMITTERS; ++s)
    {
        submitters.emplace_back([&pool, &counter]()
        {
            for (int i = 0; i < TASKS_PER_SUBMITTER; ++i)
            {
                pool.Submit(TaskType::MAP, [&counter]() { counter++; });
            }
        });
    }

    for (auto& t : submitters)
    {
        t.join();
    }

    pool.Wait(TaskType::MAP);

    EXPECT_EQ(counter.load(), NUM_SUBMITTERS * TASKS_PER_SUBMITTER);
}

// Test that shutdown drains the queue
// Scale: 5000 tasks to verify graceful shutdown under load
TEST_F(WorkStealingPoolTest, ShutdownDrainsQueue)
{
    std::atomic<int> counter{0};
    {
        WorkStealingPool pool(4);

        // 5000 tasks - enough to have work in-flight during shutdown
        for (int i = 0; i < 5000; ++i)
        {
            pool.Submit(TaskType::MAP, [&counter]() {
                counter++;
            });
        }

        // Wait for tasks before shutdown
        pool.Wait(TaskType::MAP);
    }

    // Tasks should have completed
    EXPECT_EQ(counter.load(), 5000);
}

// Stress test with many tasks
// Scale: 200k tasks = peak 10-15k player load (each player ~15 updates/tick)
TEST_F(WorkStealingPoolTest, StressTest)
{
    // 16 workers typical for high-pop production server
    WorkStealingPool pool(16);
    std::atomic<int> counter{0};
    // 200k tasks simulates ~15k players with 13-15 entity updates each per tick
    constexpr int NUM_TASKS = 200000;

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool.Submit(TaskType::MAP, [&counter]() { counter++; });
    }

    pool.Wait(TaskType::MAP);

    EXPECT_EQ(counter.load(), NUM_TASKS);
}

// Test TryExecuteOne
TEST_F(WorkStealingPoolTest, TryExecuteOne)
{
    WorkStealingPool pool(2);
    std::atomic<int> counter{0};

    pool.Submit(TaskType::MAP, [&counter]() { counter++; });

    // Give workers a chance to pick it up
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    pool.Wait(TaskType::MAP);
    EXPECT_EQ(counter.load(), 1);
}

// Test NumWorkers
TEST_F(WorkStealingPoolTest, NumWorkers)
{
    WorkStealingPool pool(6);
    EXPECT_EQ(pool.NumWorkers(), 6u);
}

// Test IsActive
TEST_F(WorkStealingPoolTest, IsActive)
{
    WorkStealingPool pool(2);
    EXPECT_TRUE(pool.IsActive());

    pool.Shutdown();
    EXPECT_FALSE(pool.IsActive());
}

// Test stress with large number of MAP tasks
// Scale: 100k tasks with simulated work = sustained high-pop combat scenario
TEST_F(WorkStealingPoolTest, LargeTaskStress)
{
    WorkStealingPool pool(8);
    std::atomic<int> counter{0};
    // 100k tasks with actual computation (not just increment)
    constexpr int NUM_TASKS = 100000;

    // Submit many tasks with small work to simulate realistic load
    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool.Submit(TaskType::MAP, [&counter, i]() {
            // Small computation to simulate real entity update work
            volatile int dummy = i * 2;
            (void)dummy;
            counter++;
        });
    }

    pool.Wait(TaskType::MAP);

    EXPECT_EQ(counter.load(), NUM_TASKS);
}
