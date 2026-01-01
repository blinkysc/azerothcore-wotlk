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

#include "MapUpdater.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Metric.h"

MapUpdater::MapUpdater()
    : _pool(nullptr)
    , _pendingTasks(0)
    , _activated(false)
{
}

MapUpdater::~MapUpdater()
{
    deactivate();
}

void MapUpdater::activate(std::size_t num_threads)
{
    if (_activated)
        return;

    _pool = std::make_unique<WorkStealingPool>(num_threads);
    _activated = true;
}

void MapUpdater::deactivate()
{
    if (!_activated)
        return;

    wait();

    _pool->Shutdown();
    _pool.reset();
    _activated = false;
}

bool MapUpdater::activated()
{
    return _activated;
}

void MapUpdater::wait()
{
    if (!_pool)
        return;

    // Spin-wait with backoff - NO LOCKS
    uint32 spinCount = 0;

    while (_pendingTasks.load(std::memory_order_acquire) > 0)
    {
        if (spinCount < 64)
        {
            #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
                __builtin_ia32_pause();
            #else
                std::atomic_signal_fence(std::memory_order_seq_cst);
            #endif
            ++spinCount;
        }
        else
        {
            std::this_thread::yield();
        }
    }
}

void MapUpdater::schedule_update(Map& map, uint32 diff, uint32 s_diff)
{
    if (!_activated)
        return;

    _pendingTasks.fetch_add(1, std::memory_order_release);

    // Use map ID for worker affinity - same map ID goes to same worker
    // This improves cache locality
    size_t workerIndex = map.GetId() % _pool->NumWorkers();

    _pool->SubmitToWorker(workerIndex, [this, &map, diff, s_diff]() {
        METRIC_TIMER("map_update_time_diff", METRIC_TAG("map_id", std::to_string(map.GetId())));
        map.Update(diff, s_diff);
        _pendingTasks.fetch_sub(1, std::memory_order_release);
    });
}

void MapUpdater::schedule_map_preload(uint32 mapId)
{
    if (!_activated)
        return;

    _pendingTasks.fetch_add(1, std::memory_order_release);

    _pool->Submit([this, mapId]() {
        Map* map = sMapMgr->CreateBaseMap(mapId);
        LOG_INFO("server.loading", ">> Loading All Grids For Map {} ({})", map->GetId(), map->GetMapName());
        map->LoadAllGrids();
        _pendingTasks.fetch_sub(1, std::memory_order_release);
    });
}

void MapUpdater::schedule_lfg_update(uint32 diff)
{
    if (!_activated)
        return;

    _pendingTasks.fetch_add(1, std::memory_order_release);

    _pool->Submit([this, diff]() {
        sLFGMgr->Update(diff, 1);
        _pendingTasks.fetch_sub(1, std::memory_order_release);
    });
}
