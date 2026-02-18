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

#ifndef TEST_DBC_STORES_H
#define TEST_DBC_STORES_H

#include "DBCStores.h"
#include "DBCStructure.h"

/**
 * TestDBC - Utilities for initializing minimal DBC data in unit tests
 *
 * DBCStorage::SetEntry() allows programmatic population without loading
 * binary DBC files. This enables tests to run with minimal DBC entries
 * for Maps, Spells, etc.
 *
 * Usage:
 *   TestDBC::InitializeMinimalStores();  // Call once in test setup
 *   // Now sMapStore.LookupEntry(0) returns valid MapEntry
 */
namespace TestDBC
{
    /**
     * Initialize minimal DBC entries required for Map construction
     * and basic entity operations.
     *
     * Idempotent - safe to call multiple times.
     */
    inline void InitializeMinimalStores()
    {
        static bool initialized = false;
        if (initialized)
            return;

        // Map 0 (Eastern Kingdoms) - minimal entry for TestMap
        MapEntry* map0 = new MapEntry{};
        map0->MapID = 0;
        map0->map_type = MAP_COMMON;
        map0->Flags = 0;
        map0->name[0] = "Eastern Kingdoms";  // English name
        map0->linked_zone = 0;
        map0->multimap_id = 0;
        map0->entrance_map = -1;
        map0->entrance_x = 0.0f;
        map0->entrance_y = 0.0f;
        map0->expansionID = 0;
        map0->maxPlayers = 0;

        sMapStore.SetEntry(0, map0);

        initialized = true;
    }

    /**
     * Add a custom map entry for testing specific map scenarios.
     *
     * @param mapId The map ID to create
     * @param name The map name
     * @param mapType MAP_COMMON, MAP_INSTANCE, MAP_RAID, etc.
     */
    inline void AddMapEntry(uint32 mapId, const char* name, uint32 mapType = MAP_COMMON)
    {
        MapEntry* entry = new MapEntry{};
        entry->MapID = mapId;
        entry->map_type = mapType;
        entry->Flags = 0;
        entry->name[0] = name;
        entry->linked_zone = 0;
        entry->multimap_id = 0;
        entry->entrance_map = -1;
        entry->entrance_x = 0.0f;
        entry->entrance_y = 0.0f;
        entry->expansionID = 0;
        entry->maxPlayers = 0;

        sMapStore.SetEntry(mapId, entry);
    }
}

#endif // TEST_DBC_STORES_H
