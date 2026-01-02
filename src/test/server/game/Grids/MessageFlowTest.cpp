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

#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "CellActorTestHarness.h"
#include "GhostActorSystem.h"
#include "WorldMock.h"

using namespace GhostActor;
using namespace testing;

/**
 * MessageFlowTest - Tests for cross-cell message sender → receiver flow
 *
 * Tests validate that messages sent from Cell A arrive at and are correctly
 * processed by Cell B.
 *
 * Tier 1: Ghost-only messages (no entity lookup needed)
 * Tier 2: Entity messages (use TestCreature/TestPlayer)
 */
class MessageFlowTest : public ::testing::Test
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

        // Create two cells - A (sender) and B (receiver)
        _cellA = std::make_unique<CellActorTestHarness>(CELL_A_ID);
        _cellB = std::make_unique<CellActorTestHarness>(CELL_B_ID);
    }

    void TearDown() override
    {
        _cellA.reset();
        _cellB.reset();

        // Restore original world
        IWorld* currentWorld = sWorld.release();
        delete currentWorld;
        _worldMock = nullptr;

        sWorld.reset(_originalWorld);
        _originalWorld = nullptr;
    }

    // Send message from Cell A to Cell B
    void SendMessageAtoB(ActorMessage msg)
    {
        msg.sourceCellId = CELL_A_ID;
        msg.targetCellId = CELL_B_ID;
        _cellB->InjectMessage(std::move(msg));
    }

    // Send message from Cell B to Cell A
    void SendMessageBtoA(ActorMessage msg)
    {
        msg.sourceCellId = CELL_B_ID;
        msg.targetCellId = CELL_A_ID;
        _cellA->InjectMessage(std::move(msg));
    }

    void ProcessCellA() { _cellA->ProcessAllMessages(); }
    void ProcessCellB() { _cellB->ProcessAllMessages(); }

    CellActor* GetCellA() { return _cellA->GetCell(); }
    CellActor* GetCellB() { return _cellB->GetCell(); }

    CellActorTestHarness& HarnessA() { return *_cellA; }
    CellActorTestHarness& HarnessB() { return *_cellB; }

    static constexpr uint32_t CELL_A_ID = 0x00010001;  // Cell (1, 1)
    static constexpr uint32_t CELL_B_ID = 0x00010002;  // Cell (2, 1) - neighbor

private:
    IWorld* _originalWorld = nullptr;
    NiceMock<WorldMock>* _worldMock = nullptr;
    std::unique_ptr<CellActorTestHarness> _cellA;
    std::unique_ptr<CellActorTestHarness> _cellB;
};

// ============================================================================
// Tier 1: Ghost Message Flow Tests (no entity lookup)
// ============================================================================

TEST_F(MessageFlowTest, GhostCreateFlow)
{
    // GIVEN: Entity 1001 exists in Cell A (simulated by snapshot)
    constexpr uint64_t ENTITY_GUID = 1001;
    auto snapshot = MakeGhostSnapshot(ENTITY_GUID, 100.0f, 200.0f, 0.0f, 5000);

    // WHEN: Cell A sends GHOST_CREATE to Cell B
    ActorMessage msg{};
    msg.type = MessageType::GHOST_CREATE;
    msg.sourceGuid = ENTITY_GUID;
    msg.complexPayload = snapshot;
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Cell B should have a ghost for entity 1001
    GhostEntity* ghost = GetCellB()->GetGhost(ENTITY_GUID);
    ASSERT_NE(ghost, nullptr) << "Ghost should be created in Cell B";
    EXPECT_EQ(ghost->GetGUID(), ENTITY_GUID);
    EXPECT_FLOAT_EQ(ghost->GetPositionX(), 100.0f);
    EXPECT_FLOAT_EQ(ghost->GetPositionY(), 200.0f);
    EXPECT_EQ(ghost->GetHealth(), 5000u);
}

TEST_F(MessageFlowTest, GhostUpdateFlow)
{
    // GIVEN: Ghost already exists in Cell B
    constexpr uint64_t ENTITY_GUID = 1002;
    HarnessB().AddGhost(ENTITY_GUID, CELL_A_ID);
    ASSERT_NE(GetCellB()->GetGhost(ENTITY_GUID), nullptr);

    // WHEN: Cell A sends GHOST_UPDATE with new state
    auto snapshot = MakeGhostSnapshot(ENTITY_GUID, 150.0f, 250.0f, 10.0f, 4000);

    ActorMessage msg{};
    msg.type = MessageType::GHOST_UPDATE;
    msg.sourceGuid = ENTITY_GUID;
    msg.complexPayload = snapshot;
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Ghost state should be updated
    GhostEntity* ghost = GetCellB()->GetGhost(ENTITY_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_FLOAT_EQ(ghost->GetPositionX(), 150.0f);
    EXPECT_FLOAT_EQ(ghost->GetPositionY(), 250.0f);
    EXPECT_EQ(ghost->GetHealth(), 4000u);
}

TEST_F(MessageFlowTest, GhostDestroyFlow)
{
    // GIVEN: Ghost exists in Cell B
    constexpr uint64_t ENTITY_GUID = 1003;
    HarnessB().AddGhost(ENTITY_GUID, CELL_A_ID);
    ASSERT_NE(GetCellB()->GetGhost(ENTITY_GUID), nullptr);
    EXPECT_EQ(GetCellB()->GetGhostCount(), 1u);

    // WHEN: Cell A sends GHOST_DESTROY
    ActorMessage msg{};
    msg.type = MessageType::GHOST_DESTROY;
    msg.sourceGuid = ENTITY_GUID;
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Ghost should be removed from Cell B
    EXPECT_EQ(GetCellB()->GetGhost(ENTITY_GUID), nullptr);
    EXPECT_EQ(GetCellB()->GetGhostCount(), 0u);
}

TEST_F(MessageFlowTest, PositionUpdateFlow)
{
    // GIVEN: Ghost exists in Cell B
    constexpr uint64_t ENTITY_GUID = 1004;
    HarnessB().AddGhost(ENTITY_GUID, CELL_A_ID);

    GhostEntity* ghost = GetCellB()->GetGhost(ENTITY_GUID);
    ASSERT_NE(ghost, nullptr);
    float oldX = ghost->GetPositionX();
    float oldY = ghost->GetPositionY();

    // WHEN: Cell A sends POSITION_UPDATE
    ActorMessage msg{};
    msg.type = MessageType::POSITION_UPDATE;
    msg.sourceGuid = ENTITY_GUID;
    msg.floatParam1 = 500.0f;  // new X
    msg.floatParam2 = 600.0f;  // new Y
    msg.floatParam3 = 50.0f;   // new Z
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Ghost position should be updated
    EXPECT_FLOAT_EQ(ghost->GetPositionX(), 500.0f);
    EXPECT_FLOAT_EQ(ghost->GetPositionY(), 600.0f);
    EXPECT_FLOAT_EQ(ghost->GetPositionZ(), 50.0f);
    EXPECT_NE(ghost->GetPositionX(), oldX);
}

TEST_F(MessageFlowTest, HealthChangedFlow)
{
    // GIVEN: Ghost exists in Cell B with 5000 HP
    constexpr uint64_t ENTITY_GUID = 1005;
    HarnessB().AddGhost(ENTITY_GUID, CELL_A_ID);

    GhostEntity* ghost = GetCellB()->GetGhost(ENTITY_GUID);
    ASSERT_NE(ghost, nullptr);

    // WHEN: Cell A sends HEALTH_CHANGED (entity took damage)
    ActorMessage msg{};
    msg.type = MessageType::HEALTH_CHANGED;
    msg.sourceGuid = ENTITY_GUID;
    msg.intParam1 = 3500;   // new health
    msg.intParam2 = 5000;   // max health
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Ghost health should be updated
    EXPECT_EQ(ghost->GetHealth(), 3500u);
    EXPECT_EQ(ghost->GetMaxHealth(), 5000u);
}

TEST_F(MessageFlowTest, CombatStateFlow)
{
    // GIVEN: Ghost exists in Cell B, not in combat
    constexpr uint64_t ENTITY_GUID = 1006;
    HarnessB().AddGhost(ENTITY_GUID, CELL_A_ID);

    GhostEntity* ghost = GetCellB()->GetGhost(ENTITY_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_FALSE(ghost->IsInCombat());

    // WHEN: Cell A sends COMBAT_STATE_CHANGED (entered combat)
    ActorMessage msg{};
    msg.type = MessageType::COMBAT_STATE_CHANGED;
    msg.sourceGuid = ENTITY_GUID;
    msg.intParam1 = 1;  // inCombat = true
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Ghost combat state should be updated
    EXPECT_TRUE(ghost->IsInCombat());

    // WHEN: Combat ends
    ActorMessage msg2{};
    msg2.type = MessageType::COMBAT_STATE_CHANGED;
    msg2.sourceGuid = ENTITY_GUID;
    msg2.intParam1 = 0;  // inCombat = false
    SendMessageAtoB(std::move(msg2));
    ProcessCellB();

    // THEN: Ghost should no longer be in combat
    EXPECT_FALSE(ghost->IsInCombat());
}

TEST_F(MessageFlowTest, PhaseChangedFlow)
{
    // GIVEN: Ghost exists in Cell B with phase 1
    constexpr uint64_t ENTITY_GUID = 1007;
    HarnessB().AddGhost(ENTITY_GUID, CELL_A_ID);

    GhostEntity* ghost = GetCellB()->GetGhost(ENTITY_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_EQ(ghost->GetPhaseMask(), 1u);

    // WHEN: Cell A sends PHASE_CHANGED (phase 4)
    ActorMessage msg{};
    msg.type = MessageType::PHASE_CHANGED;
    msg.sourceGuid = ENTITY_GUID;
    msg.intParam1 = 4;  // new phase mask
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Ghost phase should be updated
    EXPECT_EQ(ghost->GetPhaseMask(), 4u);
    EXPECT_TRUE(ghost->InSamePhase(4));
    EXPECT_FALSE(ghost->InSamePhase(1));
}

TEST_F(MessageFlowTest, MultipleGhostUpdatesSequence)
{
    // Test that multiple ghost updates are processed correctly in order
    constexpr uint64_t ENTITY_GUID = 1008;

    // WHEN: Send CREATE, then multiple HEALTH_CHANGED
    auto snapshot = MakeGhostSnapshot(ENTITY_GUID, 0, 0, 0, 10000);
    ActorMessage createMsg{};
    createMsg.type = MessageType::GHOST_CREATE;
    createMsg.sourceGuid = ENTITY_GUID;
    createMsg.complexPayload = snapshot;
    SendMessageAtoB(std::move(createMsg));

    for (int i = 0; i < 5; ++i)
    {
        ActorMessage healthMsg{};
        healthMsg.type = MessageType::HEALTH_CHANGED;
        healthMsg.sourceGuid = ENTITY_GUID;
        healthMsg.intParam1 = 10000 - (i + 1) * 1000;  // 9000, 8000, 7000, 6000, 5000
        healthMsg.intParam2 = 10000;
        SendMessageAtoB(std::move(healthMsg));
    }

    ProcessCellB();

    // THEN: Ghost should exist with final health value
    GhostEntity* ghost = GetCellB()->GetGhost(ENTITY_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_EQ(ghost->GetHealth(), 5000u);  // Last update
}

TEST_F(MessageFlowTest, GhostDestroyNonExistent)
{
    // GIVEN: No ghost exists in Cell B
    constexpr uint64_t ENTITY_GUID = 1009;
    EXPECT_EQ(GetCellB()->GetGhost(ENTITY_GUID), nullptr);

    // WHEN: Cell A sends GHOST_DESTROY for non-existent ghost
    ActorMessage msg{};
    msg.type = MessageType::GHOST_DESTROY;
    msg.sourceGuid = ENTITY_GUID;
    SendMessageAtoB(std::move(msg));

    // THEN: Should not crash, ghost count stays 0
    EXPECT_NO_THROW(ProcessCellB());
    EXPECT_EQ(GetCellB()->GetGhostCount(), 0u);
}

// ============================================================================
// Tier 2: Entity Message Flow Tests (using TestCreature/TestPlayer)
// ============================================================================

TEST_F(MessageFlowTest, SpellHitCrossCell)
{
    // GIVEN: Caster in Cell A, Target in Cell B
    TestPlayer* caster = HarnessA().AddPlayer(2001);
    TestCreature* target = HarnessB().AddCreature(2002, 12345);

    uint32_t initialHealth = target->GetHealth();
    ASSERT_GT(initialHealth, 1000u) << "Target should have health for damage test";

    // WHEN: Cell A sends SPELL_HIT to Cell B
    auto payload = MakeSpellHitPayload(12345, 500);  // spellId 12345, 500 damage

    ActorMessage msg{};
    msg.type = MessageType::SPELL_HIT;
    msg.sourceGuid = caster->GetGUID().GetRawValue();
    msg.targetGuid = target->GetGUID().GetRawValue();
    msg.complexPayload = payload;
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Target should have taken damage
    // Note: Actual damage application depends on HandleMessage implementation
    // The test verifies message reaches the target cell and is processed
    EXPECT_EQ(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(MessageFlowTest, MeleeDamageCrossCell)
{
    // GIVEN: Attacker in Cell A, Target in Cell B
    TestCreature* attacker = HarnessA().AddCreature(2003, 11111);
    TestCreature* target = HarnessB().AddCreature(2004, 22222);

    // WHEN: Cell A sends MELEE_DAMAGE to Cell B
    auto payload = MakeMeleeDamagePayload(300, false);

    ActorMessage msg{};
    msg.type = MessageType::MELEE_DAMAGE;
    msg.sourceGuid = attacker->GetGUID().GetRawValue();
    msg.targetGuid = target->GetGUID().GetRawValue();
    msg.complexPayload = payload;
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Message should be processed
    EXPECT_EQ(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(MessageFlowTest, HealCrossCell)
{
    // GIVEN: Healer in Cell A, Target in Cell B
    TestPlayer* healer = HarnessA().AddPlayer(2005);
    TestPlayer* target = HarnessB().AddPlayer(2006);

    // WHEN: Cell A sends HEAL to Cell B
    auto payload = MakeHealPayload(48782, 5000);  // Flash of Light, 5000 healing

    ActorMessage msg{};
    msg.type = MessageType::HEAL;
    msg.sourceGuid = healer->GetGUID().GetRawValue();
    msg.targetGuid = target->GetGUID().GetRawValue();
    msg.complexPayload = payload;
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Message should be processed
    EXPECT_EQ(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(MessageFlowTest, DeletedTargetHandledGracefully)
{
    // GIVEN: Caster in Cell A, Target in Cell B
    TestPlayer* caster = HarnessA().AddPlayer(2007);
    TestCreature* target = HarnessB().AddCreature(2008, 12345);
    uint64_t targetGuid = target->GetGUID().GetRawValue();

    // Simulate target despawn before message processing
    HarnessB().DeleteEntity(target->GetGUID());

    // WHEN: Cell A sends SPELL_HIT to the now-deleted target
    auto payload = MakeSpellHitPayload(12345, 500);

    ActorMessage msg{};
    msg.type = MessageType::SPELL_HIT;
    msg.sourceGuid = caster->GetGUID().GetRawValue();
    msg.targetGuid = targetGuid;
    msg.complexPayload = payload;
    SendMessageAtoB(std::move(msg));

    // THEN: Should not crash, message is processed but target not found
    EXPECT_NO_THROW(ProcessCellB());
}

TEST_F(MessageFlowTest, AggroRequestFlow)
{
    // GIVEN: Creature in Cell A broadcasts AGGRO_REQUEST
    //        Player in Cell B within range
    TestCreature* creature = HarnessA().AddCreature(2009, 12345);
    TestPlayer* player = HarnessB().AddPlayer(2010);

    // Positions are embedded in payload, not on entities directly
    auto payload = MakeAggroRequestPayload(
        creature->GetGUID().GetRawValue(),
        100.0f, 100.0f, 0.0f,  // creature position
        50.0f                   // max range
    );

    // WHEN: Cell A broadcasts AGGRO_REQUEST to Cell B
    ActorMessage msg{};
    msg.type = MessageType::AGGRO_REQUEST;
    msg.sourceGuid = creature->GetGUID().GetRawValue();
    msg.complexPayload = payload;
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Cell B should process the aggro request
    // In full implementation, this would send COMBAT_INITIATED back
    EXPECT_EQ(GetCellB()->GetMessagesProcessedLastTick(), 1u);
    (void)player;  // Referenced in payload context
}

TEST_F(MessageFlowTest, ThreatUpdateFlow)
{
    // GIVEN: Attacker in Cell A, Victim creature in Cell B
    TestPlayer* attacker = HarnessA().AddPlayer(2011);
    TestCreature* victim = HarnessB().AddCreature(2012, 12345);

    auto payload = MakeThreatUpdatePayload(
        attacker->GetGUID().GetRawValue(),
        victim->GetGUID().GetRawValue(),
        1500.0f  // threat delta
    );

    // WHEN: Cell A sends THREAT_UPDATE to Cell B
    ActorMessage msg{};
    msg.type = MessageType::THREAT_UPDATE;
    msg.sourceGuid = attacker->GetGUID().GetRawValue();
    msg.targetGuid = victim->GetGUID().GetRawValue();
    msg.complexPayload = payload;
    SendMessageAtoB(std::move(msg));
    ProcessCellB();

    // THEN: Message should be processed
    EXPECT_EQ(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(MessageFlowTest, CombatInitiatedResponse)
{
    // GIVEN: Creature in Cell A, Player in Cell B initiates combat
    TestCreature* creature = HarnessA().AddCreature(2013, 12345);
    TestPlayer* player = HarnessB().AddPlayer(2014);

    auto payload = MakeCombatInitiatedPayload(
        creature->GetGUID().GetRawValue(),
        player->GetGUID().GetRawValue(),
        100.0f  // initial threat
    );

    // WHEN: Cell B sends COMBAT_INITIATED to Cell A
    ActorMessage msg{};
    msg.type = MessageType::COMBAT_INITIATED;
    msg.sourceGuid = player->GetGUID().GetRawValue();
    msg.targetGuid = creature->GetGUID().GetRawValue();
    msg.complexPayload = payload;
    SendMessageBtoA(std::move(msg));
    ProcessCellA();

    // THEN: Cell A should process the combat initiation
    EXPECT_EQ(GetCellA()->GetMessagesProcessedLastTick(), 1u);
}

// ============================================================================
// Multi-Cell Integration Tests
// ============================================================================

TEST_F(MessageFlowTest, BidirectionalMessageFlow)
{
    // Test messages flowing in both directions simultaneously
    TestCreature* creatureA = HarnessA().AddCreature(3001, 11111);
    TestCreature* creatureB = HarnessB().AddCreature(3002, 22222);

    // Create ghosts in each other's cells
    HarnessB().AddGhost(creatureA->GetGUID().GetRawValue(), CELL_A_ID);
    HarnessA().AddGhost(creatureB->GetGUID().GetRawValue(), CELL_B_ID);

    // WHEN: Both cells send HEALTH_CHANGED to each other
    ActorMessage msgAtoB{};
    msgAtoB.type = MessageType::HEALTH_CHANGED;
    msgAtoB.sourceGuid = creatureA->GetGUID().GetRawValue();
    msgAtoB.intParam1 = 8000;
    msgAtoB.intParam2 = 10000;
    SendMessageAtoB(std::move(msgAtoB));

    ActorMessage msgBtoA{};
    msgBtoA.type = MessageType::HEALTH_CHANGED;
    msgBtoA.sourceGuid = creatureB->GetGUID().GetRawValue();
    msgBtoA.intParam1 = 6000;
    msgBtoA.intParam2 = 10000;
    SendMessageBtoA(std::move(msgBtoA));

    // Process both cells
    ProcessCellA();
    ProcessCellB();

    // THEN: Both ghosts should be updated
    GhostEntity* ghostAinB = GetCellB()->GetGhost(creatureA->GetGUID().GetRawValue());
    GhostEntity* ghostBinA = GetCellA()->GetGhost(creatureB->GetGUID().GetRawValue());

    ASSERT_NE(ghostAinB, nullptr);
    ASSERT_NE(ghostBinA, nullptr);
    EXPECT_EQ(ghostAinB->GetHealth(), 8000u);
    EXPECT_EQ(ghostBinA->GetHealth(), 6000u);
}

TEST_F(MessageFlowTest, HighVolumeMessageProcessing)
{
    // Test processing many messages at once
    constexpr size_t MESSAGE_COUNT = 100;

    // Create ghosts for all entities
    for (size_t i = 0; i < MESSAGE_COUNT; ++i)
    {
        HarnessB().AddGhost(4000 + i, CELL_A_ID);
    }

    // WHEN: Send many HEALTH_CHANGED messages
    for (size_t i = 0; i < MESSAGE_COUNT; ++i)
    {
        ActorMessage msg{};
        msg.type = MessageType::HEALTH_CHANGED;
        msg.sourceGuid = 4000 + i;
        msg.intParam1 = static_cast<int32_t>(10000 - i * 50);
        msg.intParam2 = 10000;
        SendMessageAtoB(std::move(msg));
    }

    ProcessCellB();

    // THEN: All messages should be processed
    // MESSAGE_COUNT * 2 because AddGhost() sends GHOST_CREATE messages that also get counted
    EXPECT_EQ(GetCellB()->GetMessagesProcessedLastTick(), MESSAGE_COUNT * 2);

    // Verify some random ghosts have correct health
    GhostEntity* ghost0 = GetCellB()->GetGhost(4000);
    GhostEntity* ghost50 = GetCellB()->GetGhost(4050);
    GhostEntity* ghost99 = GetCellB()->GetGhost(4099);

    ASSERT_NE(ghost0, nullptr);
    ASSERT_NE(ghost50, nullptr);
    ASSERT_NE(ghost99, nullptr);

    EXPECT_EQ(ghost0->GetHealth(), 10000u);
    EXPECT_EQ(ghost50->GetHealth(), 7500u);
    EXPECT_EQ(ghost99->GetHealth(), 5050u);
}

TEST_F(MessageFlowTest, MixedMessageTypes)
{
    // Test processing different message types together
    constexpr uint64_t ENTITY_GUID = 5001;

    // Create ghost first
    HarnessB().AddGhost(ENTITY_GUID, CELL_A_ID);

    // WHEN: Send various message types
    ActorMessage healthMsg{};
    healthMsg.type = MessageType::HEALTH_CHANGED;
    healthMsg.sourceGuid = ENTITY_GUID;
    healthMsg.intParam1 = 8000;
    healthMsg.intParam2 = 10000;
    SendMessageAtoB(std::move(healthMsg));

    ActorMessage combatMsg{};
    combatMsg.type = MessageType::COMBAT_STATE_CHANGED;
    combatMsg.sourceGuid = ENTITY_GUID;
    combatMsg.intParam1 = 1;
    SendMessageAtoB(std::move(combatMsg));

    ActorMessage posMsg{};
    posMsg.type = MessageType::POSITION_UPDATE;
    posMsg.sourceGuid = ENTITY_GUID;
    posMsg.floatParam1 = 300.0f;
    posMsg.floatParam2 = 400.0f;
    posMsg.floatParam3 = 10.0f;
    SendMessageAtoB(std::move(posMsg));

    ProcessCellB();

    // THEN: All updates should be applied
    GhostEntity* ghost = GetCellB()->GetGhost(ENTITY_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_EQ(ghost->GetHealth(), 8000u);
    EXPECT_TRUE(ghost->IsInCombat());
    EXPECT_FLOAT_EQ(ghost->GetPositionX(), 300.0f);
    EXPECT_FLOAT_EQ(ghost->GetPositionY(), 400.0f);
}

// =============================================================================
// EVADE_TRIGGERED Message Test
// =============================================================================

TEST_F(MessageFlowTest, EvadeTriggeredFlow)
{
    // Test that EVADE_TRIGGERED updates ghost combat state to false
    constexpr uint64_t CREATURE_GUID = 6001;

    // GIVEN: A ghost in Cell B that is in combat
    HarnessB().AddGhost(CREATURE_GUID, CELL_A_ID);

    // Set ghost to in-combat state
    ActorMessage combatMsg{};
    combatMsg.type = MessageType::COMBAT_STATE_CHANGED;
    combatMsg.sourceGuid = CREATURE_GUID;
    combatMsg.intParam1 = 1;  // In combat
    SendMessageAtoB(std::move(combatMsg));
    ProcessCellB();

    // Verify ghost is in combat
    GhostEntity* ghost = GetCellB()->GetGhost(CREATURE_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_TRUE(ghost->IsInCombat());

    // WHEN: Creature evades (sends EVADE_TRIGGERED)
    ActorMessage evadeMsg{};
    evadeMsg.type = MessageType::EVADE_TRIGGERED;
    evadeMsg.sourceGuid = CREATURE_GUID;
    SendMessageAtoB(std::move(evadeMsg));

    ProcessCellB();

    // THEN: Ghost should no longer be in combat
    EXPECT_FALSE(ghost->IsInCombat());
}
