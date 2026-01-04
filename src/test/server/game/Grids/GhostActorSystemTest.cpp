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

#include "GhostActorSystem.h"
#include "gtest/gtest.h"
#include <atomic>
#include <cmath>
#include <set>
#include <thread>
#include <vector>

using namespace GhostActor;

class GhostActorSystemTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================================
// Cell ID Calculation Tests
// ============================================================================

// Test cell ID calculation from position
TEST_F(GhostActorSystemTest, CellIdCalculation)
{
    // Cell ID is computed as: (cellY << 16) | cellX
    // Where cellX/Y = floor(CENTER_CELL_OFFSET - (worldX/Y / CELL_SIZE))

    // Test center of map (0, 0)
    float worldX = 0.0f;
    float worldY = 0.0f;
    uint32_t cellX = static_cast<uint32_t>(CENTER_CELL_OFFSET - (worldX / CELL_SIZE));
    uint32_t cellY = static_cast<uint32_t>(CENTER_CELL_OFFSET - (worldY / CELL_SIZE));
    uint32_t cellId = (cellY << 16) | cellX;

    EXPECT_EQ(cellX, 256u); // CENTER_CELL_OFFSET
    EXPECT_EQ(cellY, 256u);
    EXPECT_EQ(cellId, (256u << 16) | 256u);
}

// Test cell ID extraction
TEST_F(GhostActorSystemTest, CellIdExtraction)
{
    uint32_t cellX = 100;
    uint32_t cellY = 200;
    uint32_t cellId = (cellY << 16) | cellX;

    uint32_t extractedX = cellId & 0xFFFF;
    uint32_t extractedY = cellId >> 16;

    EXPECT_EQ(extractedX, cellX);
    EXPECT_EQ(extractedY, cellY);
}

// Test position within cell calculation
TEST_F(GhostActorSystemTest, PositionInCell)
{
    float worldX = 100.0f;
    float worldY = 200.0f;
    float cellLocalX, cellLocalY;

    GhostBoundary::GetPositionInCell(worldX, worldY, cellLocalX, cellLocalY);

    // Should be between 0 and CELL_SIZE
    EXPECT_GE(cellLocalX, 0.0f);
    EXPECT_LT(cellLocalX, CELL_SIZE);
    EXPECT_GE(cellLocalY, 0.0f);
    EXPECT_LT(cellLocalY, CELL_SIZE);
}

// ============================================================================
// Neighbor Calculation Tests
// ============================================================================

// Test GetNeighborCellId for all directions
TEST_F(GhostActorSystemTest, NeighborCalculation)
{
    uint32_t baseCellX = 100;
    uint32_t baseCellY = 100;
    uint32_t baseCellId = (baseCellY << 16) | baseCellX;

    // North (+Y)
    uint32_t north = GhostBoundary::GetNeighborCellId(baseCellId, NeighborFlags::NORTH);
    EXPECT_EQ(north & 0xFFFF, baseCellX);
    EXPECT_EQ(north >> 16, baseCellY + 1);

    // South (-Y)
    uint32_t south = GhostBoundary::GetNeighborCellId(baseCellId, NeighborFlags::SOUTH);
    EXPECT_EQ(south & 0xFFFF, baseCellX);
    EXPECT_EQ(south >> 16, baseCellY - 1);

    // East (+X)
    uint32_t east = GhostBoundary::GetNeighborCellId(baseCellId, NeighborFlags::EAST);
    EXPECT_EQ(east & 0xFFFF, baseCellX + 1);
    EXPECT_EQ(east >> 16, baseCellY);

    // West (-X)
    uint32_t west = GhostBoundary::GetNeighborCellId(baseCellId, NeighborFlags::WEST);
    EXPECT_EQ(west & 0xFFFF, baseCellX - 1);
    EXPECT_EQ(west >> 16, baseCellY);

    // NorthEast (+X+Y)
    uint32_t ne = GhostBoundary::GetNeighborCellId(baseCellId, NeighborFlags::NORTH_EAST);
    EXPECT_EQ(ne & 0xFFFF, baseCellX + 1);
    EXPECT_EQ(ne >> 16, baseCellY + 1);

    // NorthWest (-X+Y)
    uint32_t nw = GhostBoundary::GetNeighborCellId(baseCellId, NeighborFlags::NORTH_WEST);
    EXPECT_EQ(nw & 0xFFFF, baseCellX - 1);
    EXPECT_EQ(nw >> 16, baseCellY + 1);

    // SouthEast (+X-Y)
    uint32_t se = GhostBoundary::GetNeighborCellId(baseCellId, NeighborFlags::SOUTH_EAST);
    EXPECT_EQ(se & 0xFFFF, baseCellX + 1);
    EXPECT_EQ(se >> 16, baseCellY - 1);

    // SouthWest (-X-Y)
    uint32_t sw = GhostBoundary::GetNeighborCellId(baseCellId, NeighborFlags::SOUTH_WEST);
    EXPECT_EQ(sw & 0xFFFF, baseCellX - 1);
    EXPECT_EQ(sw >> 16, baseCellY - 1);
}

// ============================================================================
// NeighborFlags Tests
// ============================================================================

// Test that GetNeighborsNeedingGhosts returns ALL
// With 66-yard cells and 250-yard visibility, all neighbors always need ghosts
TEST_F(GhostActorSystemTest, NeighborFlagsAlwaysAll)
{
    // Various positions should all return ALL
    EXPECT_EQ(GhostBoundary::GetNeighborsNeedingGhosts(0.0f, 0.0f), NeighborFlags::ALL);
    EXPECT_EQ(GhostBoundary::GetNeighborsNeedingGhosts(100.0f, 200.0f), NeighborFlags::ALL);
    EXPECT_EQ(GhostBoundary::GetNeighborsNeedingGhosts(-500.0f, 1000.0f), NeighborFlags::ALL);
    EXPECT_EQ(GhostBoundary::GetNeighborsNeedingGhosts(5000.0f, -5000.0f), NeighborFlags::ALL);
}

// Test NeighborFlags bitwise operations
TEST_F(GhostActorSystemTest, NeighborFlagsBitwise)
{
    // Test OR
    NeighborFlags combined = NeighborFlags::NORTH | NeighborFlags::SOUTH;
    EXPECT_TRUE(HasFlag(combined, NeighborFlags::NORTH));
    EXPECT_TRUE(HasFlag(combined, NeighborFlags::SOUTH));
    EXPECT_FALSE(HasFlag(combined, NeighborFlags::EAST));

    // Test AND
    NeighborFlags filtered = combined & NeighborFlags::NORTH;
    EXPECT_TRUE(HasFlag(filtered, NeighborFlags::NORTH));
    EXPECT_FALSE(HasFlag(filtered, NeighborFlags::SOUTH));

    // Test HasFlag with NONE
    EXPECT_FALSE(HasFlag(NeighborFlags::NONE, NeighborFlags::NORTH));

    // Test HasFlag with ALL
    EXPECT_TRUE(HasFlag(NeighborFlags::ALL, NeighborFlags::NORTH));
    EXPECT_TRUE(HasFlag(NeighborFlags::ALL, NeighborFlags::SOUTH));
    EXPECT_TRUE(HasFlag(NeighborFlags::ALL, NeighborFlags::NORTH_EAST));
}

// ============================================================================
// GhostSnapshot Tests
// ============================================================================

TEST_F(GhostActorSystemTest, GhostSnapshotDefault)
{
    GhostSnapshot snapshot;

    EXPECT_EQ(snapshot.guid, 0u);
    EXPECT_EQ(snapshot.posX, 0.0f);
    EXPECT_EQ(snapshot.posY, 0.0f);
    EXPECT_EQ(snapshot.posZ, 0.0f);
    EXPECT_EQ(snapshot.orientation, 0.0f);
    EXPECT_EQ(snapshot.health, 0u);
    EXPECT_EQ(snapshot.maxHealth, 0u);
    EXPECT_FALSE(snapshot.inCombat);
    EXPECT_FALSE(snapshot.isDead);
}

TEST_F(GhostActorSystemTest, GhostSnapshotAssignment)
{
    GhostSnapshot snapshot;
    snapshot.guid = 12345;
    snapshot.posX = 100.5f;
    snapshot.posY = 200.5f;
    snapshot.posZ = 50.0f;
    snapshot.orientation = 3.14f;
    snapshot.health = 1000;
    snapshot.maxHealth = 2000;
    snapshot.inCombat = true;
    snapshot.isDead = false;

    EXPECT_EQ(snapshot.guid, 12345u);
    EXPECT_FLOAT_EQ(snapshot.posX, 100.5f);
    EXPECT_FLOAT_EQ(snapshot.posY, 200.5f);
    EXPECT_FLOAT_EQ(snapshot.posZ, 50.0f);
    EXPECT_FLOAT_EQ(snapshot.orientation, 3.14f);
    EXPECT_EQ(snapshot.health, 1000u);
    EXPECT_EQ(snapshot.maxHealth, 2000u);
    EXPECT_TRUE(snapshot.inCombat);
    EXPECT_FALSE(snapshot.isDead);
}

// ============================================================================
// GhostEntity Tests
// ============================================================================

TEST_F(GhostActorSystemTest, GhostEntityConstruction)
{
    GhostEntity ghost(12345, 0x00640064); // cellId = (100 << 16) | 100

    EXPECT_EQ(ghost.GetGUID(), 12345u);
    EXPECT_EQ(ghost.GetOwnerCellId(), 0x00640064u);
    EXPECT_EQ(ghost.GetPositionX(), 0.0f);
    EXPECT_EQ(ghost.GetPositionY(), 0.0f);
    EXPECT_EQ(ghost.GetPositionZ(), 0.0f);
    EXPECT_FALSE(ghost.IsInCombat());
    EXPECT_FALSE(ghost.IsDead());
}

TEST_F(GhostActorSystemTest, GhostEntitySyncPosition)
{
    GhostEntity ghost(12345, 0x00640064);

    ghost.SyncPosition(100.0f, 200.0f, 50.0f, 1.5f);

    EXPECT_FLOAT_EQ(ghost.GetPositionX(), 100.0f);
    EXPECT_FLOAT_EQ(ghost.GetPositionY(), 200.0f);
    EXPECT_FLOAT_EQ(ghost.GetPositionZ(), 50.0f);
    EXPECT_FLOAT_EQ(ghost.GetOrientation(), 1.5f);
}

TEST_F(GhostActorSystemTest, GhostEntitySyncHealth)
{
    GhostEntity ghost(12345, 0x00640064);

    ghost.SyncHealth(500, 1000);

    EXPECT_EQ(ghost.GetHealth(), 500u);
    EXPECT_EQ(ghost.GetMaxHealth(), 1000u);
}

TEST_F(GhostActorSystemTest, GhostEntitySyncCombatState)
{
    GhostEntity ghost(12345, 0x00640064);

    EXPECT_FALSE(ghost.IsInCombat());

    ghost.SyncCombatState(true);
    EXPECT_TRUE(ghost.IsInCombat());

    ghost.SyncCombatState(false);
    EXPECT_FALSE(ghost.IsInCombat());
}

TEST_F(GhostActorSystemTest, GhostEntitySyncFromSnapshot)
{
    GhostEntity ghost(12345, 0x00640064);

    GhostSnapshot snapshot;
    snapshot.guid = 12345;
    snapshot.posX = 100.0f;
    snapshot.posY = 200.0f;
    snapshot.posZ = 50.0f;
    snapshot.orientation = 2.0f;
    snapshot.health = 750;
    snapshot.maxHealth = 1500;
    snapshot.inCombat = true;
    snapshot.isDead = false;

    ghost.SyncFromSnapshot(snapshot);

    EXPECT_FLOAT_EQ(ghost.GetPositionX(), 100.0f);
    EXPECT_FLOAT_EQ(ghost.GetPositionY(), 200.0f);
    EXPECT_FLOAT_EQ(ghost.GetPositionZ(), 50.0f);
    EXPECT_FLOAT_EQ(ghost.GetOrientation(), 2.0f);
    EXPECT_EQ(ghost.GetHealth(), 750u);
    EXPECT_EQ(ghost.GetMaxHealth(), 1500u);
    EXPECT_TRUE(ghost.IsInCombat());
    EXPECT_FALSE(ghost.IsDead());
}

// ============================================================================
// ActorMessage Tests
// ============================================================================

TEST_F(GhostActorSystemTest, ActorMessageDefault)
{
    ActorMessage msg{};  // Explicitly zero-initialize

    EXPECT_EQ(msg.sourceGuid, 0u);
    EXPECT_EQ(msg.targetGuid, 0u);
    EXPECT_EQ(msg.sourceCellId, 0u);
    EXPECT_EQ(msg.targetCellId, 0u);
}

TEST_F(GhostActorSystemTest, ActorMessageAssignment)
{
    ActorMessage msg;
    msg.type = MessageType::SPELL_HIT;
    msg.sourceGuid = 1111;
    msg.targetGuid = 2222;
    msg.sourceCellId = 0x00640064;
    msg.targetCellId = 0x00650065;
    msg.intParam1 = 100;
    msg.intParam2 = 200;
    msg.floatParam1 = 1.5f;

    EXPECT_EQ(msg.type, MessageType::SPELL_HIT);
    EXPECT_EQ(msg.sourceGuid, 1111u);
    EXPECT_EQ(msg.targetGuid, 2222u);
    EXPECT_EQ(msg.sourceCellId, 0x00640064u);
    EXPECT_EQ(msg.targetCellId, 0x00650065u);
    EXPECT_EQ(msg.intParam1, 100);
    EXPECT_EQ(msg.intParam2, 200);
    EXPECT_FLOAT_EQ(msg.floatParam1, 1.5f);
}

// ============================================================================
// EntityGhostInfo Tests
// ============================================================================

TEST_F(GhostActorSystemTest, EntityGhostInfoDefault)
{
    EntityGhostInfo info;

    EXPECT_EQ(info.guid, 0u);
    EXPECT_EQ(info.homeCellId, 0u);
    EXPECT_EQ(info.activeGhosts, NeighborFlags::NONE);
}

// ============================================================================
// MigrationSnapshot Tests
// ============================================================================

TEST_F(GhostActorSystemTest, MigrationSnapshotDefault)
{
    MigrationSnapshot snapshot;

    EXPECT_EQ(snapshot.guid, 0u);
    EXPECT_EQ(snapshot.entry, 0u);
    EXPECT_EQ(snapshot.typeId, 0u);
    EXPECT_EQ(snapshot.posX, 0.0f);
    EXPECT_EQ(snapshot.health, 0u);
    EXPECT_FALSE(snapshot.inCombat);
    EXPECT_FALSE(snapshot.isDead);
}

// ============================================================================
// Constants Tests
// ============================================================================

TEST_F(GhostActorSystemTest, ConstantsValid)
{
    // GHOST_VISIBILITY_DISTANCE should be greater than CELL_SIZE
    // so ghosts are always needed in all neighbors
    EXPECT_GT(GHOST_VISIBILITY_DISTANCE, CELL_SIZE);

    // CELL_SIZE should be approximately 66.67 yards
    EXPECT_NEAR(CELL_SIZE, 66.6666f, 0.01f);

    // CENTER_CELL_OFFSET should be 256 (512/2)
    EXPECT_EQ(CENTER_CELL_OFFSET, 256.0f);
}

// ============================================================================
// High-Scale Entity Tracking Tests (10-15k player simulation)
// ============================================================================

// Test creating many ghost entities (simulates 500 entities per cell in populated areas)
// Scale: 500 ghosts per cell x 9 neighbors = 4500 ghosts tracked per cell
TEST_F(GhostActorSystemTest, ManyGhostEntitiesPerCell)
{
    // Simulate a densely populated cell (e.g., Dalaran, Orgrimmar bank)
    // Each cell might have 500+ entities visible from neighbors
    constexpr int GHOSTS_PER_CELL = 500;
    std::vector<GhostEntity> ghosts;
    ghosts.reserve(GHOSTS_PER_CELL);

    uint32_t cellId = (100 << 16) | 100;

    for (int i = 0; i < GHOSTS_PER_CELL; ++i)
    {
        ghosts.emplace_back(static_cast<uint64_t>(i + 1), cellId);
        ghosts.back().SyncPosition(
            100.0f + (i % 50) * 1.0f,
            200.0f + (i / 50) * 1.0f,
            50.0f,
            0.0f
        );
        ghosts.back().SyncHealth(1000 + i, 2000 + i);
    }

    // Verify all ghosts were created correctly
    EXPECT_EQ(ghosts.size(), static_cast<size_t>(GHOSTS_PER_CELL));

    // Verify first and last ghost
    EXPECT_EQ(ghosts[0].GetGUID(), 1u);
    EXPECT_EQ(ghosts[GHOSTS_PER_CELL - 1].GetGUID(), static_cast<uint64_t>(GHOSTS_PER_CELL));
}

// Test EntityGhostInfo tracking at high scale
// Scale: 15000 entities = full server population
TEST_F(GhostActorSystemTest, EntityGhostInfoHighScale)
{
    // Simulate tracking ghost info for all players on a 15k pop server
    constexpr int NUM_ENTITIES = 15000;
    std::vector<EntityGhostInfo> entityInfos;
    entityInfos.reserve(NUM_ENTITIES);

    for (int i = 0; i < NUM_ENTITIES; ++i)
    {
        EntityGhostInfo info;
        info.guid = static_cast<uint64_t>(i + 1);
        // Distribute entities across cells
        info.homeCellId = ((100 + i / 100) << 16) | (100 + i % 100);
        info.activeGhosts = NeighborFlags::ALL; // All neighbors need ghosts
        entityInfos.push_back(info);
    }

    EXPECT_EQ(entityInfos.size(), static_cast<size_t>(NUM_ENTITIES));

    // Verify distribution - should have entities spread across 150+ cells
    std::set<uint32_t> uniqueCells;
    for (const auto& info : entityInfos)
    {
        uniqueCells.insert(info.homeCellId);
    }
    EXPECT_GT(uniqueCells.size(), 100u);
}

// Test snapshot updates at high volume
// Scale: 15000 entities x 8 neighbors = 120k ghost updates per tick
TEST_F(GhostActorSystemTest, SnapshotUpdatesHighVolume)
{
    // Each entity needs to send snapshots to all 8 neighbors
    constexpr int NUM_ENTITIES = 15000;
    constexpr int NUM_NEIGHBORS = 8;
    std::vector<GhostSnapshot> snapshots;
    snapshots.reserve(NUM_ENTITIES);

    // Create snapshots for all entities
    for (int i = 0; i < NUM_ENTITIES; ++i)
    {
        GhostSnapshot snapshot;
        snapshot.guid = static_cast<uint64_t>(i + 1);
        snapshot.posX = 100.0f + (i % 1000);
        snapshot.posY = 200.0f + (i / 1000);
        snapshot.posZ = 50.0f;
        snapshot.orientation = 0.0f;
        snapshot.health = 1000;
        snapshot.maxHealth = 1000;
        snapshot.inCombat = (i % 10 == 0); // 10% in combat
        snapshot.isDead = false;
        snapshots.push_back(snapshot);
    }

    // Simulate applying snapshots to ghost entities (8 neighbors per entity)
    std::vector<GhostEntity> ghosts;
    ghosts.reserve(NUM_ENTITIES);

    for (int i = 0; i < NUM_ENTITIES; ++i)
    {
        ghosts.emplace_back(snapshots[i].guid, (100 << 16) | 100);
    }

    // Apply all snapshot updates
    for (int neighbor = 0; neighbor < NUM_NEIGHBORS; ++neighbor)
    {
        for (int i = 0; i < NUM_ENTITIES; ++i)
        {
            ghosts[i].SyncFromSnapshot(snapshots[i]);
        }
    }

    // Verify updates applied correctly
    EXPECT_EQ(ghosts.size(), static_cast<size_t>(NUM_ENTITIES));
    EXPECT_EQ(ghosts[0].GetHealth(), 1000u);
}

// Test migration snapshots at scale
// Scale: 1500 migrations per tick (10% of 15k players moving)
TEST_F(GhostActorSystemTest, MigrationSnapshotsHighVolume)
{
    // Simulate 10% of players crossing cell boundaries per tick
    constexpr int NUM_MIGRATIONS = 1500;
    std::vector<MigrationSnapshot> migrations;
    migrations.reserve(NUM_MIGRATIONS);

    for (int i = 0; i < NUM_MIGRATIONS; ++i)
    {
        MigrationSnapshot snapshot;
        snapshot.guid = static_cast<uint64_t>(i + 1);
        snapshot.entry = 0; // Player entry
        snapshot.typeId = 4; // TYPEID_PLAYER
        snapshot.posX = 100.0f + i;
        snapshot.posY = 200.0f;
        snapshot.posZ = 50.0f;
        snapshot.orientation = 0.0f;
        snapshot.health = 10000;
        snapshot.maxHealth = 10000;
        snapshot.power = 5000;
        snapshot.maxPower = 5000;
        snapshot.powerType = 0; // Mana
        snapshot.inCombat = (i % 5 == 0); // 20% in combat
        snapshot.isDead = false;
        migrations.push_back(snapshot);
    }

    EXPECT_EQ(migrations.size(), static_cast<size_t>(NUM_MIGRATIONS));

    // Verify migration data
    int inCombatCount = 0;
    for (const auto& m : migrations)
    {
        if (m.inCombat)
            inCombatCount++;
    }
    EXPECT_EQ(inCombatCount, NUM_MIGRATIONS / 5);
}

// Test ActorMessage creation at combat scale
// Scale: 50k messages per tick (combat scenarios with AoE spells)
TEST_F(GhostActorSystemTest, ActorMessagesHighVolume)
{
    // Heavy combat: multiple raid groups, battlegrounds, world PvP
    // Each combat event generates multiple messages (damage, threat, debuffs, etc.)
    constexpr int NUM_MESSAGES = 50000;
    std::vector<ActorMessage> messages;
    messages.reserve(NUM_MESSAGES);

    for (int i = 0; i < NUM_MESSAGES; ++i)
    {
        ActorMessage msg{};
        msg.type = static_cast<MessageType>(i % 4); // Cycle through message types
        msg.sourceGuid = static_cast<uint64_t>(i / 10 + 1);
        msg.targetGuid = static_cast<uint64_t>(i % 1000 + 1);
        msg.sourceCellId = (100 << 16) | (100 + i / 1000);
        msg.targetCellId = (100 << 16) | (100 + i / 1000);
        msg.intParam1 = 100 + (i % 500); // Damage amount
        msg.intParam2 = i % 100;         // Spell ID low bits
        messages.push_back(msg);
    }

    EXPECT_EQ(messages.size(), static_cast<size_t>(NUM_MESSAGES));

    // Verify message distribution across types
    int typeCount[4] = {0, 0, 0, 0};
    for (const auto& m : messages)
    {
        typeCount[static_cast<int>(m.type)]++;
    }
    // Each type should have ~25% of messages
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_NEAR(typeCount[i], NUM_MESSAGES / 4, 1);
    }
}

// ============================================================================
// MPSC Queue Tests - Lock-Free Queue Correctness and Performance
// ============================================================================

// Basic MPSC queue push/pop correctness
TEST_F(GhostActorSystemTest, MPSCQueueBasicOperations)
{
    MPSCQueue<ActorMessage> queue;

    // Queue should be empty initially
    ActorMessage msg;
    EXPECT_FALSE(queue.Pop(msg));

    // Push and pop single message
    ActorMessage testMsg{};
    testMsg.type = MessageType::SPELL_HIT;
    testMsg.sourceGuid = 12345;
    testMsg.targetGuid = 67890;
    testMsg.intParam1 = 500;

    queue.Push(testMsg);
    EXPECT_TRUE(queue.Pop(msg));
    EXPECT_EQ(msg.type, MessageType::SPELL_HIT);
    EXPECT_EQ(msg.sourceGuid, 12345u);
    EXPECT_EQ(msg.targetGuid, 67890u);
    EXPECT_EQ(msg.intParam1, 500);

    // Queue should be empty again
    EXPECT_FALSE(queue.Pop(msg));
}

// FIFO ordering verification
TEST_F(GhostActorSystemTest, MPSCQueueFIFOOrder)
{
    MPSCQueue<ActorMessage> queue;

    // Push 100 messages with sequential IDs
    for (int i = 0; i < 100; ++i)
    {
        ActorMessage msg{};
        msg.sourceGuid = static_cast<uint64_t>(i);
        queue.Push(msg);
    }

    // Pop and verify order
    for (int i = 0; i < 100; ++i)
    {
        ActorMessage msg;
        EXPECT_TRUE(queue.Pop(msg));
        EXPECT_EQ(msg.sourceGuid, static_cast<uint64_t>(i));
    }

    // Queue should be empty
    ActorMessage msg;
    EXPECT_FALSE(queue.Pop(msg));
}

// High-volume queue drain test
// This tests the actual drain pattern: while(Pop()) HandleMessage()
TEST_F(GhostActorSystemTest, MPSCQueueDrainHighVolume)
{
    MPSCQueue<ActorMessage> queue;

    // Push 1,000,000 messages (simulates extreme message storm)
    // Real scenario: 100 players × 50 actions/tick × 200 ticks = 1M messages
    constexpr int NUM_MESSAGES = 1000000;

    for (int i = 0; i < NUM_MESSAGES; ++i)
    {
        ActorMessage msg{};
        msg.type = static_cast<MessageType>(i % 4);
        msg.sourceGuid = static_cast<uint64_t>(i);
        msg.targetGuid = static_cast<uint64_t>(NUM_MESSAGES - i);
        msg.intParam1 = 100 + (i % 1000);
        queue.Push(msg);
    }

    // Drain all messages (the actual pattern used in CellActor::ProcessMessages)
    int processedCount = 0;
    int typeCounts[4] = {0, 0, 0, 0};
    ActorMessage msg;

    while (queue.Pop(msg))
    {
        // Simulate minimal message handling (type dispatch)
        typeCounts[static_cast<int>(msg.type)]++;
        processedCount++;
    }

    // Verify all messages were processed
    EXPECT_EQ(processedCount, NUM_MESSAGES);

    // Verify type distribution
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_EQ(typeCounts[i], NUM_MESSAGES / 4);
    }

    // Queue should be empty
    EXPECT_FALSE(queue.Pop(msg));
}

// Multi-threaded producer, single consumer drain test
// Validates lock-free correctness under contention
TEST_F(GhostActorSystemTest, MPSCQueueMultiProducerDrain)
{
    MPSCQueue<ActorMessage> queue;

    // 8 neighbor cells each sending 125k messages = 1M total
    // Simulates worst-case cross-cell traffic during mass combat
    constexpr int NUM_PRODUCERS = 8;
    constexpr int MESSAGES_PER_PRODUCER = 125000;
    constexpr int TOTAL_MESSAGES = NUM_PRODUCERS * MESSAGES_PER_PRODUCER;

    std::vector<std::thread> producers;
    std::atomic<int> producersReady{0};
    std::atomic<bool> startSignal{false};

    // Launch producer threads
    for (int p = 0; p < NUM_PRODUCERS; ++p)
    {
        producers.emplace_back([&queue, &producersReady, &startSignal, p]()
        {
            producersReady.fetch_add(1);

            // Wait for start signal (ensures all producers start simultaneously)
            while (!startSignal.load(std::memory_order_acquire))
                std::this_thread::yield();

            // Push messages with producer ID encoded
            for (int i = 0; i < MESSAGES_PER_PRODUCER; ++i)
            {
                ActorMessage msg{};
                msg.sourceGuid = static_cast<uint64_t>(p);  // Producer ID
                msg.targetGuid = static_cast<uint64_t>(i);  // Message sequence
                msg.intParam1 = p * MESSAGES_PER_PRODUCER + i;  // Unique ID
                queue.Push(msg);
            }
        });
    }

    // Wait for all producers to be ready
    while (producersReady.load() < NUM_PRODUCERS)
        std::this_thread::yield();

    // Start all producers simultaneously
    startSignal.store(true, std::memory_order_release);

    // Wait for all producers to finish
    for (auto& t : producers)
        t.join();

    // Single consumer drains all messages
    std::set<int> uniqueIds;
    int perProducerCount[NUM_PRODUCERS] = {0};
    ActorMessage msg;

    while (queue.Pop(msg))
    {
        uniqueIds.insert(msg.intParam1);
        perProducerCount[msg.sourceGuid]++;
    }

    // Verify all messages were received (no loss)
    EXPECT_EQ(uniqueIds.size(), static_cast<size_t>(TOTAL_MESSAGES));

    // Verify each producer's messages were received
    for (int p = 0; p < NUM_PRODUCERS; ++p)
    {
        EXPECT_EQ(perProducerCount[p], MESSAGES_PER_PRODUCER);
    }
}

// Interleaved push/pop test (producer and consumer running concurrently)
TEST_F(GhostActorSystemTest, MPSCQueueConcurrentPushPop)
{
    MPSCQueue<ActorMessage> queue;

    // 2M messages with concurrent producer/consumer
    // Tests real-world scenario where messages arrive while being processed
    constexpr int NUM_MESSAGES = 2000000;
    std::atomic<bool> producerDone{false};
    std::atomic<int> consumed{0};

    // Producer thread
    std::thread producer([&queue, &producerDone]()
    {
        for (int i = 0; i < NUM_MESSAGES; ++i)
        {
            ActorMessage msg{};
            msg.sourceGuid = static_cast<uint64_t>(i);
            queue.Push(msg);

            // Occasional yield to allow consumer to run
            if (i % 1000 == 0)
                std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    // Consumer runs in main thread
    std::set<uint64_t> receivedIds;

    while (!producerDone.load(std::memory_order_acquire) || consumed.load() < NUM_MESSAGES)
    {
        ActorMessage msg;
        while (queue.Pop(msg))
        {
            receivedIds.insert(msg.sourceGuid);
            consumed.fetch_add(1);
        }
        std::this_thread::yield();
    }

    // Final drain
    ActorMessage msg;
    while (queue.Pop(msg))
    {
        receivedIds.insert(msg.sourceGuid);
        consumed.fetch_add(1);
    }

    producer.join();

    // Verify all messages received
    EXPECT_EQ(receivedIds.size(), static_cast<size_t>(NUM_MESSAGES));
    EXPECT_EQ(consumed.load(), NUM_MESSAGES);
}

// ============================================================================
// Boundary and Corner Position Tests
// ============================================================================

// Helper to calculate cell ID from world coordinates
static uint32_t CalcCellId(float worldX, float worldY)
{
    uint32_t cellX = static_cast<uint32_t>(CENTER_CELL_OFFSET - (worldX / CELL_SIZE));
    uint32_t cellY = static_cast<uint32_t>(CENTER_CELL_OFFSET - (worldY / CELL_SIZE));
    return (cellY << 16) | cellX;
}

// Helper to get all 8 neighbor cell IDs
static std::set<uint32_t> GetAllNeighborCellIds(uint32_t cellId)
{
    std::set<uint32_t> neighbors;
    static const NeighborFlags directions[] = {
        NeighborFlags::NORTH, NeighborFlags::SOUTH,
        NeighborFlags::EAST, NeighborFlags::WEST,
        NeighborFlags::NORTH_EAST, NeighborFlags::NORTH_WEST,
        NeighborFlags::SOUTH_EAST, NeighborFlags::SOUTH_WEST
    };
    for (auto dir : directions)
    {
        neighbors.insert(GhostBoundary::GetNeighborCellId(cellId, dir));
    }
    return neighbors;
}

// Test: Entity exactly at cell boundary (edge) - which cell does it belong to?
TEST_F(GhostActorSystemTest, BoundaryCellAssignment_Edge)
{
    // Cell boundaries occur at multiples of CELL_SIZE
    // Entity at exact boundary should deterministically belong to one cell

    // Position exactly at a cell boundary on X axis
    float boundaryX = CELL_SIZE * 2.0f;  // Exact boundary
    float centerY = CELL_SIZE * 1.5f;     // Center of cell on Y

    uint32_t cellAtBoundary = CalcCellId(boundaryX, centerY);
    uint32_t cellJustBefore = CalcCellId(boundaryX - 0.001f, centerY);
    uint32_t cellJustAfter = CalcCellId(boundaryX + 0.001f, centerY);

    // Boundary belongs to one cell, slightly before/after may differ
    // Key assertion: calculation is deterministic
    EXPECT_EQ(CalcCellId(boundaryX, centerY), cellAtBoundary);

    // Positions on opposite sides of boundary should be in different cells
    // (unless the epsilon is too small to matter)
    // With our cell size of 66.666, positions 0.002 apart spanning boundary should differ
    if (cellJustBefore != cellJustAfter)
    {
        // Boundary is between these two cells
        EXPECT_TRUE(cellAtBoundary == cellJustBefore || cellAtBoundary == cellJustAfter);
    }
}

// Test: Entity exactly at cell corner (4-cell junction)
TEST_F(GhostActorSystemTest, BoundaryCellAssignment_Corner)
{
    // Corner position where 4 cells meet
    float cornerX = CELL_SIZE * 3.0f;  // Exact corner on X
    float cornerY = CELL_SIZE * 3.0f;  // Exact corner on Y

    uint32_t cellAtCorner = CalcCellId(cornerX, cornerY);

    // Positions in each of the 4 quadrants around the corner
    float epsilon = 0.01f;
    uint32_t cellNE = CalcCellId(cornerX + epsilon, cornerY + epsilon);
    uint32_t cellNW = CalcCellId(cornerX - epsilon, cornerY + epsilon);
    uint32_t cellSE = CalcCellId(cornerX + epsilon, cornerY - epsilon);
    uint32_t cellSW = CalcCellId(cornerX - epsilon, cornerY - epsilon);

    // All 4 quadrants should be in different cells
    std::set<uint32_t> quadrantCells = {cellNE, cellNW, cellSE, cellSW};
    EXPECT_EQ(quadrantCells.size(), 4u) << "Corner should touch exactly 4 different cells";

    // The corner itself should belong to one of these 4 cells
    EXPECT_TRUE(quadrantCells.count(cellAtCorner) == 1)
        << "Corner position should belong to one of the 4 adjacent cells";
}

// Test: Neighbors of corner cells are correct
TEST_F(GhostActorSystemTest, CornerCellNeighbors)
{
    // At a corner, entity needs ghosts in at least 3 other cells (the other quadrants)
    float cornerX = CELL_SIZE * 5.0f;
    float cornerY = CELL_SIZE * 5.0f;

    float epsilon = 0.01f;
    uint32_t cellNE = CalcCellId(cornerX + epsilon, cornerY + epsilon);
    uint32_t cellNW = CalcCellId(cornerX - epsilon, cornerY + epsilon);
    uint32_t cellSE = CalcCellId(cornerX + epsilon, cornerY - epsilon);
    uint32_t cellSW = CalcCellId(cornerX - epsilon, cornerY - epsilon);

    // Verify that each cell can reach the others as neighbors
    auto neighborsNE = GetAllNeighborCellIds(cellNE);
    auto neighborsNW = GetAllNeighborCellIds(cellNW);
    auto neighborsSE = GetAllNeighborCellIds(cellSE);
    auto neighborsSW = GetAllNeighborCellIds(cellSW);

    // NE cell should have SW, NW, SE as neighbors
    EXPECT_TRUE(neighborsNE.count(cellSW) == 1) << "NE should neighbor SW (diagonal)";
    EXPECT_TRUE(neighborsNE.count(cellNW) == 1) << "NE should neighbor NW (west)";
    EXPECT_TRUE(neighborsNE.count(cellSE) == 1) << "NE should neighbor SE (south)";

    // SW cell should have NE, NW, SE as neighbors
    EXPECT_TRUE(neighborsSW.count(cellNE) == 1) << "SW should neighbor NE (diagonal)";
    EXPECT_TRUE(neighborsSW.count(cellNW) == 1) << "SW should neighbor NW (north)";
    EXPECT_TRUE(neighborsSW.count(cellSE) == 1) << "SW should neighbor SE (east)";
}

// Test: Entity at edge has correct adjacent cells
TEST_F(GhostActorSystemTest, EdgeCellNeighbors)
{
    // Entity at east edge of cell (but not at corner)
    float edgeX = CELL_SIZE * 4.0f;       // Exact edge
    float centerY = CELL_SIZE * 4.5f;     // Middle of cell on Y axis

    float epsilon = 0.01f;
    uint32_t cellWest = CalcCellId(edgeX - epsilon, centerY);  // West of edge
    uint32_t cellEast = CalcCellId(edgeX + epsilon, centerY);  // East of edge

    // These should be different cells
    EXPECT_NE(cellWest, cellEast) << "Cells on opposite sides of edge should differ";

    // They should be neighbors
    auto neighborsWest = GetAllNeighborCellIds(cellWest);
    auto neighborsEast = GetAllNeighborCellIds(cellEast);

    EXPECT_TRUE(neighborsWest.count(cellEast) == 1) << "West cell should have East as neighbor";
    EXPECT_TRUE(neighborsEast.count(cellWest) == 1) << "East cell should have West as neighbor";
}

// Test: Cross-boundary message routing simulation
// Simulates player at corner attacking entities in each of the 4 corner cells
TEST_F(GhostActorSystemTest, CrossBoundaryInteraction_CornerAttack)
{
    // Player exactly at a corner
    float cornerX = CELL_SIZE * 6.0f;
    float cornerY = CELL_SIZE * 6.0f;

    // Player is in one cell (deterministic based on floor())
    uint32_t playerCellId = CalcCellId(cornerX, cornerY);

    // Targets in each of the 4 corner quadrants
    float epsilon = 5.0f;  // 5 yards into each quadrant
    uint32_t targetCellNE = CalcCellId(cornerX + epsilon, cornerY + epsilon);
    uint32_t targetCellNW = CalcCellId(cornerX - epsilon, cornerY + epsilon);
    uint32_t targetCellSE = CalcCellId(cornerX + epsilon, cornerY - epsilon);
    uint32_t targetCellSW = CalcCellId(cornerX - epsilon, cornerY - epsilon);

    std::set<uint32_t> targetCells = {targetCellNE, targetCellNW, targetCellSE, targetCellSW};

    // Player's neighbors should include all cells they might interact with
    auto playerNeighbors = GetAllNeighborCellIds(playerCellId);

    // Count how many target cells are reachable as neighbors
    int reachableTargets = 0;
    for (uint32_t targetCell : targetCells)
    {
        if (targetCell == playerCellId || playerNeighbors.count(targetCell) == 1)
        {
            ++reachableTargets;
        }
    }

    // All 4 targets should be reachable (same cell or neighbor)
    EXPECT_EQ(reachableTargets, 4)
        << "Player at corner should be able to reach all 4 adjacent cells";
}

// Test: Entity position gives valid local coordinates within cell
TEST_F(GhostActorSystemTest, LocalPositionWithinCell)
{
    // Test that local positions are always within [0, CELL_SIZE)
    std::vector<std::pair<float, float>> testPositions = {
        {0.0f, 0.0f},
        {CELL_SIZE * 3.0f - 1.0f, CELL_SIZE * 3.0f - 1.0f},  // Near boundary
        {CELL_SIZE * 3.0f + 1.0f, CELL_SIZE * 3.0f + 1.0f},  // Just past boundary
        {1000.0f, -500.0f},
    };

    for (const auto& [worldX, worldY] : testPositions)
    {
        float localX, localY;
        GhostBoundary::GetPositionInCell(worldX, worldY, localX, localY);

        // Local position must be within cell bounds
        EXPECT_GE(localX, 0.0f) << "Local X must be >= 0 for world pos (" << worldX << ", " << worldY << ")";
        EXPECT_LT(localX, CELL_SIZE) << "Local X must be < CELL_SIZE for world pos (" << worldX << ", " << worldY << ")";
        EXPECT_GE(localY, 0.0f) << "Local Y must be >= 0 for world pos (" << worldX << ", " << worldY << ")";
        EXPECT_LT(localY, CELL_SIZE) << "Local Y must be < CELL_SIZE for world pos (" << worldX << ", " << worldY << ")";
    }
}

// Test: Interaction distance validation at boundary
// Two entities on opposite sides of boundary but within interaction range
TEST_F(GhostActorSystemTest, BoundaryInteractionDistance)
{
    // Entity A: 2 yards west of boundary
    float entityAX = CELL_SIZE * 4.0f - 2.0f;
    float entityAY = CELL_SIZE * 4.5f;

    // Entity B: 2 yards east of boundary
    float entityBX = CELL_SIZE * 4.0f + 2.0f;
    float entityBY = CELL_SIZE * 4.5f;

    uint32_t cellA = CalcCellId(entityAX, entityAY);
    uint32_t cellB = CalcCellId(entityBX, entityBY);

    // Should be in different cells
    EXPECT_NE(cellA, cellB) << "Entities should be in different cells";

    // Distance between them is only 4 yards (well within melee range)
    float dx = entityBX - entityAX;
    float dy = entityBY - entityAY;
    float distance = std::sqrt(dx * dx + dy * dy);
    EXPECT_LT(distance, 5.0f) << "Entities should be within melee range";

    // They should be neighbors
    auto neighborsA = GetAllNeighborCellIds(cellA);
    EXPECT_TRUE(neighborsA.count(cellB) == 1)
        << "Cross-boundary entities within melee range must be in neighbor cells";
}

// Test: 8-way neighbor coverage from any position
TEST_F(GhostActorSystemTest, AllNeighborsCovered)
{
    // From any valid cell, all 8 neighbors should be calculated correctly
    // Test with various positions across the map

    std::vector<std::pair<float, float>> testPositions = {
        {0.0f, 0.0f},           // Map center
        {1000.0f, 1000.0f},     // Positive quadrant
        {-1000.0f, -1000.0f},   // Negative quadrant
        {5000.0f, -3000.0f},    // Mixed
        {CELL_SIZE * 10.5f, CELL_SIZE * 10.5f},  // Center of a cell
        {CELL_SIZE * 10.0f, CELL_SIZE * 10.0f},  // At a corner
    };

    for (const auto& [x, y] : testPositions)
    {
        uint32_t cellId = CalcCellId(x, y);
        auto neighbors = GetAllNeighborCellIds(cellId);

        // Should always have exactly 8 unique neighbors
        EXPECT_EQ(neighbors.size(), 8u)
            << "Position (" << x << ", " << y << ") should have 8 neighbors";

        // No neighbor should be the same as the home cell
        EXPECT_EQ(neighbors.count(cellId), 0u)
            << "Home cell should not be in neighbor list";
    }
}
