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
#include <cmath>
#include <set>
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
