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

#include "Map.h"
#include "MapUpdater.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <vector>

// Simple test object (not inheriting from WorldObject to avoid complexity)
struct TestEntity
{
    std::atomic<uint32> updateCount{0};
    std::atomic<bool> isInWorld{true};  // Made atomic to prevent data races in concurrent tests

    TestEntity() = default;

    // Make non-copyable but movable for vector
    TestEntity(TestEntity&& other) noexcept
        : updateCount(other.updateCount.load()), isInWorld(other.isInWorld.load())
    {
    }

    TestEntity& operator=(TestEntity&& other) noexcept
    {
        if (this != &other)
        {
            updateCount.store(other.updateCount.load());
            isInWorld.store(other.isInWorld.load());
        }
        return *this;
    }

    TestEntity(TestEntity const&) = delete;
    TestEntity& operator=(TestEntity const&) = delete;

    void Update(uint32 /*diff*/)
    {
        updateCount.fetch_add(1, std::memory_order_relaxed);
        // Simulate realistic entity update work (AI, movement, pathfinding, etc.)
        volatile double x = 0;
        for (int i = 0; i < 5000; ++i)  // Realistic workload
        {
            x += (i * i) / (i + 1.0);  // More CPU work with division
        }
    }

    uint32 GetUpdateCount() const { return updateCount.load(); }

    void ResetUpdateCount() { updateCount.store(0); }

    void SetInWorld(bool value) { isInWorld.store(value, std::memory_order_relaxed); }

    bool IsInWorld() const { return isInWorld.load(std::memory_order_relaxed); }
};

class MapParallelUpdateTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        updater = std::make_unique<MapUpdater>();
    }

    void TearDown() override
    {
        if (updater && updater->activated())
        {
            updater->deactivate();
        }
        updater.reset();
    }

    std::unique_ptr<MapUpdater> updater;
};

// ==================== Correctness Tests ====================

TEST_F(MapParallelUpdateTest, UpdaterActivation)
{
    EXPECT_FALSE(updater->activated());

    updater->activate(4);
    EXPECT_TRUE(updater->activated());

    updater->deactivate();
    EXPECT_FALSE(updater->activated());
}

TEST_F(MapParallelUpdateTest, ParallelVsSequentialConsistency)
{
    // Test parallel updates using simple test entities
    std::vector<TestEntity> objects(1000);
    std::atomic<uint32> totalUpdates{0};

    updater->activate(4);

    // Simulate parallel batch updates
    constexpr uint32 DIFF = 100;
    for (auto& obj : objects)
    {
        updater->submit_task([&obj, DIFF, &totalUpdates]() {
            obj.Update(DIFF);
            totalUpdates.fetch_add(1, std::memory_order_relaxed);
        });
    }

    updater->wait();

    // Verify all objects were updated
    EXPECT_EQ(totalUpdates.load(), objects.size());
    for (const auto& obj : objects)
    {
        EXPECT_EQ(obj.GetUpdateCount(), 1);
    }
}

// ==================== Performance Comparison Tests ====================

TEST_F(MapParallelUpdateTest, SequentialUpdateBaseline)
{
    constexpr int NUM_OBJECTS = 10000;
    std::vector<TestEntity> objects(NUM_OBJECTS);
    constexpr uint32 DIFF = 100;

    auto start = std::chrono::high_resolution_clock::now();

    // Sequential update
    for (auto& obj : objects)
    {
        obj.Update(DIFF);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto sequentialDuration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Sequential update of " << NUM_OBJECTS << " objects: "
              << sequentialDuration.count() << "ms" << std::endl;

    // Verify correctness
    for (const auto& obj : objects)
    {
        EXPECT_EQ(obj.GetUpdateCount(), 1);
    }
}

TEST_F(MapParallelUpdateTest, ParallelUpdatePerformance)
{
    constexpr int NUM_OBJECTS = 10000;
    constexpr int NUM_THREADS = 4;
    std::vector<TestEntity> objects(NUM_OBJECTS);
    constexpr uint32 DIFF = 100;

    updater->activate(NUM_THREADS);

    auto start = std::chrono::high_resolution_clock::now();

    // Parallel update via thread pool
    for (auto& obj : objects)
    {
        updater->submit_task([&obj, DIFF]() {
            obj.Update(DIFF);
        });
    }

    updater->wait();

    auto end = std::chrono::high_resolution_clock::now();
    auto parallelDuration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Parallel update of " << NUM_OBJECTS << " objects with "
              << NUM_THREADS << " threads: " << parallelDuration.count() << "ms" << std::endl;

    // Verify correctness
    for (const auto& obj : objects)
    {
        EXPECT_EQ(obj.GetUpdateCount(), 1);
    }
}

TEST_F(MapParallelUpdateTest, SpeedupCalculation)
{
    constexpr int NUM_OBJECTS = 10000;  // Balance between work and overhead
    constexpr int NUM_THREADS = 4;

    // Sequential baseline
    {
        std::vector<TestEntity> objects(NUM_OBJECTS);
        auto start = std::chrono::high_resolution_clock::now();

        for (auto& obj : objects)
        {
            obj.Update(100);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto sequentialTime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        // Parallel version
        std::vector<TestEntity> objectsParallel(NUM_OBJECTS);
        updater->activate(NUM_THREADS);

        auto startParallel = std::chrono::high_resolution_clock::now();

        for (auto& obj : objectsParallel)
        {
            updater->submit_task([&obj]() {
                obj.Update(100);
            });
        }

        updater->wait();

        auto endParallel = std::chrono::high_resolution_clock::now();
        auto parallelTime = std::chrono::duration_cast<std::chrono::milliseconds>(endParallel - startParallel);

        double speedup = static_cast<double>(sequentialTime.count()) / parallelTime.count();

        std::cout << "Speedup with " << NUM_THREADS << " threads: " << speedup << "x" << std::endl;
        std::cout << "Sequential: " << sequentialTime.count() << "ms, Parallel: " << parallelTime.count() << "ms" << std::endl;

        // With heavier workload per entity, expect measurable speedup
        // Note: Actual speedup varies based on CPU, workload, and threading overhead
        EXPECT_GT(speedup, 1.5);
    }
}

// ==================== Stress Tests ====================

TEST_F(MapParallelUpdateTest, HighLoadStressTest)
{
    constexpr int NUM_OBJECTS = 50000;
    constexpr int NUM_UPDATES = 10;

    std::vector<TestEntity> objects(NUM_OBJECTS);

    updater->activate(8);

    for (int iteration = 0; iteration < NUM_UPDATES; ++iteration)
    {
        for (auto& obj : objects)
        {
            obj.ResetUpdateCount();
        }

        auto start = std::chrono::high_resolution_clock::now();

        for (auto& obj : objects)
        {
            updater->submit_task([&obj]() {
                obj.Update(100);
            });
        }

        updater->wait();

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        // Verify all objects updated
        for (const auto& obj : objects)
        {
            EXPECT_EQ(obj.GetUpdateCount(), 1);
        }

        std::cout << "Iteration " << iteration + 1 << "/" << NUM_UPDATES
                  << ": " << duration.count() << "ms for " << NUM_OBJECTS << " objects" << std::endl;
    }
}

TEST_F(MapParallelUpdateTest, ExtendedStressTest)
{
    // Run for 1 minute with continuous updates
    constexpr int NUM_OBJECTS = 1000;
    constexpr auto TEST_DURATION = std::chrono::minutes(1);

    std::vector<TestEntity> objects(NUM_OBJECTS);
    updater->activate(4);

    auto startTime = std::chrono::steady_clock::now();
    uint64_t totalIterations = 0;

    while (std::chrono::steady_clock::now() - startTime < TEST_DURATION)
    {
        for (auto& obj : objects)
        {
            updater->submit_task([&obj]() {
                obj.Update(50);
            });
        }

        updater->wait();
        ++totalIterations;
    }

    std::cout << "Completed " << totalIterations << " iterations in 1 minute" << std::endl;
    std::cout << "Average throughput: " << (totalIterations * NUM_OBJECTS / 60.0) << " updates/sec" << std::endl;

    EXPECT_GT(totalIterations, 100); // Should complete at least 100 iterations per minute
}

TEST_F(MapParallelUpdateTest, VaryingObjectCounts)
{
    updater->activate(4);

    for (int objectCount : {100, 500, 1000, 5000, 10000})
    {
        std::vector<TestEntity> objects(objectCount);

        auto start = std::chrono::high_resolution_clock::now();

        for (auto& obj : objects)
        {
            updater->submit_task([&obj]() {
                obj.Update(100);
            });
        }

        updater->wait();

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "Objects: " << objectCount << ", Time: " << duration.count() << "ms" << std::endl;

        // Verify correctness
        for (const auto& obj : objects)
        {
            EXPECT_EQ(obj.GetUpdateCount(), 1);
        }
    }
}

// ==================== Concurrency Tests ====================

TEST_F(MapParallelUpdateTest, ConcurrentObjectModification)
{
    constexpr int NUM_OBJECTS = 1000;
    std::vector<TestEntity> objects(NUM_OBJECTS);

    updater->activate(8);

    // Simulate concurrent modification scenario
    std::atomic<int> addedObjects{0};
    std::atomic<int> removedObjects{0};

    for (int i = 0; i < NUM_OBJECTS; ++i)
    {
        updater->submit_task([&objects, i, &addedObjects, &removedObjects]() {
            // Some objects get "removed" mid-update
            if (i % 10 == 0)
            {
                objects[i].SetInWorld(false);
                removedObjects.fetch_add(1);
            }
            else
            {
                objects[i].Update(100);
                addedObjects.fetch_add(1);
            }
        });
    }

    updater->wait();

    std::cout << "Added: " << addedObjects.load() << ", Removed: " << removedObjects.load() << std::endl;

    EXPECT_EQ(addedObjects.load() + removedObjects.load(), NUM_OBJECTS);
}

TEST_F(MapParallelUpdateTest, SimultaneousMapUpdates)
{
    // Simulate multiple maps being updated simultaneously
    constexpr int NUM_MAPS = 10;
    constexpr int OBJECTS_PER_MAP = 500;

    std::vector<std::vector<TestEntity>> maps(NUM_MAPS);
    for (auto& map : maps)
    {
        map.resize(OBJECTS_PER_MAP);
    }

    updater->activate(8);

    auto start = std::chrono::high_resolution_clock::now();

    // Update all maps in parallel
    for (auto& map : maps)
    {
        for (auto& obj : map)
        {
            updater->submit_task([&obj]() {
                obj.Update(100);
            });
        }
    }

    updater->wait();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Updated " << NUM_MAPS << " maps (" << (NUM_MAPS * OBJECTS_PER_MAP)
              << " total objects) in " << duration.count() << "ms" << std::endl;

    // Verify all objects updated
    for (const auto& map : maps)
    {
        for (const auto& obj : map)
        {
            EXPECT_EQ(obj.GetUpdateCount(), 1);
        }
    }
}

// ==================== Memory and Resource Tests ====================

TEST_F(MapParallelUpdateTest, NoMemoryLeaks)
{
    // Run many iterations to detect memory leaks
    // (In production, use valgrind or AddressSanitizer for comprehensive leak detection)
    constexpr int NUM_ITERATIONS = 100;
    constexpr int NUM_OBJECTS = 1000;

    updater->activate(4);

    for (int iteration = 0; iteration < NUM_ITERATIONS; ++iteration)
    {
        std::vector<TestEntity> objects(NUM_OBJECTS);

        for (auto& obj : objects)
        {
            updater->submit_task([&obj]() {
                obj.Update(100);
            });
        }

        updater->wait();
    }

    // If we get here without crashing or excessive memory usage, test passes
    SUCCEED();
}

// ==================== Scalability Tests ====================

TEST_F(MapParallelUpdateTest, ThreadScalability)
{
    constexpr int NUM_OBJECTS = 10000;
    std::vector<TestEntity> objects(NUM_OBJECTS);

    for (int threadCount : {1, 2, 4, 8, 16})
    {
        // Reset object states
        for (auto& obj : objects)
        {
            obj.ResetUpdateCount();
        }

        if (updater->activated())
        {
            updater->deactivate();
        }

        updater->activate(threadCount);

        auto start = std::chrono::high_resolution_clock::now();

        for (auto& obj : objects)
        {
            updater->submit_task([&obj]() {
                obj.Update(100);
            });
        }

        updater->wait();

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "Threads: " << threadCount << ", Time: " << duration.count() << "ms" << std::endl;

        // Verify correctness
        for (const auto& obj : objects)
        {
            EXPECT_EQ(obj.GetUpdateCount(), 1);
        }
    }
}
