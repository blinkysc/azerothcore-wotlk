/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "MapUpdater.h"
#include "DatabaseEnv.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Metric.h"

namespace
{
    /**
     * @brief Initialize thread-local database warning flags
     *
     * This ensures that each worker thread enables database synchronization warnings
     * exactly once, on first use. Thread-local initialization is required because
     * worker threads may not have database warnings configured by default.
     */
    void InitThreadLocalDatabaseWarnings()
    {
        static thread_local bool dbWarningsSet = false;
        if (!dbWarningsSet)
        {
            LoginDatabase.WarnAboutSyncQueries(true);
            CharacterDatabase.WarnAboutSyncQueries(true);
            WorldDatabase.WarnAboutSyncQueries(true);
            dbWarningsSet = true;
        }
    }
}

MapUpdater::MapUpdater()
{
}

MapUpdater::~MapUpdater()
{
    if (activated())
    {
        deactivate();
    }
}

void MapUpdater::activate(std::size_t num_threads)
{
    _threadPool = std::make_unique<WorkStealingThreadPool>();
    _threadPool->Activate(num_threads);

    LOG_INFO("server.loading", ">> MapUpdater activated with work-stealing thread pool ({} threads)", num_threads);
}

void MapUpdater::deactivate()
{
    if (_threadPool)
    {
        _threadPool->Deactivate();
        _threadPool.reset();
    }

    LOG_INFO("server.loading", ">> MapUpdater deactivated");
}

bool MapUpdater::activated() const
{
    return _threadPool && _threadPool->IsActivated();
}

void MapUpdater::wait()
{
    if (_threadPool)
    {
        _threadPool->WaitForCompletion();
    }
}

void MapUpdater::submit_task(std::function<void()> task)
{
    if (_threadPool)
    {
        _threadPool->Submit(std::move(task));
    }
    else
    {
        // Fallback: execute synchronously if thread pool is not active
        task();
    }
}

void MapUpdater::schedule_update(Map& map, uint32 diff, uint32 s_diff)
{
    // CRITICAL LIFETIME REQUIREMENT:
    // This lambda captures 'map' by reference. The Map object MUST remain alive
    // until wait() is called. The caller (typically MapMgr) is responsible for
    // ensuring the Map is not deleted before wait() completes.
    submit_task([&map, diff, s_diff]() {
        METRIC_TIMER("map_update_time_diff", METRIC_TAG("map_id", std::to_string(map.GetId())));

        InitThreadLocalDatabaseWarnings();

        map.Update(diff, s_diff);
    });
}

void MapUpdater::schedule_map_preload(uint32 mapid)
{
    submit_task([mapid]() {
        InitThreadLocalDatabaseWarnings();

        Map* map = sMapMgr->CreateBaseMap(mapid);
        LOG_INFO("server.loading", ">> Loading All Grids For Map {} ({})", map->GetId(), map->GetMapName());
        map->LoadAllGrids();
    });
}

void MapUpdater::schedule_lfg_update(uint32 diff)
{
    submit_task([diff]() {
        InitThreadLocalDatabaseWarnings();

        sLFGMgr->Update(diff, 1);
    });
}
