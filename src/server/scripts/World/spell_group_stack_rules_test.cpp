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

#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "SpellMgr.h"
#include "WorldScript.h"
#include <map>
#include <set>
#include <vector>

class SpellGroupStackRulesTestScript : public WorldScript
{
public:
    SpellGroupStackRulesTestScript() : WorldScript("SpellGroupStackRulesTestScript", {WORLDHOOK_ON_STARTUP}) { }

    void OnStartup() override
    {
        if (!sConfigMgr->GetOption<bool>("SpellGroupStackRulesTest.Enable", false))
            return;

        LOG_INFO("server.loading", "");
        LOG_INFO("server.loading", "=================================================================");
        LOG_INFO("server.loading", "  Running Spell Group Stack Rules Integration Tests (PR #23346)");
        LOG_INFO("server.loading", "=================================================================");

        RunAllTests();
    }

private:
    struct SpellGroupData
    {
        uint32 groupId;
        SpellGroupStackRule expectedRule;
        std::vector<uint32> spellIds;
        std::string description;
    };

    const char* GetRuleName(SpellGroupStackRule rule)
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

    std::vector<SpellGroupData> LoadSpellGroupsFromDB()
    {
        std::vector<SpellGroupData> groups;
        std::map<uint32, SpellGroupData> groupMap;

        QueryResult result = WorldDatabase.Query(
            "SELECT sg.id, sgsr.stack_rule, sgsr.description, sg.spell_id "
            "FROM spell_group sg "
            "JOIN spell_group_stack_rules sgsr ON sg.id = sgsr.group_id "
            "WHERE sg.spell_id > 0 "
            "ORDER BY sg.id, sg.spell_id");

        if (!result)
        {
            LOG_ERROR("server.loading", "SpellGroupStackRulesTest: No spell groups found in database!");
            return groups;
        }

        do
        {
            Field* fields = result->Fetch();
            uint32 groupId = fields[0].Get<uint32>();
            uint32 stackRule = fields[1].Get<uint32>();
            std::string description = fields[2].Get<std::string>();
            uint32 spellId = fields[3].Get<uint32>();

            if (groupMap.find(groupId) == groupMap.end())
            {
                SpellGroupData data;
                data.groupId = groupId;
                data.expectedRule = static_cast<SpellGroupStackRule>(stackRule);
                data.description = description.empty() ? "Group " + std::to_string(groupId) : description;
                groupMap[groupId] = data;
            }

            groupMap[groupId].spellIds.push_back(spellId);
        } while (result->NextRow());

        for (auto& pair : groupMap)
        {
            groups.push_back(pair.second);
        }

        return groups;
    }

    void RunAllTests()
    {
        std::vector<SpellGroupData> groups = LoadSpellGroupsFromDB();

        if (groups.empty())
        {
            LOG_ERROR("server.loading", "SpellGroupStackRulesTest: No groups loaded from database!");
            return;
        }

        LOG_INFO("server.loading", "SpellGroupStackRulesTest: Loaded {} groups from database", groups.size());

        uint32 totalPassed = 0;
        uint32 totalFailed = 0;
        uint32 totalSkipped = 0;
        uint32 crossPassed = 0;
        uint32 crossFailed = 0;

        std::vector<std::string> failures;

        LOG_INFO("server.loading", "TEST 1: Same-Group Spell Pairs");
        LOG_INFO("server.loading", "-------------------------------");

        for (const auto& group : groups)
        {
            if (group.spellIds.size() < 2)
                continue;

            uint32 groupPassed = 0;
            uint32 groupFailed = 0;

            for (size_t i = 0; i < group.spellIds.size(); ++i)
            {
                SpellInfo const* spell1 = sSpellMgr->GetSpellInfo(group.spellIds[i]);
                if (!spell1)
                {
                    totalSkipped++;
                    continue;
                }

                for (size_t j = i + 1; j < group.spellIds.size(); ++j)
                {
                    SpellInfo const* spell2 = sSpellMgr->GetSpellInfo(group.spellIds[j]);
                    if (!spell2)
                    {
                        totalSkipped++;
                        continue;
                    }

                    SpellGroupStackRule result = sSpellMgr->CheckSpellGroupStackRules(spell1, spell2);

                    if (result == group.expectedRule)
                    {
                        groupPassed++;
                        totalPassed++;
                    }
                    else
                    {
                        groupFailed++;
                        totalFailed++;
                        if (failures.size() < 10)
                        {
                            failures.push_back("Group " + std::to_string(group.groupId) + " (" + group.description +
                                "): Spell " + std::to_string(group.spellIds[i]) + " vs " +
                                std::to_string(group.spellIds[j]) + " - Expected " +
                                GetRuleName(group.expectedRule) + ", Got " + GetRuleName(result));
                        }
                    }
                }
            }

            if (groupFailed > 0)
            {
                LOG_INFO("server.loading", "  Group {} ({}): {} FAILED, {} passed",
                    group.groupId, group.description, groupFailed, groupPassed);
            }
        }

        LOG_INFO("server.loading", "TEST 2: Cross-Group Spell Pairs (should return DEFAULT)");
        LOG_INFO("server.loading", "---------------------------------------------------------");

        std::vector<std::pair<uint32, uint32>> crossGroupSamples;
        for (const auto& group : groups)
        {
            if (!group.spellIds.empty())
            {
                for (uint32 spellId : group.spellIds)
                {
                    if (sSpellMgr->GetSpellInfo(spellId))
                    {
                        crossGroupSamples.push_back({group.groupId, spellId});
                        break;
                    }
                }
            }
        }

        uint32 crossTestCount = 0;
        for (size_t i = 0; i < crossGroupSamples.size() && crossTestCount < 100; ++i)
        {
            for (size_t j = i + 1; j < crossGroupSamples.size() && crossTestCount < 100; ++j)
            {
                SpellInfo const* spell1 = sSpellMgr->GetSpellInfo(crossGroupSamples[i].second);
                SpellInfo const* spell2 = sSpellMgr->GetSpellInfo(crossGroupSamples[j].second);

                if (!spell1 || !spell2)
                    continue;

                SpellGroupStackRule result = sSpellMgr->CheckSpellGroupStackRules(spell1, spell2);

                if (result == SPELL_GROUP_STACK_RULE_DEFAULT)
                {
                    crossPassed++;
                }
                else
                {
                    crossFailed++;
                    LOG_INFO("server.loading", "  Cross-group FAIL: Group {} vs {}: {} vs {} = {}",
                        crossGroupSamples[i].first, crossGroupSamples[j].first,
                        crossGroupSamples[i].second, crossGroupSamples[j].second,
                        GetRuleName(result));
                }
                crossTestCount++;
            }
        }

        LOG_INFO("server.loading", "TEST 3: Issue-Specific Regression Tests");
        LOG_INFO("server.loading", "----------------------------------------");

        SpellInfo const* faerieFire = sSpellMgr->GetSpellInfo(770);
        SpellInfo const* demoRoar = sSpellMgr->GetSpellInfo(99);
        if (faerieFire && demoRoar)
        {
            SpellGroupStackRule result = sSpellMgr->CheckSpellGroupStackRules(faerieFire, demoRoar);
            if (result == SPELL_GROUP_STACK_RULE_DEFAULT)
            {
                LOG_INFO("server.loading", "  Issue #22619 (Faerie Fire vs Demo Roar): PASS");
            }
            else
            {
                LOG_INFO("server.loading", "  Issue #22619 (Faerie Fire vs Demo Roar): FAIL (got {})", GetRuleName(result));
            }
        }

        SpellInfo const* drums = sSpellMgr->GetSpellInfo(72586);
        SpellInfo const* bok = sSpellMgr->GetSpellInfo(20217);
        if (drums && bok)
        {
            SpellGroupStackRule result = sSpellMgr->CheckSpellGroupStackRules(drums, bok);
            LOG_INFO("server.loading", "  Issue #19324 (Drums vs BoK): {} ({})",
                result != SPELL_GROUP_STACK_RULE_DEFAULT ? "PASS" : "NEEDS REVIEW", GetRuleName(result));
        }

        SpellInfo const* battleElixir = sSpellMgr->GetSpellInfo(53746);
        SpellInfo const* guardianElixir = sSpellMgr->GetSpellInfo(53747);
        if (battleElixir && guardianElixir)
        {
            SpellGroupStackRule result = sSpellMgr->CheckSpellGroupStackRules(battleElixir, guardianElixir);
            if (result == SPELL_GROUP_STACK_RULE_DEFAULT)
            {
                LOG_INFO("server.loading", "  Battle vs Guardian Elixir stacking: PASS");
            }
            else
            {
                LOG_INFO("server.loading", "  Battle vs Guardian Elixir stacking: FAIL (got {})", GetRuleName(result));
            }
        }

        LOG_INFO("server.loading", "=================================================================");
        LOG_INFO("server.loading", "  SPELL GROUP STACK RULES TEST RESULTS");
        LOG_INFO("server.loading", "=================================================================");
        LOG_INFO("server.loading", "  Same-Group Tests:");
        LOG_INFO("server.loading", "    Groups tested:    {}", groups.size());
        LOG_INFO("server.loading", "    Total pairs:      {}", totalPassed + totalFailed);
        LOG_INFO("server.loading", "    Passed:           {}", totalPassed);
        LOG_INFO("server.loading", "    Failed:           {}", totalFailed);
        LOG_INFO("server.loading", "    Skipped:          {}", totalSkipped);
        LOG_INFO("server.loading", "  Cross-Group Tests:");
        LOG_INFO("server.loading", "    Pairs tested:     {}", crossPassed + crossFailed);
        LOG_INFO("server.loading", "    Passed:           {}", crossPassed);
        LOG_INFO("server.loading", "    Failed:           {}", crossFailed);

        if (totalFailed == 0 && crossFailed == 0)
        {
            LOG_INFO("server.loading", "=================================================================");
            LOG_INFO("server.loading", "  ALL TESTS PASSED!");
            LOG_INFO("server.loading", "=================================================================");
        }
        else
        {
            LOG_INFO("server.loading", "=================================================================");
            LOG_INFO("server.loading", "  SOME TESTS FAILED!");
            for (const auto& failure : failures)
            {
                LOG_INFO("server.loading", "    {}", failure);
            }
            if (failures.size() == 10 && totalFailed > 10)
            {
                LOG_INFO("server.loading", "    ... and {} more failures", totalFailed - 10);
            }
            LOG_INFO("server.loading", "=================================================================");
        }
    }
};

void AddSC_spell_group_stack_rules_test()
{
    new SpellGroupStackRulesTestScript();
}
