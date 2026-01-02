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

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <atomic>
#include <thread>
#include <vector>
#include <random>
#include "GhostActorSystem.h"
#include "CellActorTestHarness.h"
#include "WorldMock.h"

using namespace GhostActor;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::_;

/**
 * Race condition and stress tests for GhostActorSystem
 *
 * These tests verify thread safety and correctness under high concurrency.
 * Run with ThreadSanitizer (-fsanitize=thread) to detect data races.
 */
class GhostActorRaceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Save original world and install mock
        _originalWorld = sWorld.release();
        _worldMock = new NiceMock<WorldMock>();
        sWorld.reset(_worldMock);

        // Set up minimal config defaults
        ON_CALL(*_worldMock, GetDataPath()).WillByDefault(ReturnRef(_emptyString));
        ON_CALL(*_worldMock, GetRealmName()).WillByDefault(ReturnRef(_emptyString));
        ON_CALL(*_worldMock, GetDefaultDbcLocale()).WillByDefault(Return(LOCALE_enUS));
        ON_CALL(*_worldMock, getRate(_)).WillByDefault(Return(1.0f));
        ON_CALL(*_worldMock, getBoolConfig(_)).WillByDefault(Return(false));
        ON_CALL(*_worldMock, getIntConfig(_)).WillByDefault(Return(0));
        ON_CALL(*_worldMock, getFloatConfig(_)).WillByDefault(Return(0.0f));

        EnsureCellActorTestScriptsInitialized();
    }

    void TearDown() override
    {
        // Restore original world
        IWorld* currentWorld = sWorld.release();
        delete currentWorld;
        _worldMock = nullptr;

        sWorld.reset(_originalWorld);
        _originalWorld = nullptr;
    }

    // Create multiple cells for testing
    std::vector<std::unique_ptr<CellActorTestHarness>> CreateCells(size_t count)
    {
        std::vector<std::unique_ptr<CellActorTestHarness>> cells;
        for (size_t i = 0; i < count; ++i)
        {
            cells.push_back(std::make_unique<CellActorTestHarness>(static_cast<uint32_t>(i)));
        }
        return cells;
    }

private:
    IWorld* _originalWorld{nullptr};
    NiceMock<WorldMock>* _worldMock{nullptr};
    std::string _emptyString;
};

// =============================================================================
// Concurrent Ghost Operations
// =============================================================================

TEST_F(GhostActorRaceTest, ConcurrentGhostCreateDestroy)
{
    // Test: Multiple threads creating and destroying ghosts for same GUID
    // Should not crash or corrupt state

    auto harness = std::make_unique<CellActorTestHarness>(0);
    CellActor* cell = harness->GetCell();

    constexpr size_t NUM_THREADS = 8;
    constexpr size_t OPS_PER_THREAD = 100;
    constexpr uint64_t BASE_GUID = 1000;

    std::atomic<bool> start{false};
    std::atomic<size_t> completedThreads{0};
    std::vector<std::thread> threads;

    // Spawn threads
    for (size_t t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&, t]()
        {
            // Wait for start signal
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            std::mt19937 rng(static_cast<unsigned>(t));

            for (size_t i = 0; i < OPS_PER_THREAD; ++i)
            {
                uint64_t guid = BASE_GUID + (rng() % 10);  // Contention on 10 GUIDs

                if (rng() % 2 == 0)
                {
                    // Create ghost
                    ActorMessage msg{};
                    msg.type = MessageType::GHOST_CREATE;
                    msg.sourceGuid = guid;
                    msg.sourceCellId = static_cast<uint32_t>(t);

                    auto snapshot = std::make_shared<GhostSnapshot>();
                    snapshot->guid = guid;
                    snapshot->health = 1000;
                    snapshot->maxHealth = 1000;
                    msg.complexPayload = snapshot;

                    cell->SendMessage(std::move(msg));
                }
                else
                {
                    // Destroy ghost
                    ActorMessage msg{};
                    msg.type = MessageType::GHOST_DESTROY;
                    msg.sourceGuid = guid;
                    cell->SendMessage(std::move(msg));
                }
            }

            completedThreads.fetch_add(1, std::memory_order_release);
        });
    }

    // Start all threads simultaneously
    start.store(true, std::memory_order_release);

    // Process messages while threads are running
    while (completedThreads.load(std::memory_order_acquire) < NUM_THREADS)
    {
        cell->Update(0);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Final processing
    cell->Update(0);

    // Join threads
    for (auto& t : threads)
        t.join();

    // Should not crash - state should be consistent
    // Final state may have 0-10 ghosts depending on last operations
    SUCCEED();  // If we get here without crash/hang, test passes
}

TEST_F(GhostActorRaceTest, MessageStormToSingleCell)
{
    // Test: Many threads sending messages to one cell
    // Verify all messages processed without loss

    auto harness = std::make_unique<CellActorTestHarness>(0);
    CellActor* cell = harness->GetCell();

    constexpr size_t NUM_THREADS = 8;
    constexpr size_t MSGS_PER_THREAD = 1000;

    std::atomic<bool> start{false};
    std::atomic<size_t> messagesSent{0};

    // Pre-create ghosts for health updates
    for (size_t i = 0; i < 100; ++i)
    {
        harness->AddGhost(1000 + i, 99);
    }

    std::vector<std::thread> threads;

    for (size_t t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&, t]()
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (size_t i = 0; i < MSGS_PER_THREAD; ++i)
            {
                uint64_t guid = 1000 + ((t * MSGS_PER_THREAD + i) % 100);

                ActorMessage msg{};
                msg.type = MessageType::HEALTH_CHANGED;
                msg.sourceGuid = guid;
                msg.intParam1 = static_cast<int32_t>(5000 + (i % 1000));
                msg.intParam2 = 10000;

                cell->SendMessage(std::move(msg));
                messagesSent.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);

    for (auto& t : threads)
        t.join();

    // Process all messages - use while loop (same pattern as HighVolumeMessageBurst)
    size_t totalProcessed = 0;
    size_t expectedTotal = NUM_THREADS * MSGS_PER_THREAD + 100;  // +100 for GHOST_CREATE messages
    while (totalProcessed < expectedTotal)
    {
        cell->Update(0);
        size_t processed = cell->GetMessagesProcessedLastTick();
        if (processed == 0)
            break;  // No more messages to process
        totalProcessed += processed;
    }

    EXPECT_EQ(totalProcessed, expectedTotal);
}

TEST_F(GhostActorRaceTest, ConcurrentGhostUpdates)
{
    // Test: Multiple threads updating same ghost's state
    // Final state should be one of the valid values (no corruption)

    auto harness = std::make_unique<CellActorTestHarness>(0);
    CellActor* cell = harness->GetCell();

    constexpr uint64_t GHOST_GUID = 1000;
    constexpr size_t NUM_THREADS = 8;
    constexpr size_t UPDATES_PER_THREAD = 500;

    harness->AddGhost(GHOST_GUID, 99);

    std::atomic<bool> start{false};
    std::vector<std::thread> threads;

    for (size_t t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&, t]()
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (size_t i = 0; i < UPDATES_PER_THREAD; ++i)
            {
                // Each thread writes distinctive health values
                uint32_t health = static_cast<uint32_t>((t + 1) * 1000 + i);

                ActorMessage msg{};
                msg.type = MessageType::HEALTH_CHANGED;
                msg.sourceGuid = GHOST_GUID;
                msg.intParam1 = static_cast<int32_t>(health);
                msg.intParam2 = 50000;  // Max health

                cell->SendMessage(std::move(msg));
            }
        });
    }

    start.store(true, std::memory_order_release);

    for (auto& t : threads)
        t.join();

    // Process all messages
    for (int batch = 0; batch < 50; ++batch)
        cell->Update(0);

    // Ghost should exist and have valid health
    GhostEntity* ghost = cell->GetGhost(GHOST_GUID);
    ASSERT_NE(ghost, nullptr);

    // Health should be in valid range (some thread's value)
    uint32_t health = ghost->GetHealth();
    EXPECT_GT(health, 0u);
    EXPECT_LE(health, 50000u);
}

// =============================================================================
// Multi-Cell Concurrent Operations
// =============================================================================

TEST_F(GhostActorRaceTest, CrossCellMessageRace)
{
    // Test: Messages flying between cells concurrently
    constexpr size_t NUM_CELLS = 4;
    constexpr size_t MSGS_PER_PAIR = 200;

    auto cells = CreateCells(NUM_CELLS);

    // Pre-create ghosts in each cell
    for (size_t c = 0; c < NUM_CELLS; ++c)
    {
        for (size_t g = 0; g < 10; ++g)
        {
            cells[c]->AddGhost(1000 + g, static_cast<uint32_t>((c + 1) % NUM_CELLS));
        }
    }

    std::atomic<bool> start{false};
    std::vector<std::thread> senders;
    std::vector<std::thread> processors;

    // Sender threads - send messages between cells
    for (size_t c = 0; c < NUM_CELLS; ++c)
    {
        senders.emplace_back([&, c]()
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (size_t i = 0; i < MSGS_PER_PAIR; ++i)
            {
                size_t targetCell = (c + 1) % NUM_CELLS;
                uint64_t guid = 1000 + (i % 10);

                ActorMessage msg{};
                msg.type = MessageType::HEALTH_CHANGED;
                msg.sourceGuid = guid;
                msg.sourceCellId = static_cast<uint32_t>(c);
                msg.intParam1 = static_cast<int32_t>(5000 + i);
                msg.intParam2 = 10000;

                cells[targetCell]->GetCell()->SendMessage(std::move(msg));
            }
        });
    }

    // Processor threads - process messages in each cell
    std::atomic<bool> stop{false};
    for (size_t c = 0; c < NUM_CELLS; ++c)
    {
        processors.emplace_back([&, c]()
        {
            while (!stop.load(std::memory_order_acquire))
            {
                cells[c]->GetCell()->Update(0);
                std::this_thread::yield();
            }
            // Final drain
            cells[c]->GetCell()->Update(0);
        });
    }

    start.store(true, std::memory_order_release);

    // Wait for senders to complete
    for (auto& t : senders)
        t.join();

    // Let processors finish
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop.store(true, std::memory_order_release);

    for (auto& t : processors)
        t.join();

    // All cells should be in consistent state
    for (size_t c = 0; c < NUM_CELLS; ++c)
    {
        // Ghosts should exist
        for (size_t g = 0; g < 10; ++g)
        {
            EXPECT_NE(cells[c]->GetCell()->GetGhost(1000 + g), nullptr)
                << "Ghost " << (1000 + g) << " missing in cell " << c;
        }
    }
}

TEST_F(GhostActorRaceTest, CellMigrationRace)
{
    // Test: Entity migration while messages in flight
    // This tests that MPSC queues handle concurrent message sends correctly
    // while ghosts are being created/destroyed
    constexpr size_t NUM_CELLS = 3;
    constexpr uint64_t MIGRATING_GUID = 1001;
    constexpr size_t MIGRATION_CYCLES = 50;

    auto cells = CreateCells(NUM_CELLS);

    // Create ghost in all cells initially
    for (size_t c = 0; c < NUM_CELLS; ++c)
    {
        cells[c]->AddGhost(MIGRATING_GUID, static_cast<uint32_t>((c + 1) % NUM_CELLS));
    }

    // Process initial ghost creation
    for (size_t c = 0; c < NUM_CELLS; ++c)
    {
        cells[c]->GetCell()->Update(0);
    }

    std::atomic<bool> start{false};
    std::atomic<size_t> sendersDone{0};

    // Message senders - multiple threads sending to cells concurrently
    std::vector<std::thread> senders;
    constexpr size_t NUM_SENDERS = 4;

    for (size_t s = 0; s < NUM_SENDERS; ++s)
    {
        senders.emplace_back([&, s]()
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (size_t i = 0; i < MIGRATION_CYCLES * 10; ++i)
            {
                size_t targetCell = (s + i) % NUM_CELLS;

                ActorMessage msg{};
                msg.type = MessageType::HEALTH_CHANGED;
                msg.sourceGuid = MIGRATING_GUID;
                msg.intParam1 = static_cast<int32_t>(5000 + i);
                msg.intParam2 = 10000;

                cells[targetCell]->GetCell()->SendMessage(std::move(msg));
            }
            sendersDone.fetch_add(1, std::memory_order_release);
        });
    }

    start.store(true, std::memory_order_release);

    // Main thread handles migration and processing
    for (size_t cycle = 0; cycle < MIGRATION_CYCLES; ++cycle)
    {
        size_t oldCell = cycle % NUM_CELLS;
        size_t newCell = (cycle + 1) % NUM_CELLS;

        // Process any pending messages
        for (size_t c = 0; c < NUM_CELLS; ++c)
        {
            cells[c]->GetCell()->Update(0);
        }

        // Migrate: destroy in old cell, create in new
        cells[oldCell]->DestroyGhost(MIGRATING_GUID);
        cells[newCell]->AddGhost(MIGRATING_GUID, static_cast<uint32_t>(oldCell));
    }

    // Wait for all senders
    for (auto& t : senders)
        t.join();

    // Final processing to drain queues
    for (int batch = 0; batch < 10; ++batch)
    {
        for (size_t c = 0; c < NUM_CELLS; ++c)
        {
            cells[c]->GetCell()->Update(0);
        }
    }

    // Should complete without crash - verifies MPSC queue handles concurrent sends
    SUCCEED();
}

// =============================================================================
// High-Volume Stress Tests
// =============================================================================

TEST_F(GhostActorRaceTest, HighVolumeMessageBurst)
{
    // Test: Burst of 100000 messages processed correctly
    auto harness = std::make_unique<CellActorTestHarness>(0);
    CellActor* cell = harness->GetCell();

    constexpr size_t MESSAGE_COUNT = 100000;
    constexpr size_t GHOST_COUNT = 100;

    // Create ghosts
    for (size_t g = 0; g < GHOST_COUNT; ++g)
    {
        harness->AddGhost(1000 + g, 99);
    }

    // Queue all messages
    auto startTime = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < MESSAGE_COUNT; ++i)
    {
        uint64_t guid = 1000 + (i % GHOST_COUNT);

        ActorMessage msg{};
        msg.type = MessageType::HEALTH_CHANGED;
        msg.sourceGuid = guid;
        msg.intParam1 = static_cast<int32_t>(5000 + (i % 1000));
        msg.intParam2 = 10000;

        cell->SendMessage(std::move(msg));
    }

    auto queueTime = std::chrono::high_resolution_clock::now();

    // Process all messages
    size_t totalProcessed = 0;
    while (totalProcessed < MESSAGE_COUNT + GHOST_COUNT)  // +GHOST_COUNT for create messages
    {
        cell->Update(0);
        totalProcessed += cell->GetMessagesProcessedLastTick();
    }

    auto processTime = std::chrono::high_resolution_clock::now();

    auto queueDuration = std::chrono::duration_cast<std::chrono::milliseconds>(queueTime - startTime);
    auto processDuration = std::chrono::duration_cast<std::chrono::milliseconds>(processTime - queueTime);

    // Log performance (for debugging, not a strict test)
    EXPECT_LT(queueDuration.count(), 5000);  // Should queue in < 5 seconds
    EXPECT_LT(processDuration.count(), 5000);  // Should process in < 5 seconds

    // All messages processed
    EXPECT_EQ(totalProcessed, MESSAGE_COUNT + GHOST_COUNT);
}

TEST_F(GhostActorRaceTest, MixedMessageTypes)
{
    // Test: Different message types processed concurrently
    auto harness = std::make_unique<CellActorTestHarness>(0);
    CellActor* cell = harness->GetCell();

    constexpr size_t GHOST_COUNT = 50;
    constexpr size_t ITERATIONS = 100;

    // Create initial ghosts
    for (size_t g = 0; g < GHOST_COUNT; ++g)
    {
        harness->AddGhost(1000 + g, 99);
    }

    std::atomic<bool> start{false};
    std::vector<std::thread> threads;

    // Thread sending HEALTH_CHANGED
    threads.emplace_back([&]()
    {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();

        for (size_t i = 0; i < ITERATIONS; ++i)
        {
            for (size_t g = 0; g < GHOST_COUNT; ++g)
            {
                ActorMessage msg{};
                msg.type = MessageType::HEALTH_CHANGED;
                msg.sourceGuid = 1000 + g;
                msg.intParam1 = static_cast<int32_t>(5000 + i);
                msg.intParam2 = 10000;
                cell->SendMessage(std::move(msg));
            }
        }
    });

    // Thread sending COMBAT_STATE_CHANGED
    threads.emplace_back([&]()
    {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();

        for (size_t i = 0; i < ITERATIONS; ++i)
        {
            for (size_t g = 0; g < GHOST_COUNT; ++g)
            {
                ActorMessage msg{};
                msg.type = MessageType::COMBAT_STATE_CHANGED;
                msg.sourceGuid = 1000 + g;
                msg.intParam1 = static_cast<int32_t>(i % 2);  // Toggle combat
                cell->SendMessage(std::move(msg));
            }
        }
    });

    // Thread sending POSITION_UPDATE
    threads.emplace_back([&]()
    {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();

        for (size_t i = 0; i < ITERATIONS; ++i)
        {
            for (size_t g = 0; g < GHOST_COUNT; ++g)
            {
                ActorMessage msg{};
                msg.type = MessageType::POSITION_UPDATE;
                msg.sourceGuid = 1000 + g;
                msg.floatParam1 = 100.0f + static_cast<float>(i);
                msg.floatParam2 = 200.0f + static_cast<float>(g);
                msg.floatParam3 = 0.0f;
                cell->SendMessage(std::move(msg));
            }
        }
    });

    // Thread sending AURA_APPLY/REMOVE
    threads.emplace_back([&]()
    {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();

        for (size_t i = 0; i < ITERATIONS; ++i)
        {
            for (size_t g = 0; g < GHOST_COUNT; ++g)
            {
                ActorMessage msg{};
                msg.type = (i % 2 == 0) ? MessageType::AURA_APPLY : MessageType::AURA_REMOVE;
                msg.sourceGuid = 1000 + g;
                msg.intParam1 = 12345;  // Aura ID
                msg.intParam2 = 1;      // Stack
                cell->SendMessage(std::move(msg));
            }
        }
    });

    start.store(true, std::memory_order_release);

    for (auto& t : threads)
        t.join();

    // Process all messages - use while loop pattern
    size_t totalProcessed = 0;
    size_t expectedMessages = GHOST_COUNT + (4 * ITERATIONS * GHOST_COUNT);
    while (totalProcessed < expectedMessages)
    {
        cell->Update(0);
        size_t processed = cell->GetMessagesProcessedLastTick();
        if (processed == 0)
            break;
        totalProcessed += processed;
    }

    EXPECT_EQ(totalProcessed, expectedMessages);

    // All ghosts should still exist
    for (size_t g = 0; g < GHOST_COUNT; ++g)
    {
        EXPECT_NE(cell->GetGhost(1000 + g), nullptr);
    }
}

// =============================================================================
// Specific Race Condition Regression Tests
// =============================================================================

TEST_F(GhostActorRaceTest, GhostDestroyDuringUpdate)
{
    // Test: Messages sent while ghost is being created/destroyed
    // Verifies MPSC queue handles concurrent sends while processing lifecycle changes
    auto harness = std::make_unique<CellActorTestHarness>(0);
    CellActor* cell = harness->GetCell();

    constexpr uint64_t GHOST_GUID = 1000;
    constexpr size_t ITERATIONS = 100;

    std::atomic<bool> start{false};
    std::atomic<size_t> sendersDone{0};

    // Multiple sender threads sending updates concurrently
    std::vector<std::thread> senders;
    constexpr size_t NUM_SENDERS = 4;

    for (size_t s = 0; s < NUM_SENDERS; ++s)
    {
        senders.emplace_back([&, s]()
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (size_t i = 0; i < ITERATIONS * 10; ++i)
            {
                ActorMessage msg{};
                msg.type = MessageType::HEALTH_CHANGED;
                msg.sourceGuid = GHOST_GUID;
                msg.intParam1 = static_cast<int32_t>(5000 + i);
                msg.intParam2 = 10000;

                cell->SendMessage(std::move(msg));
            }
            sendersDone.fetch_add(1, std::memory_order_release);
        });
    }

    start.store(true, std::memory_order_release);

    // Main thread handles lifecycle and processing
    for (size_t i = 0; i < ITERATIONS; ++i)
    {
        // Create ghost
        harness->AddGhost(GHOST_GUID, 99);

        // Process some messages
        cell->Update(0);

        // Destroy ghost
        harness->DestroyGhost(GHOST_GUID);

        // Process more messages (some may be for non-existent ghost)
        cell->Update(0);
    }

    // Wait for senders
    for (auto& t : senders)
        t.join();

    // Final drain
    for (int batch = 0; batch < 10; ++batch)
        cell->Update(0);

    // Should complete without crash - ghost may or may not exist at end
    SUCCEED();
}
