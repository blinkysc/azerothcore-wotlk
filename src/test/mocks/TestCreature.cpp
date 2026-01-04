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

void TestCreature::InitializeThreatManager()
{
    // Call ThreatManager::Initialize() which sets _ownerCanHaveThreatList
    m_threatManager.Initialize();
}
