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

#ifndef AZEROTHCORE_TEST_CREATURE_H
#define AZEROTHCORE_TEST_CREATURE_H

#include "Creature.h"
#include "Map.h"

class TestCreature : public Creature
{
public:
    TestCreature() : Creature() { }

    void UpdateObjectVisibility(bool /*forced*/ = true, bool /*fromUpdate*/ = false) override { }

    void AddToWorld() override { Object::AddToWorld(); }
    void RemoveFromWorld() override { Object::RemoveFromWorld(); }

    void ForceInit(ObjectGuid::LowType guidLow, CreatureTemplate const* cTemplate, Map* map)
    {
        m_creatureInfo = cTemplate;
        Object::_Create(guidLow, cTemplate->Entry, HighGuid::Unit);
        SetMap(map);
        SetFactionDirect(cTemplate->faction);

        SetLevel(cTemplate->maxlevel);
    }

    void SetFactionDirect(uint32 factionId)
    {
        SetUInt32Value(UNIT_FIELD_FACTIONTEMPLATE, factionId);
    }
};

#endif //AZEROTHCORE_TEST_CREATURE_H
