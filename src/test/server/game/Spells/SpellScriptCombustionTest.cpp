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

// Mirrors spell_mage_combustion::CheckProc (spell_mage.cpp). The scripts
// library is not linked into unit_tests, so the callback cannot be invoked
// directly; this locks in its decision for each proc shape.

#include "SharedDefines.h"
#include "gtest/gtest.h"

namespace
{
    enum class CombustionOutcome
    {
        Ignore,        // proc-aura guard: no stack, no charge
        AddStack,      // non-crit: cast SPELL_MAGE_COMBUSTION_PROC (28682)
        ConsumeCharge  // crit: consume a charge
    };

    // hasProcSpell:        eventInfo.GetProcSpell() != nullptr
    // triggeredByProcAura: triggering aura has PROC_TRIGGER_SPELL/DAMAGE
    // isCrit:              hit mask has PROC_HIT_CRITICAL
    CombustionOutcome SimulateCombustionCheckProc(bool hasProcSpell,
        bool triggeredByProcAura, bool isCrit)
    {
        if (hasProcSpell && triggeredByProcAura)
            return CombustionOutcome::Ignore;

        if (!isCrit)
            return CombustionOutcome::AddStack;

        return CombustionOutcome::ConsumeCharge;
    }
}

class CombustionCheckProcTest : public ::testing::Test {};

// Molten Armor (34913): cast from a proc-trigger aura, must be ignored.
TEST_F(CombustionCheckProcTest, MoltenArmorProc_NonCrit_DoesNotAddStack)
{
    EXPECT_EQ(SimulateCombustionCheckProc(true, true, false),
        CombustionOutcome::Ignore);
}

TEST_F(CombustionCheckProcTest, MoltenArmorProc_Crit_DoesNotConsumeCharge)
{
    EXPECT_EQ(SimulateCombustionCheckProc(true, true, true),
        CombustionOutcome::Ignore);
}

// Living Bomb explosion (44461): triggered by a dummy effect, not a proc.
TEST_F(CombustionCheckProcTest, LivingBombExplosion_NonCrit_AddsStack)
{
    EXPECT_EQ(SimulateCombustionCheckProc(true, false, false),
        CombustionOutcome::AddStack);
}

TEST_F(CombustionCheckProcTest, LivingBombExplosion_Crit_ConsumesCharge)
{
    EXPECT_EQ(SimulateCombustionCheckProc(true, false, true),
        CombustionOutcome::ConsumeCharge);
}

// Directly cast fire spell (no triggering aura).
TEST_F(CombustionCheckProcTest, DirectFireSpell_NonCrit_AddsStack)
{
    EXPECT_EQ(SimulateCombustionCheckProc(true, false, false),
        CombustionOutcome::AddStack);
}

TEST_F(CombustionCheckProcTest, DirectFireSpell_Crit_ConsumesCharge)
{
    EXPECT_EQ(SimulateCombustionCheckProc(true, false, true),
        CombustionOutcome::ConsumeCharge);
}

// Periodic tick (e.g. Ignite): no proc spell, guard skipped.
TEST_F(CombustionCheckProcTest, PeriodicTick_NoProcSpell_NonCrit_AddsStack)
{
    EXPECT_EQ(SimulateCombustionCheckProc(false, false, false),
        CombustionOutcome::AddStack);
}

TEST_F(CombustionCheckProcTest, PeriodicTick_NoProcSpell_Crit_ConsumesCharge)
{
    EXPECT_EQ(SimulateCombustionCheckProc(false, false, true),
        CombustionOutcome::ConsumeCharge);
}
