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
#include "ObjectGuid.h"

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

    // Force initialization without database
    void ForceInit(ObjectGuid::LowType guidLow, uint32 entry = 0)
    {
        Object::_Create(guidLow, entry, HighGuid::Unit);

        // Set values directly without triggering UpdateSpeed and other side effects
        SetUInt32Value(UNIT_FIELD_MAXHEALTH, 5000);
        SetUInt32Value(UNIT_FIELD_HEALTH, 5000);
        SetUInt32Value(UNIT_FIELD_LEVEL, 80);
    }

    // Safe health setter that doesn't trigger side effects
    void SetTestHealth(uint32 val)
    {
        SetUInt32Value(UNIT_FIELD_HEALTH, val);
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
