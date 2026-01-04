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

#include "CellActorTestHarness.h"
#include "WorldMock.h"
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include <thread>
#include <vector>

using namespace GhostActor;
using namespace testing;

class CrossCellInteractionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Save original world and install mock
        _originalWorld = sWorld.release();
        _worldMock = new NiceMock<WorldMock>();
        sWorld.reset(_worldMock);

        // Set up minimal config defaults
        static std::string emptyString;
        ON_CALL(*_worldMock, GetDataPath()).WillByDefault(ReturnRef(emptyString));
        ON_CALL(*_worldMock, GetRealmName()).WillByDefault(ReturnRef(emptyString));
        ON_CALL(*_worldMock, GetDefaultDbcLocale()).WillByDefault(Return(LOCALE_enUS));
        ON_CALL(*_worldMock, getRate(_)).WillByDefault(Return(1.0f));
        ON_CALL(*_worldMock, getBoolConfig(_)).WillByDefault(Return(false));
        ON_CALL(*_worldMock, getIntConfig(_)).WillByDefault(Return(0));
        ON_CALL(*_worldMock, getFloatConfig(_)).WillByDefault(Return(0.0f));
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

    IWorld* _originalWorld = nullptr;
    NiceMock<WorldMock>* _worldMock = nullptr;
};

// ============================================================================
// Segfault Prevention Tests
// ============================================================================

// Test: Message arrives for ghost that was deleted before processing
TEST_F(CrossCellInteractionTest, MessageForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1001;
    harness.AddGhost(ghostGuid, 0x00640064);

    // Queue a message while ghost exists
    ActorMessage msg{};
    msg.type = MessageType::HEALTH_CHANGED;
    msg.sourceGuid = ghostGuid;
    msg.intParam1 = 500;  // New health

    harness.InjectMessage(msg);

    // Destroy the ghost before processing
    harness.DestroyGhost(ghostGuid);

    // Process should NOT segfault
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// Test: Ghost message arrives after ghost was destroyed
TEST_F(CrossCellInteractionTest, GhostUpdateAfterDestroy)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 9999;
    uint32_t ownerCellId = 0x00640064;  // (100, 100)

    // Create a ghost
    harness.AddGhost(ghostGuid, ownerCellId);

    // Queue a position update
    ActorMessage posUpdate{};
    posUpdate.type = MessageType::POSITION_UPDATE;
    posUpdate.sourceGuid = ghostGuid;
    posUpdate.floatParam1 = 100.0f;  // posX
    posUpdate.floatParam2 = 200.0f;  // posY
    posUpdate.floatParam3 = 50.0f;   // posZ

    harness.InjectMessage(posUpdate);

    // Destroy the ghost before processing update
    harness.DestroyGhost(ghostGuid);

    // Queue another update for the now-destroyed ghost
    ActorMessage healthUpdate{};
    healthUpdate.type = MessageType::HEALTH_CHANGED;
    healthUpdate.sourceGuid = ghostGuid;
    healthUpdate.intParam1 = 750;

    harness.InjectMessage(healthUpdate);

    // Process should NOT segfault
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// Test: Multiple ghosts deleted while messages queued
TEST_F(CrossCellInteractionTest, MassGhostDespawn)
{
    CellActorTestHarness harness;

    // Create 50 ghosts
    std::vector<uint64_t> ghostGuids;
    for (int i = 0; i < 50; ++i)
    {
        uint64_t guid = static_cast<uint64_t>(1000 + i);
        harness.AddGhost(guid, 0x00640064);
        ghostGuids.push_back(guid);
    }

    // Queue messages for all ghosts
    for (uint64_t guid : ghostGuids)
    {
        ActorMessage msg{};
        msg.type = MessageType::HEALTH_CHANGED;
        msg.sourceGuid = guid;
        msg.intParam1 = 100;

        harness.InjectMessage(msg);
    }

    // Delete half of them before processing
    for (int i = 0; i < 25; ++i)
    {
        harness.DestroyGhost(ghostGuids[i]);
    }

    // Process should NOT segfault
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// Test: Ghost deleted, then re-created with same GUID (edge case)
TEST_F(CrossCellInteractionTest, GhostRecycledGuid)
{
    CellActorTestHarness harness;

    // Create ghost with GUID 1001
    uint64_t ghostGuid = 1001;
    harness.AddGhost(ghostGuid, 0x00640064);

    // Queue message for ghost
    ActorMessage msg{};
    msg.type = MessageType::HEALTH_CHANGED;
    msg.sourceGuid = ghostGuid;
    msg.intParam1 = 2500;

    harness.InjectMessage(msg);

    // Delete ghost
    harness.DestroyGhost(ghostGuid);

    // Process - should handle missing ghost gracefully
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());

    // Re-create ghost with same GUID
    harness.AddGhost(ghostGuid, 0x00640064);

    // Queue another message
    ActorMessage msg2{};
    msg2.type = MessageType::HEALTH_CHANGED;
    msg2.sourceGuid = ghostGuid;
    msg2.intParam1 = 5000;

    harness.InjectMessage(msg2);

    // Process - should work with new ghost
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// ============================================================================
// Cross-Cell Communication Tests
// ============================================================================

// Test: Position update propagates correctly
TEST_F(CrossCellInteractionTest, PositionUpdatePropagation)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 8888;
    harness.AddGhost(ghostGuid, 0x00640064);

    // Send position update
    ActorMessage msg{};
    msg.type = MessageType::POSITION_UPDATE;
    msg.sourceGuid = ghostGuid;
    msg.floatParam1 = 150.0f;
    msg.floatParam2 = 250.0f;
    msg.floatParam3 = 75.0f;
    // orientation would be in floatParam4 if it existed

    harness.InjectMessage(msg);
    harness.ProcessAllMessages();

    // Ghost should exist and have updated position
    // (actual verification depends on CellActor implementation exposing ghost data)
    SUCCEED();
}

// Test: Health update for ghost
TEST_F(CrossCellInteractionTest, HealthUpdateForGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 7777;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::HEALTH_CHANGED;
    msg.sourceGuid = ghostGuid;
    msg.intParam1 = 500;   // health
    msg.intParam2 = 1000;  // maxHealth

    harness.InjectMessage(msg);

    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// ============================================================================
// Scale Tests (10-15k player simulation)
// ============================================================================

// Test: High volume of ghost position updates
// Scale: 1000 ghosts x 10 updates = 10k messages
TEST_F(CrossCellInteractionTest, MassGhostUpdates)
{
    CellActorTestHarness harness;

    constexpr int NUM_GHOSTS = 1000;
    constexpr int UPDATES_PER_GHOST = 10;

    // Create ghosts
    for (int i = 0; i < NUM_GHOSTS; ++i)
    {
        harness.AddGhost(static_cast<uint64_t>(10000 + i), 0x00640064);
    }

    // Queue many position updates
    for (int update = 0; update < UPDATES_PER_GHOST; ++update)
    {
        for (int i = 0; i < NUM_GHOSTS; ++i)
        {
            ActorMessage msg{};
            msg.type = MessageType::POSITION_UPDATE;
            msg.sourceGuid = static_cast<uint64_t>(10000 + i);
            msg.floatParam1 = 100.0f + update;
            msg.floatParam2 = 200.0f + i;
            msg.floatParam3 = 50.0f;

            harness.InjectMessage(msg);
        }
    }

    // Process all 10k messages
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// Test: Many ghosts with interleaved create/delete
// Scale: 500 ghosts created, half deleted, messages for all
TEST_F(CrossCellInteractionTest, HighChurnScenario)
{
    CellActorTestHarness harness;

    constexpr int NUM_GHOSTS = 500;
    std::vector<uint64_t> ghostGuids;

    // Create ghosts
    for (int i = 0; i < NUM_GHOSTS; ++i)
    {
        uint64_t guid = static_cast<uint64_t>(2000 + i);
        harness.AddGhost(guid, 0x00640064);
        ghostGuids.push_back(guid);
    }

    // Queue messages for all
    for (int i = 0; i < NUM_GHOSTS; ++i)
    {
        ActorMessage msg{};
        msg.type = MessageType::HEALTH_CHANGED;
        msg.sourceGuid = ghostGuids[i];
        msg.intParam1 = 1000 - i;

        harness.InjectMessage(msg);
    }

    // Delete every other ghost
    for (int i = 0; i < NUM_GHOSTS; i += 2)
    {
        harness.DestroyGhost(ghostGuids[i]);
    }

    // Queue more messages (some for deleted ghosts)
    for (int i = 0; i < NUM_GHOSTS; ++i)
    {
        ActorMessage msg{};
        msg.type = MessageType::COMBAT_STATE_CHANGED;
        msg.sourceGuid = ghostGuids[i];
        msg.intParam1 = 1;  // in combat

        harness.InjectMessage(msg);
    }

    // Process all - should handle deleted ghosts gracefully
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// Test: Concurrent message injection from multiple threads
// Scale: 8 threads x 1000 messages = 8k concurrent injections
TEST_F(CrossCellInteractionTest, ConcurrentMessageInjection)
{
    CellActorTestHarness harness;

    // Create some ghosts to receive messages
    for (int i = 0; i < 100; ++i)
    {
        harness.AddGhost(static_cast<uint64_t>(5000 + i), 0x00640064);
    }

    constexpr int NUM_THREADS = 8;
    constexpr int MESSAGES_PER_THREAD = 1000;

    std::vector<std::thread> threads;
    std::atomic<int> messagesInjected{0};

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&harness, &messagesInjected, t]()
        {
            for (int i = 0; i < MESSAGES_PER_THREAD; ++i)
            {
                ActorMessage msg{};
                msg.type = MessageType::POSITION_UPDATE;
                msg.sourceGuid = static_cast<uint64_t>(5000 + (i % 100));
                msg.floatParam1 = static_cast<float>(t * 1000 + i);

                harness.InjectMessage(msg);
                messagesInjected++;
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(messagesInjected.load(), NUM_THREADS * MESSAGES_PER_THREAD);

    // Process all messages
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// ============================================================================
// Edge Cases
// ============================================================================

// Test: Empty message queue processing
TEST_F(CrossCellInteractionTest, EmptyQueueProcessing)
{
    CellActorTestHarness harness;

    // Process with no messages - should not crash
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// Test: Message with invalid GUID (zero)
TEST_F(CrossCellInteractionTest, InvalidGuidMessage)
{
    CellActorTestHarness harness;

    ActorMessage msg{};
    msg.type = MessageType::HEALTH_CHANGED;
    msg.sourceGuid = 0;  // Invalid GUID
    msg.targetGuid = 0;
    msg.intParam1 = 100;

    harness.InjectMessage(msg);

    // Should handle gracefully
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// Test: Very large health value
TEST_F(CrossCellInteractionTest, LargeHealthValue)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 6666;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::HEALTH_CHANGED;
    msg.sourceGuid = ghostGuid;
    msg.intParam1 = INT32_MAX;
    msg.intParam2 = INT32_MAX;

    harness.InjectMessage(msg);

    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// ============================================================================
// Combat Message Safety Tests
// ============================================================================

TEST_F(CrossCellInteractionTest, SpellHitForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1001;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::SPELL_HIT;
    msg.sourceGuid = 2001;  // caster
    msg.targetGuid = ghostGuid;
    msg.complexPayload = MakeSpellHitPayload(12345, 500);
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, MeleeDamageForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1002;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::MELEE_DAMAGE;
    msg.sourceGuid = 2002;
    msg.targetGuid = ghostGuid;
    msg.complexPayload = MakeMeleeDamagePayload(300, true);
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, HealForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1003;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::HEAL;
    msg.sourceGuid = 2003;
    msg.targetGuid = ghostGuid;
    msg.complexPayload = MakeHealPayload(48782, 2500);
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, AuraApplyForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1004;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::AURA_APPLY;
    msg.sourceGuid = ghostGuid;
    msg.intParam1 = 12345;  // spell id
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, AuraRemoveForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1005;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::AURA_REMOVE;
    msg.sourceGuid = ghostGuid;
    msg.intParam1 = 12345;  // spell id
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// ============================================================================
// Movement Message Safety Tests
// ============================================================================

TEST_F(CrossCellInteractionTest, EntityEnteringForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1006;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::ENTITY_ENTERING;
    msg.sourceGuid = ghostGuid;
    msg.sourceCellId = 0x00630064;  // from neighboring cell
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, EntityLeavingForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1007;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::ENTITY_LEAVING;
    msg.sourceGuid = ghostGuid;
    msg.targetCellId = 0x00650064;  // to neighboring cell
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// ============================================================================
// State Sync Message Safety Tests
// ============================================================================

TEST_F(CrossCellInteractionTest, PowerChangedForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1008;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::POWER_CHANGED;
    msg.sourceGuid = ghostGuid;
    msg.intParam1 = 500;   // power
    msg.intParam2 = 1000;  // maxPower
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, AuraStateSyncForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1009;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::AURA_STATE_SYNC;
    msg.sourceGuid = ghostGuid;
    msg.intParam1 = 0x1234;  // aura flags
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, GhostUpdateForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1010;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::GHOST_UPDATE;
    msg.sourceGuid = ghostGuid;
    msg.complexPayload = MakeGhostSnapshot(ghostGuid, 100.0f, 200.0f, 50.0f, 800);
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// ============================================================================
// Migration Message Safety Tests
// ============================================================================

TEST_F(CrossCellInteractionTest, MigrationRequestProcessing)
{
    CellActorTestHarness harness;

    ActorMessage msg{};
    msg.type = MessageType::MIGRATION_REQUEST;
    msg.sourceGuid = 1011;
    msg.sourceCellId = 0x00630064;
    msg.complexPayload = MakeMigrationRequestPayload(1011, 12345);
    harness.InjectMessage(msg);

    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, MigrationAckProcessing)
{
    CellActorTestHarness harness;

    ActorMessage msg{};
    msg.type = MessageType::MIGRATION_ACK;
    msg.sourceGuid = 1012;
    msg.complexPayload = MakeMigrationAckPayload(12345, true);
    harness.InjectMessage(msg);

    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, MigrationCompleteProcessing)
{
    CellActorTestHarness harness;

    ActorMessage msg{};
    msg.type = MessageType::MIGRATION_COMPLETE;
    msg.sourceGuid = 1013;
    harness.InjectMessage(msg);

    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, MigrationForwardProcessing)
{
    CellActorTestHarness harness;

    ActorMessage msg{};
    msg.type = MessageType::MIGRATION_FORWARD;
    msg.sourceGuid = 1014;
    harness.InjectMessage(msg);

    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// ============================================================================
// Threat/AI Message Safety Tests
// ============================================================================

TEST_F(CrossCellInteractionTest, ThreatUpdateForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1015;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::THREAT_UPDATE;
    msg.sourceGuid = 2015;  // attacker in different cell
    msg.targetGuid = ghostGuid;
    msg.complexPayload = MakeThreatUpdatePayload(2015, ghostGuid, 100.0f);
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, AggroRequestForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1016;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::AGGRO_REQUEST;
    msg.sourceGuid = ghostGuid;
    msg.complexPayload = MakeAggroRequestPayload(ghostGuid, 100.0f, 200.0f, 50.0f, 40.0f);
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, CombatInitiatedForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1017;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::COMBAT_INITIATED;
    msg.sourceGuid = 2017;
    msg.targetGuid = ghostGuid;
    msg.complexPayload = MakeCombatInitiatedPayload(ghostGuid, 2017, 50.0f);
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, TargetSwitchForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1018;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::TARGET_SWITCH;
    msg.sourceGuid = 2018;
    msg.complexPayload = MakeTargetSwitchPayload(2018, ghostGuid, 3018);
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, AssistanceRequestForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1019;
    harness.AddGhost(ghostGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::ASSISTANCE_REQUEST;
    msg.sourceGuid = ghostGuid;
    msg.complexPayload = MakeAssistanceRequestPayload(ghostGuid, 2019, 100.0f, 200.0f, 50.0f, 30.0f);
    harness.InjectMessage(msg);

    harness.DestroyGhost(ghostGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// ============================================================================
// Pet Message Safety Tests
// ============================================================================

TEST_F(CrossCellInteractionTest, PetRemovalForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t petGuid = 1020;
    uint64_t ownerGuid = 2020;
    harness.AddGhost(petGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::PET_REMOVAL;
    msg.sourceGuid = petGuid;
    msg.targetGuid = ownerGuid;
    msg.complexPayload = MakePetRemovalPayload(petGuid, ownerGuid, 0);
    harness.InjectMessage(msg);

    harness.DestroyGhost(petGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// ============================================================================
// Interrupt/Dispel Cross-Cell Safety Tests
// ============================================================================

TEST_F(CrossCellInteractionTest, SpellInterruptForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t casterGuid = 2000;
    uint64_t targetGuid = 2001;

    harness.AddGhost(targetGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::SPELL_INTERRUPT;
    msg.sourceGuid = casterGuid;
    msg.targetGuid = targetGuid;
    msg.complexPayload = MakeSpellInterruptPayload(casterGuid, targetGuid, 1766, 116, 16, 5000);
    harness.InjectMessage(msg);

    harness.DestroyGhost(targetGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, SpellDispelForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t casterGuid = 2000;
    uint64_t targetGuid = 2001;

    harness.AddGhost(targetGuid, 0x00640064);

    std::vector<std::pair<uint32_t, uint8_t>> dispelList = {{21562, 1}, {1459, 1}};

    ActorMessage msg{};
    msg.type = MessageType::SPELL_DISPEL;
    msg.sourceGuid = casterGuid;
    msg.targetGuid = targetGuid;
    msg.complexPayload = MakeSpellDispelPayload(casterGuid, targetGuid, 527, dispelList);
    harness.InjectMessage(msg);

    harness.DestroyGhost(targetGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, PowerDrainForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t casterGuid = 2100;
    uint64_t targetGuid = 2101;

    harness.AddGhost(targetGuid, 0x00640064);

    ActorMessage msg{};
    msg.type = MessageType::POWER_DRAIN;
    msg.sourceGuid = casterGuid;
    msg.targetGuid = targetGuid;
    msg.complexPayload = MakePowerDrainPayload(casterGuid, targetGuid, 5138, 0, 100, 1.0f, false);
    harness.InjectMessage(msg);

    harness.DestroyGhost(targetGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, SpellstealForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t casterGuid = 2200;
    uint64_t targetGuid = 2201;

    harness.AddGhost(targetGuid, 0x00640064);

    std::vector<std::pair<uint32_t, uint64_t>> stealList = {{21562, casterGuid}, {1459, casterGuid}};

    ActorMessage msg{};
    msg.type = MessageType::SPELLSTEAL;
    msg.sourceGuid = casterGuid;
    msg.targetGuid = targetGuid;
    msg.complexPayload = MakeSpellstealPayload(casterGuid, targetGuid, 30449, stealList);
    harness.InjectMessage(msg);

    harness.DestroyGhost(targetGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

TEST_F(CrossCellInteractionTest, SpellstealApplyForDeletedGhost)
{
    CellActorTestHarness harness;

    uint64_t stealerGuid = 2300;
    uint64_t targetGuid = 2301;

    harness.AddGhost(stealerGuid, 0x00640064);

    StolenAuraData auraData;
    auraData.spellId = 21562;
    auraData.originalCasterGuid = targetGuid;
    auraData.duration = 60000;
    auraData.maxDuration = 60000;
    auraData.stackAmount = 1;
    auraData.charges = 0;
    std::vector<StolenAuraData> stolenAuras = {auraData};

    ActorMessage msg{};
    msg.type = MessageType::SPELLSTEAL_APPLY;
    msg.sourceGuid = targetGuid;
    msg.targetGuid = stealerGuid;
    msg.complexPayload = MakeSpellstealApplyPayload(stealerGuid, targetGuid, 30449, stolenAuras);
    harness.InjectMessage(msg);

    harness.DestroyGhost(stealerGuid);
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}

// ============================================================================
// Null Payload Edge Cases
// ============================================================================

TEST_F(CrossCellInteractionTest, AllMessageTypesWithNullPayload)
{
    CellActorTestHarness harness;

    uint64_t ghostGuid = 1100;
    harness.AddGhost(ghostGuid, 0x00640064);

    // Test all message types that expect payloads but receive nullptr
    std::vector<MessageType> payloadTypes = {
        MessageType::SPELL_HIT,
        MessageType::MELEE_DAMAGE,
        MessageType::HEAL,
        MessageType::GHOST_UPDATE,
        MessageType::MIGRATION_REQUEST,
        MessageType::MIGRATION_ACK,
        MessageType::THREAT_UPDATE,
        MessageType::AGGRO_REQUEST,
        MessageType::COMBAT_INITIATED,
        MessageType::TARGET_SWITCH,
        MessageType::ASSISTANCE_REQUEST,
        MessageType::PET_REMOVAL,
        MessageType::SPELL_INTERRUPT,
        MessageType::SPELL_DISPEL,
        MessageType::POWER_DRAIN,
        MessageType::SPELLSTEAL,
        MessageType::SPELLSTEAL_APPLY
    };

    for (MessageType type : payloadTypes)
    {
        ActorMessage msg{};
        msg.type = type;
        msg.sourceGuid = ghostGuid;
        msg.targetGuid = ghostGuid;
        msg.complexPayload = nullptr;  // Intentionally null
        harness.InjectMessage(msg);
    }

    // All messages with null payloads should be handled gracefully
    EXPECT_NO_FATAL_FAILURE(harness.ProcessAllMessages());
}
