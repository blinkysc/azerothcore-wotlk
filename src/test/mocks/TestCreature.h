/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TEST_CREATURE_H
#define TEST_CREATURE_H

#include "Creature.h"
#include "ObjectGuid.h"

class TestMap;

/**
 * TestCreature - A test harness for Creature that bypasses database dependencies.
 *
 * Usage:
 *   TestCreature* creature = new TestCreature();
 *   creature->ForceInitValues(1, 12345);  // guidLow, entry
 *   creature->SetTestMap(testMap);
 *   creature->SetAlive(true);
 */
class TestCreature : public Creature
{
public:
    TestCreature();
    ~TestCreature() override;

    // Override methods that require database/world access
    void UpdateObjectVisibility(bool /*forced*/ = true, bool /*fromUpdate*/ = false) override { }
    void AddToWorld() override { }
    void RemoveFromWorld() override { }

    // Force initialization without database
    void ForceInitValues(ObjectGuid::LowType guidLow, uint32 entry);

    // Test control methods - these methods are not virtual in base class
    // so we shadow them for testing purposes
    void SetTestMap(Map* map) { _testMap = map; }
    Map* GetMap() const { return _testMap ? _testMap : Creature::GetMap(); }

    void SetInWorld(bool inWorld) { _isInWorld = inWorld; }
    // Note: IsInWorld() is virtual in Object, use carefully

    void SetAlive(bool alive) { _isAlive = alive; }
    // Note: IsAlive() is virtual in Unit

    void SetPhase(uint32 phase) { _phaseMask = phase; }

    // Simplified faction/friendliness for testing
    void SetFaction(uint32 faction) { m_faction = faction; }
    uint32 GetFaction() const { return m_faction; }


    // Initialize ThreatManager for testing
    void InitializeThreatManager();

    // Access managers directly for testing
    ThreatManager& TestGetThreatMgr() { return m_threatManager; }
    CombatManager& TestGetCombatMgr() { return m_combatManager; }

private:
    Map* _testMap = nullptr;
    bool _isInWorld = true;
    bool _isAlive = true;
    uint32 _phaseMask = 1;
    uint32 m_faction = 35; // Friendly faction by default
};

#endif // TEST_CREATURE_H
