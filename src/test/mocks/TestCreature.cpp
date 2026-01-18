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

#include "TestCreature.h"
#include "ThreatManager.h"
#include "CombatManager.h"

TestCreature::TestCreature() : Creature()
{
    // Base Creature constructor already initializes most things
    // We override database-dependent behavior
}

TestCreature::~TestCreature()
{
    CleanupCombatState();
}

void TestCreature::CleanupCombatState()
{
    // Clean up threat/combat references before destruction
    // to avoid assertion failures
    m_threatManager.ClearAllThreat();
    m_combatManager.EndAllCombat();
}

void TestCreature::ForceInitValues(ObjectGuid::LowType guidLow, uint32 entry)
{
    // Initialize object GUID and entry without database
    Object::_Create(guidLow, entry, HighGuid::Unit);

    // Set creature as valid type
    m_objectType |= TYPEMASK_UNIT;
    m_objectTypeId = TYPEID_UNIT;

    // Store entry for creature template lookups
    m_originalEntry = entry;

    // Note: IsPet/IsTotem/IsTrigger are based on creature template data
    // which we don't load in tests. The default values allow threat lists.
}

void TestCreature::SetTestMap(Map* map)
{
    _testMap = map;
}

void TestCreature::SetAlive(bool alive)
{
    m_deathState = alive ? DeathState::Alive : DeathState::Dead;
}

void TestCreature::SetInWorld(bool inWorld)
{
    // m_inWorld is private, so we use the base class methods
    // Note: This requires m_uint32Values to be initialized via _Create first
    if (inWorld && !Object::IsInWorld())
        Object::AddToWorld();
    else if (!inWorld && Object::IsInWorld())
        Object::RemoveFromWorld();
}

void TestCreature::SetPhase(uint32 phase)
{
    SetPhaseMask(phase, false);
}

void TestCreature::SetFaction(uint32 faction)
{
    Unit::SetFaction(faction);
}

void TestCreature::SetupForCombatTest(Map* map, ObjectGuid::LowType guidLow, uint32 entry)
{
    ForceInitValues(guidLow, entry);
    SetTestMap(map);
    SetInWorld(true);
    SetAlive(true);
    SetPhase(1);
    SetHostileFaction();
    SetIsCombatDisallowed(false);
    ClearUnitState(UNIT_STATE_EVADE);
    ClearUnitState(UNIT_STATE_IN_FLIGHT);
    InitializeThreatManager();
}

void TestCreature::InitializeThreatManager()
{
    // Call ThreatManager::Initialize() which sets _ownerCanHaveThreatList
    m_threatManager.Initialize();
}
