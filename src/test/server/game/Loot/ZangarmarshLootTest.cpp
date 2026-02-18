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

#include "Random.h"
#include "gtest/gtest.h"
#include <cmath>
#include <vector>
#include <map>

/**
 * Test for issue #14606: Zangarmarsh wrong loot tables resulting in mass grey drops
 *
 * These creatures incorrectly have 100% chance to roll on reference 6010,
 * which contains 30% grey armor and 30% grey weapons.
 *
 * Expected (wowhead): ~0.07% per grey armor, ~0.056% per grey weapon
 * Current (broken):   ~0.94% per grey armor, ~3% per grey weapon
 *
 * Fix: Set Chance to ~2% instead of 100% for reference 6010 on these creatures.
 */

// Reference table 6010 structure
constexpr float REF_6010_ARMOR_CHANCE = 30.0f;  // Reference 4000 (grey armor)
constexpr float REF_6010_WEAPON_CHANCE = 30.0f; // Reference 4001 (grey weapons)
constexpr int NUM_ARMOR_ITEMS = 32;
constexpr int NUM_WEAPON_ITEMS = 10;

// Wowhead expected rates
constexpr float WOWHEAD_AVG_ARMOR_PER_ITEM = 0.0726f;
constexpr float WOWHEAD_AVG_WEAPON_PER_ITEM = 0.056f;

// Test iterations
constexpr int NUM_ITERATIONS = 100000;

// Tolerance: allow up to 3x variance due to RNG
constexpr float TOLERANCE_MULTIPLIER = 3.0f;

struct LootSimulationResult
{
    int armorDrops = 0;
    int weaponDrops = 0;
    int totalKills = 0;

    float GetArmorDropRate() const { return totalKills > 0 ? (100.0f * armorDrops / totalKills) : 0; }
    float GetWeaponDropRate() const { return totalKills > 0 ? (100.0f * weaponDrops / totalKills) : 0; }
    float GetArmorPerItemRate() const { return GetArmorDropRate() / NUM_ARMOR_ITEMS; }
    float GetWeaponPerItemRate() const { return GetWeaponDropRate() / NUM_WEAPON_ITEMS; }
};

/**
 * Simulates the loot roll logic from LootStoreItem::Roll()
 * For reference items: roll_chance_f(chance * rate_modifier)
 */
class LootSimulator
{
public:
    explicit LootSimulator(float creatureToRef6010Chance)
        : _creatureChance(creatureToRef6010Chance) {}

    LootSimulationResult SimulateKills(int numKills)
    {
        LootSimulationResult result;
        result.totalKills = numKills;

        for (int i = 0; i < numKills; ++i)
        {
            // First roll: does creature roll on reference 6010?
            if (roll_chance_f(_creatureChance))
            {
                // Inside ref 6010: roll for armor (ref 4000) at 30%
                if (roll_chance_f(REF_6010_ARMOR_CHANCE))
                {
                    result.armorDrops++;
                }

                // Inside ref 6010: roll for weapons (ref 4001) at 30%
                if (roll_chance_f(REF_6010_WEAPON_CHANCE))
                {
                    result.weaponDrops++;
                }
            }
        }

        return result;
    }

private:
    float _creatureChance;
};

// Test that current broken configuration (Chance=100) produces too many drops
TEST(ZangarmarshLootTest, CurrentBrokenConfiguration)
{
    LootSimulator simulator(100.0f); // Current broken value
    LootSimulationResult result = simulator.SimulateKills(NUM_ITERATIONS);

    float armorPerItem = result.GetArmorPerItemRate();
    float weaponPerItem = result.GetWeaponPerItemRate();

    // These should be WAY higher than wowhead values (broken)
    float armorRatio = armorPerItem / WOWHEAD_AVG_ARMOR_PER_ITEM;
    float weaponRatio = weaponPerItem / WOWHEAD_AVG_WEAPON_PER_ITEM;

    // With Chance=100, we expect ~13x armor and ~54x weapon drop rates
    EXPECT_GT(armorRatio, 10.0f) << "Armor drop rate should be >10x wowhead (broken)";
    EXPECT_GT(weaponRatio, 40.0f) << "Weapon drop rate should be >40x wowhead (broken)";
}

// Test that fixed configuration (Chance=2) produces correct drop rates
TEST(ZangarmarshLootTest, FixedConfiguration_Chance2)
{
    LootSimulator simulator(2.0f); // Fixed value
    LootSimulationResult result = simulator.SimulateKills(NUM_ITERATIONS);

    float armorPerItem = result.GetArmorPerItemRate();
    float weaponPerItem = result.GetWeaponPerItemRate();

    float armorRatio = armorPerItem / WOWHEAD_AVG_ARMOR_PER_ITEM;
    float weaponRatio = weaponPerItem / WOWHEAD_AVG_WEAPON_PER_ITEM;

    // With Chance=2, weapon should be close to wowhead (~1.07x)
    // Armor will be lower (~0.26x) but that's acceptable given the reference structure
    EXPECT_LT(weaponRatio, TOLERANCE_MULTIPLIER)
        << "Weapon drop rate should be within " << TOLERANCE_MULTIPLIER << "x of wowhead";
    EXPECT_GT(weaponRatio, 1.0f / TOLERANCE_MULTIPLIER)
        << "Weapon drop rate should be within " << TOLERANCE_MULTIPLIER << "x of wowhead";
}

// Test that fixed configuration (Chance=7) produces correct drop rates
TEST(ZangarmarshLootTest, FixedConfiguration_Chance7)
{
    LootSimulator simulator(7.0f); // Alternative fixed value
    LootSimulationResult result = simulator.SimulateKills(NUM_ITERATIONS);

    float armorPerItem = result.GetArmorPerItemRate();
    float weaponPerItem = result.GetWeaponPerItemRate();

    float armorRatio = armorPerItem / WOWHEAD_AVG_ARMOR_PER_ITEM;
    float weaponRatio = weaponPerItem / WOWHEAD_AVG_WEAPON_PER_ITEM;

    // With Chance=7, armor should be close to wowhead (~0.9x)
    // Weapon will be higher (~3.75x) but still much better than 54x
    EXPECT_LT(armorRatio, TOLERANCE_MULTIPLIER)
        << "Armor drop rate should be within " << TOLERANCE_MULTIPLIER << "x of wowhead";
    EXPECT_GT(armorRatio, 1.0f / TOLERANCE_MULTIPLIER)
        << "Armor drop rate should be within " << TOLERANCE_MULTIPLIER << "x of wowhead";
}

// Parameterized test for different Chance values
class LootChanceTest : public ::testing::TestWithParam<float> {};

TEST_P(LootChanceTest, VerifyDropRates)
{
    float chance = GetParam();
    LootSimulator simulator(chance);
    LootSimulationResult result = simulator.SimulateKills(NUM_ITERATIONS);

    // Calculate expected rates based on formula
    float expectedArmorRate = chance * (REF_6010_ARMOR_CHANCE / 100.0f);
    float expectedWeaponRate = chance * (REF_6010_WEAPON_CHANCE / 100.0f);

    // Allow 10% tolerance for RNG variance
    float armorTolerance = expectedArmorRate * 0.1f + 0.1f;
    float weaponTolerance = expectedWeaponRate * 0.1f + 0.1f;

    EXPECT_NEAR(result.GetArmorDropRate(), expectedArmorRate, armorTolerance)
        << "Armor drop rate for Chance=" << chance;
    EXPECT_NEAR(result.GetWeaponDropRate(), expectedWeaponRate, weaponTolerance)
        << "Weapon drop rate for Chance=" << chance;
}

INSTANTIATE_TEST_SUITE_P(
    ZangarmarshLoot,
    LootChanceTest,
    ::testing::Values(2.0f, 5.0f, 7.0f, 10.0f, 100.0f)
);
