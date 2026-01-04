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

#ifndef TEST_CREATURE_H
#define TEST_CREATURE_H

#include "Creature.h"
#include "CreatureData.h"
#include "ObjectGuid.h"

// Static minimal creature template for unit tests
inline CreatureTemplate& GetTestCreatureTemplate()
{
    static CreatureTemplate tmpl{};
    static bool initialized = false;
    if (!initialized)
    {
        tmpl.Entry = 1;
        tmpl.KillCredit[0] = 0;
        tmpl.KillCredit[1] = 0;
        tmpl.Name = "TestCreature";
        tmpl.SubName = "";
        tmpl.IconName = "";
        tmpl.unit_class = CLASS_WARRIOR;
        tmpl.minlevel = 80;
        tmpl.maxlevel = 80;
        tmpl.faction = 35; // Friendly to all
        tmpl.BaseAttackTime = 2000;
        tmpl.speed_walk = 1.0f;
        tmpl.speed_run = 1.0f;
        tmpl.scale = 1.0f;
        tmpl.unit_flags = 0;
        tmpl.unit_flags2 = 0;
        tmpl.rank = CREATURE_ELITE_NORMAL;
        tmpl.dmgschool = 0;
        tmpl.DamageModifier = 1.0f;
        tmpl.BaseVariance = 1.0f;
        tmpl.RangeVariance = 1.0f;
        tmpl.type = CREATURE_TYPE_HUMANOID;
        tmpl.type_flags = 0;
        tmpl.flags_extra = 0;
        tmpl.AIName = "NullCreatureAI";
        tmpl.ScriptID = 0;
        initialized = true;
    }
    return tmpl;
}

/**
 * TestCreature - A Creature subclass for unit testing
 *
 * Inherits from real Creature class but overrides heavy methods
 * (visibility updates, network packets) to no-op for testing.
 *
 * Usage:
 *   TestCreature creature;
 *   creature.ForceInit(1001, 12345);  // guid=1001, entry=12345
 *   creature.SetHealth(5000);
 *   creature.SetMaxHealth(10000);
 */
class TestCreature : public Creature
{
public:
    TestCreature() : Creature() {}

    // Override heavy methods to no-op
    void UpdateObjectVisibility(bool /*forced*/ = true, bool /*fromUpdate*/ = false) override { }

    // Override IsInWorld to return our test state
    [[nodiscard]] bool IsInWorld() const override { return _testInWorld; }

    // Force initialization without database
    void ForceInit(ObjectGuid::LowType guidLow, uint32 entry = 0)
    {
        Object::_Create(guidLow, entry, HighGuid::Unit);

        // Set up minimal creature template for UpdateSpeed and other checks
        m_creatureInfo = &GetTestCreatureTemplate();

        // Set values directly without triggering UpdateSpeed and other side effects
        SetUInt32Value(UNIT_FIELD_MAXHEALTH, 5000);
        SetUInt32Value(UNIT_FIELD_HEALTH, 5000);
        SetUInt32Value(UNIT_FIELD_LEVEL, 80);

        // Mark as alive for handler checks
        // Note: Don't set _testInWorld here - let AddToWorld() handle it
        m_deathState = DeathState::Alive;
    }

    // Override AddToWorld/RemoveFromWorld to control our test in-world state
    // Don't call base class - they require infrastructure we don't have
    void AddToWorld() override
    {
        _testInWorld = true;
    }

    void RemoveFromWorld() override
    {
        _testInWorld = false;
    }

    // Allow tests to control in-world state
    void SetTestInWorld(bool inWorld) { _testInWorld = inWorld; }

private:
    bool _testInWorld = false;

public:

    // Safe health setter that doesn't trigger side effects
    // Also updates max health if needed to allow setting higher values
    void SetTestHealth(uint32 val)
    {
        if (val > GetUInt32Value(UNIT_FIELD_MAXHEALTH))
        {
            SetUInt32Value(UNIT_FIELD_MAXHEALTH, val);
        }
        SetUInt32Value(UNIT_FIELD_HEALTH, val);
    }

    // Safe health getter for testing
    uint32 GetTestHealth() const
    {
        return GetUInt32Value(UNIT_FIELD_HEALTH);
    }

    // Safe max health setter
    void SetTestMaxHealth(uint32 val)
    {
        SetUInt32Value(UNIT_FIELD_MAXHEALTH, val);
    }

    // Track validity for segfault tests
    bool _testMarkedDeleted = false;

    void MarkTestDeleted()
    {
        _testMarkedDeleted = true;
    }

    bool IsTestDeleted() const { return _testMarkedDeleted; }
};

#endif // TEST_CREATURE_H
