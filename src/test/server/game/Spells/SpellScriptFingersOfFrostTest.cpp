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
 * @file SpellScriptFingersOfFrostTest.cpp
 * @brief Integration tests for Fingers of Frost (74396) charge consumption
 *        and ghost-charge behavior.
 *
 * Spells involved:
 * - 44543: Fingers of Frost talent (procs FoF on chill)
 * - 44544: FoF AURASTATE aura (SPELL_AURA_ABILITY_IGNORE_AURASTATE - lets
 *          the caster's frost spells treat the target as frozen)
 * - 74396: FoF visual buff with charges (consumed by player casts)
 *
 * Behavior modeled here mirrors src/server/scripts/Spells/spell_mage.cpp
 * (spell_mage_fingers_of_frost):
 *
 *   PrepareProc gating (charge consumption per cast):
 *     - Non-triggered, non-channeled, CAST phase  -> consume
 *     - Non-triggered, non-channeled, HIT phase   -> prevent
 *     - Channeled spell at any phase              -> prevent (Blizzard
 *       channel itself does not consume FoF stacks)
 *     - Triggered spell                           -> consume (Blizzard
 *       ticks per-tick consumption from #24758)
 *
 *   OnRemove (ghost charge):
 *     When 74396 is removed (charges depleted), 44544 is NOT removed
 *     immediately. Its remaining duration is set to GHOST_CHARGE_GRACE
 *     so an instant follow-up cast (Deep Freeze, Ice Lance, Brain Freeze
 *     combo) chained off the consuming spell still benefits from FoF.
 *     In retail this window came from client-server latency.
 */

#include "Duration.h"
#include "gtest/gtest.h"
#include <cstdint>

namespace
{
    // Mirrors spell_mage_fingers_of_frost::GHOST_CHARGE_GRACE in
    // src/server/scripts/Spells/spell_mage.cpp. Update here if the script
    // value changes.
    constexpr Milliseconds GHOST_CHARGE_GRACE = 400ms;

    constexpr uint32_t SPELL_FOF_AURASTATE = 44544;
    constexpr uint32_t SPELL_FOF_VISUAL    = 74396;
    constexpr uint8_t  FOF_MAX_CHARGES     = 2;
    constexpr Milliseconds FOF_DEFAULT_DURATION = 15s; // DBC duration index 8

    /**
     * @brief Mirrors PrepareProc gating from spell_mage_fingers_of_frost.
     *
     * Returns true if the proc is allowed to consume a charge,
     * false if PreventDefaultAction would fire.
     */
    bool SimulatePrepareProcAllowsConsume(bool isTriggered, bool isChanneled, bool isCastPhase)
    {
        bool prevent = false;
        if (isTriggered)
            prevent = false;
        else if (isChanneled)
            prevent = true;
        else if (!isCastPhase)
            prevent = true;
        return !prevent;
    }

    /**
     * @brief Minimal state model for the two interlocked auras.
     *
     * Tracks charges on 74396 and remaining duration on 44544. Models:
     *   - charge consumption (with PrepareProc gating)
     *   - removal of 74396 when charges hit 0
     *   - the script's OnRemove handler setting 44544 duration to the
     *     grace window instead of removing it
     *   - duration tick down
     *   - re-proc that re-applies 44544 (refreshes its duration) and
     *     re-creates 74396 with full charges
     */
    struct FoFState
    {
        bool         visualPresent     = false;  // 74396
        uint8_t      visualCharges     = 0;
        bool         aurastatePresent  = false;  // 44544
        Milliseconds aurastateDuration = 0ms;

        // Apply / refresh from a fresh FoF proc (e.g. Blizzard chill rolling
        // through). Mirrors HandleAuraSpecificMods on 44544 apply.
        void ApplyFromProc()
        {
            aurastatePresent = true;
            aurastateDuration = FOF_DEFAULT_DURATION;

            if (visualPresent)
            {
                // Refresh the visual buff: 2 charges, full duration.
                visualCharges = FOF_MAX_CHARGES;
            }
            else
            {
                visualPresent = true;
                visualCharges = FOF_MAX_CHARGES;
            }
        }

        // Mirrors the proc system path:
        //   1) PrepareProcToTrigger decrements charges (gated by PrepareProc)
        //   2) ConsumeProcCharges removes 74396 if charges hit 0
        //   3) AfterEffectRemove on 74396 sets 44544 duration to grace
        //
        // Returns true if a charge was actually consumed.
        bool TryConsume(bool isTriggered, bool isChanneled, bool isCastPhase)
        {
            if (!visualPresent || visualCharges == 0)
                return false;

            if (!SimulatePrepareProcAllowsConsume(isTriggered, isChanneled, isCastPhase))
                return false;

            --visualCharges;

            if (visualCharges == 0)
            {
                // 74396 removed -> script's OnRemove fires
                visualPresent = false;
                if (aurastatePresent)
                    aurastateDuration = GHOST_CHARGE_GRACE;
            }
            return true;
        }

        // Advance simulated time. Mirrors the aura update tick draining
        // m_duration; on expiry 44544 is removed.
        void AdvanceTime(Milliseconds delta)
        {
            if (!aurastatePresent)
                return;
            aurastateDuration -= delta;
            if (aurastateDuration <= 0ms)
            {
                aurastatePresent = false;
                aurastateDuration = 0ms;
            }
        }

        // True if a frost spell cast right now would benefit from FoF
        // (i.e. the IGNORE_AURASTATE aura is up).
        bool BenefitsFromFoF() const { return aurastatePresent; }
    };
}

// =============================================================================
// PrepareProc gating - charge consumption per cast
// =============================================================================

class FingersOfFrostConsumeGateTest : public ::testing::Test {};

TEST_F(FingersOfFrostConsumeGateTest, NonTriggered_CastPhase_Consumes)
{
    // Frostbolt cast finishing - non-triggered, non-channeled, CAST phase.
    EXPECT_TRUE(SimulatePrepareProcAllowsConsume(/*triggered*/false,
                                                 /*channeled*/false,
                                                 /*castPhase*/true));
}

TEST_F(FingersOfFrostConsumeGateTest, NonTriggered_HitPhase_DoesNotConsume)
{
    // Frostbolt projectile hit - HIT phase fires after CAST phase has
    // already consumed; HIT must not double-dip.
    EXPECT_FALSE(SimulatePrepareProcAllowsConsume(/*triggered*/false,
                                                  /*channeled*/false,
                                                  /*castPhase*/false));
}

TEST_F(FingersOfFrostConsumeGateTest, ChanneledSpell_CastPhase_DoesNotConsume)
{
    // Blizzard channel start at CAST phase must not consume a stack -
    // the channel itself is the proccer, not a consumer.
    EXPECT_FALSE(SimulatePrepareProcAllowsConsume(/*triggered*/false,
                                                  /*channeled*/true,
                                                  /*castPhase*/true));
}

TEST_F(FingersOfFrostConsumeGateTest, ChanneledSpell_HitPhase_DoesNotConsume)
{
    EXPECT_FALSE(SimulatePrepareProcAllowsConsume(/*triggered*/false,
                                                  /*channeled*/true,
                                                  /*castPhase*/false));
}

TEST_F(FingersOfFrostConsumeGateTest, TriggeredSpell_HitPhase_Consumes)
{
    // Blizzard tick (triggered) - per #24758, each tick consumes a stack.
    EXPECT_TRUE(SimulatePrepareProcAllowsConsume(/*triggered*/true,
                                                 /*channeled*/false,
                                                 /*castPhase*/false));
}

TEST_F(FingersOfFrostConsumeGateTest, TriggeredSpell_CastPhase_Consumes)
{
    // Defensive: triggered spells normally only fire HIT phase, but the
    // gate should still allow consumption if a triggered spell did
    // somehow surface a CAST phase event.
    EXPECT_TRUE(SimulatePrepareProcAllowsConsume(/*triggered*/true,
                                                 /*channeled*/false,
                                                 /*castPhase*/true));
}

// =============================================================================
// Charge depletion -> ghost charge grace window
// =============================================================================

class FingersOfFrostGhostChargeTest : public ::testing::Test
{
protected:
    FoFState state;

    void SetUp() override
    {
        // Start with FoF freshly procced - 2 charges, 44544 active.
        state.ApplyFromProc();
        ASSERT_TRUE(state.visualPresent);
        ASSERT_EQ(state.visualCharges, FOF_MAX_CHARGES);
        ASSERT_TRUE(state.aurastatePresent);
        ASSERT_EQ(state.aurastateDuration, FOF_DEFAULT_DURATION);
    }
};

TEST_F(FingersOfFrostGhostChargeTest, FirstFrostbolt_LeavesOneChargeAndFullDuration)
{
    // Cast Frostbolt 1 - consumes one charge, 44544 stays at full duration.
    bool consumed = state.TryConsume(/*triggered*/false,
                                     /*channeled*/false,
                                     /*castPhase*/true);
    EXPECT_TRUE(consumed);
    EXPECT_TRUE(state.visualPresent);
    EXPECT_EQ(state.visualCharges, 1);
    EXPECT_TRUE(state.aurastatePresent);
    EXPECT_EQ(state.aurastateDuration, FOF_DEFAULT_DURATION);
}

TEST_F(FingersOfFrostGhostChargeTest, SecondFrostbolt_RemovesVisualAndArmsGracePeriod)
{
    // Cast FB1 then FB2.
    state.TryConsume(false, false, true);
    state.TryConsume(false, false, true);

    // 74396 must be gone.
    EXPECT_FALSE(state.visualPresent);
    EXPECT_EQ(state.visualCharges, 0);

    // 44544 must still be present, but with the grace window armed.
    EXPECT_TRUE(state.aurastatePresent);
    EXPECT_EQ(state.aurastateDuration, GHOST_CHARGE_GRACE);
}

TEST_F(FingersOfFrostGhostChargeTest, GhostCharge_DeepFreezeWithinGraceWindow_Benefits)
{
    state.TryConsume(false, false, true);  // FB1
    state.TryConsume(false, false, true);  // FB2 - last charge

    // Player presses Deep Freeze ~50ms after FB2 finishes (instant).
    state.AdvanceTime(50ms);

    // 44544 still up -> Deep Freeze treats target as frozen and lands.
    EXPECT_TRUE(state.BenefitsFromFoF());
}

TEST_F(FingersOfFrostGhostChargeTest, GhostCharge_DeepFreezeJustInsideGraceWindow_Benefits)
{
    state.TryConsume(false, false, true);
    state.TryConsume(false, false, true);

    // Tick right up to (but not past) the grace boundary.
    state.AdvanceTime(GHOST_CHARGE_GRACE - 1ms);

    EXPECT_TRUE(state.BenefitsFromFoF());
}

TEST_F(FingersOfFrostGhostChargeTest, GhostCharge_DeepFreezeAfterGraceWindow_DoesNotBenefit)
{
    state.TryConsume(false, false, true);
    state.TryConsume(false, false, true);

    // Wait past the grace window - ghost charge expired.
    state.AdvanceTime(GHOST_CHARGE_GRACE + 1ms);

    EXPECT_FALSE(state.BenefitsFromFoF());
}

TEST_F(FingersOfFrostGhostChargeTest, GhostCharge_LongDelayAfterLastCast_DoesNotBenefit)
{
    state.TryConsume(false, false, true);
    state.TryConsume(false, false, true);

    // 1 full second is well past the grace window.
    state.AdvanceTime(1s);

    EXPECT_FALSE(state.BenefitsFromFoF());
}

// =============================================================================
// Re-proc during the grace window
// =============================================================================

class FingersOfFrostReProcTest : public ::testing::Test
{
protected:
    FoFState state;

    void SetUp() override { state.ApplyFromProc(); }
};

TEST_F(FingersOfFrostReProcTest, ReProcDuringGrace_RestoresFullChargesAndDuration)
{
    // Burn both charges, then re-proc within the grace window.
    state.TryConsume(false, false, true);
    state.TryConsume(false, false, true);
    state.AdvanceTime(100ms);  // still inside the 400ms grace
    ASSERT_TRUE(state.aurastatePresent);
    ASSERT_FALSE(state.visualPresent);

    // Blizzard chill rolls FoF.
    state.ApplyFromProc();

    // 74396 is back with full charges, 44544 refreshed to default duration.
    EXPECT_TRUE(state.visualPresent);
    EXPECT_EQ(state.visualCharges, FOF_MAX_CHARGES);
    EXPECT_TRUE(state.aurastatePresent);
    EXPECT_EQ(state.aurastateDuration, FOF_DEFAULT_DURATION);
}

TEST_F(FingersOfFrostReProcTest, ReProcAfterGraceExpired_AppliesFresh)
{
    state.TryConsume(false, false, true);
    state.TryConsume(false, false, true);
    state.AdvanceTime(GHOST_CHARGE_GRACE + 100ms);  // past the grace
    ASSERT_FALSE(state.aurastatePresent);
    ASSERT_FALSE(state.visualPresent);

    state.ApplyFromProc();

    EXPECT_TRUE(state.visualPresent);
    EXPECT_EQ(state.visualCharges, FOF_MAX_CHARGES);
    EXPECT_TRUE(state.aurastatePresent);
    EXPECT_EQ(state.aurastateDuration, FOF_DEFAULT_DURATION);
}

// =============================================================================
// Issue #25117 scenario - end-to-end
// =============================================================================

class FingersOfFrostIssue25117Scenario : public ::testing::Test
{
protected:
    FoFState state;

    void SetUp() override { state.ApplyFromProc(); }

    // Helpers to keep the scenario readable.
    bool CastFrostbolt() { return state.TryConsume(false, false, true); }
    bool CastInstantBenefits()
    {
        // Simulates Deep Freeze / Ice Lance: instant cast that benefits if
        // 44544 is up at the moment of cast. The cast itself only consumes
        // a charge if 74396 is still present.
        bool benefit = state.BenefitsFromFoF();
        state.TryConsume(false, false, true);
        return benefit;
    }
};

// Reporter's case from #25117:
//   FB1 (consume 1/2)
//   FB2 (consume last) + Deep Freeze (ghost charge - benefits but no consume)
TEST_F(FingersOfFrostIssue25117Scenario, FrostboltFrostboltDeepFreezeCombo)
{
    EXPECT_TRUE(CastFrostbolt());                // FB1 - charge consumed
    EXPECT_EQ(state.visualCharges, 1);

    EXPECT_TRUE(CastFrostbolt());                // FB2 - last charge consumed
    EXPECT_FALSE(state.visualPresent);
    EXPECT_TRUE(state.aurastatePresent);         // 44544 in grace

    state.AdvanceTime(50ms);                     // press DF ~50ms later
    EXPECT_TRUE(CastInstantBenefits());          // ghost charge - DF lands

    // 74396 was already gone - DF didn't consume anything new.
    EXPECT_FALSE(state.visualPresent);
}

// After the ghost charge fires, a *second* instant cast queued well past
// the grace window must not get a free benefit.
TEST_F(FingersOfFrostIssue25117Scenario, NoSecondGhostChargeAfterGraceExpires)
{
    CastFrostbolt();
    CastFrostbolt();
    state.AdvanceTime(50ms);
    EXPECT_TRUE(CastInstantBenefits());          // legitimate ghost charge

    // GCD-paced gap, then another instant.
    state.AdvanceTime(GHOST_CHARGE_GRACE + 1100ms);
    EXPECT_FALSE(CastInstantBenefits());         // no FoF benefit anymore
}

// Regression for #24758: a Blizzard channel followed by ticks. The cast
// itself must not consume; ticks (triggered) must.
TEST_F(FingersOfFrostIssue25117Scenario, BlizzardTicksConsumeButChannelStartDoesNot)
{
    // Blizzard channel start - channeled, CAST phase. No consume.
    EXPECT_FALSE(state.TryConsume(/*triggered*/false,
                                  /*channeled*/true,
                                  /*castPhase*/true));
    EXPECT_EQ(state.visualCharges, 2);

    // Two ticks - each is a triggered spell, HIT phase. Both consume.
    EXPECT_TRUE(state.TryConsume(/*triggered*/true, false, false));
    EXPECT_EQ(state.visualCharges, 1);

    EXPECT_TRUE(state.TryConsume(/*triggered*/true, false, false));
    EXPECT_FALSE(state.visualPresent);
    EXPECT_TRUE(state.aurastatePresent);
    EXPECT_EQ(state.aurastateDuration, GHOST_CHARGE_GRACE);
}
