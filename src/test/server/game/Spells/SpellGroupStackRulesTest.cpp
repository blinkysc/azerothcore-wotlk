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

#include "SpellMgr.h"
#include "gtest/gtest.h"
#include <map>
#include <set>
#include <vector>

/*
 * SpellGroupStackRules Comprehensive Integration Tests
 *
 * These tests verify the spell_group_stack_rules system implemented in PR #23346.
 * Tests ALL permutations of spell pairs within each group from the database.
 *
 * Stack Rules:
 * - Rule 0 (DEFAULT): No special stacking rule - spells stack normally
 * - Rule 1 (EXCLUSIVE): Only one buff from group allowed - new replaces old
 * - Rule 2 (EXCLUSIVE_FROM_SAME_CASTER): One blessing per paladin on target
 * - Rule 3 (EXCLUSIVE_SAME_EFFECT): Both visible, only strongest effect applies
 * - Rule 4 (EXCLUSIVE_HIGHEST): Highest value buff wins, lower is replaced
 *
 * NOTE: Integration tests require SpellMgr to be initialized with database data.
 * Run these tests in an environment where the worldserver has loaded spell data.
 */

TEST(SpellGroupStackRulesTest, EnumValues_AreCorrect)
{
    EXPECT_EQ(SPELL_GROUP_STACK_RULE_DEFAULT, 0);
    EXPECT_EQ(SPELL_GROUP_STACK_RULE_EXCLUSIVE, 1);
    EXPECT_EQ(SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER, 2);
    EXPECT_EQ(SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT, 3);
    EXPECT_EQ(SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, 4);
    EXPECT_EQ(SPELL_GROUP_STACK_RULE_MAX, 5);
}

TEST(SpellGroupStackRulesTest, EnumMax_IsValid)
{
    EXPECT_GT(SPELL_GROUP_STACK_RULE_MAX, SPELL_GROUP_STACK_RULE_DEFAULT);
    EXPECT_GT(SPELL_GROUP_STACK_RULE_MAX, SPELL_GROUP_STACK_RULE_EXCLUSIVE);
    EXPECT_GT(SPELL_GROUP_STACK_RULE_MAX, SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER);
    EXPECT_GT(SPELL_GROUP_STACK_RULE_MAX, SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT);
    EXPECT_GT(SPELL_GROUP_STACK_RULE_MAX, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST);
}

TEST(SpellGroupStackRulesTest, RuleExclusive_IsNonZero)
{
    EXPECT_NE(SPELL_GROUP_STACK_RULE_EXCLUSIVE, SPELL_GROUP_STACK_RULE_DEFAULT);
}

TEST(SpellGroupStackRulesTest, RuleFromSameCaster_IsNonZero)
{
    EXPECT_NE(SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER, SPELL_GROUP_STACK_RULE_DEFAULT);
}

TEST(SpellGroupStackRulesTest, RuleSameEffect_IsNonZero)
{
    EXPECT_NE(SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT, SPELL_GROUP_STACK_RULE_DEFAULT);
}

TEST(SpellGroupStackRulesTest, RuleHighest_IsNonZero)
{
    EXPECT_NE(SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, SPELL_GROUP_STACK_RULE_DEFAULT);
}

TEST(SpellGroupStackRulesTest, AllRulesAreDifferent)
{
    std::set<int> rules = {
        SPELL_GROUP_STACK_RULE_DEFAULT,
        SPELL_GROUP_STACK_RULE_EXCLUSIVE,
        SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER,
        SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT,
        SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST
    };
    EXPECT_EQ(rules.size(), 5u) << "All stack rules should be unique values";
}

TEST(SpellGroupStackRulesTest, RulesAreSequential)
{
    EXPECT_EQ(SPELL_GROUP_STACK_RULE_DEFAULT + 1, SPELL_GROUP_STACK_RULE_EXCLUSIVE);
    EXPECT_EQ(SPELL_GROUP_STACK_RULE_EXCLUSIVE + 1, SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER);
    EXPECT_EQ(SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER + 1, SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT);
    EXPECT_EQ(SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT + 1, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST);
    EXPECT_EQ(SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST + 1, SPELL_GROUP_STACK_RULE_MAX);
}

class SpellGroupStackRulesIntegrationTest : public ::testing::Test
{
protected:
    bool IsSpellMgrLoaded()
    {
        return sSpellMgr->GetSpellInfo(1) != nullptr;
    }

    const char* GetRuleName(SpellGroupStackRule rule)
    {
        switch (rule)
        {
            case SPELL_GROUP_STACK_RULE_DEFAULT: return "DEFAULT";
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE: return "EXCLUSIVE";
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER: return "EXCLUSIVE_FROM_SAME_CASTER";
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT: return "EXCLUSIVE_SAME_EFFECT";
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST: return "EXCLUSIVE_HIGHEST";
            default: return "UNKNOWN";
        }
    }

    struct SpellGroupData
    {
        uint32 groupId;
        SpellGroupStackRule expectedRule;
        std::vector<uint32> spellIds;
        std::string description;
    };

    std::vector<SpellGroupData> GetAllSpellGroups()
    {
        std::vector<SpellGroupData> groups;

        groups.push_back({1, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {2367, 2374, 3160, 3164, 7844, 8212, 10667, 10669, 11328, 11334, 11390, 11405, 11406, 11474, 16322, 16323, 16329, 17038, 17537, 17538, 17539, 17626, 17627, 17628, 17629, 21920, 26276, 28486, 28488, 28490, 28491, 28493, 28497, 28501, 28503, 28518, 28519, 28520, 28521, 28540, 33053, 33720, 33721, 33726, 38954, 40567, 40568, 40572, 40573, 40575, 40576, 41608, 41609, 41610, 41611, 42735, 45373, 46837, 46839, 53746, 53748, 53749, 53752, 53755, 53758, 53760, 54212, 54452, 54494, 60340, 60341, 60344, 60345, 60346, 62380, 63729, 67016, 67017, 67018}, "Battle Elixir"});

        groups.push_back({2, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {673, 2378, 2380, 3166, 3219, 3220, 3222, 3223, 3593, 10668, 10692, 10693, 11319, 11348, 11349, 11371, 11396, 15231, 15233, 16321, 16325, 16326, 17535, 17549, 21151, 24361, 24363, 24382, 24383, 24417, 27652, 27653, 28502, 28509, 28514, 39625, 39626, 39627, 39628, 40569, 40570, 40571, 40577, 41606, 41607, 41612, 42736, 43185, 43186, 43194, 44927, 45371, 46838, 53747, 53751, 53753, 53763, 53764, 54917, 60343, 60347, 61348, 62276, 63721, 65685, 67019, 67020, 67713, 67714, 67715, 67716, 67717, 67718, 67730, 67731, 67732}, "Guardian Elixir"});

        groups.push_back({1001, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {5004, 5007, 6410, 7272, 7910, 7916, 8091, 8095, 8096, 8114, 8116, 8118, 8125, 8170, 9343, 9345, 9348, 9362, 9365, 10256, 10257, 17222, 17224, 17229, 18141, 18191, 18193, 18194, 18222, 18229, 18230, 18231, 18232, 18233, 19705, 19706, 19708, 19709, 19710, 19711, 22730, 22731, 22732, 22787, 23697, 24005, 24800, 24869, 24870, 25660, 25661, 25694, 25804, 25941, 33254, 33256, 33257, 33259, 33261, 33263, 33265, 33268, 33272, 40323, 41030, 43763, 43764, 43771, 43772, 44106, 45245, 45548, 45619, 46687, 46899, 57073, 57079, 57097, 57100, 57102, 57107, 57111, 57139, 57286, 57288, 57291, 57294, 57325, 57327, 57329, 57332, 57334, 57356, 57358, 57360, 57363, 57365, 57367, 57371, 57399, 59227, 65412, 66623}, "Well Fed"});

        groups.push_back({1002, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {19740, 19834, 19835, 19836, 19837, 19838, 25291, 27140, 48931, 48932, 56520}, "Blessing of Might"});

        groups.push_back({1003, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {6673, 5242, 6192, 11549, 11550, 11551, 25289, 2048, 47436}, "Battle Shout"});

        groups.push_back({1005, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {19742, 19850, 19852, 19853, 19854, 25290, 27142, 48935, 48936}, "Blessing of Wisdom"});

        groups.push_back({1006, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {20217, 25898, 72586, 69378, 25899, 43223}, "All Stats Percentage"});

        groups.push_back({1007, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {20911, 25899}, "Blessing of Sanctuary"});

        groups.push_back({1008, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {1022, 5599}, "Blessing of Protection"});

        groups.push_back({1016, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {770, 16857}, "Faerie Fire"});

        groups.push_back({1037, SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT, {17800, 22959, 12579, 12873, 28593, 64085, 71552}, "Spell Crit Debuffs"});

        groups.push_back({1058, SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT, {2818, 3583, 8679, 8680, 25349}, "Poisons"});

        groups.push_back({1059, SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT, {6343, 8042, 8044, 8045, 8046, 10414, 25464, 49231, 49232, 55095, 58179, 58180, 58181}, "Attack Speed Debuffs"});

        groups.push_back({1060, SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT, {3043, 5570}, "Hit Chance Debuffs"});

        groups.push_back({1061, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {12294, 21551, 21552, 21553, 25248, 30330, 47485, 47486}, "Healing Taken Debuffs"});

        groups.push_back({1062, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {99, 1735, 9490, 9747, 9898, 26998, 48559, 48560}, "AP Debuffs"});

        groups.push_back({1084, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {21562, 21564, 25392, 48161, 48162}, "Single Stamina Buffs"});

        groups.push_back({1089, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {1126, 5232, 6756, 5234, 8907, 9884, 9885, 26990, 48469, 21849, 21850, 26991, 48470}, "Mark of the Wild"});

        groups.push_back({1094, SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT, {34123, 63534}, "Healing Taken Buffs"});

        groups.push_back({1095, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {57339, 57340}, "Physical Taken Buffs"});

        groups.push_back({1096, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {1714, 5760, 13975, 31125, 50274}, "Cast Time Debuffs"});

        groups.push_back({1097, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {6136, 7321, 12484}, "Frost Novals"});

        groups.push_back({1098, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {976, 27683}, "Shadow Protection"});

        groups.push_back({1099, SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER, {348, 30108}, "Immolate and Unstable Affliction"});

        groups.push_back({1100, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {604, 1008}, "Dampen/Amplify Magic"});

        groups.push_back({1101, SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT, {1490, 17803, 34889}, "Spell Damage Taken Debuffs"});

        groups.push_back({1104, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {12880, 14201, 14202}, "Warrior Enrages"});

        groups.push_back({1106, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {6562, 28878}, "Heroic Presence"});

        groups.push_back({1107, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {13877, 22482, 32182, 49016, 51271, 53148, 55694}, "Temporary Damage Increases"});

        groups.push_back({1108, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {58683, 58684}, "Phys Damage Taken Debuffs"});

        groups.push_back({1110, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {70701, 70702, 70703, 70704, 70705, 70706, 70707, 70708, 70709, 70710, 70711}, "Corporeality"});

        groups.push_back({1111, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {62594, 62595, 62596, 63395, 63396, 63397, 63398, 63399, 63400, 63401}, "Champion's Pennants"});

        groups.push_back({1112, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {8219, 8220, 8221, 8222}, "Flip Out"});

        groups.push_back({1121, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {36798, 36799, 36800}, "Elemental Slave Buffs"});

        groups.push_back({1122, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {2825, 32182, 80353}, "Temporary Haste Buffs"});

        groups.push_back({1123, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {10060, 12042}, "Power Infusion and Arcane Power"});

        return groups;
    }
};

TEST_F(SpellGroupStackRulesIntegrationTest, AllPermutations_SameGroup_ReturnCorrectRule)
{
    if (!IsSpellMgrLoaded())
    {
        GTEST_SKIP() << "SpellMgr not initialized - skipping integration test";
    }

    auto groups = GetAllSpellGroups();

    int totalGroups = 0;
    int totalPairs = 0;
    int passedPairs = 0;
    int failedPairs = 0;
    int skippedPairs = 0;

    std::vector<std::string> failures;

    for (const auto& group : groups)
    {
        if (group.spellIds.size() < 2)
            continue;

        totalGroups++;

        for (size_t i = 0; i < group.spellIds.size(); ++i)
        {
            for (size_t j = i + 1; j < group.spellIds.size(); ++j)
            {
                uint32 spell1 = group.spellIds[i];
                uint32 spell2 = group.spellIds[j];

                SpellInfo const* info1 = sSpellMgr->GetSpellInfo(spell1);
                SpellInfo const* info2 = sSpellMgr->GetSpellInfo(spell2);

                if (!info1 || !info2)
                {
                    skippedPairs++;
                    continue;
                }

                totalPairs++;
                auto result = sSpellMgr->CheckSpellGroupStackRules(info1, info2);

                if (result == group.expectedRule)
                {
                    passedPairs++;
                }
                else
                {
                    failedPairs++;
                    std::string failure = "Group " + std::to_string(group.groupId) + " (" + group.description + "): "
                        + "Spell " + std::to_string(spell1) + " vs " + std::to_string(spell2)
                        + " - Expected " + GetRuleName(group.expectedRule)
                        + ", Got " + GetRuleName(result);
                    failures.push_back(failure);

                    if (failures.size() <= 20)
                    {
                        ADD_FAILURE() << failure;
                    }
                }
            }
        }
    }

    std::cout << "\n=== SAME GROUP PERMUTATION TEST RESULTS ===" << std::endl;
    std::cout << "Groups tested: " << totalGroups << std::endl;
    std::cout << "Total pairs: " << totalPairs << std::endl;
    std::cout << "Passed: " << passedPairs << std::endl;
    std::cout << "Failed: " << failedPairs << std::endl;
    std::cout << "Skipped (spell not found): " << skippedPairs << std::endl;

    if (failures.size() > 20)
    {
        std::cout << "\n(Showing first 20 failures, " << (failures.size() - 20) << " more omitted)" << std::endl;
    }

    EXPECT_EQ(failedPairs, 0) << "Some spell pairs returned incorrect stack rules";
    EXPECT_GT(totalPairs, 0) << "No spell pairs were tested";
}

TEST_F(SpellGroupStackRulesIntegrationTest, AllPermutations_CrossGroup_ReturnDefault)
{
    if (!IsSpellMgrLoaded())
    {
        GTEST_SKIP() << "SpellMgr not initialized - skipping integration test";
    }

    auto groups = GetAllSpellGroups();

    int totalPairs = 0;
    int passedPairs = 0;
    int failedPairs = 0;
    int skippedPairs = 0;

    std::vector<std::string> failures;

    for (size_t g1 = 0; g1 < groups.size(); ++g1)
    {
        for (size_t g2 = g1 + 1; g2 < groups.size(); ++g2)
        {
            if (groups[g1].spellIds.empty() || groups[g2].spellIds.empty())
                continue;

            uint32 spell1 = groups[g1].spellIds[0];
            uint32 spell2 = groups[g2].spellIds[0];

            SpellInfo const* info1 = sSpellMgr->GetSpellInfo(spell1);
            SpellInfo const* info2 = sSpellMgr->GetSpellInfo(spell2);

            if (!info1 || !info2)
            {
                skippedPairs++;
                continue;
            }

            totalPairs++;
            auto result = sSpellMgr->CheckSpellGroupStackRules(info1, info2);

            if (result == SPELL_GROUP_STACK_RULE_DEFAULT)
            {
                passedPairs++;
            }
            else
            {
                failedPairs++;
                std::string failure = "Cross-group: Group " + std::to_string(groups[g1].groupId)
                    + " (" + groups[g1].description + ") vs Group " + std::to_string(groups[g2].groupId)
                    + " (" + groups[g2].description + "): Spell " + std::to_string(spell1)
                    + " vs " + std::to_string(spell2) + " - Expected DEFAULT, Got " + GetRuleName(result);
                failures.push_back(failure);

                if (failures.size() <= 10)
                {
                    ADD_FAILURE() << failure;
                }
            }
        }
    }

    std::cout << "\n=== CROSS GROUP TEST RESULTS ===" << std::endl;
    std::cout << "Cross-group pairs tested: " << totalPairs << std::endl;
    std::cout << "Passed: " << passedPairs << std::endl;
    std::cout << "Failed: " << failedPairs << std::endl;
    std::cout << "Skipped: " << skippedPairs << std::endl;

    EXPECT_EQ(failedPairs, 0) << "Some cross-group pairs did not return DEFAULT";
    EXPECT_GT(totalPairs, 0) << "No cross-group pairs were tested";
}

TEST_F(SpellGroupStackRulesIntegrationTest, Symmetry_AllPairs)
{
    if (!IsSpellMgrLoaded())
    {
        GTEST_SKIP() << "SpellMgr not initialized - skipping integration test";
    }

    auto groups = GetAllSpellGroups();

    int totalPairs = 0;
    int asymmetricPairs = 0;

    for (const auto& group : groups)
    {
        for (size_t i = 0; i < group.spellIds.size(); ++i)
        {
            for (size_t j = i + 1; j < group.spellIds.size(); ++j)
            {
                uint32 spell1 = group.spellIds[i];
                uint32 spell2 = group.spellIds[j];

                SpellInfo const* info1 = sSpellMgr->GetSpellInfo(spell1);
                SpellInfo const* info2 = sSpellMgr->GetSpellInfo(spell2);

                if (!info1 || !info2)
                    continue;

                totalPairs++;

                auto result1 = sSpellMgr->CheckSpellGroupStackRules(info1, info2);
                auto result2 = sSpellMgr->CheckSpellGroupStackRules(info2, info1);

                if (result1 != result2)
                {
                    asymmetricPairs++;
                    ADD_FAILURE() << "Asymmetry: Spell " << spell1 << " vs " << spell2
                        << " = " << GetRuleName(result1) << ", but " << spell2 << " vs " << spell1
                        << " = " << GetRuleName(result2);
                }
            }
        }
    }

    std::cout << "\n=== SYMMETRY TEST RESULTS ===" << std::endl;
    std::cout << "Pairs tested: " << totalPairs << std::endl;
    std::cout << "Asymmetric pairs: " << asymmetricPairs << std::endl;

    EXPECT_EQ(asymmetricPairs, 0) << "CheckSpellGroupStackRules is not symmetric for all pairs";
}

TEST_F(SpellGroupStackRulesIntegrationTest, Issue22619_FaerieFireVsDemoRoar)
{
    if (!IsSpellMgrLoaded())
    {
        GTEST_SKIP() << "SpellMgr not initialized - skipping integration test";
    }

    SpellInfo const* faerieFire = sSpellMgr->GetSpellInfo(770);
    SpellInfo const* demoRoar = sSpellMgr->GetSpellInfo(99);

    if (!faerieFire || !demoRoar)
    {
        GTEST_SKIP() << "Required spells not found";
    }

    auto result = sSpellMgr->CheckSpellGroupStackRules(faerieFire, demoRoar);
    EXPECT_EQ(result, SPELL_GROUP_STACK_RULE_DEFAULT)
        << "Issue #22619: Faerie Fire (770) and Demo Roar (99) should stack (return DEFAULT)";
}

TEST_F(SpellGroupStackRulesIntegrationTest, Issue19324_DrumsVsBlessingOfKings)
{
    if (!IsSpellMgrLoaded())
    {
        GTEST_SKIP() << "SpellMgr not initialized - skipping integration test";
    }

    SpellInfo const* drumsBuff = sSpellMgr->GetSpellInfo(72586);
    SpellInfo const* blessingKings = sSpellMgr->GetSpellInfo(20217);

    if (!drumsBuff || !blessingKings)
    {
        GTEST_SKIP() << "Required spells not found";
    }

    auto result = sSpellMgr->CheckSpellGroupStackRules(drumsBuff, blessingKings);

    EXPECT_EQ(result, SPELL_GROUP_STACK_RULE_EXCLUSIVE)
        << "Issue #19324: Drums buff (72586) and BoK (20217) should be in same exclusive group";
}

TEST_F(SpellGroupStackRulesIntegrationTest, BattleVsGuardianElixir_CanStack)
{
    if (!IsSpellMgrLoaded())
    {
        GTEST_SKIP() << "SpellMgr not initialized - skipping integration test";
    }

    SpellInfo const* battleElixir = sSpellMgr->GetSpellInfo(53748);
    SpellInfo const* guardianElixir = sSpellMgr->GetSpellInfo(53751);

    if (!battleElixir || !guardianElixir)
    {
        GTEST_SKIP() << "Required spells not found";
    }

    auto result = sSpellMgr->CheckSpellGroupStackRules(battleElixir, guardianElixir);
    EXPECT_EQ(result, SPELL_GROUP_STACK_RULE_DEFAULT)
        << "Battle Elixir and Guardian Elixir should be able to stack (different groups)";
}

TEST_F(SpellGroupStackRulesIntegrationTest, MarkOfTheWild_HigherRankReplaces)
{
    if (!IsSpellMgrLoaded())
    {
        GTEST_SKIP() << "SpellMgr not initialized - skipping integration test";
    }

    SpellInfo const* motwRank1 = sSpellMgr->GetSpellInfo(1126);
    SpellInfo const* motwRank8 = sSpellMgr->GetSpellInfo(48469);

    if (!motwRank1 || !motwRank8)
    {
        GTEST_SKIP() << "Required spells not found";
    }

    auto result = sSpellMgr->CheckSpellGroupStackRules(motwRank1, motwRank8);
    EXPECT_EQ(result, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST)
        << "Mark of the Wild ranks should use EXCLUSIVE_HIGHEST rule";
}

TEST_F(SpellGroupStackRulesIntegrationTest, SameCaster_Blessings)
{
    if (!IsSpellMgrLoaded())
    {
        GTEST_SKIP() << "SpellMgr not initialized - skipping integration test";
    }

    SpellInfo const* bom = sSpellMgr->GetSpellInfo(19740);
    SpellInfo const* bow = sSpellMgr->GetSpellInfo(19742);

    if (!bom || !bow)
    {
        GTEST_SKIP() << "Required spells not found";
    }

    auto bomVsBow = sSpellMgr->CheckSpellGroupStackRules(bom, bow);

    std::cout << "\nBlessing of Might vs Blessing of Wisdom: " << GetRuleName(bomVsBow) << std::endl;

    EXPECT_NE(bomVsBow, SPELL_GROUP_STACK_RULE_DEFAULT)
        << "Blessings should have a stacking rule (not DEFAULT)";
}

/*
 * ============================================================================
 * RUNTIME TEST FUNCTION - Can be called from worldserver for full integration
 * ============================================================================
 *
 * To run these tests with SpellMgr initialized, you can add a GM command or
 * script hook that calls RunSpellGroupStackRulesTests() after server startup.
 *
 * Example usage in a script:
 *   #include "SpellGroupStackRulesTest.h"
 *   RunSpellGroupStackRulesTests();
 */

namespace SpellGroupStackRulesTestRunner
{
    struct TestResult
    {
        int totalGroups = 0;
        int totalPairs = 0;
        int passedPairs = 0;
        int failedPairs = 0;
        int skippedPairs = 0;
        std::vector<std::string> failures;
    };

    inline const char* GetRuleName(SpellGroupStackRule rule)
    {
        switch (rule)
        {
            case SPELL_GROUP_STACK_RULE_DEFAULT: return "DEFAULT";
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE: return "EXCLUSIVE";
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER: return "FROM_SAME_CASTER";
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT: return "SAME_EFFECT";
            case SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST: return "HIGHEST";
            default: return "UNKNOWN";
        }
    }

    inline TestResult RunAllTests()
    {
        TestResult result;

        struct SpellGroupData
        {
            uint32 groupId;
            SpellGroupStackRule expectedRule;
            std::vector<uint32> spellIds;
            std::string description;
        };

        std::vector<SpellGroupData> groups = {
            {1, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {2367, 2374, 3160, 53746, 53748, 53749}, "Battle Elixir"},
            {2, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {673, 2378, 53751, 53763}, "Guardian Elixir"},
            {1001, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {57325, 57327, 57329}, "Well Fed"},
            {1002, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {19740, 48931, 48932}, "Blessing of Might"},
            {1005, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {19742, 48935, 48936}, "Blessing of Wisdom"},
            {1006, SPELL_GROUP_STACK_RULE_EXCLUSIVE, {20217, 25898, 72586}, "All Stats Percentage"},
            {1016, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {770, 16857}, "Faerie Fire"},
            {1060, SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT, {3043, 5570}, "Hit Chance Debuffs"},
            {1062, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {99, 48559, 48560}, "AP Debuffs"},
            {1089, SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST, {1126, 48469, 21849, 48470}, "Mark of the Wild"},
            {1099, SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER, {348, 30108}, "Immolate and UA"},
        };

        for (const auto& group : groups)
        {
            if (group.spellIds.size() < 2)
                continue;

            result.totalGroups++;

            for (size_t i = 0; i < group.spellIds.size(); ++i)
            {
                for (size_t j = i + 1; j < group.spellIds.size(); ++j)
                {
                    uint32 spell1 = group.spellIds[i];
                    uint32 spell2 = group.spellIds[j];

                    SpellInfo const* info1 = sSpellMgr->GetSpellInfo(spell1);
                    SpellInfo const* info2 = sSpellMgr->GetSpellInfo(spell2);

                    if (!info1 || !info2)
                    {
                        result.skippedPairs++;
                        continue;
                    }

                    result.totalPairs++;
                    auto testResult = sSpellMgr->CheckSpellGroupStackRules(info1, info2);

                    if (testResult == group.expectedRule)
                    {
                        result.passedPairs++;
                    }
                    else
                    {
                        result.failedPairs++;
                        std::string failure = "Group " + std::to_string(group.groupId)
                            + " (" + group.description + "): "
                            + "Spell " + std::to_string(spell1) + " vs " + std::to_string(spell2)
                            + " - Expected " + GetRuleName(group.expectedRule)
                            + ", Got " + GetRuleName(testResult);
                        result.failures.push_back(failure);
                    }
                }
            }
        }

        return result;
    }
}
