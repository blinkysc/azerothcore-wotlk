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

#ifndef TEST_OBJECT_ACCESSOR_H
#define TEST_OBJECT_ACCESSOR_H

#include "Object.h"
#include "ObjectGuid.h"
#include <unordered_map>
#include <unordered_set>

class WorldObject;
class Unit;
class Creature;
class Player;

/**
 * TestObjectAccessor - Simulates ObjectAccessor for testing
 *
 * Tracks registered objects and supports marking objects as "deleted"
 * to simulate stale pointer scenarios without actual memory deallocation.
 *
 * Usage:
 *   TestObjectAccessor accessor;
 *   accessor.Register(&creature);
 *   accessor.MarkDeleted(creature.GetGUID());
 *   // Now FindWorldObject returns nullptr for that GUID
 */
class TestObjectAccessor
{
public:
    void Register(WorldObject* obj)
    {
        if (obj)
            _objects[obj->GetGUID()] = obj;
    }

    void Unregister(ObjectGuid guid)
    {
        _objects.erase(guid);
    }

    void MarkDeleted(ObjectGuid guid)
    {
        _deletedGuids.insert(guid);
    }

    void ClearDeleted(ObjectGuid guid)
    {
        _deletedGuids.erase(guid);
    }

    void Clear()
    {
        _objects.clear();
        _deletedGuids.clear();
    }

    // Find object - returns nullptr if marked deleted
    WorldObject* FindWorldObject(ObjectGuid guid)
    {
        if (_deletedGuids.count(guid))
            return nullptr;

        auto it = _objects.find(guid);
        return it != _objects.end() ? it->second : nullptr;
    }

    // Find unit - returns nullptr if not a unit or marked deleted
    Unit* GetUnit(ObjectGuid guid)
    {
        WorldObject* obj = FindWorldObject(guid);
        return obj ? obj->ToUnit() : nullptr;
    }

    // Find creature - returns nullptr if not a creature or marked deleted
    Creature* GetCreature(ObjectGuid guid)
    {
        WorldObject* obj = FindWorldObject(guid);
        return obj ? obj->ToCreature() : nullptr;
    }

    // Find player - returns nullptr if not a player or marked deleted
    Player* GetPlayer(ObjectGuid guid)
    {
        WorldObject* obj = FindWorldObject(guid);
        return obj ? obj->ToPlayer() : nullptr;
    }

    // Check if object is registered
    bool Contains(ObjectGuid guid) const
    {
        return _objects.count(guid) > 0;
    }

    // Check if object is marked deleted
    bool IsMarkedDeleted(ObjectGuid guid) const
    {
        return _deletedGuids.count(guid) > 0;
    }

    size_t Size() const { return _objects.size(); }
    size_t DeletedCount() const { return _deletedGuids.size(); }

private:
    std::unordered_map<ObjectGuid, WorldObject*> _objects;
    std::unordered_set<ObjectGuid> _deletedGuids;
};

#endif // TEST_OBJECT_ACCESSOR_H
