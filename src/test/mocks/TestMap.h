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

#ifndef AZEROTHCORE_TEST_MAP_H
#define AZEROTHCORE_TEST_MAP_H

#include "Map.h"
#include "DBCStores.h"
#include "ScriptMgr.h"
#include "ScriptDefines/AllMapScript.h"
#include "ScriptDefines/AllSpellScript.h"
#include "ScriptDefines/GlobalScript.h"
#include "ScriptDefines/MiscScript.h"
#include "ScriptDefines/PlayerScript.h"
#include "ScriptDefines/WorldObjectScript.h"
#include "ScriptDefines/UnitScript.h"
#include "ScriptDefines/CommandScript.h"

class TestMap : public Map
{
public:
    static void EnsureDBC(uint32 mapId = 999)
    {
        static bool initialized = false;
        if (initialized)
            return;
        initialized = true;

        // Insert a fake MapEntry into sMapStore
        auto* entry = new MapEntry();
        std::memset(entry, 0, sizeof(MapEntry));
        entry->MapID = mapId;
        entry->map_type = MAP_COMMON;
        for (auto& n : entry->name)
            n = "";
        sMapStore.SetEntry(mapId, entry);

        // Initialize all script registries needed by game objects
        ScriptRegistry<AllMapScript>::InitEnabledHooksIfNeeded(ALLMAPHOOK_END);
        ScriptRegistry<AllSpellScript>::InitEnabledHooksIfNeeded(ALLSPELLHOOK_END);
        ScriptRegistry<GlobalScript>::InitEnabledHooksIfNeeded(GLOBALHOOK_END);
        ScriptRegistry<MiscScript>::InitEnabledHooksIfNeeded(MISCHOOK_END);
        ScriptRegistry<WorldObjectScript>::InitEnabledHooksIfNeeded(WORLDOBJECTHOOK_END);
        ScriptRegistry<UnitScript>::InitEnabledHooksIfNeeded(UNITHOOK_END);
        ScriptRegistry<PlayerScript>::InitEnabledHooksIfNeeded(PLAYERHOOK_END);
        ScriptRegistry<CommandSC>::InitEnabledHooksIfNeeded(ALLCOMMANDHOOK_END);
    }

    explicit TestMap(uint32 mapId = 999)
        : Map(mapId, 0, REGULAR_DIFFICULTY, nullptr)
    {
    }
};

#endif //AZEROTHCORE_TEST_MAP_H
