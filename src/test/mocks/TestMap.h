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

#ifndef TEST_MAP_H
#define TEST_MAP_H

#include "Map.h"
#include "DBCStores.h"

/**
 * TestMap - A minimal Map implementation for unit testing
 *
 * Provides just enough functionality to test message handling that
 * requires entities to be "in world" without full map/grid infrastructure.
 *
 * Usage:
 *   TestMap map;
 *   TestCreature creature;
 *   creature.ForceInit(1001, 12345);
 *   map.AddTestEntity(&creature);
 *   // creature->IsInWorld() now returns true
 */
class TestMap : public Map
{
public:
    // Use Eastern Kingdoms (map 0) as default - it has a valid MapEntry
    TestMap(uint32 mapId = 0, uint32 instanceId = 0)
        : Map(mapId, instanceId, REGULAR_DIFFICULTY, nullptr)
    {
    }

    ~TestMap() override = default;

    // Override heavy operations to no-op
    void Update(const uint32 /*t_diff*/, const uint32 /*s_diff*/, bool /*thread*/ = true) override { }
    void UnloadAll() override { }
    bool AddPlayerToMap(Player* /*player*/) override { return true; }
    void RemovePlayerFromMap(Player* /*player*/, bool /*remove*/) override { }
    void RemoveAllPlayers() override { }
    void InitVisibilityDistance() override { m_VisibleDistance = 100.0f; }

    // Add entity to map for testing (sets m_inWorld = true)
    void AddTestEntity(WorldObject* obj)
    {
        if (obj)
        {
            obj->SetMap(this);
            obj->AddToWorld();
            _testEntities.push_back(obj);
        }
    }

    // Remove entity from map
    void RemoveTestEntity(WorldObject* obj)
    {
        if (obj)
        {
            obj->RemoveFromWorld();
            obj->ResetMap();
            _testEntities.erase(
                std::remove(_testEntities.begin(), _testEntities.end(), obj),
                _testEntities.end());
        }
    }

    // Get test entities count
    size_t GetTestEntityCount() const { return _testEntities.size(); }

private:
    std::vector<WorldObject*> _testEntities;
};

#endif // TEST_MAP_H
