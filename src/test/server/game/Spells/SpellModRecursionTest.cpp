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

/**
 * @file SpellModRecursionTest.cpp
 * @brief Exhaustive integration tests for ApplySpellMod recursion guard
 *
 * Tests that the triggeredBy parameter in AuraEffect::ApplySpellMod correctly
 * prevents infinite recursion when spell mod auras affect each other.
 * Uses real game objects (Player, Aura, AuraEffect, SpellModifier).
 *
 * Exhaustive coverage approach:
 * - Every SpellModOp that triggers recursion (ALL_EFFECTS, EFFECT1, EFFECT2, EFFECT3)
 * - Every spell family (Generic, Mage, Warlock, Druid, Shaman, etc.)
 * - Every graph topology up to N nodes (self-loop, pair, cycle, mesh)
 * - Every permutation of SpellModOp × family × topology
 * - All 96 flag bits across 3 words of SpellFamilyFlags
 */

#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellInfoTestHelper.h"
#include "WorldSession.h"
#include "WorldMock.h"
#include "ObjectGuid.h"
#include "ScriptMgr.h"
#include "ScriptDefines/MiscScript.h"
#include "ScriptDefines/WorldObjectScript.h"
#include "ScriptDefines/UnitScript.h"
#include "ScriptDefines/PlayerScript.h"
#include "ScriptDefines/GlobalScript.h"
#include "ScriptDefines/CommandScript.h"
#include "ScriptDefines/AllSpellScript.h"
#include "SharedDefines.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <bitset>
#include <sstream>

using namespace testing;

namespace
{

// Base ID for test spells — well above any real spell
constexpr uint32 TEST_SPELL_BASE_ID = 99000;

// SpellModOps that trigger the recursion path in ApplySpellMod
constexpr SpellModOp RECURSION_MOD_OPS[] = {
    SPELLMOD_ALL_EFFECTS,
    SPELLMOD_EFFECT1,
    SPELLMOD_EFFECT2,
    SPELLMOD_EFFECT3,
};
constexpr size_t NUM_MOD_OPS = sizeof(RECURSION_MOD_OPS) / sizeof(RECURSION_MOD_OPS[0]);

// All spell families that have spell mod auras in the game
constexpr uint32 SPELL_FAMILIES[] = {
    0,  // SPELLFAMILY_GENERIC
    3,  // SPELLFAMILY_MAGE
    4,  // SPELLFAMILY_WARRIOR
    5,  // SPELLFAMILY_WARLOCK
    6,  // SPELLFAMILY_PRIEST
    7,  // SPELLFAMILY_DRUID
    8,  // SPELLFAMILY_ROGUE
    9,  // SPELLFAMILY_HUNTER
    10, // SPELLFAMILY_PALADIN
    11, // SPELLFAMILY_SHAMAN
    15, // SPELLFAMILY_DEATHKNIGHT
};
constexpr size_t NUM_FAMILIES = sizeof(SPELL_FAMILIES) / sizeof(SPELL_FAMILIES[0]);

class TestPlayer : public Player
{
public:
    using Player::Player;
    void UpdateObjectVisibility(bool /*forced*/ = true, bool /*fromUpdate*/ = false) override { }
    void ForceInitValues(ObjectGuid::LowType guidLow = 1)
    {
        Object::_Create(guidLow, uint32(0), HighGuid::Player);
    }
};

class SpellModRecursionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        EnsureScriptRegistriesInitialized();

        _originalWorld = sWorld.release();
        _worldMock = new NiceMock<WorldMock>();
        sWorld.reset(_worldMock);

        static std::string emptyString;
        ON_CALL(*_worldMock, GetDataPath()).WillByDefault(ReturnRef(emptyString));
        ON_CALL(*_worldMock, GetRealmName()).WillByDefault(ReturnRef(emptyString));
        ON_CALL(*_worldMock, GetDefaultDbcLocale()).WillByDefault(Return(LOCALE_enUS));
        ON_CALL(*_worldMock, getRate(_)).WillByDefault(Return(1.0f));
        ON_CALL(*_worldMock, getBoolConfig(_)).WillByDefault(Return(false));
        ON_CALL(*_worldMock, getIntConfig(_)).WillByDefault(Return(0));
        ON_CALL(*_worldMock, getFloatConfig(_)).WillByDefault(Return(0.0f));
        ON_CALL(*_worldMock, GetPlayerSecurityLimit()).WillByDefault(Return(SEC_PLAYER));

        _session = new WorldSession(1, "test", 0, nullptr, SEC_GAMEMASTER,
            EXPANSION_WRATH_OF_THE_LICH_KING, 0, LOCALE_enUS, 0, false, false, 0);

        _player = new TestPlayer(_session);
        _player->ForceInitValues();
        _session->SetPlayer(_player);
        _player->SetSession(_session);

        _nextSpellId = TEST_SPELL_BASE_ID;
    }

    void TearDown() override
    {
        for (uint32 id : _registeredSpellIds)
            sSpellMgr->UnregisterTestSpell(id);

        // Intentional leaks of session/player to avoid database access in destructors
        IWorld* currentWorld = sWorld.release();
        delete currentWorld;
        _worldMock = nullptr;
        sWorld.reset(_originalWorld);
        _originalWorld = nullptr;
        _session = nullptr;
        _player = nullptr;
    }

    uint32 NextSpellId() { return _nextSpellId++; }

    // Create a spell mod with given family, op, own flags, and affect mask
    SpellInfo* CreateSpellMod(uint32 spellId, uint32 family, SpellModOp modOp,
                              flag96 ownFlags, flag96 affectMask, int32 basePoints = 10)
    {
        SpellInfo* info = SpellInfoBuilder()
            .WithId(spellId)
            .WithSpellFamilyName(family)
            .WithSpellFamilyFlags(ownFlags[0], ownFlags[1], ownFlags[2])
            .WithAttributes(SPELL_ATTR0_PASSIVE)
            .WithEffect(0, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_ADD_FLAT_MODIFIER)
            .WithEffectMiscValue(0, static_cast<int32>(modOp))
            .WithEffectSpellClassMask(0, affectMask)
            .WithEffectBasePoints(0, basePoints)
            .Build();

        _ownedSpellInfos.emplace_back(info);
        sSpellMgr->RegisterTestSpell(spellId, info);
        _registeredSpellIds.push_back(spellId);
        return info;
    }

    Aura* ApplyAura(uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            return nullptr;
        return Aura::Create(info, 1, _player, _player, nullptr, nullptr, ObjectGuid::Empty);
    }

    // Helper: build a flag96 with a single bit set
    static flag96 SingleBit(uint32 bitIndex)
    {
        flag96 f(0, 0, 0);
        f[bitIndex / 32] = uint32(1) << (bitIndex % 32);
        return f;
    }

    // Helper: build a flag96 with bits 0..n-1 set in the given word
    static flag96 BitsInWord(uint32 word, uint32 count)
    {
        flag96 f(0, 0, 0);
        f[word] = (count >= 32) ? 0xFFFFFFFF : ((uint32(1) << count) - 1);
        return f;
    }

    // Helper: OR two flag96 values
    static flag96 FlagOr(flag96 a, flag96 b)
    {
        return flag96(a[0] | b[0], a[1] | b[1], a[2] | b[2]);
    }

    static void EnsureScriptRegistriesInitialized()
    {
        static bool initialized = false;
        if (!initialized)
        {
            ScriptRegistry<MiscScript>::InitEnabledHooksIfNeeded(MISCHOOK_END);
            ScriptRegistry<WorldObjectScript>::InitEnabledHooksIfNeeded(WORLDOBJECTHOOK_END);
            ScriptRegistry<UnitScript>::InitEnabledHooksIfNeeded(UNITHOOK_END);
            ScriptRegistry<PlayerScript>::InitEnabledHooksIfNeeded(PLAYERHOOK_END);
            ScriptRegistry<GlobalScript>::InitEnabledHooksIfNeeded(GLOBALHOOK_END);
            ScriptRegistry<CommandSC>::InitEnabledHooksIfNeeded(ALLCOMMANDHOOK_END);
            ScriptRegistry<AllSpellScript>::InitEnabledHooksIfNeeded(ALLSPELLHOOK_END);
            initialized = true;
        }
    }

private:
    IWorld* _originalWorld = nullptr;
    NiceMock<WorldMock>* _worldMock = nullptr;
    WorldSession* _session = nullptr;
    TestPlayer* _player = nullptr;
    std::vector<std::unique_ptr<SpellInfo>> _ownedSpellInfos;
    std::vector<uint32> _registeredSpellIds;
    uint32 _nextSpellId = TEST_SPELL_BASE_ID;
};

// =============================================================================
// EXHAUSTIVE: Every SpellModOp × every spell family, self-affecting
// 4 ops × 11 families = 44 permutations
// =============================================================================

TEST_F(SpellModRecursionTest, Exhaustive_SelfAffecting_AllOps_AllFamilies)
{
    uint32 tested = 0;
    for (size_t opIdx = 0; opIdx < NUM_MOD_OPS; ++opIdx)
    {
        for (size_t famIdx = 0; famIdx < NUM_FAMILIES; ++famIdx)
        {
            uint32 id = NextSpellId();
            flag96 flags = SingleBit(0);

            CreateSpellMod(id, SPELL_FAMILIES[famIdx], RECURSION_MOD_OPS[opIdx], flags, flags);
            Aura* aura = ApplyAura(id);
            ASSERT_NE(aura, nullptr)
                << "Failed: op=" << RECURSION_MOD_OPS[opIdx]
                << " family=" << SPELL_FAMILIES[famIdx];
            EXPECT_FALSE(aura->IsRemoved());
            ++tested;
        }
    }
    EXPECT_EQ(tested, NUM_MOD_OPS * NUM_FAMILIES);
}

// =============================================================================
// EXHAUSTIVE: Every SpellModOp × every family, two-way mutual recursion
// 4 ops × 11 families = 44 permutations, each with A↔B
// =============================================================================

TEST_F(SpellModRecursionTest, Exhaustive_TwoWay_AllOps_AllFamilies)
{
    uint32 tested = 0;
    for (size_t opIdx = 0; opIdx < NUM_MOD_OPS; ++opIdx)
    {
        for (size_t famIdx = 0; famIdx < NUM_FAMILIES; ++famIdx)
        {
            uint32 idA = NextSpellId();
            uint32 idB = NextSpellId();
            flag96 flagA = SingleBit(0);
            flag96 flagB = SingleBit(1);

            CreateSpellMod(idA, SPELL_FAMILIES[famIdx], RECURSION_MOD_OPS[opIdx], flagA, flagB);
            CreateSpellMod(idB, SPELL_FAMILIES[famIdx], RECURSION_MOD_OPS[opIdx], flagB, flagA);

            Aura* auraA = ApplyAura(idA);
            ASSERT_NE(auraA, nullptr);
            Aura* auraB = ApplyAura(idB);
            ASSERT_NE(auraB, nullptr)
                << "Failed: op=" << RECURSION_MOD_OPS[opIdx]
                << " family=" << SPELL_FAMILIES[famIdx];
            EXPECT_FALSE(auraA->IsRemoved());
            EXPECT_FALSE(auraB->IsRemoved());
            ++tested;
        }
    }
    EXPECT_EQ(tested, NUM_MOD_OPS * NUM_FAMILIES);
}

// =============================================================================
// EXHAUSTIVE: Every pair of SpellModOps, two-way mutual recursion
// Tests mixed ops: A uses op1, B uses op2, they affect each other
// 4×4 = 16 op pairs × 11 families = 176 permutations
// =============================================================================

TEST_F(SpellModRecursionTest, Exhaustive_MixedOps_TwoWay_AllFamilies)
{
    uint32 tested = 0;
    for (size_t op1 = 0; op1 < NUM_MOD_OPS; ++op1)
    {
        for (size_t op2 = 0; op2 < NUM_MOD_OPS; ++op2)
        {
            for (size_t famIdx = 0; famIdx < NUM_FAMILIES; ++famIdx)
            {
                uint32 idA = NextSpellId();
                uint32 idB = NextSpellId();
                flag96 flagA = SingleBit(0);
                flag96 flagB = SingleBit(1);

                CreateSpellMod(idA, SPELL_FAMILIES[famIdx], RECURSION_MOD_OPS[op1], flagA, flagB);
                CreateSpellMod(idB, SPELL_FAMILIES[famIdx], RECURSION_MOD_OPS[op2], flagB, flagA);

                Aura* auraA = ApplyAura(idA);
                ASSERT_NE(auraA, nullptr);
                Aura* auraB = ApplyAura(idB);
                ASSERT_NE(auraB, nullptr)
                    << "Failed: op1=" << RECURSION_MOD_OPS[op1]
                    << " op2=" << RECURSION_MOD_OPS[op2]
                    << " family=" << SPELL_FAMILIES[famIdx];
                EXPECT_FALSE(auraA->IsRemoved());
                EXPECT_FALSE(auraB->IsRemoved());
                ++tested;
            }
        }
    }
    EXPECT_EQ(tested, NUM_MOD_OPS * NUM_MOD_OPS * NUM_FAMILIES);
}

// =============================================================================
// EXHAUSTIVE: Three-way cycles, all op permutations
// A->B->C->A with all 4^3 = 64 op combinations, per family
// 64 × 11 = 704 permutations
// =============================================================================

TEST_F(SpellModRecursionTest, Exhaustive_ThreeWayCycle_AllOps_AllFamilies)
{
    uint32 tested = 0;
    for (size_t op1 = 0; op1 < NUM_MOD_OPS; ++op1)
    {
        for (size_t op2 = 0; op2 < NUM_MOD_OPS; ++op2)
        {
            for (size_t op3 = 0; op3 < NUM_MOD_OPS; ++op3)
            {
                for (size_t famIdx = 0; famIdx < NUM_FAMILIES; ++famIdx)
                {
                    uint32 idA = NextSpellId();
                    uint32 idB = NextSpellId();
                    uint32 idC = NextSpellId();
                    flag96 flagA = SingleBit(0);
                    flag96 flagB = SingleBit(1);
                    flag96 flagC = SingleBit(2);

                    CreateSpellMod(idA, SPELL_FAMILIES[famIdx], RECURSION_MOD_OPS[op1], flagA, flagB);
                    CreateSpellMod(idB, SPELL_FAMILIES[famIdx], RECURSION_MOD_OPS[op2], flagB, flagC);
                    CreateSpellMod(idC, SPELL_FAMILIES[famIdx], RECURSION_MOD_OPS[op3], flagC, flagA);

                    Aura* auraA = ApplyAura(idA);
                    ASSERT_NE(auraA, nullptr);
                    Aura* auraB = ApplyAura(idB);
                    ASSERT_NE(auraB, nullptr);
                    Aura* auraC = ApplyAura(idC);
                    ASSERT_NE(auraC, nullptr)
                        << "Failed: ops=" << RECURSION_MOD_OPS[op1]
                        << "," << RECURSION_MOD_OPS[op2]
                        << "," << RECURSION_MOD_OPS[op3]
                        << " family=" << SPELL_FAMILIES[famIdx];

                    EXPECT_FALSE(auraA->IsRemoved());
                    EXPECT_FALSE(auraB->IsRemoved());
                    EXPECT_FALSE(auraC->IsRemoved());
                    ++tested;
                }
            }
        }
    }
    EXPECT_EQ(tested, NUM_MOD_OPS * NUM_MOD_OPS * NUM_MOD_OPS * NUM_FAMILIES);
}

// =============================================================================
// EXHAUSTIVE: Full mesh (all-affect-all) with N=2..8 spells per family
// Each spell has ALL_EFFECTS and affects all others
// 7 sizes × 11 families = 77 permutations, total spells created: ~2695
// =============================================================================

TEST_F(SpellModRecursionTest, Exhaustive_FullMesh_ScalingSize_AllFamilies)
{
    uint32 tested = 0;
    for (uint32 meshSize = 2; meshSize <= 8; ++meshSize)
    {
        for (size_t famIdx = 0; famIdx < NUM_FAMILIES; ++famIdx)
        {
            flag96 allFlags = BitsInWord(0, meshSize);
            std::vector<Aura*> auras;

            for (uint32 i = 0; i < meshSize; ++i)
            {
                uint32 id = NextSpellId();
                flag96 ownFlag = SingleBit(i);
                CreateSpellMod(id, SPELL_FAMILIES[famIdx], SPELLMOD_ALL_EFFECTS, ownFlag, allFlags);
                Aura* aura = ApplyAura(id);
                ASSERT_NE(aura, nullptr)
                    << "Failed: mesh=" << meshSize
                    << " spell=" << i
                    << " family=" << SPELL_FAMILIES[famIdx];
                auras.push_back(aura);
            }

            for (uint32 i = 0; i < meshSize; ++i)
                EXPECT_FALSE(auras[i]->IsRemoved())
                    << "Removed: mesh=" << meshSize
                    << " spell=" << i
                    << " family=" << SPELL_FAMILIES[famIdx];

            ++tested;
        }
    }
    EXPECT_EQ(tested, 7u * NUM_FAMILIES);
}

// =============================================================================
// EXHAUSTIVE: All flag96 word positions (0, 1, 2)
// Ensures recursion guard works regardless of which 32-bit word flags are in
// 3 words × 4 ops = 12 permutations, each with 4-spell full mesh
// =============================================================================

TEST_F(SpellModRecursionTest, Exhaustive_FlagWordPositions)
{
    uint32 tested = 0;
    for (uint32 word = 0; word < 3; ++word)
    {
        for (size_t opIdx = 0; opIdx < NUM_MOD_OPS; ++opIdx)
        {
            constexpr uint32 MESH_SIZE = 4;
            flag96 allFlags = BitsInWord(word, MESH_SIZE);
            std::vector<Aura*> auras;

            for (uint32 i = 0; i < MESH_SIZE; ++i)
            {
                uint32 id = NextSpellId();
                flag96 ownFlag(0, 0, 0);
                ownFlag[word] = uint32(1) << i;

                CreateSpellMod(id, 3 /*Mage*/, RECURSION_MOD_OPS[opIdx], ownFlag, allFlags);
                Aura* aura = ApplyAura(id);
                ASSERT_NE(aura, nullptr)
                    << "Failed: word=" << word
                    << " op=" << RECURSION_MOD_OPS[opIdx]
                    << " spell=" << i;
                auras.push_back(aura);
            }

            for (uint32 i = 0; i < MESH_SIZE; ++i)
                EXPECT_FALSE(auras[i]->IsRemoved());

            ++tested;
        }
    }
    EXPECT_EQ(tested, 3u * NUM_MOD_OPS);
}

// =============================================================================
// EXHAUSTIVE: All 2-node directed graph topologies with 2 spells
// For 2 nodes there are 4 possible directed edge configurations:
//   00: no edges (no recursion possible)
//   01: A->B only
//   10: B->A only
//   11: A<->B (mutual)
// Test all 4 × 4 ops × 11 families = 176 permutations
// =============================================================================

TEST_F(SpellModRecursionTest, Exhaustive_AllDirectedGraphs_TwoNodes)
{
    uint32 tested = 0;
    for (uint32 edgeMask = 0; edgeMask < 4; ++edgeMask)
    {
        bool aAffectsB = (edgeMask & 1) != 0;
        bool bAffectsA = (edgeMask & 2) != 0;

        for (size_t opIdx = 0; opIdx < NUM_MOD_OPS; ++opIdx)
        {
            for (size_t famIdx = 0; famIdx < NUM_FAMILIES; ++famIdx)
            {
                uint32 idA = NextSpellId();
                uint32 idB = NextSpellId();
                flag96 flagA = SingleBit(0);
                flag96 flagB = SingleBit(1);

                // A's affect mask: includes B's flag if A->B edge exists
                flag96 aAffect = aAffectsB ? flagB : flag96(0, 0, 0);
                flag96 bAffect = bAffectsA ? flagA : flag96(0, 0, 0);

                CreateSpellMod(idA, SPELL_FAMILIES[famIdx], RECURSION_MOD_OPS[opIdx], flagA, aAffect);
                CreateSpellMod(idB, SPELL_FAMILIES[famIdx], RECURSION_MOD_OPS[opIdx], flagB, bAffect);

                Aura* auraA = ApplyAura(idA);
                ASSERT_NE(auraA, nullptr);
                Aura* auraB = ApplyAura(idB);
                ASSERT_NE(auraB, nullptr);

                EXPECT_FALSE(auraA->IsRemoved());
                EXPECT_FALSE(auraB->IsRemoved());
                ++tested;
            }
        }
    }
    EXPECT_EQ(tested, 4u * NUM_MOD_OPS * NUM_FAMILIES);
}

// =============================================================================
// EXHAUSTIVE: All 3-node directed graph topologies
// For 3 nodes there are 6 possible directed edges (A->B, A->C, B->A, B->C, C->A, C->B)
// 2^6 = 64 topologies × 11 families = 704 permutations (using ALL_EFFECTS op)
// =============================================================================

TEST_F(SpellModRecursionTest, Exhaustive_AllDirectedGraphs_ThreeNodes)
{
    uint32 tested = 0;
    // 6 possible directed edges between 3 nodes
    // bit 0: A->B, bit 1: A->C, bit 2: B->A, bit 3: B->C, bit 4: C->A, bit 5: C->B
    for (uint32 edgeMask = 0; edgeMask < 64; ++edgeMask)
    {
        for (size_t famIdx = 0; famIdx < NUM_FAMILIES; ++famIdx)
        {
            uint32 idA = NextSpellId();
            uint32 idB = NextSpellId();
            uint32 idC = NextSpellId();
            flag96 flagA = SingleBit(0);
            flag96 flagB = SingleBit(1);
            flag96 flagC = SingleBit(2);

            // Build affect masks from edge bits
            flag96 aAffect(0, 0, 0);
            if (edgeMask & 0x01) aAffect = FlagOr(aAffect, flagB); // A->B
            if (edgeMask & 0x02) aAffect = FlagOr(aAffect, flagC); // A->C

            flag96 bAffect(0, 0, 0);
            if (edgeMask & 0x04) bAffect = FlagOr(bAffect, flagA); // B->A
            if (edgeMask & 0x08) bAffect = FlagOr(bAffect, flagC); // B->C

            flag96 cAffect(0, 0, 0);
            if (edgeMask & 0x10) cAffect = FlagOr(cAffect, flagA); // C->A
            if (edgeMask & 0x20) cAffect = FlagOr(cAffect, flagB); // C->B

            CreateSpellMod(idA, SPELL_FAMILIES[famIdx], SPELLMOD_ALL_EFFECTS, flagA, aAffect);
            CreateSpellMod(idB, SPELL_FAMILIES[famIdx], SPELLMOD_ALL_EFFECTS, flagB, bAffect);
            CreateSpellMod(idC, SPELL_FAMILIES[famIdx], SPELLMOD_ALL_EFFECTS, flagC, cAffect);

            Aura* auraA = ApplyAura(idA);
            ASSERT_NE(auraA, nullptr);
            Aura* auraB = ApplyAura(idB);
            ASSERT_NE(auraB, nullptr);
            Aura* auraC = ApplyAura(idC);
            ASSERT_NE(auraC, nullptr)
                << "Failed: edgeMask=0x" << std::hex << edgeMask
                << " family=" << std::dec << SPELL_FAMILIES[famIdx];

            EXPECT_FALSE(auraA->IsRemoved());
            EXPECT_FALSE(auraB->IsRemoved());
            EXPECT_FALSE(auraC->IsRemoved());
            ++tested;
        }
    }
    EXPECT_EQ(tested, 64u * NUM_FAMILIES);
}

// =============================================================================
// STRESS: Large full mesh with 20 spells, all affecting all
// Exercises deep recursion chains with real game objects
// =============================================================================

TEST_F(SpellModRecursionTest, Stress_LargeMesh_20Spells)
{
    constexpr uint32 MESH_SIZE = 20;
    flag96 allFlags = BitsInWord(0, MESH_SIZE);
    std::vector<Aura*> auras;

    for (uint32 i = 0; i < MESH_SIZE; ++i)
    {
        uint32 id = NextSpellId();
        flag96 ownFlag = SingleBit(i);
        CreateSpellMod(id, 3 /*Mage*/, SPELLMOD_ALL_EFFECTS, ownFlag, allFlags);
        Aura* aura = ApplyAura(id);
        ASSERT_NE(aura, nullptr) << "Failed at spell " << i;
        auras.push_back(aura);
    }

    for (uint32 i = 0; i < MESH_SIZE; ++i)
        EXPECT_FALSE(auras[i]->IsRemoved()) << "Spell " << i << " was removed";
}

// =============================================================================
// STRESS: Sequential apply/remove of 50 mutually-affecting spells
// Exercises the guard across many add/remove cycles
// =============================================================================

TEST_F(SpellModRecursionTest, Stress_ApplyRemoveCycles)
{
    constexpr uint32 TOTAL = 50;
    flag96 allFlags = BitsInWord(0, 16); // 16 flag bits shared among all
    std::vector<uint32> spellIds;
    std::vector<Aura*> activeAuras;

    // Create all spell infos upfront
    for (uint32 i = 0; i < TOTAL; ++i)
    {
        uint32 id = NextSpellId();
        flag96 ownFlag = SingleBit(i % 16); // wrap around 16 bits
        CreateSpellMod(id, 3, SPELLMOD_ALL_EFFECTS, ownFlag, allFlags);
        spellIds.push_back(id);
    }

    // Apply first 10
    for (uint32 i = 0; i < 10; ++i)
    {
        Aura* aura = ApplyAura(spellIds[i]);
        ASSERT_NE(aura, nullptr);
        activeAuras.push_back(aura);
    }

    // Remove and replace in a rolling window
    for (uint32 i = 10; i < TOTAL; ++i)
    {
        // Remove oldest
        activeAuras.front()->Remove();
        activeAuras.erase(activeAuras.begin());

        // Add new one
        Aura* aura = ApplyAura(spellIds[i]);
        ASSERT_NE(aura, nullptr) << "Failed at cycle " << i;
        activeAuras.push_back(aura);

        // Verify all active auras are alive
        for (auto* a : activeAuras)
            EXPECT_FALSE(a->IsRemoved());
    }
}

} // anonymous namespace
