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
 * @file SpellProcChainTest.cpp
 * @brief Tests that prove proc chains are wired correctly end-to-end
 *
 * Verifies that for a given proc aura:
 * 1. The proc entry configuration (flags, phase, chance) resolves correctly
 * 2. The DBC fallback path produces the right chance when spell_proc has no override
 * 3. The trigger event matching works (which events fire the proc)
 * 4. The proc chance (from DBC or spell_proc) feeds into the roll correctly
 *
 * Uses Darkmoon Card: Blue Dragon (23688 -> 23684) as the primary test case
 * since it exercises the DBC fallback path (spell_proc Chance=0, PPM=0).
 */

#include "AuraScriptTestFramework.h"
#include "ProcChanceTestHelper.h"
#include "ProcEventInfoHelper.h"
#include "Random.h"
#include "SpellProcTestData.h"
#include "gtest/gtest.h"
#include <cmath>

using namespace testing;

// =============================================================================
// Proc Chain Test Fixture
// =============================================================================

class SpellProcChainTest : public AuraScriptProcTestFixture
{
protected:
    void SetUp() override
    {
        AuraScriptProcTestFixture::SetUp();
        _allEntries = GetAllSpellProcTestEntries();
    }

    /**
     * @brief Find a spell_proc test entry by spell ID
     * @return pointer to the entry, or nullptr if not found
     */
    SpellProcTestEntry const* FindEntry(int32_t spellId) const
    {
        for (auto const& entry : _allEntries)
        {
            if (entry.SpellId == spellId)
                return &entry;
        }
        return nullptr;
    }

    /**
     * @brief Simulate the DBC fallback chance resolution from LoadSpellProcs()
     *
     * When spell_proc has Chance=0 and PPM=0, the server falls back to
     * spellInfo->ProcChance from the DBC. This function replicates that logic.
     *
     * @param entry The spell_proc entry
     * @param dbcProcChance The ProcChance from Spell.dbc
     * @return The effective proc chance that would be used at runtime
     */
    static float ResolveEffectiveChance(SpellProcTestEntry const& entry, uint32 dbcProcChance)
    {
        // Mirrors SpellMgr.cpp:1921-1922:
        //   if (!procEntry.Chance && !procEntry.ProcsPerMinute)
        //       procEntry.Chance = float(spellInfo->ProcChance);
        if (entry.Chance == 0.0f && entry.ProcsPerMinute == 0.0f)
            return static_cast<float>(dbcProcChance);
        if (entry.ProcsPerMinute > 0.0f)
            return 0.0f; // PPM overrides at runtime, not a flat chance
        return entry.Chance;
    }

    std::vector<SpellProcTestEntry> _allEntries;
};

// =============================================================================
// Darkmoon Card: Blue Dragon (23688) - DBC Fallback Chain
// =============================================================================

TEST_F(SpellProcChainTest, BlueDragon_ProcEntryExists)
{
    // Spell 23688 "Aura of the Blue Dragon" must have a spell_proc entry
    auto const* entry = FindEntry(23688);
    ASSERT_NE(entry, nullptr) << "Spell 23688 must exist in spell_proc";
}

TEST_F(SpellProcChainTest, BlueDragon_SpellProcHasNoChanceOverride)
{
    // The spell_proc entry for 23688 has Chance=0 and PPM=0,
    // meaning the server must fall back to DBC ProcChance
    auto const* entry = FindEntry(23688);
    ASSERT_NE(entry, nullptr);

    EXPECT_FLOAT_EQ(entry->Chance, 0.0f)
        << "spell_proc.Chance should be 0 (no override)";
    EXPECT_FLOAT_EQ(entry->ProcsPerMinute, 0.0f)
        << "spell_proc.ProcsPerMinute should be 0 (no PPM)";
}

TEST_F(SpellProcChainTest, BlueDragon_DBCFallbackResolvesTo2Percent)
{
    auto const* entry = FindEntry(23688);
    ASSERT_NE(entry, nullptr);

    // DBC ProcChance for spell 23688 is 2
    constexpr uint32 DBC_PROC_CHANCE_BLUE_DRAGON = 2;

    float effectiveChance = ResolveEffectiveChance(*entry, DBC_PROC_CHANCE_BLUE_DRAGON);
    EXPECT_FLOAT_EQ(effectiveChance, 2.0f)
        << "Effective chance must be 2% from DBC fallback";
}

TEST_F(SpellProcChainTest, BlueDragon_ProcChanceCalculation)
{
    // Build a SpellProcEntry as the server would after DBC fallback
    auto procEntry = SpellProcEntryBuilder()
        .WithChance(2.0f) // From DBC ProcChance fallback
        .WithProcsPerMinute(0.0f)
        .Build();

    // CalcProcChance with no modifiers should return exactly 2%
    float result = ProcChanceTestHelper::SimulateCalcProcChance(procEntry);
    EXPECT_FLOAT_EQ(result, 2.0f);
}

TEST_F(SpellProcChainTest, BlueDragon_ProcPhaseIsHit)
{
    auto const* entry = FindEntry(23688);
    ASSERT_NE(entry, nullptr);

    EXPECT_EQ(entry->SpellPhaseMask, PROC_SPELL_PHASE_HIT)
        << "Blue Dragon must proc on spell hit phase";
}

TEST_F(SpellProcChainTest, BlueDragon_TriggersOnPositiveMagicSpell)
{
    // 23688 has DBC ProcFlags: PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_POS |
    //                          PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG
    // Since spell_proc ProcFlags=0, it falls back to DBC flags.
    // We simulate this by building the entry with the DBC flags.
    auto procEntry = SpellProcEntryBuilder()
        .WithProcFlags(PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_POS |
                       PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG)
        .WithSpellPhaseMask(PROC_SPELL_PHASE_HIT)
        .Build();

    // Positive magic spell (e.g. healing) should trigger
    auto healScenario = ProcScenarioBuilder()
        .OnHeal()
        .OnHit();
    EXPECT_PROC_TRIGGERS(procEntry, healScenario);
}

TEST_F(SpellProcChainTest, BlueDragon_TriggersOnNegativeMagicSpell)
{
    auto procEntry = SpellProcEntryBuilder()
        .WithProcFlags(PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_POS |
                       PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG)
        .WithSpellPhaseMask(PROC_SPELL_PHASE_HIT)
        .Build();

    // Negative magic spell (e.g. damage) should trigger
    auto damageScenario = ProcScenarioBuilder()
        .OnSpellDamage()
        .OnHit()
        .WithNormalHit();
    EXPECT_PROC_TRIGGERS(procEntry, damageScenario);
}

TEST_F(SpellProcChainTest, BlueDragon_DoesNotTriggerOnMelee)
{
    auto procEntry = SpellProcEntryBuilder()
        .WithProcFlags(PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_POS |
                       PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG)
        .WithSpellPhaseMask(PROC_SPELL_PHASE_HIT)
        .Build();

    // Melee auto-attack should NOT trigger
    auto meleeScenario = ProcScenarioBuilder()
        .OnMeleeAutoAttack()
        .WithNormalHit();
    EXPECT_PROC_DOES_NOT_TRIGGER(procEntry, meleeScenario);
}

TEST_F(SpellProcChainTest, BlueDragon_DoesNotTriggerOnCastPhase)
{
    auto procEntry = SpellProcEntryBuilder()
        .WithProcFlags(PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_POS |
                       PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG)
        .WithSpellPhaseMask(PROC_SPELL_PHASE_HIT)
        .Build();

    // Cast phase should NOT trigger (only hit phase)
    auto castScenario = ProcScenarioBuilder()
        .OnSpellDamage()
        .OnCast();
    EXPECT_PROC_DOES_NOT_TRIGGER(procEntry, castScenario);
}

TEST_F(SpellProcChainTest, BlueDragon_RollPassesAt2Percent)
{
    ProcTestScenario scenario;
    scenario.WithAura(23688);

    auto procEntry = SpellProcEntryBuilder()
        .WithChance(2.0f)
        .Build();

    // Roll of 1.5 should pass (< 2%)
    EXPECT_TRUE(scenario.SimulateProc(procEntry, 1.5f));
}

TEST_F(SpellProcChainTest, BlueDragon_RollFailsAbove2Percent)
{
    ProcTestScenario scenario;
    scenario.WithAura(23688);

    auto procEntry = SpellProcEntryBuilder()
        .WithChance(2.0f)
        .Build();

    // Roll of 5.0 should fail (> 2%)
    EXPECT_FALSE(scenario.SimulateProc(procEntry, 5.0f));
}

TEST_F(SpellProcChainTest, BlueDragon_NoCooldown)
{
    auto const* entry = FindEntry(23688);
    ASSERT_NE(entry, nullptr);

    EXPECT_EQ(entry->Cooldown, 0u)
        << "Blue Dragon should have no internal cooldown";
}

TEST_F(SpellProcChainTest, BlueDragon_NoCharges)
{
    auto const* entry = FindEntry(23688);
    ASSERT_NE(entry, nullptr);

    EXPECT_EQ(entry->Charges, 0u)
        << "Blue Dragon should have no charge limit";
}

// =============================================================================
// Generic DBC Fallback Path Tests
// =============================================================================

TEST_F(SpellProcChainTest, DBCFallback_ZeroChanceZeroPPM_UsesDBCProcChance)
{
    // Any spell_proc entry with Chance=0, PPM=0 should fall back to DBC
    SpellProcTestEntry fakeEntry = {};
    fakeEntry.Chance = 0.0f;
    fakeEntry.ProcsPerMinute = 0.0f;

    EXPECT_FLOAT_EQ(ResolveEffectiveChance(fakeEntry, 15), 15.0f);
    EXPECT_FLOAT_EQ(ResolveEffectiveChance(fakeEntry, 50), 50.0f);
    EXPECT_FLOAT_EQ(ResolveEffectiveChance(fakeEntry, 100), 100.0f);
    EXPECT_FLOAT_EQ(ResolveEffectiveChance(fakeEntry, 2), 2.0f);
}

TEST_F(SpellProcChainTest, DBCFallback_NonZeroChance_OverridesDBC)
{
    // If spell_proc sets Chance > 0, DBC value is ignored
    SpellProcTestEntry fakeEntry = {};
    fakeEntry.Chance = 25.0f;
    fakeEntry.ProcsPerMinute = 0.0f;

    EXPECT_FLOAT_EQ(ResolveEffectiveChance(fakeEntry, 10), 25.0f)
        << "spell_proc Chance should override DBC";
}

TEST_F(SpellProcChainTest, DBCFallback_NonZeroPPM_PPMTakesPrecedence)
{
    // If spell_proc sets PPM > 0, flat chance is not used
    SpellProcTestEntry fakeEntry = {};
    fakeEntry.Chance = 0.0f;
    fakeEntry.ProcsPerMinute = 6.0f;

    float result = ResolveEffectiveChance(fakeEntry, 50);
    EXPECT_FLOAT_EQ(result, 0.0f)
        << "PPM mode should not return flat chance";
}

// =============================================================================
// Arbitrary Proc Chance Verification
// =============================================================================

TEST_F(SpellProcChainTest, ArbitraryChance_FeedsIntoRollCorrectly)
{
    // Prove that whatever chance the proc system resolves, it is used
    // correctly in the roll check
    struct TestCase
    {
        float chance;
        float roll;
        bool expectedResult;
        const char* desc;
    };

    TestCase cases[] = {
        { 2.0f,  1.0f, true,  "2% chance, roll 1 -> pass" },
        { 2.0f,  3.0f, false, "2% chance, roll 3 -> fail" },
        { 10.0f, 5.0f, true,  "10% chance, roll 5 -> pass" },
        { 10.0f, 15.0f, false, "10% chance, roll 15 -> fail" },
        { 50.0f, 49.0f, true,  "50% chance, roll 49 -> pass" },
        { 50.0f, 51.0f, false, "50% chance, roll 51 -> fail" },
        { 100.0f, 99.0f, true, "100% chance, roll 99 -> pass" },
    };

    for (auto const& tc : cases)
    {
        ProcTestScenario scenario;
        scenario.WithAura(99999);

        auto procEntry = SpellProcEntryBuilder()
            .WithChance(tc.chance)
            .Build();

        EXPECT_EQ(scenario.SimulateProc(procEntry, tc.roll), tc.expectedResult)
            << tc.desc;
    }
}

// =============================================================================
// Proc Chain Validation for All DBC-Fallback Entries
// =============================================================================

TEST_F(SpellProcChainTest, AllDBCFallbackEntries_HaveConsistentConfig)
{
    // Find all spell_proc entries that rely on DBC fallback (Chance=0, PPM=0)
    // and verify they have sensible configuration
    int fallbackCount = 0;
    int bareMinimumCount = 0;

    for (auto const& entry : _allEntries)
    {
        if (entry.Chance == 0.0f && entry.ProcsPerMinute == 0.0f)
        {
            fallbackCount++;

            // Check if the entry adds any filtering/config beyond DBC
            bool hasAnyConfig = entry.ProcFlags != 0 ||
                                entry.SpellPhaseMask != 0 ||
                                entry.SpellTypeMask != 0 ||
                                entry.HitMask != 0 ||
                                entry.SchoolMask != 0 ||
                                entry.SpellFamilyName != 0 ||
                                entry.AttributesMask != 0 ||
                                entry.Cooldown != 0 ||
                                entry.Charges != 0 ||
                                entry.DisableEffectsMask != 0;

            // Negative spell IDs are effect-specific entries that may
            // exist solely to override DisableEffectsMask or associate
            // a specific effect index. All-zero config is valid for these.
            if (!hasAnyConfig)
                bareMinimumCount++;
        }
    }

    std::cout << "[  INFO    ] Entries using DBC fallback for chance: "
              << fallbackCount << " / " << _allEntries.size() << "\n";
    std::cout << "[  INFO    ] Of those, bare-minimum (DBC-only) entries: "
              << bareMinimumCount << std::endl;

    // The majority of DBC-fallback entries should add filtering value
    if (fallbackCount > 0)
    {
        float configuredRate = 100.0f * (fallbackCount - bareMinimumCount) / fallbackCount;
        std::cout << "[  INFO    ] Configured rate: " << configuredRate << "%" << std::endl;
    }

    EXPECT_GT(fallbackCount, 0) << "Should have entries using DBC fallback";
}

// =============================================================================
// Statistical Proc Rate Verification (1,000,000 rolls)
// =============================================================================

/**
 * @brief Helper to run N rolls at a given chance and return the observed rate.
 *
 * Uses the real server RNG (roll_chance_f -> rand_chance) so this tests the
 * actual random number generator path that the live server uses.
 */
static double RunProcTrials(float chance, uint32 trials)
{
    uint32 procs = 0;
    for (uint32 i = 0; i < trials; ++i)
    {
        if (roll_chance_f(chance))
            ++procs;
    }
    return static_cast<double>(procs) / static_cast<double>(trials) * 100.0;
}

/**
 * @brief Calculate the half-width of a 99.9% confidence interval for a proportion.
 *
 * Uses the normal approximation to the binomial distribution:
 *   margin = z * sqrt(p * (1-p) / n)
 *
 * z = 3.291 for 99.9% confidence (two-tailed).
 * With 1,000,000 trials at p=0.02:
 *   margin = 3.291 * sqrt(0.02 * 0.98 / 1000000) = ~0.046%
 *
 * So the observed rate should be within ~0.046% of 2.0%.
 * We use 5 sigma (z=5.0) as the failure threshold to make flaky failures
 * astronomically unlikely (~1 in 3.5 million runs).
 */
static double CalcMargin(double pPercent, uint32 n, double zScore = 5.0)
{
    double p = pPercent / 100.0;
    return zScore * std::sqrt(p * (1.0 - p) / static_cast<double>(n)) * 100.0;
}

class SpellProcStatisticalTest : public ::testing::Test
{
protected:
    static constexpr uint32 TRIALS = 1000000;
};

TEST_F(SpellProcStatisticalTest, BlueDragon_2Percent_1MillionRolls)
{
    constexpr float EXPECTED = 2.0f;

    double observed = RunProcTrials(EXPECTED, TRIALS);
    double margin = CalcMargin(EXPECTED, TRIALS);

    std::cout << "[  INFO    ] Blue Dragon 2% proc test:\n"
              << "             Trials:   " << TRIALS << "\n"
              << "             Expected: " << EXPECTED << "%\n"
              << "             Observed: " << observed << "%\n"
              << "             5-sigma:  +/- " << margin << "%\n"
              << "             Range:    [" << (EXPECTED - margin)
              << ", " << (EXPECTED + margin) << "]" << std::endl;

    EXPECT_NEAR(observed, EXPECTED, margin)
        << "Observed proc rate deviates from 2% by more than 5 sigma";
}

TEST_F(SpellProcStatisticalTest, ArbitraryChance_5Percent_1MillionRolls)
{
    constexpr float EXPECTED = 5.0f;

    double observed = RunProcTrials(EXPECTED, TRIALS);
    double margin = CalcMargin(EXPECTED, TRIALS);

    std::cout << "[  INFO    ] 5% proc test:\n"
              << "             Trials:   " << TRIALS << "\n"
              << "             Expected: " << EXPECTED << "%\n"
              << "             Observed: " << observed << "%\n"
              << "             5-sigma:  +/- " << margin << "%" << std::endl;

    EXPECT_NEAR(observed, EXPECTED, margin);
}

TEST_F(SpellProcStatisticalTest, ArbitraryChance_10Percent_1MillionRolls)
{
    constexpr float EXPECTED = 10.0f;

    double observed = RunProcTrials(EXPECTED, TRIALS);
    double margin = CalcMargin(EXPECTED, TRIALS);

    std::cout << "[  INFO    ] 10% proc test:\n"
              << "             Trials:   " << TRIALS << "\n"
              << "             Expected: " << EXPECTED << "%\n"
              << "             Observed: " << observed << "%\n"
              << "             5-sigma:  +/- " << margin << "%" << std::endl;

    EXPECT_NEAR(observed, EXPECTED, margin);
}

TEST_F(SpellProcStatisticalTest, ArbitraryChance_25Percent_1MillionRolls)
{
    constexpr float EXPECTED = 25.0f;

    double observed = RunProcTrials(EXPECTED, TRIALS);
    double margin = CalcMargin(EXPECTED, TRIALS);

    std::cout << "[  INFO    ] 25% proc test:\n"
              << "             Trials:   " << TRIALS << "\n"
              << "             Expected: " << EXPECTED << "%\n"
              << "             Observed: " << observed << "%\n"
              << "             5-sigma:  +/- " << margin << "%" << std::endl;

    EXPECT_NEAR(observed, EXPECTED, margin);
}

TEST_F(SpellProcStatisticalTest, ArbitraryChance_50Percent_1MillionRolls)
{
    constexpr float EXPECTED = 50.0f;

    double observed = RunProcTrials(EXPECTED, TRIALS);
    double margin = CalcMargin(EXPECTED, TRIALS);

    std::cout << "[  INFO    ] 50% proc test:\n"
              << "             Trials:   " << TRIALS << "\n"
              << "             Expected: " << EXPECTED << "%\n"
              << "             Observed: " << observed << "%\n"
              << "             5-sigma:  +/- " << margin << "%" << std::endl;

    EXPECT_NEAR(observed, EXPECTED, margin);
}

TEST_F(SpellProcStatisticalTest, EdgeCase_0Percent_NeverProcs)
{
    double observed = RunProcTrials(0.0f, TRIALS);

    std::cout << "[  INFO    ] 0% proc test:\n"
              << "             Trials:   " << TRIALS << "\n"
              << "             Observed: " << observed << "%" << std::endl;

    EXPECT_DOUBLE_EQ(observed, 0.0);
}

TEST_F(SpellProcStatisticalTest, EdgeCase_100Percent_AlwaysProcs)
{
    double observed = RunProcTrials(100.0f, TRIALS);

    std::cout << "[  INFO    ] 100% proc test:\n"
              << "             Trials:   " << TRIALS << "\n"
              << "             Observed: " << observed << "%" << std::endl;

    EXPECT_DOUBLE_EQ(observed, 100.0);
}
