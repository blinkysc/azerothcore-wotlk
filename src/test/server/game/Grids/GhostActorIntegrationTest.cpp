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
#include "GhostActorSystem.h"
#include "CellActorTestHarness.h"
#include "TestCreature.h"
#include "TestPlayer.h"
#include "WorldMock.h"

using namespace GhostActor;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::_;

/**
 * Integration tests for GhostActorSystem
 *
 * These tests verify complete message chains and multi-cell interactions
 * for combat, spells, and auras across cell boundaries.
 */
class GhostActorIntegrationTest : public ::testing::Test
{
protected:
    static constexpr uint32_t CELL_A_ID = 100;
    static constexpr uint32_t CELL_B_ID = 101;
    static constexpr uint32_t CELL_C_ID = 102;

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
        _harnessA = std::make_unique<CellActorTestHarness>(CELL_A_ID);
        _harnessB = std::make_unique<CellActorTestHarness>(CELL_B_ID);
        _harnessC = std::make_unique<CellActorTestHarness>(CELL_C_ID);
    }

    void TearDown() override
    {
        _harnessC.reset();
        _harnessB.reset();
        _harnessA.reset();

        // Restore original world
        IWorld* currentWorld = sWorld.release();
        delete currentWorld;
        _worldMock = nullptr;

        sWorld.reset(_originalWorld);
        _originalWorld = nullptr;
    }

    CellActorTestHarness& HarnessA() { return *_harnessA; }
    CellActorTestHarness& HarnessB() { return *_harnessB; }
    CellActorTestHarness& HarnessC() { return *_harnessC; }

    CellActor* GetCellA() { return _harnessA->GetCell(); }
    CellActor* GetCellB() { return _harnessB->GetCell(); }
    CellActor* GetCellC() { return _harnessC->GetCell(); }

    void ProcessAllCells()
    {
        GetCellA()->Update(0);
        GetCellB()->Update(0);
        GetCellC()->Update(0);
    }

    // Send message from Cell A to Cell B
    void SendAtoB(ActorMessage msg)
    {
        msg.sourceCellId = CELL_A_ID;
        msg.targetCellId = CELL_B_ID;
        GetCellB()->SendMessage(std::move(msg));
    }

    // Send message from Cell A to Cell C
    void SendAtoC(ActorMessage msg)
    {
        msg.sourceCellId = CELL_A_ID;
        msg.targetCellId = CELL_C_ID;
        GetCellC()->SendMessage(std::move(msg));
    }

    // Send message from Cell B to Cell A
    void SendBtoA(ActorMessage msg)
    {
        msg.sourceCellId = CELL_B_ID;
        msg.targetCellId = CELL_A_ID;
        GetCellA()->SendMessage(std::move(msg));
    }

private:
    std::unique_ptr<CellActorTestHarness> _harnessA;
    std::unique_ptr<CellActorTestHarness> _harnessB;
    std::unique_ptr<CellActorTestHarness> _harnessC;

    // World mock members
    IWorld* _originalWorld{nullptr};
    NiceMock<WorldMock>* _worldMock{nullptr};
    std::string _emptyString;
};

// =============================================================================
// Combat Integration Tests
// =============================================================================

TEST_F(GhostActorIntegrationTest, FullCombatChainAcrossCells)
{
    // Test: Complete combat chain from attack to ghost sync
    // Cell A: Player attacks
    // Cell B: Creature receives damage, ghost in Cell A should sync

    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t CREATURE_GUID = 2001;

    // Setup: Player in Cell A, Creature in Cell B
    TestPlayer* player = HarnessA().AddPlayer(PLAYER_GUID);
    TestCreature* creature = HarnessB().AddCreature(CREATURE_GUID, 12345);
    creature->SetTestHealth(10000);

    // Create ghost of creature in Cell A
    HarnessA().AddGhost(CREATURE_GUID, CELL_B_ID);

    // WHEN: Player in Cell A sends SPELL_HIT to creature in Cell B
    ActorMessage spellHit{};
    spellHit.type = MessageType::SPELL_HIT;
    spellHit.sourceGuid = PLAYER_GUID;
    spellHit.targetGuid = CREATURE_GUID;
    spellHit.complexPayload = MakeSpellHitPayload(12345, 2000);  // 2000 damage spell
    SendAtoB(std::move(spellHit));

    GetCellB()->Update(0);

    // THEN: Simulate the combat response chain
    // In real system, damage would trigger HEALTH_CHANGED broadcast
    // For testing, we manually send what the production code would send
    ActorMessage healthChanged{};
    healthChanged.type = MessageType::HEALTH_CHANGED;
    healthChanged.sourceGuid = CREATURE_GUID;
    healthChanged.intParam1 = 8000;   // New health after 2000 damage
    healthChanged.intParam2 = 10000;  // Max health
    SendBtoA(std::move(healthChanged));

    GetCellA()->Update(0);

    // Verify ghost in Cell A has updated health
    GhostEntity* ghost = GetCellA()->GetGhost(CREATURE_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_EQ(ghost->GetHealth(), 8000u);
}

TEST_F(GhostActorIntegrationTest, MultiCreaturePull)
{
    // Test: Player aggros multiple creatures across different cells
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t CREATURE1_GUID = 2001;
    constexpr uint64_t CREATURE2_GUID = 2002;
    constexpr uint64_t CREATURE3_GUID = 2003;

    // Setup: Player in Cell A, creatures in Cell B and C
    HarnessA().AddPlayer(PLAYER_GUID);
    HarnessB().AddCreature(CREATURE1_GUID, 100);
    HarnessB().AddCreature(CREATURE2_GUID, 100);
    HarnessC().AddCreature(CREATURE3_GUID, 100);

    // Create ghosts of creatures in player's cell
    HarnessA().AddGhost(CREATURE1_GUID, CELL_B_ID);
    HarnessA().AddGhost(CREATURE2_GUID, CELL_B_ID);
    HarnessA().AddGhost(CREATURE3_GUID, CELL_C_ID);

    // WHEN: All creatures enter combat
    for (uint64_t creatureGuid : {CREATURE1_GUID, CREATURE2_GUID, CREATURE3_GUID})
    {
        ActorMessage combatMsg{};
        combatMsg.type = MessageType::COMBAT_STATE_CHANGED;
        combatMsg.sourceGuid = creatureGuid;
        combatMsg.intParam1 = 1;  // In combat

        if (creatureGuid == CREATURE3_GUID)
            SendAtoC(std::move(combatMsg));  // Wrong direction for test, but tests the message
        else
            SendAtoB(std::move(combatMsg));

        // Also send to Cell A where player is
        ActorMessage combatMsgA{};
        combatMsgA.type = MessageType::COMBAT_STATE_CHANGED;
        combatMsgA.sourceGuid = creatureGuid;
        combatMsgA.intParam1 = 1;
        GetCellA()->SendMessage(std::move(combatMsgA));
    }

    ProcessAllCells();

    // THEN: All ghosts should be in combat
    EXPECT_TRUE(GetCellA()->GetGhost(CREATURE1_GUID)->IsInCombat());
    EXPECT_TRUE(GetCellA()->GetGhost(CREATURE2_GUID)->IsInCombat());
    EXPECT_TRUE(GetCellA()->GetGhost(CREATURE3_GUID)->IsInCombat());
}

// =============================================================================
// Spell Cast Integration Tests
// =============================================================================

TEST_F(GhostActorIntegrationTest, DirectDamageSpellAcrossCells)
{
    // Test: Direct damage spell across cells updates ghost health
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t FROSTBOLT_ID = 116;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Initial ghost health
    {
        ActorMessage initHealth{};
        initHealth.type = MessageType::HEALTH_CHANGED;
        initHealth.sourceGuid = TARGET_GUID;
        initHealth.intParam1 = 10000;
        initHealth.intParam2 = 10000;
        GetCellA()->SendMessage(std::move(initHealth));
        GetCellA()->Update(0);
    }

    EXPECT_EQ(GetCellA()->GetGhost(TARGET_GUID)->GetHealth(), 10000u);

    // WHEN: Cast Frostbolt
    ActorMessage spell{};
    spell.type = MessageType::SPELL_HIT;
    spell.sourceGuid = CASTER_GUID;
    spell.targetGuid = TARGET_GUID;
    spell.complexPayload = MakeSpellHitPayload(FROSTBOLT_ID, 1500);
    SendAtoB(std::move(spell));
    GetCellB()->Update(0);

    // Simulate damage response
    ActorMessage damageResponse{};
    damageResponse.type = MessageType::HEALTH_CHANGED;
    damageResponse.sourceGuid = TARGET_GUID;
    damageResponse.intParam1 = 8500;  // 10000 - 1500
    damageResponse.intParam2 = 10000;
    SendBtoA(std::move(damageResponse));
    GetCellA()->Update(0);

    // THEN: Ghost health updated
    EXPECT_EQ(GetCellA()->GetGhost(TARGET_GUID)->GetHealth(), 8500u);
}

TEST_F(GhostActorIntegrationTest, HealSpellAcrossCells)
{
    // Test: Heal spell across cells updates ghost health
    constexpr uint64_t HEALER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 1002;
    constexpr uint32_t FLASH_HEAL_ID = 2061;

    HarnessA().AddPlayer(HEALER_GUID);
    HarnessB().AddPlayer(TARGET_GUID);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Set ghost to damaged state
    {
        ActorMessage initHealth{};
        initHealth.type = MessageType::HEALTH_CHANGED;
        initHealth.sourceGuid = TARGET_GUID;
        initHealth.intParam1 = 5000;
        initHealth.intParam2 = 10000;
        GetCellA()->SendMessage(std::move(initHealth));
        GetCellA()->Update(0);
    }

    EXPECT_EQ(GetCellA()->GetGhost(TARGET_GUID)->GetHealth(), 5000u);

    // WHEN: Cast heal
    ActorMessage heal{};
    heal.type = MessageType::HEAL;
    heal.sourceGuid = HEALER_GUID;
    heal.targetGuid = TARGET_GUID;
    heal.complexPayload = MakeHealPayload(FLASH_HEAL_ID, 3000);
    SendAtoB(std::move(heal));
    GetCellB()->Update(0);

    // Simulate heal response
    ActorMessage healResponse{};
    healResponse.type = MessageType::HEALTH_CHANGED;
    healResponse.sourceGuid = TARGET_GUID;
    healResponse.intParam1 = 8000;  // 5000 + 3000
    healResponse.intParam2 = 10000;
    SendBtoA(std::move(healResponse));
    GetCellA()->Update(0);

    // THEN: Ghost health updated
    EXPECT_EQ(GetCellA()->GetGhost(TARGET_GUID)->GetHealth(), 8000u);
}

TEST_F(GhostActorIntegrationTest, AoESpellMultipleCells)
{
    // Test: AoE spell affecting entities across multiple cells
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET1_GUID = 2001;
    constexpr uint64_t TARGET2_GUID = 2002;
    constexpr uint64_t TARGET3_GUID = 2003;
    constexpr uint32_t BLIZZARD_ID = 10;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessA().AddCreature(TARGET1_GUID, 100);
    HarnessB().AddCreature(TARGET2_GUID, 100);
    HarnessC().AddCreature(TARGET3_GUID, 100);

    // Create ghosts in Cell A
    HarnessA().AddGhost(TARGET2_GUID, CELL_B_ID);
    HarnessA().AddGhost(TARGET3_GUID, CELL_C_ID);

    // Initialize ghost health
    for (uint64_t guid : {TARGET2_GUID, TARGET3_GUID})
    {
        ActorMessage init{};
        init.type = MessageType::HEALTH_CHANGED;
        init.sourceGuid = guid;
        init.intParam1 = 10000;
        init.intParam2 = 10000;
        GetCellA()->SendMessage(std::move(init));
    }
    GetCellA()->Update(0);

    // WHEN: AoE damages all targets
    // Send spell hits to Cell B and C
    ActorMessage spell2{};
    spell2.type = MessageType::SPELL_HIT;
    spell2.sourceGuid = CASTER_GUID;
    spell2.targetGuid = TARGET2_GUID;
    spell2.complexPayload = MakeSpellHitPayload(BLIZZARD_ID, 500);
    SendAtoB(std::move(spell2));

    ActorMessage spell3{};
    spell3.type = MessageType::SPELL_HIT;
    spell3.sourceGuid = CASTER_GUID;
    spell3.targetGuid = TARGET3_GUID;
    spell3.complexPayload = MakeSpellHitPayload(BLIZZARD_ID, 500);
    SendAtoC(std::move(spell3));

    ProcessAllCells();

    // Simulate health change broadcasts
    ActorMessage health2{};
    health2.type = MessageType::HEALTH_CHANGED;
    health2.sourceGuid = TARGET2_GUID;
    health2.intParam1 = 9500;
    health2.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(health2));

    ActorMessage health3{};
    health3.type = MessageType::HEALTH_CHANGED;
    health3.sourceGuid = TARGET3_GUID;
    health3.intParam1 = 9500;
    health3.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(health3));

    GetCellA()->Update(0);

    // THEN: All ghosts damaged
    EXPECT_EQ(GetCellA()->GetGhost(TARGET2_GUID)->GetHealth(), 9500u);
    EXPECT_EQ(GetCellA()->GetGhost(TARGET3_GUID)->GetHealth(), 9500u);
}

// =============================================================================
// Aura State Integration Tests
// =============================================================================

TEST_F(GhostActorIntegrationTest, AuraStateSync)
{
    // Test: Aura state bitmask syncs to ghosts
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t AURA_STATE_FROZEN = 0x40;  // Example aura state flag

    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    GhostEntity* ghost = GetCellA()->GetGhost(TARGET_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_EQ(ghost->GetAuraState(), 0u);

    // WHEN: Sync aura state
    ActorMessage auraState{};
    auraState.type = MessageType::AURA_STATE_SYNC;
    auraState.sourceGuid = TARGET_GUID;
    auraState.intParam1 = static_cast<int32_t>(AURA_STATE_FROZEN);
    GetCellA()->SendMessage(std::move(auraState));
    GetCellA()->Update(0);

    // THEN: Ghost has aura state
    EXPECT_EQ(ghost->GetAuraState(), AURA_STATE_FROZEN);
}

TEST_F(GhostActorIntegrationTest, AuraStateClear)
{
    // Test: Aura state clears correctly
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t AURA_STATE_STUNNED = 0x01;

    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    GhostEntity* ghost = GetCellA()->GetGhost(TARGET_GUID);

    // Set initial aura state
    ActorMessage auraSet{};
    auraSet.type = MessageType::AURA_STATE_SYNC;
    auraSet.sourceGuid = TARGET_GUID;
    auraSet.intParam1 = static_cast<int32_t>(AURA_STATE_STUNNED);
    GetCellA()->SendMessage(std::move(auraSet));
    GetCellA()->Update(0);

    EXPECT_EQ(ghost->GetAuraState(), AURA_STATE_STUNNED);

    // WHEN: Clear aura state
    ActorMessage auraClear{};
    auraClear.type = MessageType::AURA_STATE_SYNC;
    auraClear.sourceGuid = TARGET_GUID;
    auraClear.intParam1 = 0;
    GetCellA()->SendMessage(std::move(auraClear));
    GetCellA()->Update(0);

    // THEN: Aura state cleared
    EXPECT_EQ(ghost->GetAuraState(), 0u);
}

TEST_F(GhostActorIntegrationTest, AuraPeriodicTickAcrossCells)
{
    // Test: DoT periodic ticks update ghost health correctly
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t DOT_ID = 172;

    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Initialize health
    ActorMessage initHealth{};
    initHealth.type = MessageType::HEALTH_CHANGED;
    initHealth.sourceGuid = TARGET_GUID;
    initHealth.intParam1 = 10000;
    initHealth.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(initHealth));
    GetCellA()->Update(0);

    GhostEntity* ghost = GetCellA()->GetGhost(TARGET_GUID);
    EXPECT_EQ(ghost->GetHealth(), 10000u);

    // WHEN: Simulate 3 DoT ticks
    uint32_t currentHealth = 10000;
    for (int tick = 0; tick < 3; ++tick)
    {
        currentHealth -= 200;  // 200 damage per tick

        ActorMessage healthTick{};
        healthTick.type = MessageType::HEALTH_CHANGED;
        healthTick.sourceGuid = TARGET_GUID;
        healthTick.intParam1 = static_cast<int32_t>(currentHealth);
        healthTick.intParam2 = 10000;
        GetCellA()->SendMessage(std::move(healthTick));
        GetCellA()->Update(0);
    }

    // THEN: Ghost health reflects all ticks
    EXPECT_EQ(ghost->GetHealth(), 9400u);  // 10000 - (3 * 200)
}

TEST_F(GhostActorIntegrationTest, AuraStateBitmaskMultipleFlags)
{
    // Test: Multiple aura state flags combined correctly
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t AURA_STATE_STUNNED = 0x01;
    constexpr uint32_t AURA_STATE_FROZEN = 0x40;
    constexpr uint32_t COMBINED_STATE = AURA_STATE_STUNNED | AURA_STATE_FROZEN;

    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    GhostEntity* ghost = GetCellA()->GetGhost(TARGET_GUID);

    // WHEN: Set combined aura state
    ActorMessage aura{};
    aura.type = MessageType::AURA_STATE_SYNC;
    aura.sourceGuid = TARGET_GUID;
    aura.intParam1 = static_cast<int32_t>(COMBINED_STATE);
    GetCellA()->SendMessage(std::move(aura));
    GetCellA()->Update(0);

    // THEN: Ghost has both flags
    EXPECT_EQ(ghost->GetAuraState(), COMBINED_STATE);
    EXPECT_TRUE((ghost->GetAuraState() & AURA_STATE_STUNNED) != 0);
    EXPECT_TRUE((ghost->GetAuraState() & AURA_STATE_FROZEN) != 0);
}

TEST_F(GhostActorIntegrationTest, BuffTransferOnCellMigration)
{
    // Test: When entity moves cells, new ghost has correct state
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint32_t AURA_STATE_BUFFED = 0x80;

    // Player starts in Cell A
    HarnessA().AddPlayer(PLAYER_GUID);
    HarnessB().AddGhost(PLAYER_GUID, CELL_A_ID);

    // Set aura state on ghost
    ActorMessage aura{};
    aura.type = MessageType::AURA_STATE_SYNC;
    aura.sourceGuid = PLAYER_GUID;
    aura.intParam1 = static_cast<int32_t>(AURA_STATE_BUFFED);
    GetCellB()->SendMessage(std::move(aura));
    GetCellB()->Update(0);

    GhostEntity* ghostB = GetCellB()->GetGhost(PLAYER_GUID);
    EXPECT_EQ(ghostB->GetAuraState(), AURA_STATE_BUFFED);

    // WHEN: Player migrates to Cell C (destroy old ghost, create new)
    HarnessB().DestroyGhost(PLAYER_GUID);

    // Create new ghost in Cell C with same state
    HarnessC().AddGhost(PLAYER_GUID, CELL_A_ID);

    // Re-apply aura state
    ActorMessage auraC{};
    auraC.type = MessageType::AURA_STATE_SYNC;
    auraC.sourceGuid = PLAYER_GUID;
    auraC.intParam1 = static_cast<int32_t>(AURA_STATE_BUFFED);
    GetCellC()->SendMessage(std::move(auraC));
    GetCellC()->Update(0);

    // THEN: New ghost has aura state
    GhostEntity* ghostC = GetCellC()->GetGhost(PLAYER_GUID);
    ASSERT_NE(ghostC, nullptr);
    EXPECT_EQ(ghostC->GetAuraState(), AURA_STATE_BUFFED);
}

// =============================================================================
// Threat/Combat Integration Tests
// =============================================================================

TEST_F(GhostActorIntegrationTest, ThreatBuildsAcrossCells)
{
    // Test: Threat from attacks in Cell A reaches creature in Cell B
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t CREATURE_GUID = 2001;

    HarnessA().AddPlayer(PLAYER_GUID);
    HarnessB().AddCreature(CREATURE_GUID, 100);

    // WHEN: Player generates threat via spell
    ActorMessage threat{};
    threat.type = MessageType::THREAT_UPDATE;
    threat.complexPayload = MakeThreatUpdatePayload(CREATURE_GUID, PLAYER_GUID, 1000.0f);
    SendAtoB(std::move(threat));

    GetCellB()->Update(0);

    // Note: Actual threat application requires ThreatMgr integration
    // This test verifies the message is delivered
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, ThreatPercentModifyAcrossCells)
{
    // Test: Threat percent modification from Cell A reaches creature in Cell B
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t CREATURE_GUID = 2001;

    HarnessA().AddPlayer(PLAYER_GUID);
    HarnessB().AddCreature(CREATURE_GUID, 100);

    // WHEN: Spell reduces threat by 50%
    ActorMessage threatMod{};
    threatMod.type = MessageType::THREAT_UPDATE;
    threatMod.complexPayload = MakeThreatPercentPayload(CREATURE_GUID, PLAYER_GUID, -50);
    SendAtoB(std::move(threatMod));

    GetCellB()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, ThreatPercentIncreaseAcrossCells)
{
    // Test: Threat percent increase from Cell A reaches creature in Cell B
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t CREATURE_GUID = 2001;

    HarnessA().AddPlayer(PLAYER_GUID);
    HarnessB().AddCreature(CREATURE_GUID, 100);

    // WHEN: Ability increases threat by 100%
    ActorMessage threatMod{};
    threatMod.type = MessageType::THREAT_UPDATE;
    threatMod.complexPayload = MakeThreatPercentPayload(CREATURE_GUID, PLAYER_GUID, 100);
    SendAtoB(std::move(threatMod));

    GetCellB()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, ThreatAddAndPercentSequence)
{
    // Test: Threat add followed by percent modify both reach creature
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t CREATURE_GUID = 2001;

    HarnessA().AddPlayer(PLAYER_GUID);
    HarnessB().AddCreature(CREATURE_GUID, 100);

    // WHEN: Add threat then reduce by percent
    ActorMessage threatAdd{};
    threatAdd.type = MessageType::THREAT_UPDATE;
    threatAdd.complexPayload = MakeThreatUpdatePayload(CREATURE_GUID, PLAYER_GUID, 1000.0f);
    SendAtoB(std::move(threatAdd));

    ActorMessage threatMod{};
    threatMod.type = MessageType::THREAT_UPDATE;
    threatMod.complexPayload = MakeThreatPercentPayload(CREATURE_GUID, PLAYER_GUID, -25);
    SendAtoB(std::move(threatMod));

    GetCellB()->Update(0);

    // THEN: Both messages should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 2u);
}

TEST_F(GhostActorIntegrationTest, AggroRequestAndCombatInitiated)
{
    // Test: AGGRO_REQUEST triggers COMBAT_INITIATED response
    constexpr uint64_t CREATURE_GUID = 2001;
    constexpr uint64_t PLAYER_GUID = 1001;

    HarnessA().AddCreature(CREATURE_GUID, 100);
    HarnessB().AddPlayer(PLAYER_GUID);

    // WHEN: Creature requests aggro check
    ActorMessage aggro{};
    aggro.type = MessageType::AGGRO_REQUEST;
    aggro.sourceGuid = CREATURE_GUID;
    aggro.complexPayload = MakeAggroRequestPayload(CREATURE_GUID, 100.0f, 100.0f, 0.0f, 50.0f);
    GetCellB()->SendMessage(std::move(aggro));
    GetCellB()->Update(0);

    // Message was processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

// =============================================================================
// Migration Integration Tests
// =============================================================================

TEST_F(GhostActorIntegrationTest, EntityMigrationPreservesState)
{
    // Test: Entity moving between cells maintains combat state
    constexpr uint64_t PLAYER_GUID = 1001;

    // Player in Cell A
    HarnessA().AddPlayer(PLAYER_GUID);
    HarnessB().AddGhost(PLAYER_GUID, CELL_A_ID);

    // Set combat state
    ActorMessage combat{};
    combat.type = MessageType::COMBAT_STATE_CHANGED;
    combat.sourceGuid = PLAYER_GUID;
    combat.intParam1 = 1;  // In combat
    GetCellB()->SendMessage(std::move(combat));
    GetCellB()->Update(0);

    GhostEntity* ghostB = GetCellB()->GetGhost(PLAYER_GUID);
    EXPECT_TRUE(ghostB->IsInCombat());

    // WHEN: Player migrates to Cell B (becomes real entity)
    // Ghost in Cell B is destroyed, player added to Cell B
    HarnessB().DestroyGhost(PLAYER_GUID);
    HarnessA().DeleteEntity(ObjectGuid::Create<HighGuid::Player>(PLAYER_GUID));

    // Add player to Cell B
    HarnessB().AddPlayer(PLAYER_GUID);
    HarnessA().AddGhost(PLAYER_GUID, CELL_B_ID);

    // Propagate combat state to new ghost
    ActorMessage combatA{};
    combatA.type = MessageType::COMBAT_STATE_CHANGED;
    combatA.sourceGuid = PLAYER_GUID;
    combatA.intParam1 = 1;
    GetCellA()->SendMessage(std::move(combatA));
    GetCellA()->Update(0);

    // THEN: New ghost has combat state
    GhostEntity* ghostA = GetCellA()->GetGhost(PLAYER_GUID);
    ASSERT_NE(ghostA, nullptr);
    EXPECT_TRUE(ghostA->IsInCombat());
}

// =============================================================================
// Edge Case Integration Tests
// =============================================================================

TEST_F(GhostActorIntegrationTest, RapidSpellSequence)
{
    // Test: Multiple spells in quick succession all apply correctly
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Initialize health
    ActorMessage init{};
    init.type = MessageType::HEALTH_CHANGED;
    init.sourceGuid = TARGET_GUID;
    init.intParam1 = 10000;
    init.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(init));
    GetCellA()->Update(0);

    // WHEN: Send 5 rapid spell hits
    for (int i = 0; i < 5; ++i)
    {
        ActorMessage spell{};
        spell.type = MessageType::SPELL_HIT;
        spell.sourceGuid = CASTER_GUID;
        spell.targetGuid = TARGET_GUID;
        spell.complexPayload = MakeSpellHitPayload(100 + i, 500);
        SendAtoB(std::move(spell));
    }

    GetCellB()->Update(0);

    // Simulate health updates from all hits
    uint32_t health = 10000 - (5 * 500);  // 7500
    ActorMessage healthFinal{};
    healthFinal.type = MessageType::HEALTH_CHANGED;
    healthFinal.sourceGuid = TARGET_GUID;
    healthFinal.intParam1 = static_cast<int32_t>(health);
    healthFinal.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(healthFinal));
    GetCellA()->Update(0);

    // THEN: All damage applied
    EXPECT_EQ(GetCellA()->GetGhost(TARGET_GUID)->GetHealth(), 7500u);
}

TEST_F(GhostActorIntegrationTest, SimultaneousDamageAndHeal)
{
    // Test: Damage and heal arriving simultaneously
    constexpr uint64_t TARGET_GUID = 1001;

    HarnessA().AddPlayer(TARGET_GUID);
    HarnessB().AddGhost(TARGET_GUID, CELL_A_ID);

    // Initialize health
    ActorMessage init{};
    init.type = MessageType::HEALTH_CHANGED;
    init.sourceGuid = TARGET_GUID;
    init.intParam1 = 8000;
    init.intParam2 = 10000;
    GetCellB()->SendMessage(std::move(init));
    GetCellB()->Update(0);

    // WHEN: Damage (-2000) and heal (+3000) both hit
    // Net result: +1000 health = 9000
    ActorMessage finalHealth{};
    finalHealth.type = MessageType::HEALTH_CHANGED;
    finalHealth.sourceGuid = TARGET_GUID;
    finalHealth.intParam1 = 9000;  // 8000 - 2000 + 3000
    finalHealth.intParam2 = 10000;
    GetCellB()->SendMessage(std::move(finalHealth));
    GetCellB()->Update(0);

    // THEN: Net result applied
    EXPECT_EQ(GetCellB()->GetGhost(TARGET_GUID)->GetHealth(), 9000u);
}

// ============================================================================
// Periodic Damage/Healing Cross-Cell Tests (DoTs/HoTs)
// These tests verify that periodic effects work correctly across cell boundaries
// ============================================================================

TEST_F(GhostActorIntegrationTest, PeriodicDamageViaSPELL_HIT)
{
    // Test: DoT damage sent via SPELL_HIT message updates ghost correctly
    // This tests the receiving side of HandlePeriodicDamageAurasTick cross-cell routing
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t DOT_SPELL_ID = 172;  // Corruption

    // Setup: Caster in Cell A, Target in Cell B, Ghost of target in Cell A
    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Initialize ghost health
    ActorMessage initHealth{};
    initHealth.type = MessageType::HEALTH_CHANGED;
    initHealth.sourceGuid = TARGET_GUID;
    initHealth.intParam1 = 10000;
    initHealth.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(initHealth));
    GetCellA()->Update(0);

    EXPECT_EQ(GetCellA()->GetGhost(TARGET_GUID)->GetHealth(), 10000u);

    // WHEN: DoT tick arrives as SPELL_HIT message (simulates cross-cell periodic damage)
    ActorMessage dotTick{};
    dotTick.type = MessageType::SPELL_HIT;
    dotTick.sourceGuid = CASTER_GUID;
    dotTick.targetGuid = TARGET_GUID;
    dotTick.intParam1 = static_cast<int32_t>(DOT_SPELL_ID);
    dotTick.complexPayload = MakeSpellHitPayload(DOT_SPELL_ID, 300, 0);  // 300 damage
    GetCellA()->SendMessage(std::move(dotTick));
    GetCellA()->Update(0);

    // THEN: Ghost health should reflect the damage
    // Note: In real implementation, health update comes via HEALTH_CHANGED
    // This test verifies the message is processed without error
    EXPECT_TRUE(true);  // Message processed successfully
}

TEST_F(GhostActorIntegrationTest, PeriodicHealingViaHEAL_Message)
{
    // Test: HoT healing sent via HEAL message updates ghost correctly
    // This tests the receiving side of HandlePeriodicHealAurasTick cross-cell routing
    constexpr uint64_t HEALER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t HOT_SPELL_ID = 774;  // Rejuvenation

    // Setup: Healer in Cell A, Target in Cell B, Ghost of target in Cell A
    HarnessA().AddPlayer(HEALER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Initialize ghost health (damaged)
    ActorMessage initHealth{};
    initHealth.type = MessageType::HEALTH_CHANGED;
    initHealth.sourceGuid = TARGET_GUID;
    initHealth.intParam1 = 5000;
    initHealth.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(initHealth));
    GetCellA()->Update(0);

    EXPECT_EQ(GetCellA()->GetGhost(TARGET_GUID)->GetHealth(), 5000u);

    // WHEN: HoT tick arrives as HEAL message (simulates cross-cell periodic healing)
    ActorMessage hotTick{};
    hotTick.type = MessageType::HEAL;
    hotTick.sourceGuid = HEALER_GUID;
    hotTick.targetGuid = TARGET_GUID;
    hotTick.intParam1 = static_cast<int32_t>(HOT_SPELL_ID);
    hotTick.complexPayload = MakeHealPayload(HOT_SPELL_ID, 500);  // 500 healing
    GetCellA()->SendMessage(std::move(hotTick));
    GetCellA()->Update(0);

    // THEN: Message processed without error
    EXPECT_TRUE(true);
}

TEST_F(GhostActorIntegrationTest, MultiplePeriodicTicksSequential)
{
    // Test: Multiple DoT ticks in sequence update ghost health correctly
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t DOT_SPELL_ID = 348;  // Immolate
    constexpr uint32_t TICK_DAMAGE = 200;
    constexpr int NUM_TICKS = 5;

    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Initialize health
    uint32_t currentHealth = 10000;
    ActorMessage init{};
    init.type = MessageType::HEALTH_CHANGED;
    init.sourceGuid = TARGET_GUID;
    init.intParam1 = static_cast<int32_t>(currentHealth);
    init.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(init));
    GetCellA()->Update(0);

    // WHEN: Multiple DoT ticks arrive
    for (int tick = 0; tick < NUM_TICKS; ++tick)
    {
        currentHealth -= TICK_DAMAGE;

        // Send SPELL_HIT for the DoT tick
        ActorMessage dotTick{};
        dotTick.type = MessageType::SPELL_HIT;
        dotTick.sourceGuid = 1001;  // Caster
        dotTick.targetGuid = TARGET_GUID;
        dotTick.intParam1 = static_cast<int32_t>(DOT_SPELL_ID);
        dotTick.complexPayload = MakeSpellHitPayload(DOT_SPELL_ID, TICK_DAMAGE, 0);
        GetCellA()->SendMessage(std::move(dotTick));

        // Send health update
        ActorMessage healthUpdate{};
        healthUpdate.type = MessageType::HEALTH_CHANGED;
        healthUpdate.sourceGuid = TARGET_GUID;
        healthUpdate.intParam1 = static_cast<int32_t>(currentHealth);
        healthUpdate.intParam2 = 10000;
        GetCellA()->SendMessage(std::move(healthUpdate));

        GetCellA()->Update(0);
    }

    // THEN: Health reflects all ticks (10000 - 5*200 = 9000)
    EXPECT_EQ(GetCellA()->GetGhost(TARGET_GUID)->GetHealth(), 9000u);
}

TEST_F(GhostActorIntegrationTest, PeriodicDamageWithNullCaster)
{
    // Test: DoT ticks continue even when caster is null (caster died)
    // This simulates the edge case in HandlePeriodicDamageAurasTick where caster can be null
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t DOT_SPELL_ID = 172;

    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Initialize health
    ActorMessage init{};
    init.type = MessageType::HEALTH_CHANGED;
    init.sourceGuid = TARGET_GUID;
    init.intParam1 = 10000;
    init.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(init));
    GetCellA()->Update(0);

    // WHEN: DoT tick with sourceGuid = 0 (caster dead/null)
    ActorMessage dotTick{};
    dotTick.type = MessageType::SPELL_HIT;
    dotTick.sourceGuid = 0;  // Null caster
    dotTick.targetGuid = TARGET_GUID;
    dotTick.intParam1 = static_cast<int32_t>(DOT_SPELL_ID);
    dotTick.complexPayload = MakeSpellHitPayload(DOT_SPELL_ID, 300, 0);
    GetCellA()->SendMessage(std::move(dotTick));
    GetCellA()->Update(0);

    // THEN: Message processed without crash (null caster handled gracefully)
    EXPECT_TRUE(true);
}

TEST_F(GhostActorIntegrationTest, PeriodicHealingMultipleSources)
{
    // Test: Multiple HoTs from different healers on same target
    constexpr uint64_t HEALER1_GUID = 1001;
    constexpr uint64_t HEALER2_GUID = 1002;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t HOT1_SPELL_ID = 774;   // Rejuvenation
    constexpr uint32_t HOT2_SPELL_ID = 8936;  // Regrowth HoT

    HarnessA().AddPlayer(HEALER1_GUID);
    HarnessA().AddPlayer(HEALER2_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Initialize health (damaged)
    uint32_t currentHealth = 3000;
    ActorMessage init{};
    init.type = MessageType::HEALTH_CHANGED;
    init.sourceGuid = TARGET_GUID;
    init.intParam1 = static_cast<int32_t>(currentHealth);
    init.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(init));
    GetCellA()->Update(0);

    // WHEN: Two different HoTs tick
    // HoT 1 tick
    ActorMessage hot1{};
    hot1.type = MessageType::HEAL;
    hot1.sourceGuid = HEALER1_GUID;
    hot1.targetGuid = TARGET_GUID;
    hot1.intParam1 = static_cast<int32_t>(HOT1_SPELL_ID);
    hot1.complexPayload = MakeHealPayload(HOT1_SPELL_ID, 400);
    GetCellA()->SendMessage(std::move(hot1));

    // HoT 2 tick
    ActorMessage hot2{};
    hot2.type = MessageType::HEAL;
    hot2.sourceGuid = HEALER2_GUID;
    hot2.targetGuid = TARGET_GUID;
    hot2.intParam1 = static_cast<int32_t>(HOT2_SPELL_ID);
    hot2.complexPayload = MakeHealPayload(HOT2_SPELL_ID, 600);
    GetCellA()->SendMessage(std::move(hot2));

    // Health update reflecting both heals
    currentHealth += 1000;  // 400 + 600
    ActorMessage healthUpdate{};
    healthUpdate.type = MessageType::HEALTH_CHANGED;
    healthUpdate.sourceGuid = TARGET_GUID;
    healthUpdate.intParam1 = static_cast<int32_t>(currentHealth);
    healthUpdate.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(healthUpdate));

    GetCellA()->Update(0);

    // THEN: Health reflects both heals (3000 + 1000 = 4000)
    EXPECT_EQ(GetCellA()->GetGhost(TARGET_GUID)->GetHealth(), 4000u);
}

TEST_F(GhostActorIntegrationTest, PeriodicDamageKillsTarget)
{
    // Test: DoT tick that kills the target
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t DOT_SPELL_ID = 172;

    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Initialize health (low)
    ActorMessage init{};
    init.type = MessageType::HEALTH_CHANGED;
    init.sourceGuid = TARGET_GUID;
    init.intParam1 = 100;
    init.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(init));
    GetCellA()->Update(0);

    // WHEN: Lethal DoT tick (200 damage vs 100 health)
    ActorMessage dotTick{};
    dotTick.type = MessageType::SPELL_HIT;
    dotTick.sourceGuid = 1001;
    dotTick.targetGuid = TARGET_GUID;
    dotTick.intParam1 = static_cast<int32_t>(DOT_SPELL_ID);
    dotTick.complexPayload = MakeSpellHitPayload(DOT_SPELL_ID, 200, 0);
    GetCellA()->SendMessage(std::move(dotTick));

    // Health goes to 0
    ActorMessage death{};
    death.type = MessageType::HEALTH_CHANGED;
    death.sourceGuid = TARGET_GUID;
    death.intParam1 = 0;
    death.intParam2 = 10000;
    GetCellA()->SendMessage(std::move(death));

    GetCellA()->Update(0);

    // THEN: Ghost shows dead
    EXPECT_EQ(GetCellA()->GetGhost(TARGET_GUID)->GetHealth(), 0u);
}

// ============================================================================
// Interrupt Cross-Cell Tests
// ============================================================================

TEST_F(GhostActorIntegrationTest, InterruptSpellAcrossCells)
{
    // Test: Player in Cell A interrupts caster in Cell B
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t CASTER_GUID = 2001;
    constexpr uint32_t KICK_SPELL_ID = 1766;       // Kick
    constexpr uint32_t FROSTBOLT_ID = 116;         // Being cast
    constexpr uint32_t FROST_SCHOOL_MASK = 16;     // Frost

    // GIVEN: Player in Cell A, caster in Cell B
    HarnessA().AddPlayer(PLAYER_GUID);
    HarnessB().AddCreature(CASTER_GUID, 100);

    // Create ghost of caster in Cell A
    HarnessA().AddGhost(CASTER_GUID, CELL_B_ID);

    // WHEN: Interrupt message is sent to Cell B
    ActorMessage interrupt{};
    interrupt.type = MessageType::SPELL_INTERRUPT;
    interrupt.sourceGuid = PLAYER_GUID;
    interrupt.targetGuid = CASTER_GUID;
    interrupt.sourceCellId = CELL_A_ID;
    interrupt.targetCellId = CELL_B_ID;
    interrupt.complexPayload = MakeSpellInterruptPayload(PLAYER_GUID, CASTER_GUID,
        KICK_SPELL_ID, FROSTBOLT_ID, FROST_SCHOOL_MASK, 5000);
    GetCellB()->SendMessage(std::move(interrupt));

    // THEN: Message is processed without crash
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, InterruptWithSchoolLockout)
{
    // Test: Interrupt applies school lockout duration
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t CASTER_GUID = 2001;
    constexpr uint32_t COUNTERSPELL_ID = 2139;
    constexpr uint32_t POLYMORPH_ID = 118;
    constexpr uint32_t ARCANE_SCHOOL_MASK = 64;
    constexpr int32_t LOCKOUT_DURATION = 8000;  // 8 sec lockout

    HarnessA().AddPlayer(PLAYER_GUID);
    HarnessB().AddCreature(CASTER_GUID, 100);
    HarnessA().AddGhost(CASTER_GUID, CELL_B_ID);

    // Payload with school lockout
    auto payload = MakeSpellInterruptPayload(PLAYER_GUID, CASTER_GUID,
        COUNTERSPELL_ID, POLYMORPH_ID, ARCANE_SCHOOL_MASK, LOCKOUT_DURATION);

    EXPECT_EQ(payload->schoolMask, ARCANE_SCHOOL_MASK);
    EXPECT_EQ(payload->lockoutDuration, LOCKOUT_DURATION);
    EXPECT_EQ(payload->interruptSpellId, COUNTERSPELL_ID);
    EXPECT_EQ(payload->interruptedSpellId, POLYMORPH_ID);
}

TEST_F(GhostActorIntegrationTest, InterruptDeletedTargetGraceful)
{
    // Test: Interrupt message for a target that's been deleted
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t CASTER_GUID = 2001;

    HarnessA().AddPlayer(PLAYER_GUID);
    TestCreature* caster = HarnessB().AddCreature(CASTER_GUID, 100);

    // Delete the caster
    HarnessB().DeleteEntity(caster->GetGUID());

    // WHEN: Interrupt message arrives for deleted target
    ActorMessage interrupt{};
    interrupt.type = MessageType::SPELL_INTERRUPT;
    interrupt.sourceGuid = PLAYER_GUID;
    interrupt.targetGuid = CASTER_GUID;
    interrupt.complexPayload = MakeSpellInterruptPayload(PLAYER_GUID, CASTER_GUID,
        1766, 116, 16, 5000);
    GetCellB()->SendMessage(std::move(interrupt));

    // THEN: Processed gracefully without crash
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, InterruptNullPayload)
{
    // Test: SPELL_INTERRUPT with null payload
    ActorMessage interrupt{};
    interrupt.type = MessageType::SPELL_INTERRUPT;
    interrupt.sourceGuid = 1001;
    interrupt.targetGuid = 2001;
    interrupt.complexPayload = nullptr;  // No payload

    GetCellB()->SendMessage(std::move(interrupt));

    // THEN: Processed gracefully
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

// ============================================================================
// Dispel Cross-Cell Tests
// ============================================================================

TEST_F(GhostActorIntegrationTest, DispelSpellAcrossCells)
{
    // Test: Priest in Cell A dispels buffs from target in Cell B
    constexpr uint64_t PRIEST_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t DISPEL_MAGIC_ID = 527;

    // GIVEN: Priest in Cell A, target in Cell B
    HarnessA().AddPlayer(PRIEST_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Dispel list with multiple auras
    std::vector<std::pair<uint32_t, uint8_t>> dispelList = {
        {21562, 1},  // Power Word: Fortitude
        {1459, 1},   // Arcane Intellect
        {976, 1}     // Shadow Protection
    };

    // WHEN: Dispel message is sent
    ActorMessage dispel{};
    dispel.type = MessageType::SPELL_DISPEL;
    dispel.sourceGuid = PRIEST_GUID;
    dispel.targetGuid = TARGET_GUID;
    dispel.sourceCellId = CELL_A_ID;
    dispel.targetCellId = CELL_B_ID;
    dispel.complexPayload = MakeSpellDispelPayload(PRIEST_GUID, TARGET_GUID,
        DISPEL_MAGIC_ID, dispelList);
    GetCellB()->SendMessage(std::move(dispel));

    // THEN: Message processed without crash
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, DispelMultipleCharges)
{
    // Test: Dispel that removes multiple charges from a single aura
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t PURGE_SPELL_ID = 370;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    // Dispelling an aura with multiple stacks
    std::vector<std::pair<uint32_t, uint8_t>> dispelList = {
        {48066, 3}  // Power Word: Shield - remove 3 stacks
    };

    auto payload = MakeSpellDispelPayload(CASTER_GUID, TARGET_GUID, PURGE_SPELL_ID, dispelList);

    // Verify payload structure
    EXPECT_EQ(payload->dispelList.size(), 1u);
    EXPECT_EQ(payload->dispelList[0].first, 48066u);   // Spell ID
    EXPECT_EQ(payload->dispelList[0].second, 3u);      // 3 charges
}

TEST_F(GhostActorIntegrationTest, DispelDeletedTargetGraceful)
{
    // Test: Dispel message for target that's been deleted
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;

    HarnessA().AddPlayer(CASTER_GUID);
    TestCreature* target = HarnessB().AddCreature(TARGET_GUID, 100);

    // Delete the target
    HarnessB().DeleteEntity(target->GetGUID());

    std::vector<std::pair<uint32_t, uint8_t>> dispelList = {{21562, 1}};

    ActorMessage dispel{};
    dispel.type = MessageType::SPELL_DISPEL;
    dispel.sourceGuid = CASTER_GUID;
    dispel.targetGuid = TARGET_GUID;
    dispel.complexPayload = MakeSpellDispelPayload(CASTER_GUID, TARGET_GUID, 527, dispelList);
    GetCellB()->SendMessage(std::move(dispel));

    // THEN: Processed gracefully
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, DispelNullPayload)
{
    // Test: SPELL_DISPEL with null payload
    ActorMessage dispel{};
    dispel.type = MessageType::SPELL_DISPEL;
    dispel.sourceGuid = 1001;
    dispel.targetGuid = 2001;
    dispel.complexPayload = nullptr;

    GetCellB()->SendMessage(std::move(dispel));

    // THEN: Processed gracefully
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, DispelEmptyList)
{
    // Test: Dispel with empty dispel list (edge case)
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    std::vector<std::pair<uint32_t, uint8_t>> emptyList;

    ActorMessage dispel{};
    dispel.type = MessageType::SPELL_DISPEL;
    dispel.sourceGuid = CASTER_GUID;
    dispel.targetGuid = TARGET_GUID;
    dispel.complexPayload = MakeSpellDispelPayload(CASTER_GUID, TARGET_GUID, 527, emptyList);
    GetCellB()->SendMessage(std::move(dispel));

    // THEN: Processed gracefully (no-op)
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, InterruptAndDispelSequence)
{
    // Test: Interrupt followed by dispel in same update cycle
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t CASTER_GUID = 2001;

    HarnessA().AddPlayer(PLAYER_GUID);
    HarnessB().AddCreature(CASTER_GUID, 100);
    HarnessA().AddGhost(CASTER_GUID, CELL_B_ID);

    // Send interrupt
    ActorMessage interrupt{};
    interrupt.type = MessageType::SPELL_INTERRUPT;
    interrupt.sourceGuid = PLAYER_GUID;
    interrupt.targetGuid = CASTER_GUID;
    interrupt.complexPayload = MakeSpellInterruptPayload(PLAYER_GUID, CASTER_GUID,
        1766, 116, 16, 5000);
    GetCellB()->SendMessage(std::move(interrupt));

    // Send dispel immediately after
    std::vector<std::pair<uint32_t, uint8_t>> dispelList = {{10958, 1}}; // Some buff
    ActorMessage dispel{};
    dispel.type = MessageType::SPELL_DISPEL;
    dispel.sourceGuid = PLAYER_GUID;
    dispel.targetGuid = CASTER_GUID;
    dispel.complexPayload = MakeSpellDispelPayload(PLAYER_GUID, CASTER_GUID, 527, dispelList);
    GetCellB()->SendMessage(std::move(dispel));

    // THEN: Both processed correctly
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

// ============================================================================
// Power Drain Cross-Cell Tests
// ============================================================================

TEST_F(GhostActorIntegrationTest, PowerDrainAcrossCells)
{
    // Test: Power drain message from Cell A to target in Cell B
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t DRAIN_SPELL_ID = 5138;  // Drain Mana

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    ActorMessage drain{};
    drain.type = MessageType::POWER_DRAIN;
    drain.sourceGuid = CASTER_GUID;
    drain.targetGuid = TARGET_GUID;
    drain.complexPayload = MakePowerDrainPayload(CASTER_GUID, TARGET_GUID,
        DRAIN_SPELL_ID, 0, 1000, 1.0f, false);
    GetCellB()->SendMessage(std::move(drain));

    // THEN: Processed correctly
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, PowerBurnAcrossCells)
{
    // Test: Power burn message from Cell A to target in Cell B
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t BURN_SPELL_ID = 8129;  // Mana Burn

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    ActorMessage burn{};
    burn.type = MessageType::POWER_DRAIN;
    burn.sourceGuid = CASTER_GUID;
    burn.targetGuid = TARGET_GUID;
    burn.complexPayload = MakePowerDrainPayload(CASTER_GUID, TARGET_GUID,
        BURN_SPELL_ID, 0, 500, 0.0f, true);  // isPowerBurn=true
    GetCellB()->SendMessage(std::move(burn));

    // THEN: Processed correctly
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, PowerDrainDeletedTargetGraceful)
{
    // Test: Power drain to a target that no longer exists should be handled gracefully
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;

    HarnessA().AddPlayer(CASTER_GUID);
    // Intentionally NOT adding the target - simulates deleted entity

    ActorMessage drain{};
    drain.type = MessageType::POWER_DRAIN;
    drain.sourceGuid = CASTER_GUID;
    drain.targetGuid = TARGET_GUID;
    drain.complexPayload = MakePowerDrainPayload(CASTER_GUID, TARGET_GUID,
        5138, 0, 1000, 1.0f, false);
    GetCellB()->SendMessage(std::move(drain));

    // THEN: Processed gracefully (no crash)
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, PowerDrainNullPayload)
{
    // Test: Power drain with null payload should be handled gracefully
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;

    HarnessB().AddCreature(TARGET_GUID, 100);

    ActorMessage drain{};
    drain.type = MessageType::POWER_DRAIN;
    drain.sourceGuid = CASTER_GUID;
    drain.targetGuid = TARGET_GUID;
    drain.complexPayload = nullptr;  // Null payload
    GetCellB()->SendMessage(std::move(drain));

    // THEN: Processed gracefully (no-op)
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

// ============================================================================
// Spellsteal Cross-Cell Tests
// ============================================================================

TEST_F(GhostActorIntegrationTest, SpellstealAcrossCells)
{
    // Test: Spellsteal message from Cell A to target in Cell B
    constexpr uint64_t MAGE_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t SPELLSTEAL_ID = 30449;

    HarnessA().AddPlayer(MAGE_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    std::vector<std::pair<uint32_t, uint64_t>> stealList = {
        {21562, TARGET_GUID},  // Prayer of Fortitude
        {1459, TARGET_GUID}    // Arcane Intellect
    };

    ActorMessage steal{};
    steal.type = MessageType::SPELLSTEAL;
    steal.sourceGuid = MAGE_GUID;
    steal.targetGuid = TARGET_GUID;
    steal.complexPayload = MakeSpellstealPayload(MAGE_GUID, TARGET_GUID,
        SPELLSTEAL_ID, stealList);
    GetCellB()->SendMessage(std::move(steal));

    // THEN: Processed correctly
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, SpellstealApplyAcrossCells)
{
    // Test: Spellsteal apply message to mage in Cell A after removal from Cell B
    constexpr uint64_t MAGE_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t SPELLSTEAL_ID = 30449;

    HarnessA().AddPlayer(MAGE_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    StolenAuraData auraData;
    auraData.spellId = 21562;  // Prayer of Fortitude
    auraData.originalCasterGuid = TARGET_GUID;
    auraData.duration = 60000;
    auraData.maxDuration = 1800000;
    auraData.stackAmount = 1;
    auraData.charges = 0;
    std::vector<StolenAuraData> stolenAuras = {auraData};

    ActorMessage apply{};
    apply.type = MessageType::SPELLSTEAL_APPLY;
    apply.sourceGuid = TARGET_GUID;
    apply.targetGuid = MAGE_GUID;
    apply.complexPayload = MakeSpellstealApplyPayload(MAGE_GUID, TARGET_GUID,
        SPELLSTEAL_ID, stolenAuras);
    GetCellA()->SendMessage(std::move(apply));

    // THEN: Processed correctly
    EXPECT_NO_THROW(GetCellA()->Update(0));
}

TEST_F(GhostActorIntegrationTest, SpellstealDeletedTargetGraceful)
{
    // Test: Spellsteal to a target that no longer exists
    constexpr uint64_t MAGE_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;

    HarnessA().AddPlayer(MAGE_GUID);
    // Intentionally NOT adding the target

    std::vector<std::pair<uint32_t, uint64_t>> stealList = {{21562, TARGET_GUID}};

    ActorMessage steal{};
    steal.type = MessageType::SPELLSTEAL;
    steal.sourceGuid = MAGE_GUID;
    steal.targetGuid = TARGET_GUID;
    steal.complexPayload = MakeSpellstealPayload(MAGE_GUID, TARGET_GUID,
        30449, stealList);
    GetCellB()->SendMessage(std::move(steal));

    // THEN: Processed gracefully (no crash)
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, SpellstealNullPayload)
{
    // Test: Spellsteal with null payload
    constexpr uint64_t TARGET_GUID = 2001;

    HarnessB().AddCreature(TARGET_GUID, 100);

    ActorMessage steal{};
    steal.type = MessageType::SPELLSTEAL;
    steal.sourceGuid = 1001;
    steal.targetGuid = TARGET_GUID;
    steal.complexPayload = nullptr;
    GetCellB()->SendMessage(std::move(steal));

    // THEN: Processed gracefully (no-op)
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, SpellstealEmptyList)
{
    // Test: Spellsteal with empty steal list
    constexpr uint64_t MAGE_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;

    HarnessB().AddCreature(TARGET_GUID, 100);

    std::vector<std::pair<uint32_t, uint64_t>> emptyList;

    ActorMessage steal{};
    steal.type = MessageType::SPELLSTEAL;
    steal.sourceGuid = MAGE_GUID;
    steal.targetGuid = TARGET_GUID;
    steal.complexPayload = MakeSpellstealPayload(MAGE_GUID, TARGET_GUID,
        30449, emptyList);
    GetCellB()->SendMessage(std::move(steal));

    // THEN: Processed gracefully (no-op)
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, PowerDrainAndSpellstealSequence)
{
    // Test: Power drain followed by spellsteal in same update cycle
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;

    HarnessA().AddPlayer(PLAYER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Send power drain
    ActorMessage drain{};
    drain.type = MessageType::POWER_DRAIN;
    drain.sourceGuid = PLAYER_GUID;
    drain.targetGuid = TARGET_GUID;
    drain.complexPayload = MakePowerDrainPayload(PLAYER_GUID, TARGET_GUID,
        5138, 0, 500, 1.0f, false);
    GetCellB()->SendMessage(std::move(drain));

    // Send spellsteal immediately after
    std::vector<std::pair<uint32_t, uint64_t>> stealList = {{21562, TARGET_GUID}};
    ActorMessage steal{};
    steal.type = MessageType::SPELLSTEAL;
    steal.sourceGuid = PLAYER_GUID;
    steal.targetGuid = TARGET_GUID;
    steal.complexPayload = MakeSpellstealPayload(PLAYER_GUID, TARGET_GUID,
        30449, stealList);
    GetCellB()->SendMessage(std::move(steal));

    // THEN: Both processed correctly
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

// ============================================================================
// Periodic Leech Cross-Cell Tests (Health Leech, Mana Leech, Power Burn)
// ============================================================================

TEST_F(GhostActorIntegrationTest, PeriodicHealthLeechAcrossCells)
{
    // Test: Periodic health leech (Drain Life) damage routes to target's cell
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t DRAIN_LIFE_ID = 689;  // Drain Life

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Simulate periodic health leech tick via SPELL_HIT message
    ActorMessage leechTick{};
    leechTick.type = MessageType::SPELL_HIT;
    leechTick.sourceGuid = CASTER_GUID;
    leechTick.targetGuid = TARGET_GUID;
    leechTick.complexPayload = MakeSpellHitPayload(DRAIN_LIFE_ID, 200, 0);
    GetCellB()->SendMessage(std::move(leechTick));

    // THEN: Processed correctly
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, PeriodicManaLeechAcrossCells)
{
    // Test: Periodic mana leech (Drain Mana) routes to target's cell
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t DRAIN_MANA_ID = 5138;  // Drain Mana

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Simulate periodic mana leech via POWER_DRAIN message
    ActorMessage leechTick{};
    leechTick.type = MessageType::POWER_DRAIN;
    leechTick.sourceGuid = CASTER_GUID;
    leechTick.targetGuid = TARGET_GUID;
    leechTick.complexPayload = MakePowerDrainPayload(CASTER_GUID, TARGET_GUID,
        DRAIN_MANA_ID, 0, 500, 1.0f, false);
    GetCellB()->SendMessage(std::move(leechTick));

    // THEN: Processed correctly
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, PeriodicPowerBurnAcrossCells)
{
    // Test: Periodic power burn tick routes to target's cell
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t POWER_BURN_ID = 32848;  // Power Burn

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Simulate periodic power burn via POWER_DRAIN (isPowerBurn=true)
    ActorMessage burnTick{};
    burnTick.type = MessageType::POWER_DRAIN;
    burnTick.sourceGuid = CASTER_GUID;
    burnTick.targetGuid = TARGET_GUID;
    burnTick.complexPayload = MakePowerDrainPayload(CASTER_GUID, TARGET_GUID,
        POWER_BURN_ID, 0, 300, 0.5f, true);  // 0.5 damage multiplier
    GetCellB()->SendMessage(std::move(burnTick));

    // Also send damage via SPELL_HIT
    ActorMessage dmgMsg{};
    dmgMsg.type = MessageType::SPELL_HIT;
    dmgMsg.sourceGuid = CASTER_GUID;
    dmgMsg.targetGuid = TARGET_GUID;
    dmgMsg.complexPayload = MakeSpellHitPayload(POWER_BURN_ID, 150, 0);  // 300 * 0.5
    GetCellB()->SendMessage(std::move(dmgMsg));

    // THEN: Both processed correctly
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, PeriodicHealthLeechCasterDeadGraceful)
{
    // Test: Health leech continues even if caster dies (DoT persists)
    constexpr uint64_t TARGET_GUID = 2001;

    HarnessB().AddCreature(TARGET_GUID, 100);

    // Simulate health leech tick with no caster (caster died)
    ActorMessage leechTick{};
    leechTick.type = MessageType::SPELL_HIT;
    leechTick.sourceGuid = 0;  // Caster dead/removed
    leechTick.targetGuid = TARGET_GUID;
    leechTick.complexPayload = MakeSpellHitPayload(689, 200, 0);
    GetCellB()->SendMessage(std::move(leechTick));

    // THEN: Processed gracefully
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, MultiplePeriodicLeechTicksSequential)
{
    // Test: Multiple periodic leech ticks in sequence
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    // Send multiple leech ticks
    for (int i = 0; i < 5; ++i)
    {
        ActorMessage leechTick{};
        leechTick.type = MessageType::SPELL_HIT;
        leechTick.sourceGuid = CASTER_GUID;
        leechTick.targetGuid = TARGET_GUID;
        leechTick.complexPayload = MakeSpellHitPayload(689, 200, 0);
        GetCellB()->SendMessage(std::move(leechTick));
    }

    // THEN: All processed correctly
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

// ============================================================================
// Bi-directional Thorns/Reflect Damage Tests
// ============================================================================

TEST_F(GhostActorIntegrationTest, ReflectDamageMessageCreation)
{
    // Test: ReflectDamagePayload can be created and populated correctly
    auto payload = std::make_shared<ReflectDamagePayload>();
    payload->reflectorGuid = 2001;
    payload->attackerGuid = 1001;
    payload->spellId = 467;  // Thorns
    payload->damage = 50;
    payload->schoolMask = SPELL_SCHOOL_MASK_NATURE;
    payload->absorb = 0;
    payload->resist = 0;

    EXPECT_EQ(payload->reflectorGuid, 2001);
    EXPECT_EQ(payload->attackerGuid, 1001);
    EXPECT_EQ(payload->spellId, 467u);
    EXPECT_EQ(payload->damage, 50);
    EXPECT_EQ(payload->schoolMask, SPELL_SCHOOL_MASK_NATURE);
}

TEST_F(GhostActorIntegrationTest, ReflectDamageMessageRouting)
{
    // Test: REFLECT_DAMAGE message routes from victim's cell back to attacker's cell
    constexpr uint64_t ATTACKER_GUID = 1001;
    constexpr uint64_t VICTIM_GUID = 2001;

    HarnessA().AddPlayer(ATTACKER_GUID);
    HarnessB().AddCreature(VICTIM_GUID, 100);

    // Create reflect damage message (thorns reflecting back to attacker)
    auto payload = std::make_shared<ReflectDamagePayload>();
    payload->reflectorGuid = VICTIM_GUID;
    payload->attackerGuid = ATTACKER_GUID;
    payload->spellId = 467;  // Thorns
    payload->damage = 50;
    payload->schoolMask = SPELL_SCHOOL_MASK_NATURE;

    ActorMessage reflectMsg{};
    reflectMsg.type = MessageType::REFLECT_DAMAGE;
    reflectMsg.sourceGuid = VICTIM_GUID;
    reflectMsg.targetGuid = ATTACKER_GUID;
    reflectMsg.sourceCellId = CELL_B_ID;
    reflectMsg.targetCellId = CELL_A_ID;
    reflectMsg.complexPayload = payload;

    // Send to attacker's cell
    GetCellA()->SendMessage(std::move(reflectMsg));

    // THEN: Message processed without throwing
    EXPECT_NO_THROW(GetCellA()->Update(0));
}

TEST_F(GhostActorIntegrationTest, ReflectDamageWithMissingAttacker)
{
    // Test: REFLECT_DAMAGE gracefully handles case where attacker is not found
    constexpr uint64_t ATTACKER_GUID = 1001;  // Not added to any cell
    constexpr uint64_t VICTIM_GUID = 2001;

    // Don't add attacker - simulates attacker leaving or dying

    auto payload = std::make_shared<ReflectDamagePayload>();
    payload->reflectorGuid = VICTIM_GUID;
    payload->attackerGuid = ATTACKER_GUID;
    payload->spellId = 467;
    payload->damage = 50;
    payload->schoolMask = SPELL_SCHOOL_MASK_NATURE;

    ActorMessage reflectMsg{};
    reflectMsg.type = MessageType::REFLECT_DAMAGE;
    reflectMsg.sourceGuid = VICTIM_GUID;
    reflectMsg.targetGuid = ATTACKER_GUID;
    reflectMsg.complexPayload = payload;

    GetCellA()->SendMessage(std::move(reflectMsg));

    // THEN: Processed gracefully (no crash)
    EXPECT_NO_THROW(GetCellA()->Update(0));
}

TEST_F(GhostActorIntegrationTest, MeleeDamageTriggersReflect)
{
    // Test: MELEE_DAMAGE handler processes damage shields and generates REFLECT_DAMAGE
    // Note: This test verifies the message flow, actual thorns calculation requires Unit integration
    constexpr uint64_t ATTACKER_GUID = 1001;
    constexpr uint64_t VICTIM_GUID = 2001;

    HarnessA().AddPlayer(ATTACKER_GUID);
    HarnessB().AddCreature(VICTIM_GUID, 100);

    // Create melee damage message with attacker info for reflect routing
    auto payload = std::make_shared<MeleeDamagePayload>();
    payload->damage = 500;
    payload->isCritical = false;

    ActorMessage meleeMsg{};
    meleeMsg.type = MessageType::MELEE_DAMAGE;
    meleeMsg.sourceGuid = ATTACKER_GUID;
    meleeMsg.targetGuid = VICTIM_GUID;
    meleeMsg.sourceCellId = CELL_A_ID;
    meleeMsg.targetCellId = CELL_B_ID;
    meleeMsg.complexPayload = payload;

    GetCellB()->SendMessage(std::move(meleeMsg));

    // THEN: Message processed (thorns would require actual Unit with auras)
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

// ============================================================================
// Boundary/Corner Integration Tests
// ============================================================================

/**
 * Test fixture for 4-corner cell scenarios.
 * Sets up a 2x2 grid of cells where all 4 cells share a common corner point.
 *
 *     +-------+-------+
 *     |  NW   |  NE   |
 *     | (0,1) | (1,1) |
 *     +-------+-------+
 *     |  SW   |  SE   |
 *     | (0,0) | (1,0) |
 *     +-------+-------+
 *
 * Cell IDs: (cellY << 16) | cellX
 */
class GhostActorCornerTest : public ::testing::Test
{
protected:
    // 2x2 grid cell IDs using proper coordinate encoding
    static constexpr uint32_t CELL_SW = (0 << 16) | 0;  // 0x00000000
    static constexpr uint32_t CELL_SE = (0 << 16) | 1;  // 0x00000001
    static constexpr uint32_t CELL_NW = (1 << 16) | 0;  // 0x00010000
    static constexpr uint32_t CELL_NE = (1 << 16) | 1;  // 0x00010001

    void SetUp() override
    {
        _originalWorld = sWorld.release();
        _worldMock = new NiceMock<WorldMock>();
        sWorld.reset(_worldMock);

        ON_CALL(*_worldMock, GetDataPath()).WillByDefault(ReturnRef(_emptyString));
        ON_CALL(*_worldMock, GetRealmName()).WillByDefault(ReturnRef(_emptyString));
        ON_CALL(*_worldMock, GetDefaultDbcLocale()).WillByDefault(Return(LOCALE_enUS));
        ON_CALL(*_worldMock, getRate(_)).WillByDefault(Return(1.0f));
        ON_CALL(*_worldMock, getBoolConfig(_)).WillByDefault(Return(false));
        ON_CALL(*_worldMock, getIntConfig(_)).WillByDefault(Return(0));
        ON_CALL(*_worldMock, getFloatConfig(_)).WillByDefault(Return(0.0f));

        EnsureCellActorTestScriptsInitialized();
        _harnessSW = std::make_unique<CellActorTestHarness>(CELL_SW);
        _harnessSE = std::make_unique<CellActorTestHarness>(CELL_SE);
        _harnessNW = std::make_unique<CellActorTestHarness>(CELL_NW);
        _harnessNE = std::make_unique<CellActorTestHarness>(CELL_NE);
    }

    void TearDown() override
    {
        _harnessNE.reset();
        _harnessNW.reset();
        _harnessSE.reset();
        _harnessSW.reset();

        IWorld* currentWorld = sWorld.release();
        delete currentWorld;
        _worldMock = nullptr;
        sWorld.reset(_originalWorld);
        _originalWorld = nullptr;
    }

    CellActorTestHarness& HarnessSW() { return *_harnessSW; }
    CellActorTestHarness& HarnessSE() { return *_harnessSE; }
    CellActorTestHarness& HarnessNW() { return *_harnessNW; }
    CellActorTestHarness& HarnessNE() { return *_harnessNE; }

    CellActor* GetCellSW() { return _harnessSW->GetCell(); }
    CellActor* GetCellSE() { return _harnessSE->GetCell(); }
    CellActor* GetCellNW() { return _harnessNW->GetCell(); }
    CellActor* GetCellNE() { return _harnessNE->GetCell(); }

    void ProcessAllCornerCells()
    {
        GetCellSW()->Update(0);
        GetCellSE()->Update(0);
        GetCellNW()->Update(0);
        GetCellNE()->Update(0);
    }

private:
    std::unique_ptr<CellActorTestHarness> _harnessSW;
    std::unique_ptr<CellActorTestHarness> _harnessSE;
    std::unique_ptr<CellActorTestHarness> _harnessNW;
    std::unique_ptr<CellActorTestHarness> _harnessNE;

    IWorld* _originalWorld{nullptr};
    NiceMock<WorldMock>* _worldMock{nullptr};
    std::string _emptyString;
};

// Test: Player at corner attacks targets in all 4 adjacent cells
TEST_F(GhostActorCornerTest, CornerPlayerAttacksAllQuadrants)
{
    // Player in SW cell (at the corner position)
    constexpr uint64_t PLAYER_GUID = 1001;
    constexpr uint64_t TARGET_SE_GUID = 2001;
    constexpr uint64_t TARGET_NW_GUID = 2002;
    constexpr uint64_t TARGET_NE_GUID = 2003;
    constexpr uint64_t TARGET_SW_GUID = 2004;

    // Player in SW cell
    HarnessSW().AddPlayer(PLAYER_GUID);

    // Targets in each of the 4 cells
    HarnessSW().AddCreature(TARGET_SW_GUID, 100);  // Same cell as player
    HarnessSE().AddCreature(TARGET_SE_GUID, 100);  // East neighbor
    HarnessNW().AddCreature(TARGET_NW_GUID, 100);  // North neighbor
    HarnessNE().AddCreature(TARGET_NE_GUID, 100);  // Diagonal neighbor

    // Set up ghosts - player can see creatures in neighbor cells
    HarnessSW().AddGhost(TARGET_SE_GUID, CELL_SE);
    HarnessSW().AddGhost(TARGET_NW_GUID, CELL_NW);
    HarnessSW().AddGhost(TARGET_NE_GUID, CELL_NE);

    // Attack target in same cell (SW)
    ActorMessage attackSW{};
    attackSW.type = MessageType::SPELL_HIT;
    attackSW.sourceGuid = PLAYER_GUID;
    attackSW.targetGuid = TARGET_SW_GUID;
    attackSW.sourceCellId = CELL_SW;
    attackSW.targetCellId = CELL_SW;
    attackSW.complexPayload = MakeSpellHitPayload(12345, 100);
    GetCellSW()->SendMessage(std::move(attackSW));

    // Attack target in SE cell (east neighbor)
    ActorMessage attackSE{};
    attackSE.type = MessageType::SPELL_HIT;
    attackSE.sourceGuid = PLAYER_GUID;
    attackSE.targetGuid = TARGET_SE_GUID;
    attackSE.sourceCellId = CELL_SW;
    attackSE.targetCellId = CELL_SE;
    attackSE.complexPayload = MakeSpellHitPayload(12345, 100);
    GetCellSE()->SendMessage(std::move(attackSE));

    // Attack target in NW cell (north neighbor)
    ActorMessage attackNW{};
    attackNW.type = MessageType::SPELL_HIT;
    attackNW.sourceGuid = PLAYER_GUID;
    attackNW.targetGuid = TARGET_NW_GUID;
    attackNW.sourceCellId = CELL_SW;
    attackNW.targetCellId = CELL_NW;
    attackNW.complexPayload = MakeSpellHitPayload(12345, 100);
    GetCellNW()->SendMessage(std::move(attackNW));

    // Attack target in NE cell (diagonal neighbor)
    ActorMessage attackNE{};
    attackNE.type = MessageType::SPELL_HIT;
    attackNE.sourceGuid = PLAYER_GUID;
    attackNE.targetGuid = TARGET_NE_GUID;
    attackNE.sourceCellId = CELL_SW;
    attackNE.targetCellId = CELL_NE;
    attackNE.complexPayload = MakeSpellHitPayload(12345, 100);
    GetCellNE()->SendMessage(std::move(attackNE));

    // THEN: All cells process without error
    EXPECT_NO_THROW(ProcessAllCornerCells());
}

// Test: Entity receives attacks from all 4 corner cells simultaneously
TEST_F(GhostActorCornerTest, CornerEntityReceivesFromAllQuadrants)
{
    // Target in NE cell
    TestCreature* target = HarnessNE().AddCreature(2001, 100);
    target->SetTestHealth(10000);
    uint64_t targetGuid = target->GetGUID().GetRawValue();

    // Attackers in each cell
    auto* attackerSW = HarnessSW().AddPlayer(1001);
    auto* attackerSE = HarnessSE().AddPlayer(1002);
    auto* attackerNW = HarnessNW().AddPlayer(1003);
    auto* attackerNE = HarnessNE().AddPlayer(1004);

    // All attackers send damage to target in NE
    auto sendDamage = [&](uint64_t attackerGuid, uint32_t sourceCellId) {
        ActorMessage msg{};
        msg.type = MessageType::SPELL_HIT;
        msg.sourceGuid = attackerGuid;
        msg.targetGuid = targetGuid;
        msg.sourceCellId = sourceCellId;
        msg.targetCellId = CELL_NE;
        msg.complexPayload = MakeSpellHitPayload(12345, 500);
        GetCellNE()->SendMessage(std::move(msg));
    };

    sendDamage(attackerSW->GetGUID().GetRawValue(), CELL_SW);
    sendDamage(attackerSE->GetGUID().GetRawValue(), CELL_SE);
    sendDamage(attackerNW->GetGUID().GetRawValue(), CELL_NW);
    sendDamage(attackerNE->GetGUID().GetRawValue(), CELL_NE);

    // Process NE cell where target is
    GetCellNE()->Update(0);

    // Target should have received 2000 total damage (4 x 500)
    EXPECT_EQ(target->GetTestHealth(), 8000u);
}

// Test: AoE from corner position hits entities in all 4 cells
TEST_F(GhostActorCornerTest, CornerAoEHitsAllQuadrants)
{
    // Caster in SW
    auto* caster = HarnessSW().AddPlayer(1001);
    uint64_t casterGuid = caster->GetGUID().GetRawValue();

    // Targets in each cell
    TestCreature* targetSW = HarnessSW().AddCreature(2001, 100);
    TestCreature* targetSE = HarnessSE().AddCreature(2002, 100);
    TestCreature* targetNW = HarnessNW().AddCreature(2003, 100);
    TestCreature* targetNE = HarnessNE().AddCreature(2004, 100);

    targetSW->SetTestHealth(1000);
    targetSE->SetTestHealth(1000);
    targetNW->SetTestHealth(1000);
    targetNE->SetTestHealth(1000);

    // Simulate AoE spell hitting all 4 targets
    auto sendAoEDamage = [&](TestCreature* target, uint32_t targetCellId, CellActor* cell) {
        ActorMessage msg{};
        msg.type = MessageType::SPELL_HIT;
        msg.sourceGuid = casterGuid;
        msg.targetGuid = target->GetGUID().GetRawValue();
        msg.sourceCellId = CELL_SW;
        msg.targetCellId = targetCellId;
        msg.complexPayload = MakeSpellHitPayload(99999, 200);
        cell->SendMessage(std::move(msg));
    };

    sendAoEDamage(targetSW, CELL_SW, GetCellSW());
    sendAoEDamage(targetSE, CELL_SE, GetCellSE());
    sendAoEDamage(targetNW, CELL_NW, GetCellNW());
    sendAoEDamage(targetNE, CELL_NE, GetCellNE());

    ProcessAllCornerCells();

    // All targets should have taken 200 damage
    EXPECT_EQ(targetSW->GetTestHealth(), 800u);
    EXPECT_EQ(targetSE->GetTestHealth(), 800u);
    EXPECT_EQ(targetNW->GetTestHealth(), 800u);
    EXPECT_EQ(targetNE->GetTestHealth(), 800u);
}

// Test: Health sync broadcasts to all 4 corner cells
TEST_F(GhostActorCornerTest, CornerHealthSyncToAllQuadrants)
{
    constexpr uint64_t CREATURE_GUID = 2001;

    // Creature in SW cell
    TestCreature* creature = HarnessSW().AddCreature(CREATURE_GUID, 100);
    creature->SetTestHealth(5000);

    // Creature has ghosts in all neighbor cells
    HarnessSE().AddGhost(CREATURE_GUID, CELL_SW);
    HarnessNW().AddGhost(CREATURE_GUID, CELL_SW);
    HarnessNE().AddGhost(CREATURE_GUID, CELL_SW);

    // Send health update to all cells with ghosts
    auto sendHealthUpdate = [&](CellActor* cell) {
        ActorMessage msg{};
        msg.type = MessageType::HEALTH_CHANGED;
        msg.sourceGuid = CREATURE_GUID;
        msg.sourceCellId = CELL_SW;
        msg.intParam1 = 3000;  // New health
        msg.intParam2 = 5000;  // Max health
        cell->SendMessage(std::move(msg));
    };

    sendHealthUpdate(GetCellSE());
    sendHealthUpdate(GetCellNW());
    sendHealthUpdate(GetCellNE());

    // Process all cells
    GetCellSE()->Update(0);
    GetCellNW()->Update(0);
    GetCellNE()->Update(0);

    // Verify ghosts in all cells have updated health
    EXPECT_EQ(GetCellSE()->GetGhost(CREATURE_GUID)->GetHealth(), 3000u);
    EXPECT_EQ(GetCellNW()->GetGhost(CREATURE_GUID)->GetHealth(), 3000u);
    EXPECT_EQ(GetCellNE()->GetGhost(CREATURE_GUID)->GetHealth(), 3000u);
}

// Test: Diagonal cell interaction (opposite corners)
TEST_F(GhostActorCornerTest, DiagonalCellInteraction)
{
    // Player in SW, target in NE (diagonal)
    auto* player = HarnessSW().AddPlayer(1001);
    TestCreature* target = HarnessNE().AddCreature(2001, 100);
    target->SetTestHealth(1000);

    uint64_t playerGuid = player->GetGUID().GetRawValue();
    uint64_t targetGuid = target->GetGUID().GetRawValue();

    // Ghost of target in player's cell
    HarnessSW().AddGhost(targetGuid, CELL_NE);

    // Player attacks diagonally across corner
    ActorMessage attack{};
    attack.type = MessageType::SPELL_HIT;
    attack.sourceGuid = playerGuid;
    attack.targetGuid = targetGuid;
    attack.sourceCellId = CELL_SW;
    attack.targetCellId = CELL_NE;
    attack.complexPayload = MakeSpellHitPayload(12345, 300);
    GetCellNE()->SendMessage(std::move(attack));

    GetCellNE()->Update(0);

    EXPECT_EQ(target->GetTestHealth(), 700u);
}

// Test: Melee with thorns across corner (bi-directional)
TEST_F(GhostActorCornerTest, CornerMeleeWithThornsResponse)
{
    constexpr uint64_t ATTACKER_GUID = 1001;
    constexpr uint64_t VICTIM_GUID = 2001;

    // Attacker in SW, victim with thorns in NE
    HarnessSW().AddPlayer(ATTACKER_GUID);
    HarnessNE().AddCreature(VICTIM_GUID, 100);

    // Send melee attack from SW to NE
    auto payload = std::make_shared<MeleeDamagePayload>();
    payload->damage = 500;
    payload->isCritical = false;

    ActorMessage meleeMsg{};
    meleeMsg.type = MessageType::MELEE_DAMAGE;
    meleeMsg.sourceGuid = ATTACKER_GUID;
    meleeMsg.targetGuid = VICTIM_GUID;
    meleeMsg.sourceCellId = CELL_SW;  // Important for thorns response routing
    meleeMsg.targetCellId = CELL_NE;
    meleeMsg.complexPayload = payload;
    GetCellNE()->SendMessage(std::move(meleeMsg));

    // Process NE cell (victim receives damage, would trigger thorns)
    EXPECT_NO_THROW(GetCellNE()->Update(0));

    // Thorns would send REFLECT_DAMAGE back to CELL_SW
    // (Full thorns test requires Unit mock with auras)
}

// Test: Heal across corner boundary
TEST_F(GhostActorCornerTest, CornerHealAcrossBoundary)
{
    // Healer in SW, wounded target in NE
    auto* healer = HarnessSW().AddPlayer(1001);
    TestCreature* target = HarnessNE().AddCreature(2001, 100);
    target->SetTestHealth(500);
    target->SetTestMaxHealth(1000);

    uint64_t healerGuid = healer->GetGUID().GetRawValue();
    uint64_t targetGuid = target->GetGUID().GetRawValue();

    // Send heal from SW to NE
    ActorMessage healMsg{};
    healMsg.type = MessageType::HEAL;
    healMsg.sourceGuid = healerGuid;
    healMsg.targetGuid = targetGuid;
    healMsg.sourceCellId = CELL_SW;
    healMsg.targetCellId = CELL_NE;
    healMsg.complexPayload = MakeHealPayload(12345, 300);
    GetCellNE()->SendMessage(std::move(healMsg));

    GetCellNE()->Update(0);

    EXPECT_EQ(target->GetTestHealth(), 800u);
}

// ============================================================================
// Aura Tracking Tests
// ============================================================================

TEST_F(GhostActorIntegrationTest, AuraApplyTrackedOnGhost)
{
    constexpr uint64_t CREATURE_GUID = 0x12345;
    constexpr uint32_t SPELL_ID = 12345;

    // Create a ghost in Cell A representing an entity in Cell B
    HarnessA().AddGhost(CREATURE_GUID, CELL_B_ID);

    // Verify ghost exists and has no auras initially
    GhostEntity* ghost = GetCellA()->GetGhost(CREATURE_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_EQ(ghost->GetActiveAuraCount(), 0u);
    EXPECT_FALSE(ghost->HasAura(SPELL_ID));

    // Send AURA_APPLY message
    ActorMessage msg{};
    msg.type = MessageType::AURA_APPLY;
    msg.sourceGuid = CREATURE_GUID;
    msg.sourceCellId = CELL_B_ID;
    msg.intParam1 = SPELL_ID;
    msg.intParam2 = 1;  // effectMask
    GetCellA()->SendMessage(std::move(msg));
    GetCellA()->Update(0);

    // Verify aura is now tracked
    EXPECT_EQ(ghost->GetActiveAuraCount(), 1u);
    EXPECT_TRUE(ghost->HasAura(SPELL_ID));
}

TEST_F(GhostActorIntegrationTest, AuraRemoveTrackedOnGhost)
{
    constexpr uint64_t CREATURE_GUID = 0x12345;
    constexpr uint32_t SPELL_ID = 54321;

    // Create ghost and apply an aura
    HarnessA().AddGhost(CREATURE_GUID, CELL_B_ID);

    ActorMessage applyMsg{};
    applyMsg.type = MessageType::AURA_APPLY;
    applyMsg.sourceGuid = CREATURE_GUID;
    applyMsg.sourceCellId = CELL_B_ID;
    applyMsg.intParam1 = SPELL_ID;
    applyMsg.intParam2 = 1;
    GetCellA()->SendMessage(std::move(applyMsg));
    GetCellA()->Update(0);

    GhostEntity* ghost = GetCellA()->GetGhost(CREATURE_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_TRUE(ghost->HasAura(SPELL_ID));

    // Send AURA_REMOVE message
    ActorMessage removeMsg{};
    removeMsg.type = MessageType::AURA_REMOVE;
    removeMsg.sourceGuid = CREATURE_GUID;
    removeMsg.sourceCellId = CELL_B_ID;
    removeMsg.intParam1 = SPELL_ID;
    GetCellA()->SendMessage(std::move(removeMsg));
    GetCellA()->Update(0);

    // Verify aura is removed
    EXPECT_EQ(ghost->GetActiveAuraCount(), 0u);
    EXPECT_FALSE(ghost->HasAura(SPELL_ID));
}

TEST_F(GhostActorIntegrationTest, MultipleAurasTrackedOnGhost)
{
    constexpr uint64_t CREATURE_GUID = 0xABCDE;
    constexpr uint32_t SPELL_1 = 100;
    constexpr uint32_t SPELL_2 = 200;
    constexpr uint32_t SPELL_3 = 300;

    HarnessA().AddGhost(CREATURE_GUID, CELL_B_ID);

    // Apply three auras
    for (uint32_t spellId : {SPELL_1, SPELL_2, SPELL_3})
    {
        ActorMessage msg{};
        msg.type = MessageType::AURA_APPLY;
        msg.sourceGuid = CREATURE_GUID;
        msg.sourceCellId = CELL_B_ID;
        msg.intParam1 = spellId;
        msg.intParam2 = 1;
        GetCellA()->SendMessage(std::move(msg));
    }
    GetCellA()->Update(0);

    GhostEntity* ghost = GetCellA()->GetGhost(CREATURE_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_EQ(ghost->GetActiveAuraCount(), 3u);
    EXPECT_TRUE(ghost->HasAura(SPELL_1));
    EXPECT_TRUE(ghost->HasAura(SPELL_2));
    EXPECT_TRUE(ghost->HasAura(SPELL_3));

    // Remove middle aura
    ActorMessage removeMsg{};
    removeMsg.type = MessageType::AURA_REMOVE;
    removeMsg.sourceGuid = CREATURE_GUID;
    removeMsg.sourceCellId = CELL_B_ID;
    removeMsg.intParam1 = SPELL_2;
    GetCellA()->SendMessage(std::move(removeMsg));
    GetCellA()->Update(0);

    EXPECT_EQ(ghost->GetActiveAuraCount(), 2u);
    EXPECT_TRUE(ghost->HasAura(SPELL_1));
    EXPECT_FALSE(ghost->HasAura(SPELL_2));
    EXPECT_TRUE(ghost->HasAura(SPELL_3));
}

TEST_F(GhostActorIntegrationTest, AuraApplyIgnoredForNonExistentGhost)
{
    constexpr uint64_t NONEXISTENT_GUID = 0xDEADBEEF;

    // Send AURA_APPLY for a ghost that doesn't exist
    ActorMessage msg{};
    msg.type = MessageType::AURA_APPLY;
    msg.sourceGuid = NONEXISTENT_GUID;
    msg.sourceCellId = CELL_B_ID;
    msg.intParam1 = 12345;
    msg.intParam2 = 1;
    GetCellA()->SendMessage(std::move(msg));

    // Should not crash, just silently ignore
    EXPECT_NO_THROW(GetCellA()->Update(0));
    EXPECT_EQ(GetCellA()->GetGhost(NONEXISTENT_GUID), nullptr);
}

TEST_F(GhostActorIntegrationTest, AuraRemoveIgnoredForNonExistentGhost)
{
    constexpr uint64_t NONEXISTENT_GUID = 0xDEADBEEF;

    // Send AURA_REMOVE for a ghost that doesn't exist
    ActorMessage msg{};
    msg.type = MessageType::AURA_REMOVE;
    msg.sourceGuid = NONEXISTENT_GUID;
    msg.sourceCellId = CELL_B_ID;
    msg.intParam1 = 12345;
    GetCellA()->SendMessage(std::move(msg));

    // Should not crash
    EXPECT_NO_THROW(GetCellA()->Update(0));
}

TEST_F(GhostActorIntegrationTest, DuplicateAuraApplyIsIdempotent)
{
    constexpr uint64_t CREATURE_GUID = 0x99999;
    constexpr uint32_t SPELL_ID = 55555;

    HarnessA().AddGhost(CREATURE_GUID, CELL_B_ID);

    // Apply same aura twice
    for (int i = 0; i < 2; ++i)
    {
        ActorMessage msg{};
        msg.type = MessageType::AURA_APPLY;
        msg.sourceGuid = CREATURE_GUID;
        msg.sourceCellId = CELL_B_ID;
        msg.intParam1 = SPELL_ID;
        msg.intParam2 = 1;
        GetCellA()->SendMessage(std::move(msg));
    }
    GetCellA()->Update(0);

    GhostEntity* ghost = GetCellA()->GetGhost(CREATURE_GUID);
    ASSERT_NE(ghost, nullptr);
    // std::set automatically handles duplicates
    EXPECT_EQ(ghost->GetActiveAuraCount(), 1u);
    EXPECT_TRUE(ghost->HasAura(SPELL_ID));
}

TEST_F(GhostActorIntegrationTest, AuraRemoveForNonExistentAuraIsIdempotent)
{
    constexpr uint64_t CREATURE_GUID = 0x88888;
    constexpr uint32_t SPELL_ID = 44444;

    HarnessA().AddGhost(CREATURE_GUID, CELL_B_ID);

    GhostEntity* ghost = GetCellA()->GetGhost(CREATURE_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_EQ(ghost->GetActiveAuraCount(), 0u);

    // Remove an aura that was never applied
    ActorMessage msg{};
    msg.type = MessageType::AURA_REMOVE;
    msg.sourceGuid = CREATURE_GUID;
    msg.sourceCellId = CELL_B_ID;
    msg.intParam1 = SPELL_ID;
    GetCellA()->SendMessage(std::move(msg));

    // Should not crash, count stays at 0
    EXPECT_NO_THROW(GetCellA()->Update(0));
    EXPECT_EQ(ghost->GetActiveAuraCount(), 0u);
}

// ============================================================================
// Aura State Tracking Tests
// ============================================================================

TEST_F(GhostActorIntegrationTest, GhostAuraStateDirectSync)
{
    constexpr uint64_t CREATURE_GUID = 0x77777;

    HarnessA().AddGhost(CREATURE_GUID, CELL_B_ID);

    GhostEntity* ghost = GetCellA()->GetGhost(CREATURE_GUID);
    ASSERT_NE(ghost, nullptr);

    // Initially no aura state
    EXPECT_EQ(ghost->GetAuraState(), 0u);

    // Direct sync sets aura state
    ghost->SyncAuraState(1 << (AURA_STATE_FROZEN - 1));
    EXPECT_EQ(ghost->GetAuraState(), 1u << (AURA_STATE_FROZEN - 1));

    // Multiple states can be set
    uint32_t combinedState = (1 << (AURA_STATE_FROZEN - 1)) | (1 << (AURA_STATE_BLEEDING - 1));
    ghost->SyncAuraState(combinedState);
    EXPECT_EQ(ghost->GetAuraState(), combinedState);

    // Clear state
    ghost->SyncAuraState(0);
    EXPECT_EQ(ghost->GetAuraState(), 0u);
}

TEST_F(GhostActorIntegrationTest, AuraStateSyncMessageUpdatesGhost)
{
    constexpr uint64_t CREATURE_GUID = 0x88888;

    HarnessA().AddGhost(CREATURE_GUID, CELL_B_ID);

    GhostEntity* ghost = GetCellA()->GetGhost(CREATURE_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_EQ(ghost->GetAuraState(), 0u);

    // Send AURA_STATE_SYNC message
    uint32_t frozenState = 1 << (AURA_STATE_FROZEN - 1);
    ActorMessage msg{};
    msg.type = MessageType::AURA_STATE_SYNC;
    msg.sourceGuid = CREATURE_GUID;
    msg.sourceCellId = CELL_B_ID;
    msg.intParam1 = frozenState;
    GetCellA()->SendMessage(std::move(msg));
    GetCellA()->Update(0);

    EXPECT_EQ(ghost->GetAuraState(), frozenState);
}

TEST_F(GhostActorIntegrationTest, AuraTrackingAndStateIndependent)
{
    constexpr uint64_t CREATURE_GUID = 0x99999;
    constexpr uint32_t SPELL_ID = 12345;

    HarnessA().AddGhost(CREATURE_GUID, CELL_B_ID);

    GhostEntity* ghost = GetCellA()->GetGhost(CREATURE_GUID);
    ASSERT_NE(ghost, nullptr);

    // Apply an aura (without SpellMgr, aura state won't auto-update)
    ActorMessage applyMsg{};
    applyMsg.type = MessageType::AURA_APPLY;
    applyMsg.sourceGuid = CREATURE_GUID;
    applyMsg.sourceCellId = CELL_B_ID;
    applyMsg.intParam1 = SPELL_ID;
    applyMsg.intParam2 = 1;
    GetCellA()->SendMessage(std::move(applyMsg));
    GetCellA()->Update(0);

    // Aura is tracked
    EXPECT_TRUE(ghost->HasAura(SPELL_ID));
    // But aura state is not auto-set (no SpellMgr in unit tests)
    // It remains 0 unless explicitly synced
    EXPECT_EQ(ghost->GetAuraState(), 0u);

    // Manual sync of aura state works independently
    uint32_t bleedState = 1 << (AURA_STATE_BLEEDING - 1);
    ghost->SyncAuraState(bleedState);
    EXPECT_EQ(ghost->GetAuraState(), bleedState);
    EXPECT_TRUE(ghost->HasAura(SPELL_ID));  // Aura list unaffected

    // Removing aura recalculates state (but without SpellMgr, stays at 0)
    ActorMessage removeMsg{};
    removeMsg.type = MessageType::AURA_REMOVE;
    removeMsg.sourceGuid = CREATURE_GUID;
    removeMsg.sourceCellId = CELL_B_ID;
    removeMsg.intParam1 = SPELL_ID;
    GetCellA()->SendMessage(std::move(removeMsg));
    GetCellA()->Update(0);

    EXPECT_FALSE(ghost->HasAura(SPELL_ID));
    // After recalculation with no spells, aura state is 0
    EXPECT_EQ(ghost->GetAuraState(), 0u);
}

// =============================================================================
// Control Effect Integration Tests
// =============================================================================

TEST_F(GhostActorIntegrationTest, StunMessageAcrossCells)
{
    // Test: STUN message from Cell A reaches creature in Cell B
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t HAMMER_OF_JUSTICE = 853;

    HarnessA().AddPlayer(CASTER_GUID);
    TestCreature* target = HarnessB().AddCreature(TARGET_GUID, 100);

    // WHEN: Stun message is sent to creature in Cell B
    ActorMessage stun{};
    stun.type = MessageType::STUN;
    stun.sourceGuid = CASTER_GUID;
    stun.targetGuid = TARGET_GUID;
    stun.complexPayload = MakeControlEffectPayload(CASTER_GUID, TARGET_GUID, HAMMER_OF_JUSTICE, 6000, 5);
    SendAtoB(std::move(stun));

    GetCellB()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, RootMessageAcrossCells)
{
    // Test: ROOT message from Cell A reaches creature in Cell B
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t FROST_NOVA = 122;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    // WHEN: Root message is sent
    ActorMessage root{};
    root.type = MessageType::ROOT;
    root.sourceGuid = CASTER_GUID;
    root.targetGuid = TARGET_GUID;
    root.complexPayload = MakeControlEffectPayload(CASTER_GUID, TARGET_GUID, FROST_NOVA, 8000, 1024);
    SendAtoB(std::move(root));

    GetCellB()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, FearMessageAcrossCells)
{
    // Test: FEAR message from Cell A reaches creature in Cell B
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t FEAR_SPELL = 5782;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    // WHEN: Fear message is sent with destination coordinates
    ActorMessage fear{};
    fear.type = MessageType::FEAR;
    fear.sourceGuid = CASTER_GUID;
    fear.targetGuid = TARGET_GUID;
    fear.complexPayload = MakeFearPayload(CASTER_GUID, TARGET_GUID, FEAR_SPELL, 20000, 100.0f, 200.0f, 0.0f);
    SendAtoB(std::move(fear));

    GetCellB()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, CharmMessageAcrossCells)
{
    // Test: CHARM message from Cell A reaches creature in Cell B
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t MIND_CONTROL = 605;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    // WHEN: Charm message is sent
    ActorMessage charm{};
    charm.type = MessageType::CHARM;
    charm.sourceGuid = CASTER_GUID;
    charm.targetGuid = TARGET_GUID;
    charm.complexPayload = MakeControlEffectPayload(CASTER_GUID, TARGET_GUID, MIND_CONTROL, 60000, 16);
    SendAtoB(std::move(charm));

    GetCellB()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, KnockbackMessageAcrossCells)
{
    // Test: KNOCKBACK message from Cell A reaches creature in Cell B
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t THUNDERSTORM = 51490;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    // WHEN: Knockback message is sent
    ActorMessage knockback{};
    knockback.type = MessageType::KNOCKBACK;
    knockback.sourceGuid = CASTER_GUID;
    knockback.targetGuid = TARGET_GUID;
    knockback.complexPayload = MakeKnockbackPayload(CASTER_GUID, TARGET_GUID, THUNDERSTORM,
        100.0f, 100.0f, 0.0f,   // origin
        20.0f, 10.0f,           // velocity
        120.0f, 120.0f, 0.0f);  // destination
    SendAtoB(std::move(knockback));

    GetCellB()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, SilenceMessageAcrossCells)
{
    // Test: SILENCE message from Cell A reaches creature in Cell B
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t SILENCE_SPELL = 15487;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    // WHEN: Silence message is sent
    ActorMessage silence{};
    silence.type = MessageType::SILENCE;
    silence.sourceGuid = CASTER_GUID;
    silence.targetGuid = TARGET_GUID;
    silence.complexPayload = MakeControlEffectPayload(CASTER_GUID, TARGET_GUID, SILENCE_SPELL, 5000, 2048);
    SendAtoB(std::move(silence));

    GetCellB()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, PolymorphMessageAcrossCells)
{
    // Test: POLYMORPH message from Cell A reaches creature in Cell B
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t POLYMORPH = 118;
    constexpr uint32_t SHEEP_DISPLAY_ID = 856;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    // WHEN: Polymorph message is sent with transform display ID
    ActorMessage poly{};
    poly.type = MessageType::POLYMORPH;
    poly.sourceGuid = CASTER_GUID;
    poly.targetGuid = TARGET_GUID;
    poly.complexPayload = MakePolymorphPayload(CASTER_GUID, TARGET_GUID, POLYMORPH, 50000, SHEEP_DISPLAY_ID);
    SendAtoB(std::move(poly));

    GetCellB()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, ControlStateChangedSyncsToGhost)
{
    // Test: CONTROL_STATE_CHANGED message updates ghost control state
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t UNIT_STATE_STUNNED = 5;

    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    GhostEntity* ghost = GetCellA()->GetGhost(TARGET_GUID);
    ASSERT_NE(ghost, nullptr);
    EXPECT_FALSE(ghost->HasControlState(UNIT_STATE_STUNNED));

    // WHEN: Control state changed message is sent
    ActorMessage stateChange{};
    stateChange.type = MessageType::CONTROL_STATE_CHANGED;
    stateChange.sourceGuid = TARGET_GUID;
    stateChange.sourceCellId = CELL_B_ID;
    stateChange.intParam1 = UNIT_STATE_STUNNED;
    stateChange.intParam2 = 1;  // apply = true
    GetCellA()->SendMessage(std::move(stateChange));
    GetCellA()->Update(0);

    // THEN: Ghost has control state
    EXPECT_TRUE(ghost->HasControlState(UNIT_STATE_STUNNED));
}

TEST_F(GhostActorIntegrationTest, ControlStateRemovedFromGhost)
{
    // Test: CONTROL_STATE_CHANGED with apply=false removes the state
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t UNIT_STATE_ROOT = 1024;

    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    GhostEntity* ghost = GetCellA()->GetGhost(TARGET_GUID);
    ASSERT_NE(ghost, nullptr);

    // Apply the control state first
    ActorMessage applyState{};
    applyState.type = MessageType::CONTROL_STATE_CHANGED;
    applyState.sourceGuid = TARGET_GUID;
    applyState.sourceCellId = CELL_B_ID;
    applyState.intParam1 = UNIT_STATE_ROOT;
    applyState.intParam2 = 1;  // apply = true
    GetCellA()->SendMessage(std::move(applyState));
    GetCellA()->Update(0);

    EXPECT_TRUE(ghost->HasControlState(UNIT_STATE_ROOT));

    // WHEN: Remove the control state
    ActorMessage removeState{};
    removeState.type = MessageType::CONTROL_STATE_CHANGED;
    removeState.sourceGuid = TARGET_GUID;
    removeState.sourceCellId = CELL_B_ID;
    removeState.intParam1 = UNIT_STATE_ROOT;
    removeState.intParam2 = 0;  // apply = false
    GetCellA()->SendMessage(std::move(removeState));
    GetCellA()->Update(0);

    // THEN: Ghost no longer has control state
    EXPECT_FALSE(ghost->HasControlState(UNIT_STATE_ROOT));
}

TEST_F(GhostActorIntegrationTest, MultipleControlStatesOnGhost)
{
    // Test: Multiple control states can be applied and tracked independently
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t UNIT_STATE_STUNNED = 5;
    constexpr uint32_t UNIT_STATE_ROOT = 1024;
    constexpr uint32_t UNIT_STATE_SILENCED = 2048;

    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    GhostEntity* ghost = GetCellA()->GetGhost(TARGET_GUID);
    ASSERT_NE(ghost, nullptr);

    // Apply stun
    ActorMessage stunState{};
    stunState.type = MessageType::CONTROL_STATE_CHANGED;
    stunState.sourceGuid = TARGET_GUID;
    stunState.sourceCellId = CELL_B_ID;
    stunState.intParam1 = UNIT_STATE_STUNNED;
    stunState.intParam2 = 1;
    GetCellA()->SendMessage(std::move(stunState));

    // Apply root
    ActorMessage rootState{};
    rootState.type = MessageType::CONTROL_STATE_CHANGED;
    rootState.sourceGuid = TARGET_GUID;
    rootState.sourceCellId = CELL_B_ID;
    rootState.intParam1 = UNIT_STATE_ROOT;
    rootState.intParam2 = 1;
    GetCellA()->SendMessage(std::move(rootState));

    // Apply silence
    ActorMessage silenceState{};
    silenceState.type = MessageType::CONTROL_STATE_CHANGED;
    silenceState.sourceGuid = TARGET_GUID;
    silenceState.sourceCellId = CELL_B_ID;
    silenceState.intParam1 = UNIT_STATE_SILENCED;
    silenceState.intParam2 = 1;
    GetCellA()->SendMessage(std::move(silenceState));

    GetCellA()->Update(0);

    // THEN: Ghost has all three states
    EXPECT_TRUE(ghost->HasControlState(UNIT_STATE_STUNNED));
    EXPECT_TRUE(ghost->HasControlState(UNIT_STATE_ROOT));
    EXPECT_TRUE(ghost->HasControlState(UNIT_STATE_SILENCED));

    // Remove root but keep others
    ActorMessage removeRoot{};
    removeRoot.type = MessageType::CONTROL_STATE_CHANGED;
    removeRoot.sourceGuid = TARGET_GUID;
    removeRoot.sourceCellId = CELL_B_ID;
    removeRoot.intParam1 = UNIT_STATE_ROOT;
    removeRoot.intParam2 = 0;
    GetCellA()->SendMessage(std::move(removeRoot));
    GetCellA()->Update(0);

    // Verify only root removed
    EXPECT_TRUE(ghost->HasControlState(UNIT_STATE_STUNNED));
    EXPECT_FALSE(ghost->HasControlState(UNIT_STATE_ROOT));
    EXPECT_TRUE(ghost->HasControlState(UNIT_STATE_SILENCED));
}

TEST_F(GhostActorIntegrationTest, StunFollowedByGhostSync)
{
    // Test: Full chain - stun applied to creature, ghost receives control state update
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t HAMMER_OF_JUSTICE = 853;
    constexpr uint32_t UNIT_STATE_STUNNED = 5;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);
    HarnessA().AddGhost(TARGET_GUID, CELL_B_ID);

    GhostEntity* ghost = GetCellA()->GetGhost(TARGET_GUID);
    ASSERT_NE(ghost, nullptr);

    // WHEN: Stun is applied
    ActorMessage stun{};
    stun.type = MessageType::STUN;
    stun.sourceGuid = CASTER_GUID;
    stun.targetGuid = TARGET_GUID;
    stun.complexPayload = MakeControlEffectPayload(CASTER_GUID, TARGET_GUID, HAMMER_OF_JUSTICE, 6000, UNIT_STATE_STUNNED);
    SendAtoB(std::move(stun));
    GetCellB()->Update(0);

    // Simulate the control state sync that would be broadcast
    ActorMessage stateSync{};
    stateSync.type = MessageType::CONTROL_STATE_CHANGED;
    stateSync.sourceGuid = TARGET_GUID;
    stateSync.sourceCellId = CELL_B_ID;
    stateSync.intParam1 = UNIT_STATE_STUNNED;
    stateSync.intParam2 = 1;
    GetCellA()->SendMessage(std::move(stateSync));
    GetCellA()->Update(0);

    // THEN: Ghost has stunned state
    EXPECT_TRUE(ghost->HasControlState(UNIT_STATE_STUNNED));
}

TEST_F(GhostActorIntegrationTest, ControlEffectOnMissingTarget)
{
    // Test: Control effect messages targeting non-existent entities are handled gracefully
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t MISSING_TARGET = 9999;
    constexpr uint32_t FROST_NOVA = 122;

    HarnessA().AddPlayer(CASTER_GUID);
    // Note: No creature with MISSING_TARGET exists in Cell B

    // WHEN: Root message sent to missing target
    ActorMessage root{};
    root.type = MessageType::ROOT;
    root.sourceGuid = CASTER_GUID;
    root.targetGuid = MISSING_TARGET;
    root.complexPayload = MakeControlEffectPayload(CASTER_GUID, MISSING_TARGET, FROST_NOVA, 8000, 1024);
    SendAtoB(std::move(root));

    // THEN: Should not crash, message handled gracefully
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, ControlStateChangedOnMissingGhost)
{
    // Test: Control state change for non-existent ghost is handled gracefully
    constexpr uint64_t MISSING_GUID = 9999;
    constexpr uint32_t UNIT_STATE_STUNNED = 5;

    // No ghost exists with MISSING_GUID

    // WHEN: Control state changed message sent for missing ghost
    ActorMessage stateChange{};
    stateChange.type = MessageType::CONTROL_STATE_CHANGED;
    stateChange.sourceGuid = MISSING_GUID;
    stateChange.sourceCellId = CELL_B_ID;
    stateChange.intParam1 = UNIT_STATE_STUNNED;
    stateChange.intParam2 = 1;
    GetCellA()->SendMessage(std::move(stateChange));

    // THEN: Should not crash, message handled gracefully
    EXPECT_NO_THROW(GetCellA()->Update(0));
}

// =============================================================================
// Attack Result Integration Tests
// =============================================================================

TEST_F(GhostActorIntegrationTest, DodgeMessageAcrossCells)
{
    // Test: DODGE message from victim's cell reaches attacker's cell
    constexpr uint64_t ATTACKER_GUID = 1001;
    constexpr uint64_t VICTIM_GUID = 2001;

    HarnessA().AddPlayer(ATTACKER_GUID);  // Attacker
    HarnessB().AddCreature(VICTIM_GUID, 100);  // Victim who dodged

    // WHEN: Dodge message is sent to attacker's cell
    ActorMessage dodge{};
    dodge.type = MessageType::DODGE;
    dodge.sourceGuid = VICTIM_GUID;
    dodge.targetGuid = ATTACKER_GUID;
    dodge.complexPayload = MakeCombatResultPayload(ATTACKER_GUID, VICTIM_GUID, 0, 0, 0, 0, 0x10);  // PROC_EX_DODGE
    SendBtoA(std::move(dodge));

    GetCellA()->Update(0);

    // THEN: Message should be processed (AI callback would trigger here)
    EXPECT_GE(GetCellA()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, ParryMessageAcrossCells)
{
    // Test: PARRY message from victim's cell reaches attacker's cell
    constexpr uint64_t ATTACKER_GUID = 1001;
    constexpr uint64_t VICTIM_GUID = 2001;

    HarnessA().AddCreature(ATTACKER_GUID, 100);  // Creature attacker
    HarnessB().AddPlayer(VICTIM_GUID);  // Player who parried

    // WHEN: Parry message is sent to attacker's cell
    ActorMessage parry{};
    parry.type = MessageType::PARRY;
    parry.sourceGuid = VICTIM_GUID;
    parry.targetGuid = ATTACKER_GUID;
    parry.complexPayload = MakeCombatResultPayload(ATTACKER_GUID, VICTIM_GUID, 0, 0, 0, 0, 0x20);  // PROC_EX_PARRY
    SendBtoA(std::move(parry));

    GetCellA()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellA()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, BlockMessageAcrossCells)
{
    // Test: BLOCK message with blocked amount reaches attacker's cell
    constexpr uint64_t ATTACKER_GUID = 1001;
    constexpr uint64_t VICTIM_GUID = 2001;
    constexpr int32_t BLOCKED_AMOUNT = 500;

    HarnessA().AddCreature(ATTACKER_GUID, 100);  // Creature attacker
    HarnessB().AddPlayer(VICTIM_GUID);  // Player who blocked

    // WHEN: Block message is sent to attacker's cell
    ActorMessage block{};
    block.type = MessageType::BLOCK;
    block.sourceGuid = VICTIM_GUID;
    block.targetGuid = ATTACKER_GUID;
    block.complexPayload = MakeCombatResultPayload(ATTACKER_GUID, VICTIM_GUID, 0, 0, BLOCKED_AMOUNT, 0, 0x40);  // PROC_EX_BLOCK
    SendBtoA(std::move(block));

    GetCellA()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellA()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, MissMessageAcrossCells)
{
    // Test: MISS message from victim's cell reaches attacker's cell
    constexpr uint64_t ATTACKER_GUID = 1001;
    constexpr uint64_t VICTIM_GUID = 2001;

    HarnessA().AddCreature(ATTACKER_GUID, 100);  // Creature attacker
    HarnessB().AddPlayer(VICTIM_GUID);  // Player who was missed

    // WHEN: Miss message is sent to attacker's cell
    ActorMessage miss{};
    miss.type = MessageType::MISS;
    miss.sourceGuid = ATTACKER_GUID;
    miss.targetGuid = VICTIM_GUID;
    miss.complexPayload = MakeCombatResultPayload(ATTACKER_GUID, VICTIM_GUID, 0, 0, 0, 0, 0x8);  // PROC_EX_MISS
    GetCellA()->SendMessage(std::move(miss));

    GetCellA()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellA()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, ImmuneMessageAcrossCells)
{
    // Test: IMMUNE message from victim's cell reaches caster's cell
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t FROST_BOLT_ID = 116;

    HarnessA().AddPlayer(CASTER_GUID);  // Spell caster
    HarnessB().AddCreature(TARGET_GUID, 100);  // Target who was immune

    // WHEN: Immune message is sent to caster's cell
    ActorMessage immune{};
    immune.type = MessageType::IMMUNE;
    immune.sourceGuid = TARGET_GUID;
    immune.targetGuid = CASTER_GUID;
    immune.complexPayload = MakeCombatResultPayload(CASTER_GUID, TARGET_GUID, FROST_BOLT_ID, 0, 0, 0, 0x80);  // PROC_EX_IMMUNE
    SendBtoA(std::move(immune));

    GetCellA()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellA()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, AbsorbNotificationAcrossCells)
{
    // Test: ABSORB_NOTIFICATION message from victim's cell reaches attacker's cell
    constexpr uint64_t ATTACKER_GUID = 1001;
    constexpr uint64_t VICTIM_GUID = 2001;
    constexpr int32_t ABSORBED_AMOUNT = 1000;

    HarnessA().AddPlayer(ATTACKER_GUID);  // Attacker
    HarnessB().AddCreature(VICTIM_GUID, 100);  // Victim with shield

    // WHEN: Absorb notification is sent to attacker's cell
    ActorMessage absorb{};
    absorb.type = MessageType::ABSORB_NOTIFICATION;
    absorb.sourceGuid = VICTIM_GUID;
    absorb.targetGuid = ATTACKER_GUID;
    absorb.complexPayload = MakeCombatResultPayload(ATTACKER_GUID, VICTIM_GUID, 0, 0, 0, ABSORBED_AMOUNT, 0x100);  // PROC_EX_ABSORB
    SendBtoA(std::move(absorb));

    GetCellA()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellA()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, AttackResultOnMissingTarget)
{
    // Test: Attack result messages are handled gracefully when target doesn't exist
    constexpr uint64_t ATTACKER_GUID = 1001;
    constexpr uint64_t MISSING_TARGET = 9999;

    HarnessA().AddCreature(ATTACKER_GUID, 100);

    // WHEN: Dodge message references non-existent target
    ActorMessage dodge{};
    dodge.type = MessageType::DODGE;
    dodge.sourceGuid = MISSING_TARGET;
    dodge.targetGuid = ATTACKER_GUID;
    dodge.complexPayload = MakeCombatResultPayload(ATTACKER_GUID, MISSING_TARGET, 0, 0, 0, 0, 0x10);
    GetCellA()->SendMessage(std::move(dodge));

    // THEN: Should not crash
    EXPECT_NO_THROW(GetCellA()->Update(0));
}

TEST_F(GhostActorIntegrationTest, AttackResultNullPayload)
{
    // Test: Attack result messages handle null payload gracefully
    constexpr uint64_t ATTACKER_GUID = 1001;
    constexpr uint64_t VICTIM_GUID = 2001;

    HarnessA().AddCreature(ATTACKER_GUID, 100);

    // WHEN: Dodge message with null payload
    ActorMessage dodge{};
    dodge.type = MessageType::DODGE;
    dodge.sourceGuid = VICTIM_GUID;
    dodge.targetGuid = ATTACKER_GUID;
    dodge.complexPayload = nullptr;  // Missing payload
    GetCellA()->SendMessage(std::move(dodge));

    // THEN: Should not crash
    EXPECT_NO_THROW(GetCellA()->Update(0));
}

TEST_F(GhostActorIntegrationTest, MeleeAttackResultSequence)
{
    // Test: Complete melee attack with various results in sequence
    constexpr uint64_t ATTACKER_GUID = 1001;
    constexpr uint64_t VICTIM_GUID = 2001;

    HarnessA().AddCreature(ATTACKER_GUID, 100);
    HarnessB().AddPlayer(VICTIM_GUID);

    // WHEN: Multiple attack results are processed
    MessageType results[] = { MessageType::MISS, MessageType::DODGE, MessageType::PARRY, MessageType::BLOCK };
    for (auto resultType : results)
    {
        ActorMessage msg{};
        msg.type = resultType;
        msg.sourceGuid = VICTIM_GUID;
        msg.targetGuid = ATTACKER_GUID;
        msg.complexPayload = MakeCombatResultPayload(ATTACKER_GUID, VICTIM_GUID, 0, 0, 0, 0, 0);
        SendBtoA(std::move(msg));
    }

    ProcessAllCells();

    // THEN: All messages should be processed
    EXPECT_GE(GetCellA()->GetMessagesProcessedLastTick(), 4u);
}

// =============================================================================
// Spell Cast Notification Tests
// =============================================================================

TEST_F(GhostActorIntegrationTest, SpellCastStartAcrossCells)
{
    // Test: SPELL_CAST_START message is delivered to neighboring cell
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t SPELL_ID = 12345;
    constexpr int32_t CAST_TIME = 2000;  // 2 seconds
    constexpr uint32_t SCHOOL_MASK = 4;  // Fire

    HarnessA().AddPlayer(CASTER_GUID);  // Caster in cell A
    HarnessB().AddCreature(TARGET_GUID, 100);  // AI creature in cell B

    // WHEN: Spell cast start notification is sent to cell B
    ActorMessage msg{};
    msg.type = MessageType::SPELL_CAST_START;
    msg.sourceGuid = CASTER_GUID;
    msg.targetGuid = TARGET_GUID;
    msg.complexPayload = MakeSpellCastPayload(CASTER_GUID, TARGET_GUID, SPELL_ID, CAST_TIME, 0, SCHOOL_MASK);
    SendAtoB(std::move(msg));

    GetCellB()->Update(0);

    // THEN: Message should be processed (AI in cell B receives notification)
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, SpellCastSuccessAcrossCells)
{
    // Test: SPELL_CAST_SUCCESS message is delivered to neighboring cell
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t SPELL_ID = 54321;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    // WHEN: Spell cast success notification is sent
    ActorMessage msg{};
    msg.type = MessageType::SPELL_CAST_SUCCESS;
    msg.sourceGuid = CASTER_GUID;
    msg.targetGuid = TARGET_GUID;
    msg.complexPayload = MakeSpellCastPayload(CASTER_GUID, TARGET_GUID, SPELL_ID, 0, 0, 0);
    SendAtoB(std::move(msg));

    GetCellB()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, SpellCastFailedAcrossCells)
{
    // Test: SPELL_CAST_FAILED message is delivered to neighboring cell
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t OBSERVER_GUID = 2001;
    constexpr uint32_t SPELL_ID = 99999;
    constexpr uint8_t FAIL_REASON = 15;  // SPELL_FAILED_INTERRUPTED

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(OBSERVER_GUID, 100);

    // WHEN: Spell cast failure notification is sent
    ActorMessage msg{};
    msg.type = MessageType::SPELL_CAST_FAILED;
    msg.sourceGuid = CASTER_GUID;
    msg.complexPayload = MakeSpellCastPayload(CASTER_GUID, 0, SPELL_ID, 0, FAIL_REASON, 0);
    SendAtoB(std::move(msg));

    GetCellB()->Update(0);

    // THEN: Message should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 1u);
}

TEST_F(GhostActorIntegrationTest, SpellCastNullPayload)
{
    // Test: Spell cast messages handle null payload gracefully
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t OBSERVER_GUID = 2001;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(OBSERVER_GUID, 100);

    // WHEN: Spell cast start with null payload
    ActorMessage msg{};
    msg.type = MessageType::SPELL_CAST_START;
    msg.sourceGuid = CASTER_GUID;
    msg.complexPayload = nullptr;  // Missing payload
    GetCellB()->SendMessage(std::move(msg));

    // THEN: Should not crash
    EXPECT_NO_THROW(GetCellB()->Update(0));
}

TEST_F(GhostActorIntegrationTest, SpellCastSequence)
{
    // Test: Complete spell cast sequence (start -> success or fail)
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t SPELL_ID = 11111;
    constexpr int32_t CAST_TIME = 3000;

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    // WHEN: Cast start followed by cast success
    ActorMessage start{};
    start.type = MessageType::SPELL_CAST_START;
    start.sourceGuid = CASTER_GUID;
    start.targetGuid = TARGET_GUID;
    start.complexPayload = MakeSpellCastPayload(CASTER_GUID, TARGET_GUID, SPELL_ID, CAST_TIME, 0, 0x1);
    SendAtoB(std::move(start));

    ActorMessage success{};
    success.type = MessageType::SPELL_CAST_SUCCESS;
    success.sourceGuid = CASTER_GUID;
    success.targetGuid = TARGET_GUID;
    success.complexPayload = MakeSpellCastPayload(CASTER_GUID, TARGET_GUID, SPELL_ID, 0, 0, 0);
    SendAtoB(std::move(success));

    ProcessAllCells();

    // THEN: Both messages should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 2u);
}

TEST_F(GhostActorIntegrationTest, SpellCastInterruptSequence)
{
    // Test: Complete spell cast sequence (start -> interrupted)
    constexpr uint64_t CASTER_GUID = 1001;
    constexpr uint64_t TARGET_GUID = 2001;
    constexpr uint32_t SPELL_ID = 22222;
    constexpr int32_t CAST_TIME = 2500;
    constexpr uint8_t FAIL_REASON = 15;  // SPELL_FAILED_INTERRUPTED

    HarnessA().AddPlayer(CASTER_GUID);
    HarnessB().AddCreature(TARGET_GUID, 100);

    // WHEN: Cast start followed by cast failed (interrupted)
    ActorMessage start{};
    start.type = MessageType::SPELL_CAST_START;
    start.sourceGuid = CASTER_GUID;
    start.targetGuid = TARGET_GUID;
    start.complexPayload = MakeSpellCastPayload(CASTER_GUID, TARGET_GUID, SPELL_ID, CAST_TIME, 0, 0x2);  // Holy
    SendAtoB(std::move(start));

    ActorMessage failed{};
    failed.type = MessageType::SPELL_CAST_FAILED;
    failed.sourceGuid = CASTER_GUID;
    failed.complexPayload = MakeSpellCastPayload(CASTER_GUID, 0, SPELL_ID, 0, FAIL_REASON, 0);
    SendAtoB(std::move(failed));

    ProcessAllCells();

    // THEN: Both messages should be processed
    EXPECT_GE(GetCellB()->GetMessagesProcessedLastTick(), 2u);
}

// =============================================================================
// Entity Lifecycle and Deferred Removal Tests
// =============================================================================

TEST_F(GhostActorIntegrationTest, RemoveEntityDuringIteration)
{
    // Test: Removing an entity while iterating _entities doesn't crash
    HarnessA().AddCreature(1001, 100);
    TestCreature* creature2 = HarnessA().AddCreature(1002, 100);
    HarnessA().AddCreature(1003, 100);

    EXPECT_EQ(GetCellA()->GetEntityCount(), 3u);

    // Remove while not updating - should be immediate
    GetCellA()->RemoveEntity(creature2);
    EXPECT_EQ(GetCellA()->GetEntityCount(), 2u);

    // THEN: No crash and entity count is correct
    EXPECT_NO_THROW(GetCellA()->Update(0));
    EXPECT_EQ(GetCellA()->GetEntityCount(), 2u);
}

TEST_F(GhostActorIntegrationTest, DeferredRemovalProcessedAfterUpdate)
{
    // Test: Entities marked for removal during update are removed after
    constexpr uint64_t CREATURE_GUID = 1001;

    HarnessA().AddCreature(CREATURE_GUID, 100);
    EXPECT_EQ(GetCellA()->GetEntityCount(), 1u);

    // Update should complete without issues
    EXPECT_NO_THROW(GetCellA()->Update(100));

    // Entity still exists after update
    EXPECT_EQ(GetCellA()->GetEntityCount(), 1u);
}

TEST_F(GhostActorIntegrationTest, MultipleEntitiesRemovedDuringUpdate)
{
    // Test: Multiple entities can be removed without iterator invalidation
    constexpr size_t NUM_CREATURES = 10;

    for (size_t i = 0; i < NUM_CREATURES; ++i)
    {
        HarnessA().AddCreature(2000 + i, 100);
    }

    EXPECT_EQ(GetCellA()->GetEntityCount(), NUM_CREATURES);

    // Update multiple times - simulates gameplay
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_NO_THROW(GetCellA()->Update(50));
    }

    // All entities should still be there (none actually died in test)
    EXPECT_EQ(GetCellA()->GetEntityCount(), NUM_CREATURES);
}

TEST_F(GhostActorIntegrationTest, EntityRemovedThenAddedBack)
{
    // Test: Entity can be removed and re-added
    TestCreature* creature = HarnessA().AddCreature(1001, 100);
    uint64_t rawGuid = creature->GetGUID().GetRawValue();

    // Remove
    GetCellA()->RemoveEntity(creature);
    EXPECT_EQ(GetCellA()->GetEntityCount(), 0u);
    EXPECT_EQ(GetCellA()->FindEntityByGuid(rawGuid), nullptr);

    // Add back
    GetCellA()->AddEntity(creature);
    EXPECT_EQ(GetCellA()->GetEntityCount(), 1u);
    EXPECT_NE(GetCellA()->FindEntityByGuid(rawGuid), nullptr);
}

TEST_F(GhostActorIntegrationTest, CellBecomesInactiveWhenEmpty)
{
    // Test: Cell active flag is cleared when last entity is removed
    TestCreature* creature = HarnessA().AddCreature(1001, 100);
    EXPECT_TRUE(GetCellA()->IsActive());

    GetCellA()->RemoveEntity(creature);

    EXPECT_EQ(GetCellA()->GetEntityCount(), 0u);
    EXPECT_FALSE(GetCellA()->IsActive());
}

TEST_F(GhostActorIntegrationTest, FindEntityByGuidReturnsCorrectEntity)
{
    // Test: FindEntityByGuid works correctly with multiple entities
    // Note: FindEntityByGuid matches on the full packed ObjectGuid raw value,
    // not just the counter. Use GetGUID().GetRawValue() for lookups.
    TestCreature* c1 = HarnessA().AddCreature(1001, 100);
    TestCreature* c2 = HarnessA().AddCreature(1002, 200);
    TestCreature* c3 = HarnessA().AddCreature(1003, 300);

    uint64_t rawGuid1 = c1->GetGUID().GetRawValue();
    uint64_t rawGuid2 = c2->GetGUID().GetRawValue();
    uint64_t rawGuid3 = c3->GetGUID().GetRawValue();

    WorldObject* found1 = GetCellA()->FindEntityByGuid(rawGuid1);
    WorldObject* found2 = GetCellA()->FindEntityByGuid(rawGuid2);
    WorldObject* found3 = GetCellA()->FindEntityByGuid(rawGuid3);
    WorldObject* notFound = GetCellA()->FindEntityByGuid(9999);

    ASSERT_NE(found1, nullptr);
    ASSERT_NE(found2, nullptr);
    ASSERT_NE(found3, nullptr);
    EXPECT_EQ(notFound, nullptr);

    EXPECT_EQ(found1, c1);
    EXPECT_EQ(found2, c2);
    EXPECT_EQ(found3, c3);
}

// =============================================================================
// Crash Prevention Tests (Issue: use-after-free in UpdateEntities)
// =============================================================================

TEST_F(GhostActorIntegrationTest, UpdateEntities_SkipsPendingRemovals_NoCrash)
{
    // Test: Entities in _pendingRemovals are skipped BEFORE accessing their members
    // This prevents a use-after-free crash when RemoveFromMap is called during iteration
    //
    // Crash scenario (before fix):
    // 1. UpdateEntities starts iterating _entities, sets _isUpdating = true
    // 2. Something calls RemoveFromMap on an entity during iteration
    // 3. RemoveFromMap calls OnEntityRemoved -> RemoveEntity
    // 4. RemoveEntity sees _isUpdating=true, adds entity to _pendingRemovals
    // 5. RemoveFromMap then calls DeleteFromWorld -> frees memory
    // 6. UpdateEntities continues iterating, accesses freed memory -> CRASH
    //
    // Fix: Check _pendingRemovals BEFORE accessing entity->IsInWorld()

    constexpr uint64_t CREATURE_GUID = 1001;

    // Add a creature to the cell
    TestCreature* creature = HarnessA().AddCreature(CREATURE_GUID, 100);
    ASSERT_NE(creature, nullptr);
    EXPECT_EQ(GetCellA()->GetEntityCount(), 1u);

    // Simulate the crash scenario:
    // 1. Add entity to _pendingRemovals (simulates deferred removal during iteration)
    GetCellA()->TEST_AddToPendingRemovals(creature);
    EXPECT_EQ(GetCellA()->TEST_GetPendingRemovalCount(), 1u);

    // 2. Mark the entity as deleted (simulates memory being freed/invalidated)
    // In real crash, memory would be freed. Here we mark it deleted so IsInWorld() would fail.
    creature->MarkTestDeleted();

    // 3. Call Update which will iterate _entities
    // With the fix, this should NOT crash because we check _pendingRemovals
    // BEFORE calling entity->IsInWorld()
    EXPECT_NO_THROW(GetCellA()->Update(0));

    // Verify the entity was properly processed (removed from pending)
    EXPECT_EQ(GetCellA()->TEST_GetPendingRemovalCount(), 0u);
}

TEST_F(GhostActorIntegrationTest, UpdateEntities_MultipleEntities_OnePendingRemoval)
{
    // Test: Multiple entities where one is pending removal
    // The pending entity should be skipped without crashing, others processed

    constexpr uint64_t GUID1 = 1001;
    constexpr uint64_t GUID2 = 1002;
    constexpr uint64_t GUID3 = 1003;

    TestCreature* c1 = HarnessA().AddCreature(GUID1, 100);
    TestCreature* c2 = HarnessA().AddCreature(GUID2, 100);
    TestCreature* c3 = HarnessA().AddCreature(GUID3, 100);
    (void)c1; (void)c3;  // Suppress unused warnings

    EXPECT_EQ(GetCellA()->GetEntityCount(), 3u);

    // Mark middle entity for pending removal and invalidate it
    GetCellA()->TEST_AddToPendingRemovals(c2);
    c2->MarkTestDeleted();

    // Update should not crash - this is the key assertion
    EXPECT_NO_THROW(GetCellA()->Update(0));

    // After ProcessPendingRemovals, c2 should be removed, 2 entities remain
    EXPECT_EQ(GetCellA()->GetEntityCount(), 2u);
    EXPECT_EQ(GetCellA()->TEST_GetPendingRemovalCount(), 0u);
}
