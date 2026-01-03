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

#ifndef _MAP_UPDATER_H_INCLUDED
#define _MAP_UPDATER_H_INCLUDED

#include "Define.h"
#include "PCQueue.h"
#include "WorkStealingPool.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

class Map;

class MapUpdater
{
public:
    MapUpdater();
    ~MapUpdater();

    void schedule_update(Map& map, uint32 diff, uint32 s_diff);
    void schedule_map_preload(uint32 mapid);
    void schedule_lfg_update(uint32 diff);
    void wait();
    void activate(std::size_t num_threads);
    void deactivate();
    bool activated();

    WorkStealingPool* GetPool() { return _useWorkStealing ? _workStealingPool.get() : nullptr; }
    bool IsWorkStealingEnabled() const { return _useWorkStealing; }

private:
    void LegacyWorkerThread();

    // Work-stealing mode (GhostActorSystem ON)
    std::unique_ptr<WorkStealingPool> _workStealingPool;

    // Legacy mode (GhostActorSystem OFF)
    std::unique_ptr<ProducerConsumerQueue<std::function<void()>>> _legacyQueue;
    std::vector<std::thread> _legacyThreads;

    alignas(64) std::atomic<size_t> _pendingTasks{0};
    bool _activated{false};
    bool _useWorkStealing{false};
};

#endif //_MAP_UPDATER_H_INCLUDED
